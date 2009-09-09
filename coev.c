/* 
 * Bare-C io-scheduled coroutines: based on greenlet module
 *
 * Authors: 
 *      Armin Rigo, Christian Tismer (greenlet module)
 *      Alexander Sabourenkov  (C/Python split, ioscheduler)
 *
 * License: MIT License
 *
 */

#include <string.h>
#include <stddef.h>
#include <stdio.h>

#include "coev.h"

/***********************************************************

Note: comments are mostly left verbatim from the original code.
PyGreenlet is now named coev_t, and greenlet is another name for 
coroutine.


A PyGreenlet is a range of C stack addresses that must be
saved and restored in such a way that the full range of the
stack contains valid data when we switch to it.

Stack layout for a greenlet:

               |     ^^^       |
               |  older data   |
               |               |
  stack_stop . |_______________|
        .      |               |
        .      | greenlet data |
        .      |   in stack    |
        .    * |_______________| . .  _____________  stack_copy + stack_saved
        .      |               |     |             |
        .      |     data      |     |greenlet data|
        .      |   unrelated   |     |    saved    |
        .      |      to       |     |   in heap   |
 stack_start . |     this      | . . |_____________| stack_copy
               |   greenlet    |
               |               |
               |  newer data   |
               |     vvv       |


Note that a greenlet's stack data is typically partly at its correct
place in the stack, and partly saved away in the heap, but always in
the above configuration: two blocks, the more recent one in the heap
and the older one still in the stack (either block may be empty).

Greenlets are chained: each points to the previous greenlet, which is
the one that owns the data currently in the C stack above my
stack_stop.  The currently running greenlet is the first element of
this chain.  The main (initial) greenlet is the last one.  Greenlets
whose stack is entirely in the heap can be skipped from the chain.

The chain is not related to execution order, but only to the order
in which bits of C stack happen to belong to greenlets at a particular
point in time.

The main greenlet doesn't have a stack_stop: it is responsible for the
complete rest of the C stack, and we don't know where it begins.  We
use (char*) -1, the largest possible address.

States:
  stack_stop == NULL && stack_start == NULL:  did not start yet
  stack_stop != NULL && stack_start == NULL:  already finished
  stack_stop != NULL && stack_start != NULL:  active

The running greenlet's stack_start is undefined but not NULL.

 ***********************************************************/

/*** global state ***/

/* In the presence of multithreading, this is a bit tricky:

   - ts_current always store a reference to a greenlet, but it is
     not really the current greenlet after a thread switch occurred.

   - each *running* greenlet uses its run_info field to know which
     thread it is attached to.  A greenlet can only run in the thread
     where it was created.  This run_info is a ref to tstate->dict.

   - the thread state dict is used to save and restore ts_current,
     using the dictionary key 'ts_curkey'.
     
   we basically store all context in thread-locals (because thread state dict
    is now out of scope with the rest of python stuff */

#ifdef THREADING_MADNESS
#define TLS_ATTR __thread
#define CROSSTHREAD_CHECK(target, rv) \
    if (pthread_equal(ts_current->thread, (target)->thread)) { \
	_fm.crossthread_fail(ts_current, (target), (p)); \
	return NULL; \
    }

#else
#define TLS_ATTR
#define CROSSTHREAD_CHECK(target, rv)
#endif

/* The current greenlet in this thread state (holds a reference) */
static TLS_ATTR coev_t *ts_current;
/* Holds a reference to the switching-from stack during the slp switch */
static TLS_ATTR coev_t *ts_origin;
/* Holds a reference to the switching-to stack during the slp switch */
static TLS_ATTR coev_t *ts_target;
/* NULL if error, otherwise args tuple to pass around during slp switch */
static TLS_ATTR void *ts_passaround;

static TLS_ATTR int ts_count;

static TLS_ATTR coev_t *ts_root;

/* flag to signal that this switch is from scheduler */
static TLS_ATTR int ts_switch_from_scheduler;

static coev_frameth_t _fm;

#define coev_dprintf(fmt, args...) do { if (_fm.debug_output) _fm.dprintf(fmt, ## args); } while(0)
#define coev_dump(msg, coev) do { if (_fm.dump_coevs) _coev_dump(msg, coev); } while(0)


static void sleep_callback(struct ev_loop *, ev_timer *, int );
static void iotimeout_callback(struct ev_loop *, ev_timer *, int );

/** initialize a root greenlet for a thread */
static void
coev_init_root(coev_t *root) {
    if (ts_current != NULL) 
        _fm.abort("coev_init_root(): second initialization refused.");
    ts_current = root;
    ts_root = root;
    /* set parent et al to zero/NULL */
    memset(ts_current, 0, sizeof(coev_t));
    ts_current->stack_start = (char*) 1;
    ts_current->stack_stop = (char*) -1;
#ifdef THREADING_MADNESS
    ts_current->thread = pthread_self();
#endif
    ts_current->id = 0;
    ts_current->state = CSTATE_CURRENT;
    ts_current->parent = NULL;
    
    ev_timer_init(&ts_current->io_timer, iotimeout_callback, 23., 42.);
    ev_timer_init(&ts_current->sleep_timer, sleep_callback, 23., 42.);
}

void
coev_init(coev_t *child, void *(*run)(coev_t *self, void *)) {
    if (ts_current == NULL)
        _fm.abort("coev_init(): library not initialized");
    
    memset(child, 0, sizeof(coev_t));
    child->parent = ts_current;
    child->run = run;
    child->id = ts_count++;
    child->state = CSTATE_INIT;
    ev_timer_init(&child->io_timer, iotimeout_callback, 23., 42.);
    ev_timer_init(&child->sleep_timer, sleep_callback, 23., 42.);
    /* rest of initialization will occur at switch time */
}

void
coev_free(coev_t *corpse) {
    if (corpse->stack_copy)
        _fm.free(corpse->stack_copy);
}

#define MAX_CHARS_PER_LEVEL 12
#define MAX_LEVELS_REPORTED 0x100
static TLS_ATTR char tp_onebuf[MAX_CHARS_PER_LEVEL + 4];
static TLS_ATTR char tp_scrpad[MAX_CHARS_PER_LEVEL*MAX_LEVELS_REPORTED + 4];


/* returns memory allocated with init-time supplied allocator */
char *
coev_treepos(coev_t *coio) {
    coev_t *c = coio;
    int rvlen;
    char *rv;
    int written;
    char *curpos;
    
    curpos = tp_scrpad + sizeof(tp_scrpad) - 1;
    *curpos = '\0';
    curpos -= 1;
    rvlen = 1;
    while (c) {
        written = snprintf(tp_onebuf, sizeof(tp_onebuf), " %d", c->id);
        memcpy(curpos - written, tp_onebuf, written);
        curpos -= written;
        rvlen += written;
        c = c->parent;
    }
    rv = _fm.malloc(rvlen);
    if (rv)
        memcpy(rv, curpos+1, rvlen-1); /* strip leading space */
    return rv;
}

coev_t *
coev_current(void) {
    return ts_current;
}


static void
_coev_dump(char *m, coev_t *c) { 
    char *tp;
    
    if (m) 
        coev_dprintf("%s\n", m);
    tp = coev_treepos(c);
    coev_dprintf( "coev_t<%p> (current<%p> root<%p>:\n"
            "    treepos: [%s]\n"
            "    is_current: %d\n"
            "    is_root: %d\n"
            "    is_started: %d\n"
            "    is_active: %d\n"
            "    stack_start: %p\n"
            "    stack_stop: %p\n"
            "    stack_copy: %p\n"
            "    stack_saved: %ld\n"
            "    stack_prev: %p\n"
            "    parent: %p\n"
            "    run: %p\n"
            "    state: %d\n"
            "    io watcher  active=%d pending=%d\n"
            "    io timeout  active=%d pending=%d\n"
            "    sleep timer active=%d pending=%d\n",
        c,
        ts_current,
        ts_root,
        tp,
        c == ts_current,
        c == ts_root,
        COEV_STARTED(c),
        COEV_ACTIVE(c),
        c->stack_start,
        c->stack_stop,
        c->stack_copy,
        c->stack_saved,
        c->stack_prev,
        c->parent,
        c->run,
        c->state,
        ev_is_active(&c->watcher), ev_is_pending(&c->watcher),
        ev_is_active(&c->io_timer), ev_is_pending(&c->io_timer),
        ev_is_active(&c->sleep_timer), ev_is_pending(&c->sleep_timer)
        );
    fflush(stdout);
    _fm.free(tp);
}

static int
coev_save(coev_t *g, char *stop) {
    /* Save more of g's stack into the heap -- at least up to 'stop'

       g->stack_stop |________|
		     |        |
		     |    __ stop       . . . . .
		     |        |    ==>  .       .
		     |________|          _______
		     |        |         |       |
		     |        |         |       |
      g->stack_start |        |         |_______| g->stack_copy

     */
    long sz1 = g->stack_saved;
    long sz2 = stop - g->stack_start;

    if (sz2 > sz1) {
	char *c = _fm.realloc(g->stack_copy, sz2);
	if (!c)
	    return -1;
	
	memcpy(c+sz1, g->stack_start+sz1, sz2-sz1);
	g->stack_copy = c;
	g->stack_saved = sz2;
        _fm.c_bytes_copied += sz2-sz1;
    }
    return 0;
}

static void 
slp_restore_state(void)
{
    coev_t* g = ts_target;
    
    /* Restore the heap copy back into the C stack */
    if (g->stack_saved != 0) {
	memcpy(g->stack_start, g->stack_copy, g->stack_saved);
        _fm.c_bytes_copied += g->stack_saved;
	_fm.free(g->stack_copy);
	g->stack_copy = NULL;
	g->stack_saved = 0;
    }
    if (ts_current->stack_stop == g->stack_stop)
	g->stack_prev = ts_current->stack_prev;
    else
	g->stack_prev = ts_current;
}

static int 
slp_save_state(char* stackref)
{
    /* must free all the C stack up to target_stop */
    char* target_stop = ts_target->stack_stop;
    
    if (ts_current->stack_start == NULL)
	ts_current = ts_current->stack_prev;  /* not saved if dying */
    else
	ts_current->stack_start = stackref;
    
    while (ts_current->stack_stop < target_stop) {
	/* ts_current is entirely within the area to free */
	if (coev_save(ts_current, ts_current->stack_stop))
	    return -1;  /* XXX */
	ts_current = ts_current->stack_prev;
    }
    if (ts_current != ts_target) {
	if (coev_save(ts_current, target_stop))
	    return -1;  /* XXX */
    }
    return 0;
}

/*
 * the following macros are spliced into the OS/compiler
 * specific code, in order to simplify maintenance.
 */

#define SLP_SAVE_STATE(stackref, stsizediff)		\
  stackref += STACK_MAGIC;				\
  if (slp_save_state((char*)stackref)) return -1;	\
  if (!COEV_ACTIVE(ts_target)) return 1;		\
  stsizediff = ts_target->stack_start - (char*)stackref

#define SLP_RESTORE_STATE()			\
  slp_restore_state()


#define SLP_EVAL
#include "slp_platformselect.h"

#ifndef STACK_MAGIC
#error "evgreenlet needs to be ported to this platform,\
 or teached how to detect your compiler properly."
#endif /* !STACK_MAGIC */


/* This is a trick to prevent the compiler from inlining or
   removing the frames */
int (*_coev_slp_switch) (void);
int (*_coev_switchstack) (void);
void (*_coev_initialstub) (void *);

static int 
coev_switchstack(void)
{
    /* perform a stack switch according to some global variables
       that must be set before:
       - ts_current: current greenlet (holds a reference)
       - ts_target: greenlet to switch to
       - ts_passaround: NULL if PyErr_Occurred(),
		 else a tuple of args sent to ts_target (holds a reference)
    */
    int rv;
    
    ts_origin = ts_current;
    rv = _coev_slp_switch(); 
    if (rv < 0)
	/* error */
	ts_passaround = NULL;
    else
	ts_current = ts_target;
    
    return rv;
}

/** entry point: function for voluntary switching between coroutines */
coerv_t
coev_switch(coev_t *target, void *p) {
    CROSSTHREAD_CHECK(target, p);
    
    if (target == ts_current) {
        coerv_t rv;
        rv.status = COERV_SWITCH_TO_SELF;
        rv.value  = NULL;
        rv.from   = ts_current;
        return rv;
    }
    
    if (_fm.debug_output) {
        char *s, *t;
        
        if (_fm.switch_notify)
            _fm.switch_notify(ts_current, target, ts_switch_from_scheduler);
        s = coev_treepos(ts_current);
        t = coev_treepos(target);
        coev_dprintf("coev_switch(): from [%s] to [%s]; wait %d\n", 
            s, t, ts_switch_from_scheduler);
        _fm.free(s);
        _fm.free(t);
        coev_dump("switch, current", ts_current);
        coev_dump("switch, target", target);        
    }
    
    ts_passaround = p;
    _fm.c_switches++;
    
    /* if this is a voluntary switch, set status here. */
    if (ts_current->state == CSTATE_CURRENT)
        ts_current->state = CSTATE_IDLE;
    
    /* find the real target by ignoring dead greenlets,
       and if necessary starting a greenlet. */
    while (1) {
	if (COEV_ACTIVE(target)) {
            target->state = CSTATE_CURRENT;
	    ts_target = target;
            if (_fm.debug_output) {
                char *tp = coev_treepos(target);
                coev_dprintf("coev_switch(): actual target is [%s] (ACTIVE)\n", tp);
                _fm.free(tp);
            }
	    _coev_switchstack();
            {
                coerv_t rv;
                rv.from = ts_origin;
                rv.value = ts_passaround;
                rv.status = ts_switch_from_scheduler;
                return rv;
            }
	}
	if (!COEV_STARTED(target)) {
	    void *dummymarker;
            target->state = CSTATE_CURRENT;
	    ts_target = target;
            if (_fm.debug_output) {
                char *tp = coev_treepos(target);
                coev_dprintf("coev_switch(): actual target is [%s] (STARTED)\n", tp);
                _fm.free(tp);
            }
	    _coev_initialstub(&dummymarker);
            {
                coerv_t rv;
                rv.from = ts_origin;
                rv.value = ts_passaround;
                rv.status = ts_switch_from_scheduler;
                return rv;
            }
	}
	target = target->parent;
    }
}

static void coev_sched_cleanup(coev_t *);
/** the first function to run in the greenlet after its activation */
static void 
coev_initialstub(void *mark) {
    int err;

    /* ts_target->run is the function to call in the new greenlet */
    if (!ts_target)
        _fm.abort("coev_initialstub(): ts_target is NULL");
    if (!ts_target->run)
        _fm.abort("coev_initialstub(): ts_target has no runner");
    
    /* start the greenlet */
    ts_target->stack_start = NULL;
    ts_target->stack_stop = (char *) mark;
    if (ts_current->stack_start == NULL)    /* ts_current is dying WTF? */
	ts_target->stack_prev = ts_current->stack_prev;
    else
	ts_target->stack_prev = ts_current;

    coev_dump("coev_initialstub(), target, pre-switchstack", ts_target);
    err = _coev_switchstack();
    /* returns twice!
       The 1st time with err=1: we are in the new greenlet
       The 2nd time with err=0: back in the caller's greenlet
    */
    if (err == 1) {
	/* in the new greenlet */
	void *result;
	void *args  = ts_passaround;
	ts_current->stack_start = (char *) 1;  /* running */

	if (args == NULL)    /* pending exception */
	    result = NULL;
	else 
	    result = ts_current->run(ts_current, args);
        
        if (_fm.debug_output) {
            char *tp = coev_treepos(ts_current);
            coev_dprintf("coev_initialstub(): [%s] returns %p\n", tp, result);
            _fm.free(tp);
        }
        
	/* signal death to the framework */
	if (_fm.death)
	    _fm.death(ts_current);
	
	/* clean up any scheduler stuff */
	coev_sched_cleanup(ts_current);
	
	/* jump back to parent */
	ts_current->stack_start = NULL;  /* dead */
	coev_switch(ts_current->parent, result);
	/* must not return from here! */
	_fm.abort("coroutines cannot continue");
    }
    /* back in the parent */
}

/* ioscheduler functions */
static
struct _coev_scheduler_stuff {
    coev_t *scheduler;
    struct ev_loop *loop;
    struct ev_signal intsig;
} ts_scheduler;

/** for some reason there's a problem with signals.
    so by default we handle SIGINT by stopping the loop
        THIS IS A BIG FIXME */
static void 
intsig_cb(struct ev_loop *loop, ev_signal *w, int signum) {
    _fm.inthdlr();
}

static void
coev_sched_cleanup(coev_t *corpse) {
    ev_io_stop(ts_scheduler.loop, &corpse->watcher);
    /* FIXME ev_timer_stop or something */
}

/* in your io_scheduler, stopping your watcher, switching to your waiter */
static void 
io_callback(struct ev_loop *loop, ev_io *w, int revents) {
    coev_t *waiter = (coev_t *) ( ((char *)w) - offsetof(coev_t, watcher) );
    ev_io_stop(loop, w);
    
    ts_switch_from_scheduler = COERV_EVENT;
    coev_switch(waiter, NULL);
}

static void
iotimeout_callback(struct ev_loop *loop, ev_timer *w, int revents) {
    coev_t *waiter = (coev_t *) ( ((char *)w) - offsetof(coev_t, io_timer) );

    ev_io_stop(ts_scheduler.loop, &waiter->watcher);
    ev_timer_stop(ts_scheduler.loop, w);
    ts_switch_from_scheduler = COERV_TIMEOUT; /* this is timeout */
    coev_switch(waiter, NULL); 
}

static void
sleep_callback(struct ev_loop *loop, ev_timer *w, int revents) {
    coev_t *waiter = (coev_t *) ( ((char *)w) - offsetof(coev_t, sleep_timer) );

    ts_switch_from_scheduler = COERV_EVENT; /* this is scheduled */
    coev_switch(waiter, NULL); 
}

/* sets current coro to wait for revents on fd, switches to scheduler */
coerv_t 
coev_wait(int fd, int revents, ev_tstamp timeout) {
    coerv_t rv;
    coev_t *target;
    
    if (ts_current == ts_scheduler.scheduler) {
        rv.from = NULL;
        rv.value = NULL;
        rv.status = COERV_WAIT_IN_SCHEDULER;
        return rv;
    }
    
    ts_current->io_timer.repeat = timeout;
    ev_timer_again(ts_scheduler.loop, &ts_current->io_timer);
    
    ev_io_init(&ts_current->watcher, io_callback, fd, revents);
    ev_io_start(ts_scheduler.loop, &ts_current->watcher);

    _fm.c_waits++;
    
    if (ts_scheduler.scheduler) {
        target = ts_scheduler.scheduler;
    } else if (ts_current->parent) {
        target = ts_current->parent;
        ts_switch_from_scheduler = COERV_SCHEDULER_NEEDED;
    } else {
        /* epic fail: nowhere to switch. */
        rv.status = COERV_NOWHERE_TO_SWITCH;
        rv.value = NULL;
        rv.from = NULL;
        ts_switch_from_scheduler = 0;
        return rv;
    }
    
    ts_current->state = CSTATE_IOWAIT;
    rv = coev_switch(target, NULL);
    
    if (   (rv.status == COERV_EVENT) 
        || (rv.status == COERV_VOLUNTARY)) {
        /* handle I/O event or switch2wait */
        ev_timer_stop(ts_scheduler.loop, &ts_current->io_timer);
    }
    ts_switch_from_scheduler = 0;
    return rv;
}

coerv_t
coev_sleep(ev_tstamp amount) {
    coerv_t rv;
    coev_t *target;
    
    ts_current->sleep_timer.repeat = amount;
    
    ev_timer_again(ts_scheduler.loop, &ts_current->sleep_timer);
    _fm.c_sleeps++;

    if (ts_scheduler.scheduler) {
        target = ts_scheduler.scheduler;
    } else if (ts_current->parent) {
        target = ts_current->parent;
        ts_switch_from_scheduler = COERV_SCHEDULER_NEEDED;
    } else {
        /* epic fail: nowhere to switch. */
        rv.status = COERV_NOWHERE_TO_SWITCH;
        rv.value = NULL;
        rv.from = NULL;
        ts_switch_from_scheduler = 0;
        return rv;
    }
    
    ts_current->state = CSTATE_SLEEP;
    rv = coev_switch(target, NULL);
    if  (rv.status == COERV_VOLUNTARY) {
        /* handle switch2sleep */
        ev_timer_stop(ts_scheduler.loop, &ts_current->sleep_timer);
    }
    ts_switch_from_scheduler = 0;
    return rv;
}

void 
coev_loop(int flags) {
    if (ts_scheduler.scheduler == ts_current)
        _fm.abort("recursive call of coev_loop()");
    ts_scheduler.scheduler = ts_current;
    ev_loop(ts_scheduler.loop, flags);
}

void
coev_unloop(void) {
    ts_scheduler.scheduler = NULL;
    ev_unloop(ts_scheduler.loop, EVUNLOOP_ALL);
}

void
coev_getstats(uint64_t *sw, uint64_t *wa, uint64_t *sl, uint64_t *bc) {
    *sw = _fm.c_switches;
    *wa = _fm.c_waits;
    *sl = _fm.c_sleeps;
    *bc = _fm.c_bytes_copied;
}

void
coev_setdebug(int debug, int dump) {
    _fm.debug_output = debug;
    _fm.dump_coevs = dump;
}

void 
coev_initialize(const coev_frameth_t *fm, coev_t *root) {
    /* multiple calls will result in havoc */
    if (ts_count != 0)
        _fm.abort("coev_initialize(): second initialization refused.");
    
    ts_count = 1;
    
    memcpy(&_fm, (void *)fm, sizeof(coev_frameth_t));
    
    _fm.c_switches = 0;
    _fm.c_bytes_copied = 0;
    _fm.c_waits = 0;
    _fm.c_sleeps = 0;
    
    _coev_switchstack	= coev_switchstack;
    _coev_slp_switch	= slp_switch;
    _coev_initialstub	= coev_initialstub;
    
    ts_scheduler.loop = ev_default_loop(0);
    ts_scheduler.scheduler = NULL;
    
    if (_fm.inthdlr) {
        ev_signal_init(&ts_scheduler.intsig, intsig_cb, SIGINT);
        ev_signal_start(ts_scheduler.loop, &ts_scheduler.intsig);
        ev_unref(ts_scheduler.loop);
    }
    
    coev_init_root(root);
}
