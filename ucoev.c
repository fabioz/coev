/* 
 * Bare-C io-scheduled coroutines: based on ucontext libc support.
 *
 * Authors: 
 *      Alexander Sabourenkov
 *
 * License: MIT License
 *
 */

#define CUSTOM_STACK_ALLOCATOR

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>

#ifdef MMAP_STACK
#include <sys/mman.h> /* mmap/munmap */
#else
#include <stdlib.h> /* malloc/free */
#endif
#include <sys/socket.h>
#include <ucontext.h>
#include <errno.h>

#include "ucoev.h"

#ifdef THREADING_MADNESS
#define TLS_ATTR __thread
#define CROSSTHREAD_CHECK(target, rv) \
    if (pthread_equal(ts_current->thread, (target)->thread)) \
        _fm.abort("crossthread switch prohibited");
#else
#define TLS_ATTR
#define CROSSTHREAD_CHECK(target, rv)
#endif

#define cgen_dprintf(t, fmt, args...) do { if (_fm.debug & t) \
    _fm.dprintf(fmt, ## args); } while(0)

#define coev_dprintf(fmt, args...) do { if (_fm.debug & CDF_COEV) \
    _fm.dprintf(fmt, ## args); } while(0)

#define coev_dump(msg, coev) do { if (_fm.debug & CDF_COEV_DUMP) \
    _coev_dump(msg, coev); } while(0)

#define cnrb_dprintf(fmt, args...) do { if (_fm.debug & CDF_NBUF) \
    _fm.dprintf(fmt, ## args); } while(0)

#define cnrb_dump(nbuf) do { if (_fm.debug & CDF_NBUF_DUMP) \
    _cnrb_dump(nbuf); } while(0)

#define colo_dprintf(fmt, args...) do { if (_fm.debug & CDF_COLOCK) \
    _fm.dprintf(fmt, ## args); } while(0)

#define colo_dump(lb) do { if (_fm.debug & CDF_COLOCK_DUMP) \
    _colock_dump(lb); } while(0)

#define cstk_dprintf(fmt, args...) do { if (_fm.debug & CDF_STACK) \
    _fm.dprintf(fmt, ## args); } while(0)

#define cstk_dump(msg) do { if (_fm.debug & CDF_STACK_DUMP) \
    _dump_stack_bunch(msg); } while(0)

typedef struct _coev_lock_bunch colbunch_t;
struct _coev_lock_bunch {
    colbunch_t *next;  /* in case we run out of space */
    colock_t *avail;   /* freed locks are stuffed here */
    colock_t *used;    /* allocated locks are stuffed here */
    colock_t *area;    /* what to free() */
    size_t allocated;  /* tracking how much was allocated (in colock count) */
};
/* colock_t declared in headed */
struct _coev_lock {
    colock_t *next;
    coev_t *owner;
    int count;
};

static TLS_ATTR volatile coev_t *ts_current;
static TLS_ATTR volatile int ts_count;
static TLS_ATTR coev_t *ts_root;
static TLS_ATTR colbunch_t *ts_rootlockbunch;
static TLS_ATTR long ts_cls_last_key;
static coev_frameth_t _fm;

static TLS_ATTR
struct _coev_scheduler_stuff {
    coev_t *scheduler;
    struct ev_loop *loop;
    struct ev_signal intsig;
    coev_t *runq_head;
    coev_t *runq_tail;
} ts_scheduler;

/* coevst_t declared in header */
struct _coev_stack {
    void *p;
    size_t size;
    coevst_t *next;
    coevst_t *prev; /* used only in busylist for fast removal */
};

static 
struct _coev_stack_bunch {
    coevst_t *avail;
    coevst_t *busy;
} ts_stack_bunch;

#ifdef CUSTOM_STACK_ALLOCATOR


static void
_dump_stack_bunch(const char *msg) {
    coevst_t *p;
    cstk_dprintf("%s, avail=%p, busy=%p\n\tAVAIL:\n", 
        msg, ts_stack_bunch.avail, ts_stack_bunch.busy);
    p = ts_stack_bunch.avail;
    while(p) {
        cstk_dprintf("\t<%p>: prev=%p next=%p size=%zd p=%p\n",
            p, p->prev, p->next, p->size, p->p);
        p = p->next;
    }
    cstk_dprintf("\n\tBUSY:\n");
    p = ts_stack_bunch.busy;
    while(p) {
        cstk_dprintf("\t<%p>: prev=%p next=%p size=%zd p=%p\n",
            p, p->prev, p->next, p->size, p->p);
        p = p->next;
    }
}

/* for best performance of stack allocation, don't increase stack
size after application startup. */
static coevst_t *
_get_a_stack(size_t size) {
    coevst_t *rv, *prev_avail;
    
    cstk_dump("_get_a_stack()");
    
    rv = ts_stack_bunch.avail;
    prev_avail = NULL;
    while ( rv && (rv->size < size) ) {
        prev_avail = rv;
        rv = rv->next;
    }

    if (!rv) {
        size_t to_allocate = size + sizeof(coevst_t);
        
#ifdef MMAP_STACK
        rv = mmap(NULL, to_allocate, PROT_EXEC|PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0); 
        if (fv == MAP_FAILED)
            _fm.eabort("coev_init(): mmap() stack allocation failed", errno);
#else
        rv = malloc(to_allocate);
        if (rv == NULL)
           _fm.abort("coev_init(): malloc() stack allocation failed");
#endif
        
        rv->size = size;
        rv->p = ((char *)rv ) + sizeof(coevst_t);
    } else {
        /* remove from the avail list if we took it from there */
        if (prev_avail)
            /* middle or end of the avail list */
            prev_avail->next = rv->next;
        else
            /* head of the avail list */
            ts_stack_bunch.avail = rv->next;
    }
    
    /* add to the head of the busy list */
    if (ts_stack_bunch.busy) {
        assert (ts_stack_bunch.busy->prev == NULL);
            
        ts_stack_bunch.busy->prev = rv;
    }
    
    rv->prev = NULL;
    rv->next = ts_stack_bunch.busy;
    ts_stack_bunch.busy = rv;
    cstk_dump("_get_a_stack: resulting");
    return rv;
}

static void
_return_a_stack(coevst_t *sp) {
    cstk_dprintf("_return_a_stack(%p)", sp);
    cstk_dump("");
    
    /* 1. remove sp from busy list */
    if (sp->prev)
        sp->prev->next = sp->next;
    if (sp->next)
        sp->next->prev = sp->prev;
    if (sp == ts_stack_bunch.busy) {
        ts_stack_bunch.busy = sp->next;
        ts_stack_bunch.busy->prev = NULL;
    }
    
    /* 2. add sp to avail list */
    sp->prev = NULL; /* not used in avail list */
    
    sp->next = ts_stack_bunch.avail;
    ts_stack_bunch.avail = sp;
    
    cstk_dump("_return_a_stack: resulting");
}

static void
_free_stacks(void) {
    coevst_t *spa, *spb, *span, *spbn;
    spa = ts_stack_bunch.avail;
    spb = ts_stack_bunch.busy;
    while (spa || spb) {
        if (spa)
            span = spa->next;
        if (spb)
            spbn = spb->next;
#ifdef MMAP_STACK
        
        if (spa && ( 0 != munmap(spa, spa->size + sizeof(coevst_t))))
            _fm.eabort("_free_stacks(): munmap failed.");
        if (spb && ( 0 != munmap(spb, spb->size + sizeof(coevst_t))))
                _fm.eabort("_free_stacks(): munmap failed.");
#else
        if (spa)
            free(spa);
        if (spb)
            free(spb);
#endif
        spa = span;
        spb = spbn;
        
    }
}
#else
static coevst_t *
_get_a_stack(size_t size) {
    coevst_t *rv = malloc(size+sizeof(coevst_t));
    rv->size = size;
    rv->next = NULL;
    rv->p = ((char *)rv) + sizeof(coevst_t);
    return rv;
}
static void
_return_a_stack(coevst_t *sp) {
    free(sp);
}
static void
_free_stacks(void) { }
#endif

static void update_treepos(coev_t *);
static void sleep_callback(struct ev_loop *, ev_timer *, int );
static void iotimeout_callback(struct ev_loop *, ev_timer *, int );

/** initialize the root coroutine */
static void
coev_init_root(coev_t *root) {
    if (ts_current != NULL) 
        _fm.abort("coev_init_root(): second initialization refused.");
    
    ts_current = root;
    ts_root = root;

    memset(root, 0, sizeof(coev_t));
    
    root->parent = NULL;
    root->run = NULL;
    root->id = 0;
    root->stack = NULL;
    root->state = CSTATE_CURRENT;
    root->status = CSW_NONE;
    root->next = NULL;
    root->ran_out_of_order = 0;
    
    ev_timer_init(&root->io_timer, iotimeout_callback, 23., 42.);
    ev_timer_init(&root->sleep_timer, sleep_callback, 23., 42.);
    
    update_treepos(root);
    
#ifdef THREADING_MADNESS
    root->thread = pthread_self();
#endif
}

/** universal runner */
static void coev_initialstub(void);

/** initialize coev_t structure.
Note: stack is allocated using anonymous mmap, so be generous, it won't
eat physical memory until needed */
void
coev_init(coev_t *child, coev_runner_t runner, size_t stacksize) {
    coevst_t *sp;

    if (ts_current == NULL)
        _fm.abort("coev_init(): library not initialized");
    
    if (stacksize < SIGSTKSZ)
        _fm.abort("coev_init(): stack size too small (less than SIGSTKSZ)");

    sp = _get_a_stack(stacksize);

    memset(child, 0, sizeof(coev_t));
    
    if (getcontext(&child->ctx))
	_fm.eabort("coev_init(): getcontext() failed", errno);
    
    child->ctx.uc_stack.ss_sp = sp->p;
    child->ctx.uc_stack.ss_flags = 0;
    child->ctx.uc_stack.ss_size = stacksize;
    child->ctx.uc_link = &(((coev_t*)ts_current)->ctx);
    child->stack = sp;
    
    makecontext(&child->ctx, coev_initialstub, 0);
    
    child->id = ts_count++;
    child->parent = (coev_t*)ts_current;
    update_treepos(child);
    child->run = runner;
    child->state = CSTATE_RUNNABLE;
    child->status = CSW_NONE;
    child->next = NULL;
    child->ran_out_of_order = 0;
    
    ev_timer_init(&child->io_timer, iotimeout_callback, 23., 42.);
    ev_timer_init(&child->sleep_timer, sleep_callback, 23., 42.);
}

static void cls_keychain_fini(cokeychain_t *);

void
coev_fini(coev_t *corpse) {
    if (corpse->stack)
        _return_a_stack(corpse->stack);
    if (corpse->treepos)
	_fm.free(corpse->treepos);
    if (corpse->kc.next)
        cls_keychain_fini(corpse->kc.next);
}

#define MAX_CHARS_PER_LEVEL 12
#define MAX_LEVELS_REPORTED 0x100
static TLS_ATTR char tp_onebuf[MAX_CHARS_PER_LEVEL + 4];
static TLS_ATTR char tp_scrpad[MAX_CHARS_PER_LEVEL*MAX_LEVELS_REPORTED + 4];


/* returns memory allocated with init-time supplied allocator */
static void
update_treepos(coev_t *coio) {
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
        memmove(curpos - written, tp_onebuf, written);
        curpos -= written;
        rvlen += written;
        c = c->parent;
    }
    rv = _fm.malloc(rvlen);
    if (!rv)
	_fm.abort("treepos(): memory allocation failed.");
    memmove(rv, curpos+1, rvlen-1); /* strip leading space */
    coio->treepos = rv;
}

const char *
coev_treepos(coev_t *coio) {
    return coio->treepos;
}

coev_t *
coev_current(void) {
    return (coev_t*)ts_current;
}

static const char* 
str_coev_state[] = {
    "ZERO     ",
    "CURRENT  ",
    "RUNNABLE ",
    "SCHEDULED",
    "IOWAIT   ",
    "SLEEP    ",
    "DEAD     "
};

static const char* 
str_coev_status[] = {
    "NONE     ",
    "VOLUNTARY",
    "EVENT    ",
    "WAKEUP   ",
    "TIMEOUT  ",
    "SIGCHLD  ",
    "YIELD    ",
    "(not defined)",
    "(not defined)",
    "LESS_THAN_AN_ERROR",
    "NOWHERE_TO_SWITCH",
    "SCHEDULER_NEEDED ",
    "WAIT_IN_SCHEDULER",
    "SWITCH_TO_SELF   ",
    "IOERROR"
};

const char* 
coev_state(coev_t *c) {
    return str_coev_state[c->state];
}

const char* 
coev_status(coev_t *c) {
    return str_coev_status[c->status];
}


static void
_coev_dump(char *m, coev_t *c) { 
    if (m) 
        _fm.dprintf("%s\n", m);
    _fm.dprintf( "coev_t<%p> [%s] %s, %s (current<%p> root<%p>):\n"
            "    is_current: %d\n"
            "    is_root:    %d\n"
            "    is_sched:   %d\n"
            "    parent:     %p\n"
            "    run:        %p\n"
	    "    A: %p X: %p Y: %p\n"
	    "    io watcher  active=%d pending=%d\n"
            "    io timeout  active=%d pending=%d\n"
            "    sleep timer active=%d pending=%d\n",
        c, c->treepos, str_coev_state[c->state], 
        str_coev_status[c->status],
        (coev_t*)ts_current, ts_root,
        c == ts_current,
        c == ts_root,
        c == ts_scheduler.scheduler,
        c->parent,
        c->run,
	c->A, c->X, c->Y,
        ev_is_active(&c->watcher), ev_is_pending(&c->watcher),
        ev_is_active(&c->io_timer), ev_is_pending(&c->io_timer),
        ev_is_active(&c->sleep_timer), ev_is_pending(&c->sleep_timer)
        );
}

/** entry point: function for voluntary switching between coroutines */
void
coev_switch(coev_t *target) {
    coev_t *origin = (coev_t*)ts_current;
    CROSSTHREAD_CHECK(target, p);
    
    if (target == origin) {
        target->status = CSW_SWITCH_TO_SELF;
        target->origin = origin;
        return;
    }
    

    coev_dprintf("coev_switch(): from [%s] to [%s]\n", 
	origin->treepos, target->treepos);
    coev_dump("switch, origin", origin);
    coev_dump("switch, target", target);        
    
    /* find the real target by ignoring dead coroutines */
    while ((target != NULL) && (target->state == CSTATE_DEAD))
        target = target->parent;
    
    if (!target)
        _fm.abort("coev_switch(): everyone's dead, how come?");
    
    target->origin = origin;
    origin->state = CSTATE_RUNNABLE;
    target->status = CSW_VOLUNTARY;
    target->state = CSTATE_CURRENT;
    target->status = CSW_VOLUNTARY;
    ts_current = target;
    _fm.c_switches++;
    
    if (swapcontext(&origin->ctx, &target->ctx) == -1)
        _fm.abort("coev_switch(): swapcontext() failed.");
    
    /*  we're here if swapcontext() returned w/o error:
        that means a switch back.
    
        It was originated either by switch() or by
        initialstub() exiting and calling setcontext().
    
        In the former case (some other switch), 
        ts_current is defined, and ts_current->switch_rv
        has been set up so we just return.
    
        In the latter case ts_current is defined also,
        ts_current->switch_rv.status = CSW_SIGCHLD
        ts_current->switch_rv.value = (void *)corpse;
        so we just return.
    
     */
}

static void
coev_scheduled_switch(coev_t *target) {
    coev_t *origin;
    
    origin = (coev_t *)ts_current;

    coev_dprintf("coev_scheduled_switch(): from [%s] %s %s to [%s] %s %s\n",
	    origin->treepos, str_coev_state[origin->state], str_coev_status[origin->status],
            target->treepos, str_coev_state[target->state], str_coev_status[target->status]);
    
    if (ts_scheduler.scheduler == NULL)
        _fm.abort("coev_scheduled_switch(): ts_scheduler == NULL");
    if ((ts_scheduler.scheduler != origin) && (ts_scheduler.scheduler != target))
        _fm.abort("coev_scheduled_switch(): ts_scheduler != origin nor target");
    if (target == origin)
        _fm.abort("coev_scheduled_switch(): target == origin");
    if (target->state == CSTATE_DEAD) {
        /* target's dead. do nothing. */
        coev_dprintf("coev_scheduled_switch(): target->state == CSTATE_DEAD");
        return;
    }
    target->origin = origin;
    ts_current = target;

    if (swapcontext(&origin->ctx, &target->ctx) == -1)
        _fm.abort("coev_scheduled_switch(): swapcontext() failed.");
}

static void coev_sched_cleanup(coev_t *);

/** the first and last function that runs in the coroutine */
static void 
coev_initialstub(void) {
    coev_t *self = (coev_t*)ts_current;
    coev_t *parent;
    
    self->run(self);
    
    /* clean up any scheduler stuff */
    coev_sched_cleanup(self);
    
    self->state = CSTATE_DEAD;
    
    /* perform explicit switch to parent */
    parent = self->parent;
    /* set up link to exit to */
    while ((parent != NULL) && (parent->state == CSTATE_DEAD))
        parent = parent->parent;
    
    if (!parent)
        _fm.abort("coev_initialstub(): everyone's dead, how come?");
    
    /* that's it. */
    /* IFF parent is not runnable and there's a scheduler running,
       switch to scheduler. */
    if (    (parent->state != CSTATE_RUNNABLE) 
        && (ts_scheduler.scheduler != NULL ) )
        parent = ts_scheduler.scheduler;

    parent->status = CSW_SIGCHLD;
    parent->origin = self;
    
    ts_current = parent;
    setcontext(&parent->ctx);
    
    _fm.abort("coev_initialstub(): setcontext() returned. This cannot be.");
}

/* ioscheduler functions */

/** for some reason there's a problem with signals.
    so by default we handle SIGINT by stopping the loop
        THIS IS A BIG FIXME */
static void 
intsig_cb(struct ev_loop *loop, ev_signal *w, int signum) {
    _fm.inthdlr();
}

static void
coev_sched_cleanup(coev_t *corpse) {
    
    
    coev_dprintf("coev_sched_cleanup() [%s]: watcher %d/%d iotimer %d/%d sleep_timer %d/%d\n",
        corpse->treepos, 
        ev_is_active(&corpse->watcher), ev_is_pending(&corpse->watcher),
        ev_is_active(&corpse->io_timer), ev_is_pending(&corpse->io_timer),
        ev_is_active(&corpse->sleep_timer), ev_is_pending(&corpse->sleep_timer));
    
    /* stop io watcher */
    ev_io_stop(ts_scheduler.loop, &corpse->watcher);
    
    /* stop timers */
    ev_timer_stop(ts_scheduler.loop, &corpse->io_timer);
    ev_timer_stop(ts_scheduler.loop, &corpse->sleep_timer);
    
    /* remove from the runq */
    if ( ts_scheduler.runq_head == corpse ) {
	ts_scheduler.runq_head = corpse->next;
	return;
    }
    {
	coev_t *t = ts_scheduler.runq_head;
	while (t) {
	    if (t->next == corpse) {
		t->next = corpse->next;
		return;
	    }
	    t = t->next;
	}
    }
}

static int coev_runq_append(coev_t *);

static void
dump_runqueue(const char *header) {
    coev_t *next = ts_scheduler.runq_head;
    coev_dprintf("%s\n", header);
    
    if (!next)
        coev_dprintf("    RUNQUEUE EMPTY\n");
    
    while (next) {
        coev_dprintf("    <%p> [%s] %s %s\n", next, next->treepos,
            str_coev_state[next->state], str_coev_status[next->status] );
        if (next == next->next)
            _fm.abort("dump_runqueue(): runqueue loop detected");
        next = next->next;
    }
}

int
coev_schedule(coev_t *waiter) {    
    switch(waiter->state) {
	case CSTATE_ZERO:
        case CSTATE_DEAD:
            return CSCHED_DEADMEAT;
        
        case CSTATE_IOWAIT:
        case CSTATE_SLEEP:
        case CSTATE_SCHEDULED:
            return CSCHED_ALREADY;
        
        case CSTATE_CURRENT:
            if (ts_scheduler.scheduler == NULL)
                return CSCHED_NOSCHEDULER;
            break;
            
        case CSTATE_RUNNABLE:
            break;
        default: 
            _fm.abort("coev_schedule(): invalid coev_t::state");
    }
    
    waiter->status = CSW_YIELD;
    coev_runq_append(waiter);
    if (waiter->state == CSTATE_CURRENT) {
        waiter->state = CSTATE_SCHEDULED;
        coev_switch(ts_scheduler.scheduler);
    } else {
        waiter->state = CSTATE_SCHEDULED;
    }
    return 0;
}

void
coev_stall(void) {
    coev_schedule((coev_t *)ts_current);
    if (CSW_ERROR(ts_current))
	_fm.abort("coev_stall(): coev_schedule() returned with error status");
}

static int
coev_runq_append(coev_t *waiter) {
    waiter->next = NULL;
    
    if (ts_scheduler.runq_tail != NULL)
	ts_scheduler.runq_tail->next = waiter;
    
    ts_scheduler.runq_tail = waiter;    
	
    if (ts_scheduler.runq_head == NULL)
	ts_scheduler.runq_head = waiter;
    
    return 0;
}

/* in your io_scheduler, stopping your watcher, switching to your waiter */
static void 
io_callback(struct ev_loop *loop, ev_io *w, int revents) {
    coev_t *waiter = (coev_t *) ( ((char *)w) - offsetof(coev_t, watcher) );
    ev_io_stop(loop, w);
    ev_timer_stop(ts_scheduler.loop, &waiter->io_timer);
    
    waiter->state = CSTATE_RUNNABLE;
    waiter->status = CSW_EVENT;
    
    coev_runq_append(waiter);
    coev_dprintf("io_callback(): [%s]. revents=%d\n", waiter->treepos, revents);
}

static void
iotimeout_callback(struct ev_loop *loop, ev_timer *w, int revents) {
    coev_t *waiter = (coev_t *) ( ((char *)w) - offsetof(coev_t, io_timer) );

    ev_io_stop(ts_scheduler.loop, &waiter->watcher);
    ev_timer_stop(ts_scheduler.loop, w);
    
    waiter->state = CSTATE_RUNNABLE;
    waiter->status = CSW_TIMEOUT; /* this is timeout */
    
    coev_runq_append(waiter);
    coev_dprintf("iotimeout_callback(): [%s]. revents=%d\n", waiter->treepos);
}

static void
sleep_callback(struct ev_loop *loop, ev_timer *w, int revents) {
    coev_t *waiter = (coev_t *) ( ((char *)w) - offsetof(coev_t, sleep_timer) );

    waiter->state = CSTATE_RUNNABLE;
    waiter->status = CSW_WAKEUP; /* this is scheduled */

    coev_runq_append(waiter);
    coev_dprintf("sleep_callback(): [%s]\n", waiter->treepos);
}

/* sets current coro to wait for revents on fd, switches to scheduler */
void 
coev_wait(int fd, int revents, ev_tstamp timeout) {
    coev_t *target, *self;
    
    self = (coev_t*)ts_current;
    
    if (self == ts_scheduler.scheduler) {
        self->origin = self;
        self->status = CSW_WAIT_IN_SCHEDULER;
        return;
    }
    
    coev_dprintf("coev_wait(): [%s] %s %s scheduler [%s], self->parent [%s]\n", 
        self->treepos,  str_coev_state[self->state], 
        str_coev_status[self->status],
        ts_scheduler.scheduler ? ts_scheduler.scheduler->treepos : "none", 
        self->parent ? self->parent->treepos : "none");
    
    if (ts_scheduler.scheduler) {
        target = ts_scheduler.scheduler;
        target->status = CSW_YIELD;
	target->origin = self;
    } else if (self->parent) {
        target = self->parent;
        target->status = CSW_SCHEDULER_NEEDED;
	target->origin = self;
    } else {
        /* epic fail: nowhere to switch. */
        coev_dprintf("coev_wait(): epic fail: nowhere to switch\n");
        self->origin = self;
        self->status = CSW_NOWHERE_TO_SWITCH;
        return;
    }
    
    if ((fd == -1) && (revents == 0)) {
        /* this is sleep */
        self->sleep_timer.repeat = timeout;
        ev_timer_again(ts_scheduler.loop, &self->sleep_timer);
        _fm.c_sleeps++;
        self->state = CSTATE_SLEEP;
    } else {
        /* this is iowait */
        self->io_timer.repeat = timeout;
        ev_timer_again(ts_scheduler.loop, &self->io_timer);
        
        if (ev_is_active(&self->watcher)) {
            /* oh shi.... */
            coev_dprintf("coev_wait(): io watcher already active for [%s]\n", self->treepos);
	    if (ev_is_pending(&self->watcher))
            /* OH SHI.... */
                coev_dprintf("coev_wait(): io watcher is pending for [%s]\n", self->treepos);
                
        } else {
            ev_io_init(&self->watcher, io_callback, fd, revents);
            ev_io_start(ts_scheduler.loop, &self->watcher);
        }
        _fm.c_waits++;
    
        self->state = CSTATE_IOWAIT;
    }
    self->ran_out_of_order = 0;
    coev_scheduled_switch(target);
    
    self = (coev_t*)ts_current;
    /* we're here either because scheduler switched back
       or someone is being rude. */
    
    if (   (self->status != CSW_EVENT)
	&& (self->status != CSW_WAKEUP)
        && (self->status != CSW_TIMEOUT)) {
	/* someone's being rude. */
        coev_dprintf("coev_wait(): [%s]/%s is being rude to [%s] %s %s\n",
            self->origin->treepos, str_coev_state[self->origin->state],
            self->treepos, str_coev_state[self->state], 
            str_coev_status[self->status]);
        self->ran_out_of_order = 1;
    }
}

void
coev_sleep(ev_tstamp amount) {
    coev_wait(-1, 0, amount);
}

/*  the scheduler
    
    this should switch to coroutines in order they received IO events 

    ts_scheduler.runq_head:
	NULL if no events, first to handle 
	if at least one event was received.

    ts_scheduler.runq_tail:
	undefined if runq_head == NULL.
	most recent coroutie to receieve an event otherwise.
	
    coev_t::next: iff this coroutine was not last to receive an
	event, points to next one.

    runqueue is managed thus:
    -- append always at tail
    -- scheduler walks always from head
    -- if tail != NULL, tail->next == NULL, head != NULL
    -- if head == NULL, tail == NULL, queue is empty.

*/

void 
coev_loop(void) {
    coev_dprintf("coev_loop(): scheduler entered.\n");
    
    if (ts_scheduler.scheduler == ts_current)
        _fm.abort("recursive call of coev_loop()");
    
    ts_scheduler.scheduler = (coev_t*)ts_current;
    
    do {
	coev_t *target, *self, *runq_head;

	dump_runqueue("coev_loop(): runqueue before ev_loop");
	
	if (ts_scheduler.runq_head == NULL)
	    ev_loop(ts_scheduler.loop, EVLOOP_ONESHOT);
	else
	    ev_loop(ts_scheduler.loop, EVLOOP_NONBLOCK);
        dump_runqueue("coev_loop(): runqueue after ev_loop");
        
	if (ts_scheduler.runq_head == NULL) {
	    coev_dprintf("coev_loop(): no events were scheduled.\n");
	    break;
	}
        /* guard against infinite loop in scheduler in case something 
           schedules itself over and over */
	runq_head = ts_scheduler.runq_head;
        ts_scheduler.runq_head = ts_scheduler.runq_tail = NULL;
        
        coev_dprintf("coev_loop(): dispatching pending events.\n");
        
	while ((target = runq_head)) {
            coev_dprintf("coev_loop(): runqueue run: target %p head %p next %p\n", target, runq_head, target->next);
	    runq_head = target->next;
            if (runq_head == target)
                _fm.abort("coev_loop(): runqueue loop detected");
	    target->next = NULL;
            if (target->ran_out_of_order) {
                target->ran_out_of_order = 0;
                coev_dprintf("coev_loop(): target [%s] ran out of order: skipping.\n", target->treepos);
                continue;
            }
	    coev_scheduled_switch(target);
            self = (coev_t *)ts_current;
            switch (self->status) {
                case CSW_YIELD:
                    coev_dprintf("coev_loop(): yield from %p [%s]\n", self->origin, self->origin->treepos);
                    continue;
                case CSW_SIGCHLD:
                    coev_dprintf("coev_loop(): sigchld from %p [%s]\n", self->origin, self->origin->treepos);
                    
                    continue;
                case CSW_SWITCH_TO_SELF:
                    _fm.abort("scheduler wound up in the runq");
                default:
                    coev_dprintf("Unexpected switch to scheduler\n");
                    coev_dump("origin", self->origin); 
                    coev_dump("self", self);
                    _fm.abort("unexpected switch to scheduler");
            }
	}
	coev_dprintf("coev_loop(): event dispatch finished.\n");
    } while(1);
    
    ts_scheduler.scheduler = NULL;
    coev_dprintf("coev_loop(): scheduler exited.\n");
}

void
coev_unloop(void) {
    ev_unloop(ts_scheduler.loop, EVUNLOOP_ALL);
}

static void 
_colock_dump(colbunch_t *subject) {
    colbunch_t *c = subject, *p;
    colock_t *lc;
    int i;
    while (c) {
	p = c;
	c = c->next;
        colo_dprintf("bunch at <%p>, %zd locks, next is <%p>\n", p, p->allocated, c);
        colo_dprintf("        avail  <%p>, used <%p>\n", p->avail, p->used);
        colo_dprintf("        USED DUMP:\n");
        lc = p->used;
        i = 0;
        while (lc != NULL) {
            colo_dprintf("            <%p>: owner %p count %d\n", lc, lc->owner, lc->count);
            lc = lc->next;
            i++;
        }
        colo_dprintf("            TOTAL %d\n", i);
        colo_dprintf("        AVAIL DUMP:\n");
        lc = p->avail;
        i = 0;
        while (lc != NULL) {
            colo_dprintf("            <%p>: owner %p count %d\n", lc, lc->owner, lc->count);
            lc = lc->next;
            i++;
        }
        colo_dprintf("            TOTAL %d\n", i);
    }    
}

/* iff *bunch is NULL, we allocate the struct itself */
static void 
colock_bunch_init(colbunch_t **bunch_p) {
    colbunch_t *bunch = *bunch_p;
    
    if (bunch == NULL) {
	bunch = _fm.malloc(sizeof(colbunch_t));
	if (bunch == NULL)
	    _fm.abort("ENOMEM allocating lockbunch");
    }
	
    bunch->area = _fm.malloc(sizeof(colock_t) * COLOCK_PREALLOCATE);
    
    if (bunch->area == NULL)
	_fm.abort("ENOMEM allocating lock area");	
    
    memset(bunch->area, 0, sizeof(colock_t) * COLOCK_PREALLOCATE);
    bunch->allocated =  COLOCK_PREALLOCATE;
    bunch->avail = bunch->area;
    {
        int i;
        
        for(i=1; i < COLOCK_PREALLOCATE; i++)
            bunch->area[i-1].next = &(bunch->area[i]);
    }
    
    *bunch_p = bunch;
    colo_dprintf("colock_bunch_init(%p): allocated at %p.\n", bunch_p, bunch);
    colo_dump(ts_rootlockbunch);

}

static void 
colock_bunch_fini(colbunch_t *b) {
    colbunch_t *c = b, *p;
    
    colo_dprintf("colock_bunch_fini(%p): deallocating.\n", b);
    while (c) {
	p = c;
	c = c->next;
	_fm.free(p);
    }
}

colock_t *
colock_allocate(void) {
    colock_t *lock;
    colbunch_t *bunch = ts_rootlockbunch;
    
    while (!bunch->avail) {
	/* woo, we're out of locks in this bunch, get another */
	colo_dprintf("colock_allocate(): bunch %p full\n", bunch);
	if (!bunch->next) {
	    /* WOO, that was last one. allocate another */
	    colo_dprintf("colock_allocate(): all bunches full, allocating another\n", bunch);
	    colock_bunch_init(&bunch->next);
	    bunch = bunch->next;
	    break;
	}
	bunch = bunch->next;
    }
    
    lock = bunch->avail;
    
    bunch->avail = lock->next;
    lock->next = bunch->used;
    bunch->used = lock;
    lock->owner = coev_current();

    colo_dprintf("colock_allocate() -> %p\n", lock);
    colo_dump(ts_rootlockbunch);
    return (void *) lock;
}

void 
colock_free(colock_t *lock) {
    colock_t *prev;
    colbunch_t *bunch = ts_rootlockbunch;
    
    lock->owner = NULL; /* pity the fools supplying null pointers */
    
    if (bunch->next) { /* if we have >1 bunch out there .. */
	while (bunch) { 
	    /* pointer magic is so pointer */
	    if (   (lock > bunch->area) 
		&& ( (lock - bunch->area) < bunch->allocated) )
		    /* gotcha */
		    break;
	    bunch = bunch->next;
	}
    }
    colo_dprintf("colock_free(): deallocating %p\n", lock);
    
    if (!bunch)
	_fm.abort("Attempt to free non-existent lock");
    
    prev = bunch->used;
    if ( lock != prev ) {
	/* find previous lock in the used list */
	while (prev->next != lock) {
	    if (prev->next == NULL) {
                colo_dump(ts_rootlockbunch);
		_fm.abort("Whoa, colbunch_t at %p is corrupted!");
            }
	    prev = prev->next;
	}
        prev->next = lock->next;
    } else
        bunch->used = lock->next;
    
    /* put it at the top of free list */
    lock->next = bunch->avail;
    bunch->avail = lock;
    colo_dump(ts_rootlockbunch);
}

int
colock_acquire(colock_t *p, int wf) {
    if (wf == 0) {
        if (p->owner)
            return 0;
        p->owner = (coev_t *)ts_current;
        p->count ++;
        return 1;
    }
    while (p->owner) {
        
        if (p->owner == ts_current) {
            p->count ++;
            colo_dprintf("%p acquires lock %p for %dth time\n", ts_current, p, p->count);
            
            return 1;
        }
	/* the promised dire insults */
	colo_dprintf("%p attemtps to acquire lock %p that was not released by %p\n",
	    ts_current, p, p->owner);
        if (wf == 0)
            return 0;
	_fm.abort("haha busy-wait");
	coev_stall();
    }
    p->owner = (coev_t *)ts_current;
    p->count = 1;
    colo_dprintf("colock_acquire(%p, %d): acquired ok.\n", p, wf);
    
    return 1;
}

void 
colock_release(colock_t *p) {
    if (p->count == 0)
        colo_dprintf("%p releases lock %p that has no owner\n", ts_current, p);
    if (p->count >0)
        p->count--;
    if (p->owner != ts_current)
        colo_dprintf("%p releases lock %p that was acquired by %p\n", 
            ts_current, p, p->owner);
    else
        colo_dprintf("%p releases lock %p, new count=%d.\n", 
            ts_current, p, p->count);
    if (p->count == 0)
        p->owner = NULL;
}

/*  Coroutine-local storage is designed to satisfy perverse semantics 
    that Python/thread.c expects. Go figure.
    
    key value of 0 means this slot is not used. 0 is never returned 
    by cls_allocate().
*/
#define CLS_FREE_SLOT 0L

long 
cls_new(void) {
    ts_cls_last_key++;
    return ts_cls_last_key;
}

static void
cls_keychain_init(cokeychain_t **kc) {
    if (*kc == NULL) {
	*kc = _fm.malloc(sizeof(cokeychain_t));
	if (*kc == NULL) 
	    _fm.abort("ENOMEM allocating new keychain");
    }
    memset(*kc, 0, sizeof(cokeychain_t));
}

static void 
cls_keychain_fini(cokeychain_t *kc) {
    cokeychain_t *c = kc, *p;
    
    while (c) {
	p = c;
	c = c->next;
	_fm.free(p);
    }
}

static cokey_t *
cls_find(long k) {
    cokeychain_t *kc = &( ((coev_t *)ts_current)->kc);
    int i;
    
    while (kc) {
	for (i = 0; i<CLS_KEYCHAIN_SIZE; i++)
	    if (kc->keys[i].key == k)
		return &(kc->keys[i]);
	kc = kc->next;
    }

    if (k == 0) {
	/* this was an attempt to find a free slot */
	kc = NULL;
	
	cls_keychain_init(&kc);
	if (ts_current->kc_tail)
	    ts_current->kc_tail->next = kc;
	else
	    ts_current->kc_tail = kc;
	return &(kc->keys[0]);
    }
    
    return NULL;
}

void *
cls_get(long k) {
    cokey_t *t;
    t = cls_find(k);
    if (t)
	return t->value;
    return NULL;
}

int
cls_set(long k, void *v) {
    cokeychain_t *kc = &( ((coev_t *)ts_current)->kc);
    int i;

    while (kc) {
	for (i = 0; i<CLS_KEYCHAIN_SIZE; i++)
	    if (kc->keys[i].key == 0) {
		kc->keys[i].key = k;
		kc->keys[i].value = v;
		return 0;
	    }
	kc = kc->next;
    }
    return -1;
}
    
void
cls_del(long k) {
    cokey_t *t;
    
    t = cls_find(k);
    if (t)
	t->key = CLS_FREE_SLOT;
}

void
cls_drop_across(long key) {
    coev_dprintf("cls_drop_across(%ld): NOT IMPLEMENTED.\n", key);
}

void
cls_drop_others(void) {
    coev_dprintf("cls_drop_others(): NOT IMPLEMENTED.\n");
}

/* used in buf growth calculations */
static const ssize_t CNRBUF_MAGIC = 1<<12;


void 
cnrbuf_init(cnrbuf_t *self, int fd, double timeout, size_t prealloc, size_t rlim) {
    self->in_allocated = prealloc;
    self->in_limit = 0;
    self->iop_timeout = timeout;
    self->in_buffer = _fm.malloc(self->in_allocated);
    self->fd = fd;
    
    if (!self->in_buffer)
	_fm.abort("cnrbuf_init(): No memory for me!");
    self->in_position = self->in_buffer;
}

void 
cnrbuf_fini(cnrbuf_t *buf) {
    _fm.free(buf->in_buffer);
}

static void
_cnrb_dump(cnrbuf_t *self) {
    ssize_t top_free, total_free, bottom_free;

    top_free = self->in_position - self->in_buffer;
    total_free = self->in_allocated - self->in_used;
    bottom_free = total_free - top_free;

    _fm.dprintf("buffer metadata:\n"
    "\tbuf=%p pos=%p pos offset %zd\n"
    "\tallocated=%zd used=%zd limit=%zd\n"
    "\ttop_free=%zd bottom_free=%zd\ttotal_free=%zd\n",
	self->in_buffer, self->in_position, self->in_position - self->in_buffer, 
	self->in_allocated, self->in_used, self->in_limit,
	top_free, bottom_free, total_free);
}

/** makes some space at the end of the read buffer 
by either moving occupied space or growing it by reallocating 
*/
static int
sf_reshuffle_buffer(cnrbuf_t *self, ssize_t needed) {
    ssize_t top_free, total_free;
    
    top_free = self->in_position - self->in_buffer;
    total_free = self->in_allocated - self->in_used;

    cnrb_dprintf("sf_reshuffle_buffer(*,%zd):\n", needed);
    cnrb_dump(self);
    
    if (total_free - top_free >= needed)
    /* required space is available at the bottom */
	return 0;

    cnrb_dprintf("sf_reshuffle_buffer(*,%zd): %zd > %zd ?\n", 
        needed, needed + 2 * CNRBUF_MAGIC, total_free);
    if (needed + 2 * CNRBUF_MAGIC > total_free ) {
	/* reallocation imminent - grow by at most 2*CNRBUF_MAGIC more than needed */
	ssize_t newsize = (self->in_used + needed + 2*CNRBUF_MAGIC) & (~(CNRBUF_MAGIC-1));
        if (newsize > self->in_limit)
            self->in_limit = newsize;
	char *newbuf = _fm.realloc(self->in_buffer, newsize);
	if (!newbuf) {
	/* memmove imminent */
	    newbuf = _fm.malloc(newsize);
	    if (!newbuf) {
                errno = ENOMEM;
		return -1; /* no memory */
            }
	    memmove(newbuf, self->in_position, self->in_used);
	    _fm.free(self->in_buffer);
	    self->in_position = self->in_buffer = newbuf;
	    self->in_allocated = newsize;
            cnrb_dprintf("sf_reshuffle_buffer(*,%zd): realloc failed, newsize %zd\n:", 
                needed, self->in_allocated);
            cnrb_dump(self);
	    return 0;
	}
        self->in_allocated = newsize;
        cnrb_dprintf("sf_reshuffle_buffer(*,%zd): realloc successful: newsize=%zd\n", 
            needed, self->in_allocated);
    }
    /* we're still have 2*CNRBUF_MAGIC bytes more than needed */
    
    memmove(self->in_buffer, self->in_position, self->in_used);
    self->in_position = self->in_buffer;

    cnrb_dprintf("sf_reshuffle_buffer(*,%zd): after realloc and/or move\n", needed);
    cnrb_dump(self);
    
    return 0;
}

ssize_t 
cnrbuf_read(cnrbuf_t *self,void **p, ssize_t sizehint) {
    ssize_t rv;
    
    if (self->busy)
        return -2;
    
    cnrb_dprintf("cnrbuf_read(): fd=%d sizehint %zd bytes buflimit %zd bytes\n", 
        self->fd, sizehint, self->in_limit);
    
    if (sizehint > self->in_limit)
        self->in_limit = sizehint;

    do {
        ssize_t readen, to_read;    

	if (( sizehint > 0) && (self->in_used >= sizehint )) {
	    *p = self->in_position;
	    rv = sizehint;
	    self->in_used -= sizehint;
	    self->in_position += sizehint;
	    if (self->in_used == 0)
		self->in_position = self->in_buffer;
	    return rv;
	}    
        
        if ( sizehint > 0 )
            to_read = sizehint - self->in_used;
        else
            if ( self->in_used + 2 * CNRBUF_MAGIC < self->in_limit )
                to_read = 2 * CNRBUF_MAGIC;
            else 
                to_read = self->in_limit - self->in_used;            
    
        if ( sf_reshuffle_buffer(self, to_read) )
            return -1;
        	
	readen = recv(self->fd, self->in_position + self->in_used, to_read, 0);
        cnrb_dprintf("cnrbuf_read(): %zd bytes read into %p, reqd len %zd\n", 
            readen, self->in_position + self->in_used, to_read);
        
	if (readen == -1) {
	    if (errno == EAGAIN) {
		coev_wait(self->fd, COEV_READ, self->iop_timeout);
		if (ts_current->status != CSW_EVENT)
		    return -1;
		continue;
	    }
            ts_current->status = CSW_IOERROR;
	    return -1;
	}
	if (readen == 0)
	    sizehint = self->in_used; /* return whatever we managed to read. */
	else
	    self->in_used += readen;
        if ((readen == 0) && (self->in_used == 0)) { /* read nothing. */
            return 0;
        }
        cnrb_dump(self);
    } while (1);
}

/* returns:
    >0 - len of line extracted if all is ok.
     0 - need more data, and buffer limit/size hint allow.
*/
ssize_t
sf_extract_line(cnrbuf_t *self, const char *startfrom, void **p, ssize_t sizehint) {
    char *culprit;
    char *data_end;
    ssize_t len;
    
    data_end = self->in_position + self->in_used;
    len = data_end - startfrom;
    
    cnrb_dprintf("sf_extract_line(): fd=%d len=%zd, sizehint=%zd in_limit=%zd\n", 
        self->fd, len, sizehint, self->in_limit);
    
    if ( len > 0 ) {
        /* have some unscanned data, try it */
        culprit = memchr(startfrom, '\n', len);
        
        if (culprit) {
            len = culprit - self->in_position + 1;
            cnrb_dprintf("sf_extract_line(): fd=%d found."
            " culprit=%p len=%d\n", self->fd, culprit, len);
            
	    *p = self->in_position;
            self->in_used -= len;
            if (self->in_used == 0)
                self->in_position = self->in_buffer;
            else    
                self->in_position += len;
            cnrb_dprintf("buffer meta after extraction:\n");
            cnrb_dump(self);
            return len;
        }
    }
    /* at this point:
       len = 0, or len > 0, but no luck with LF -> len is effectively 0 */
    
    /* now decide if we're allowed to read more data from the fd*/
    if ( sizehint ) {
        /* bound by explicit sizehint */
        if (self->in_used < sizehint) 
            return 0;
    } else {
        /* bound by buffer size limit */
        if (self->in_allocated < self->in_limit)
            return 0;
    }
    
    /* we're over the line length limit - return what we've got so far */
    len = self->in_used;
    *p = self->in_position;
    
    self->in_used = 0;
    self->in_position = self->in_buffer;
    
    return len;
}

ssize_t 
cnrbuf_readline(cnrbuf_t *self, void **p, ssize_t sizehint) {
    ssize_t rv;
    
    if (self->busy)
        return -2;
    
    cnrb_dprintf("cnrbuf_readline(): fd=%d sizehint %zd bytes buflimit %zd bytes\n", 
        self->fd, sizehint, self->in_limit);
    
    if (sizehint > self->in_limit)
        self->in_limit = sizehint;

    /* look if we can return w/o syscalls */
    if ( self->in_used > 0 ) {
        rv = sf_extract_line(self, self->in_position, p, sizehint);
        if (rv > 0)
            return rv;
    }
    
    do {
        ssize_t to_read, readen;
        
        if ( sizehint )
            to_read = sizehint - self->in_used;
        else
            if ( self->in_used + 2 * CNRBUF_MAGIC < self->in_limit )
                to_read = 2 * CNRBUF_MAGIC;
            else 
                to_read = self->in_limit - self->in_used;
            
        if ( sf_reshuffle_buffer(self, to_read) )
            return -1; /* ENOMEM */
        
	readen = recv(self->fd, self->in_position + self->in_used, 
		      to_read, 0);
        cnrb_dprintf("cnrbuf_readline: %zd bytes read into %p, reqd len %zd\n", 
                readen, self->in_position + self->in_used, to_read );
	if (readen == 0) { 
	    /* no more data : return whatever there is */
	    *p = self->in_position;
	    rv = self->in_used;
	    self->in_used = 0;
	    self->in_position = self->in_buffer;
	    return rv;
	}
	if (readen == -1) {
	    if (errno == EAGAIN) {
		coev_wait(self->fd, COEV_READ, self->iop_timeout);
		if (ts_current->status != CSW_EVENT)
		    return -1;
		continue;
	    }
            ts_current->status = CSW_IOERROR;
	    return -1;
	}
        /* woo we read something */
        {
            char *old_position = self->in_position + self->in_used;
            self->in_used += readen;
            cnrb_dump(self);
	    rv = sf_extract_line(self, old_position, p, sizehint);
            if ( rv > 0 )
                return rv;
        }

    } while(1);
}

ssize_t
cnrbuf_write(cnrbuf_t *self, const void *data, ssize_t len) {
    ssize_t written, to_write, wrote;

    written = 0;
    to_write = len;
    
    cnrb_dprintf("cnrbuf_write(): fd=%d len=%zd bytes\n", self->fd, to_write);
    while (to_write){ 
	wrote = send(self->fd, (char *)data + written, to_write, MSG_NOSIGNAL);
	cnrb_dprintf("cnrbuf_write(): fd=%d wrote=%zd bytes\n", self->fd, wrote);
	if (wrote == -1) {
	    if (errno == EAGAIN) {
		coev_wait(self->fd, COEV_WRITE, self->iop_timeout);
		if (ts_current->status == CSW_EVENT)
		    continue;
	    }
	    break;
	}
	written += wrote;
	to_write -= wrote;
    }
    
    return written; /* should be len, but may be less. */
}

void
coev_getstats(uint64_t *sw, uint64_t *wa, uint64_t *sl, uint64_t *bc) {
    *sw = _fm.c_switches;
    *wa = _fm.c_waits;
    *sl = _fm.c_sleeps;
    *bc = _fm.c_bytes_copied;
}

void
coev_setdebug(int debug) {
    _fm.debug = debug;
}

int 
coev_setparent(coev_t *target, coev_t *newparent) {
    coev_t *p;
    
    for (p = newparent; p; p = p->next)
	if ( p == target )
	    return -1;

    target->parent = newparent;
    update_treepos(target);
    return 0;
}

void 
coev_libinit(const coev_frameth_t *fm, coev_t *root) {
    /* multiple calls will result in havoc */
    if (ts_count != 0)
        _fm.abort("coev_libinit(): second initialization refused.");
    
    ts_count = 1;
    
    memcpy(&_fm, (void *)fm, sizeof(coev_frameth_t));
    
    _fm.c_switches = 0;
    _fm.c_bytes_copied = 0;
    _fm.c_waits = 0;
    _fm.c_sleeps = 0;
    
    ts_scheduler.loop = ev_default_loop(0);
    ts_scheduler.scheduler = NULL;
    ts_scheduler.runq_head = NULL;
    ts_scheduler.runq_tail = NULL;
    
    ts_cls_last_key = 1L;
    
    if (_fm.inthdlr) {
        ev_signal_init(&ts_scheduler.intsig, intsig_cb, SIGINT);
        ev_signal_start(ts_scheduler.loop, &ts_scheduler.intsig);
        ev_unref(ts_scheduler.loop);
    }
    
    ts_rootlockbunch = NULL;
    colock_bunch_init(&ts_rootlockbunch);
    
    memset(&ts_stack_bunch, 0, sizeof(struct _coev_stack_bunch));
    
    coev_init_root(root);
}

void
coev_libfini(void) {
    /* should do something good here. */
    if (ts_current != ts_root)
	_fm.abort("coev_libfini() must be called only in root coro.");
	
    colock_bunch_fini(ts_rootlockbunch);
    cls_keychain_fini(ts_current->kc.next);
    _free_stacks(); /* this effectively kills all coroutines, unbeknowst to them. */
}

