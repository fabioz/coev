/* 
 * Bare-C io-scheduled coroutines: based on ucontext libc support.
 *
 * Authors: 
 *      Alexander Sabourenkov
 *
 * License: MIT License
 *
 */


#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>

#include <sys/mman.h> /* mmap/munmap */
#include <stdlib.h> /* malloc/free */
#include <sys/socket.h>
#include <ucontext.h>
#include <errno.h>

#include "ucoev.h"

#ifdef HAVE_VALGRIND
#include "valgrind.h"
#endif

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
    colbunch_t *bunch;
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
    int waiters;
    int stop_flag;
} ts_scheduler;

/* coevst_t declared in header */
struct _coev_stack {
    void *base;     /* what to give to munmap */
    void *sp;       /* what to put into stack_t */
    size_t size;    /* what to put into stack_t */
    coevst_t *next;
    coevst_t *prev; /* used only in busylist for fast removal */
#ifdef HAVE_VALGRIND
    int vg_id;
#endif
};

static 
struct _coev_stack_bunch {
    coevst_t *avail;
    coevst_t *busy;
} ts_stack_bunch;

static void
_dump_stack_bunch(const char *msg) {
    coevst_t *p;
    cstk_dprintf("%s, avail=%p, busy=%p\n\tAVAIL:\n", 
        msg, ts_stack_bunch.avail, ts_stack_bunch.busy);
    p = ts_stack_bunch.avail;
    while(p) {
        cstk_dprintf("\t<%p>: prev=%p next=%p size=%zd base=%p\n",
            p, p->prev, p->next, p->size, p->base);
        p = p->next;
    }
    cstk_dprintf("\n\tBUSY:\n");
    p = ts_stack_bunch.busy;
    while(p) {
        cstk_dprintf("\t<%p>: prev=%p next=%p size=%zd base=%p\n",
            p, p->prev, p->next, p->size, p->base);
        p = p->next;
    }
}

/* for best performance of stack allocation, don't increase stack
size after application startup. */
/* TODO: 
    - pay attention to stack growth direction.
    - set up guard page. 
    - align stack start on smallpage boundary (hugepages?)
    - double check minimal size. 
*/
static coevst_t *
_get_a_stack(size_t size) {
    coevst_t *rv, *prev_avail;
    void *base;
    
    cstk_dump("_get_a_stack()");
    
    rv = ts_stack_bunch.avail;
    prev_avail = NULL;
    while ( rv && (rv->size < size) ) {
        prev_avail = rv;
        rv = rv->next;
    }

    if (!rv) {
        size_t to_allocate = size + sizeof(coevst_t);
    
        base = mmap(NULL, to_allocate, PROT_EXEC|PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_GROWSDOWN, -1, 0); 
        if (base == MAP_FAILED)
            _fm.eabort("_get_a_stack(): mmap() stack allocation failed", errno);
        
        rv = (coevst_t *) ( base + size );
        rv->base = base;
        rv->size = size;
        rv->sp = base;
#ifdef HAVE_VALGRIND
        rv->vg_id = VALGRIND_STACK_REGISTER( rv->sp, rv->sp + size);
#endif
        cstk_dprintf("_get_a_stack(): requested %zd allocated %zd base %p sp %p rv %p\n",
            rv->size, to_allocate, rv->base, rv->sp, rv);
    } else {
        /* remove from the avail list if we took it from there */
        if (prev_avail)
            /* middle or end of the avail list */
            prev_avail->next = rv->next;
        else
            /* head of the avail list */
            ts_stack_bunch.avail = rv->next;
        _fm.stacks_free --;        
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
    
    _fm.stacks_used ++;
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
        if (ts_stack_bunch.busy)
            ts_stack_bunch.busy->prev = NULL;
    }
    
    /* 2. add sp to avail list */
    sp->prev = NULL; /* not used in avail list */
    
    sp->next = ts_stack_bunch.avail;
    ts_stack_bunch.avail = sp;
    
    cstk_dump("_return_a_stack: resulting");
    
    _fm.stacks_free ++;
    _fm.stacks_used --;
}

static void
_free_stacks(void) {
    coevst_t *spa, *spb, *span, *spbn;
    spa = ts_stack_bunch.avail;
    spb = ts_stack_bunch.busy;
    cstk_dprintf("%s\n", "_free_stacks()");

    while (spa || spb) {
        if (spa)
            span = spa->next;
        if (spb)
            spbn = spb->next;

        if (spa && ( 0 != munmap(spa, spa->size + sizeof(coevst_t))))
            _fm.eabort("_free_stacks(): munmap failed.", errno);
        if (spb && ( 0 != munmap(spb, spb->size + sizeof(coevst_t))))
                _fm.eabort("_free_stacks(): munmap failed.", errno);
        
#ifdef HAVE_VALGRIND
        VALGRIND_STACK_DEREGISTER(spa->vg_id);
        VALGRIND_STACK_DEREGISTER(spb->vg_id);
#endif

        spa = span;
        spb = spbn;
        
    }
    _fm.stacks_free = 0;
    _fm.stacks_used = 0;
}

/* the last, I hope, custom allocator, for the coev_t-s themselves. */

static 
struct _coev_t_bunch {
    coev_t *avail;
    coev_t *busy;
} ts_coev_bunch;

static coev_t *
_get_a_coev(void) {
    coev_t *rv, *prev_avail;
    
    rv = ts_coev_bunch.avail;
    prev_avail = NULL;

    if (!rv) {
        rv = malloc(sizeof(coev_t));
        if (rv == NULL)
           _fm.abort("_get_a_coev(): malloc() failed");
        
    } else {
        /* remove from the avail list if we took it from there */
        if (prev_avail)
            /* middle or end of the avail list */
            prev_avail->cb_next = rv->cb_next;
        else
            /* head of the avail list */        
            ts_coev_bunch.avail = rv->cb_next;
        _fm.coevs_free --;        
    }
    
    /* add to the head of the busy list */
    if (ts_coev_bunch.busy) {
        assert (ts_coev_bunch.busy->cb_prev == NULL);
        ts_coev_bunch.busy->cb_prev = rv;
    }
    
    rv->cb_prev = NULL;
    rv->cb_next = ts_coev_bunch.busy;
    ts_coev_bunch.busy = rv;
    
    _fm.coevs_used ++;

    return rv;
}

static void
_return_a_coev(coev_t *sp) {
    
    /* 1. remove from busy list */
    if (sp->cb_prev)
        sp->cb_prev->cb_next = sp->cb_next;
    if (sp->cb_next)
        sp->cb_next->cb_prev = sp->cb_prev;
    if (sp == ts_coev_bunch.busy) {
        ts_coev_bunch.busy = sp->cb_next;
        if (ts_coev_bunch.busy)
            ts_coev_bunch.busy->cb_prev = NULL;
    }

#ifdef I_AM_NOT_PARANOID
    sp->state = CSTATE_ZERO;
    sp->status = CSW_NONE;
#else
    memset(sp, 0, sizeof(coev_t));
#endif
    
    /* 2. add to avail list */
    sp->cb_prev = NULL; /* not used in avail list */
    
    sp->cb_next = ts_coev_bunch.avail;
    ts_coev_bunch.avail = sp;
    
    _fm.coevs_free ++;
    _fm.coevs_used --;
}

static void
_free_coevs(void) {
    coev_t *spa, *spb, *span, *spbn;
    spa = ts_coev_bunch.avail;
    spb = ts_coev_bunch.busy;
    while (spa || spb) {
        if (spa)
            span = spa->cb_next;
        if (spb)
            spbn = spb->cb_next;
        if (spa)
            free(spa);
        if (spb)
            free(spb);
        
        spa = span;
        spb = spbn;
    }
    /* after this point all pointers to coev_t-s are totally invalid. */
    _fm.coevs_free = 0;
    _fm.coevs_used = 0;
}

/* end of coev_t allocator */

static void update_treepos(coev_t *);
static void io_callback(struct ev_loop *, ev_io *, int );
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
    /* kc/kc_tail init not needed due to memset above */
    
    root->parent = NULL;
    root->run = NULL;
    root->id = 0;
    root->stack = NULL;
    root->state = CSTATE_CURRENT;
    root->status = CSW_NONE;
    root->rq_next = NULL;
    root->child_count = 0;
    
    ev_init(&root->watcher, io_callback);
    ev_timer_init(&root->io_timer, iotimeout_callback, 23., 42.);
    ev_timer_init(&root->sleep_timer, sleep_callback, 23., 42.);
    
    update_treepos(root);
    
#ifdef THREADING_MADNESS
    root->thread = pthread_self();
#endif
}

/** universal runner */
static void coev_initialstub(void);
static void cls_keychain_init(cokeychain_t **);

/** return a ready-to-run coroutine
Note: stack is allocated using anonymous mmap, so be generous, it won't
eat physical memory until needed. 2Mb is the libc's default on linux. */
coev_t *
coev_new(coev_runner_t runner, size_t stacksize) {
    coevst_t *cstack;
    coev_t *child;
    
    if (ts_current == NULL)
        _fm.abort("coev_init(): library not initialized");
    
    if (stacksize < SIGSTKSZ)
        _fm.abort("coev_init(): stack size too small (less than SIGSTKSZ)");

    child = _get_a_coev();
    cstack = _get_a_stack(stacksize);
    
    if (getcontext(&child->ctx))
	_fm.eabort("coev_init(): getcontext() failed", errno);
    
    child->ctx.uc_stack.ss_sp = cstack->sp;
    /* child->ctx.uc_stack.ss_flags = 0; */
    child->ctx.uc_stack.ss_size = stacksize;
    child->ctx.uc_link = &(((coev_t*)ts_current)->ctx);
    child->stack = cstack;
    
    makecontext(&child->ctx, coev_initialstub, 0);
    
    child->id = ts_count++;
    
    child->child_count = 0;
    child->parent = (coev_t*)ts_current;
    ts_current->child_count ++;
    
    update_treepos(child);
    child->run = runner;
    child->state = CSTATE_RUNNABLE;
    child->status = CSW_NONE;
    child->rq_next = NULL;
    {
        cokeychain_t *kc = &child->kc;
        cls_keychain_init(&kc);
    }
    
    child->kc_tail = NULL;
    child->origin = NULL;
    
    ev_init(&child->watcher, io_callback);
    ev_timer_init(&child->io_timer, iotimeout_callback, 23., 42.);
    ev_timer_init(&child->sleep_timer, sleep_callback, 23., 42.);
    
    return child;
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
    "DEAD     ",
    0
};

static const char* 
str_coev_status[] = {
    "NONE     ",
    "VOLUNTARY",
    "EVENT    ",
    "WAKEUP   ",
    "TIMEOUT  ",
    "YOURTURN ",
    "SIGCHLD  ",
    "(not defined)",
    "(not defined)",
    "(less than an error)",
    "SCHEDULER_NEEDED ",
    "TARGET_SELF",
    "TARGET_DEAD",
    "TARGET_BUSY",
    0
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

static void coev_runq_remove(coev_t *);

/** entry point: function for voluntary switching between coroutines */
void
coev_switch(coev_t *target) {
    coev_t *origin = (coev_t*)ts_current;
    CROSSTHREAD_CHECK(target, p);
    
    coev_dprintf("coev_switch(): from [%s] to [%s]\n", 
	origin->treepos, target->treepos);
    coev_dump("switch, origin", origin);
    coev_dump("switch, target", target);        
    
    switch (target->state) {
        case CSTATE_CURRENT:
            origin->status = CSW_TARGET_SELF;
            origin->origin = origin;
            return;
        
        case CSTATE_SCHEDULED:
            coev_runq_remove(target);
        
        case CSTATE_RUNNABLE:
            break;
        
        case CSTATE_IOWAIT:
        case CSTATE_SLEEP:
            origin->status = CSW_TARGET_BUSY;
            origin->origin = origin;
            return;
        
        case CSTATE_DEAD:
            origin->status = CSW_TARGET_DEAD;
            origin->origin = origin;
            return;
        
        case CSTATE_ZERO:
        default:
            _fm.abort("switch to uninitialized coroutine");
    }
    
    target->origin = origin;
    origin->state = CSTATE_RUNNABLE;
    target->status = CSW_VOLUNTARY;
    target->state = CSTATE_CURRENT;
    target->status = CSW_VOLUNTARY;
    ts_current = target;
    _fm.c_switches++;
    
    cstk_dump("before switch\n");
    
    if (swapcontext(&origin->ctx, &target->ctx) == -1)
        _fm.abort("coev_switch(): swapcontext() failed.");
    
    cstk_dump("after switch\n");
}

static void
coev_stop_watchers(coev_t *subject) {
    coev_dprintf("coev_stop_watchers() [%s]: watcher %d/%d iotimer %d/%d sleep_timer %d/%d\n",
        subject->treepos, 
        ev_is_active(&subject->watcher), ev_is_pending(&subject->watcher),
        ev_is_active(&subject->io_timer), ev_is_pending(&subject->io_timer),
        ev_is_active(&subject->sleep_timer), ev_is_pending(&subject->sleep_timer));
    
    /* stop io watcher */
    ev_io_stop(ts_scheduler.loop, &subject->watcher);
    
    /* stop timers */
    ev_timer_stop(ts_scheduler.loop, &subject->io_timer);
    ev_timer_stop(ts_scheduler.loop, &subject->sleep_timer);
    
}

static void cls_keychain_fini(cokeychain_t *);

/* goes and releases all dead up the ancestor chain.
   returns first unreleasable ancestor. NULL on total fail. */
static coev_t *
_coev_sweep(coev_t *suspect) {
    coev_t *parent;
    coev_dprintf("_coev_sweep(): starting at [%s] %s cc=%d\n", 
        suspect->treepos, str_coev_state[suspect->state], suspect->child_count);
    while (suspect != NULL) {
        if ((suspect->child_count > 0) || (suspect->state != CSTATE_DEAD)) {
            coev_dprintf("_coev_sweep(): returning [%s] %s cc=%d\n", 
                suspect->treepos, str_coev_state[suspect->state], suspect->child_count);
            return suspect;
        }
        coev_dprintf("_coev_sweep(): releasing [%s] %s cc=%d\n", 
            suspect->treepos, str_coev_state[suspect->state], suspect->child_count);
        
        parent = suspect->parent;
        _return_a_stack(suspect->stack);
        cls_keychain_fini(suspect->kc.next);
        _return_a_coev(suspect);
        parent->child_count --;
        suspect = parent;
    }
    coev_dprintf("_coev_sweep(): oops, no one's alive here.");
    return NULL;
}

/** the first and last function that runs in the coroutine */
static void 
coev_initialstub(void) {
    coev_t *self = (coev_t*)ts_current;
    coev_t *parent;
    
    self->run(self);
    
    /* clean up any scheduler stuff */
    coev_stop_watchers(self);
    
    /* die */
    self->state = CSTATE_DEAD;
    
    /* release resources */
    parent = _coev_sweep(self);
    
    /* find switchable target by ignoring dead and busy coroutines */
    while (    (parent != NULL)
            && (parent->state != CSTATE_RUNNABLE)
            && (parent->state != CSTATE_SCHEDULED) )
        parent = parent->parent;

    if (!parent) {
        if (ts_scheduler.scheduler && (ts_scheduler.scheduler->state == CSTATE_RUNNABLE) )
            /* here if scheduler is in another branch AND root is not RUNNABLE/SCHEDULED. */
            parent = ts_scheduler.scheduler;
        else
            _fm.abort("coev_initialstub(): absolutely no one to cede control to.");
    }
    
    /* that's it. */
    if (parent->state == CSTATE_SCHEDULED)
        coev_runq_remove(parent);
    
    parent->state  = CSTATE_CURRENT;
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
coev_runq_remove(coev_t *subject) {
    coev_t *t = ts_scheduler.runq_head;
    
    if ( ts_scheduler.runq_head == subject ) {
	ts_scheduler.runq_head = subject->rq_next;
	return;
    }
    
    while (t) {
        if (t->rq_next == subject) {
            t->rq_next = subject->rq_next;
            return;
        }
        t = t->rq_next;
    }
}

static int
coev_runq_append(coev_t *waiter) {
    waiter->rq_next = NULL;
    
    if (ts_scheduler.runq_tail != NULL)
	ts_scheduler.runq_tail->rq_next = waiter;
    
    ts_scheduler.runq_tail = waiter;    
	
    if (ts_scheduler.runq_head == NULL)
	ts_scheduler.runq_head = waiter;
    
    return 0;
}

static void
dump_runqueue(const char *header) {
    coev_t *next = ts_scheduler.runq_head;
    coev_dprintf("%s\n", header);
    
    if (!next)
        coev_dprintf("    RUNQUEUE EMPTY\n");
    
    while (next) {
        coev_dprintf("    <%p> [%s] %s %s\n", next, next->treepos,
            str_coev_state[next->state], str_coev_status[next->status] );
        if (next == next->rq_next)
            _fm.abort("dump_runqueue(): runqueue loop detected");
        next = next->rq_next;
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
        case CSTATE_RUNNABLE:
            break;
        default: 
            _fm.abort("coev_schedule(): invalid coev_t::state");
    }
    
    waiter->state = CSTATE_SCHEDULED;
    coev_runq_append(waiter);
    coev_dprintf("coev_schedule: [%s] %s scheduled.\n",
        waiter->treepos, str_coev_state[waiter->state]);
    return 0;
}

int
coev_stall(void) {
    if (ts_scheduler.scheduler) {
        int rv;
        rv = coev_schedule((coev_t *)ts_current);
        if (rv)
            return rv;
        coev_switch(ts_scheduler.scheduler);
        /* FIXME: switch may fail, though it should be impossible. */
        /* TODO: assert we're here from scheduler. */
        return 0;
    }
    return CSCHED_NOSCHEDULER;
}

/* in your io_scheduler, stopping your watcher, switching to your waiter */
static void 
io_callback(struct ev_loop *loop, ev_io *w, int revents) {
    coev_t *waiter = (coev_t *) ( ((char *)w) - offsetof(coev_t, watcher) );
    ev_io_stop(loop, w);
    ev_timer_stop(ts_scheduler.loop, &waiter->io_timer);
    
    assert(waiter->state == CSTATE_IOWAIT);
    
    waiter->state = CSTATE_SCHEDULED;
    waiter->status = CSW_EVENT;
    coev_runq_append(waiter);
    ts_scheduler.waiters -= 1;
    
    coev_dprintf("io_callback(): [%s] revents=%d\n", waiter->treepos, revents);
}

static void
iotimeout_callback(struct ev_loop *loop, ev_timer *w, int revents) {
    coev_t *waiter = (coev_t *) ( ((char *)w) - offsetof(coev_t, io_timer) );

    ev_io_stop(ts_scheduler.loop, &waiter->watcher);
    ev_timer_stop(ts_scheduler.loop, w);
    
    assert(waiter->state == CSTATE_IOWAIT);

    waiter->state = CSTATE_SCHEDULED;
    waiter->status = CSW_TIMEOUT; /* this is timeout */    
    coev_runq_append(waiter);
    ts_scheduler.waiters -= 1;
    
    coev_dprintf("iotimeout_callback(): [%s].\n", waiter->treepos);
}

static void
sleep_callback(struct ev_loop *loop, ev_timer *w, int revents) {
    coev_t *waiter = (coev_t *) ( ((char *)w) - offsetof(coev_t, sleep_timer) );

    assert(waiter->state == CSTATE_SLEEP);
    
    waiter->state = CSTATE_SCHEDULED;
    waiter->status = CSW_WAKEUP; /* this is scheduled */
    coev_runq_append(waiter);
    ts_scheduler.waiters -= 1;
    
    coev_dprintf("sleep_callback(): [%s]\n", waiter->treepos);
}

/* sets current coro to wait for revents on fd, switches to scheduler */
void 
coev_wait(int fd, int revents, ev_tstamp timeout) {
    coev_t *self = (coev_t*)ts_current;
    
    coev_dprintf("coev_wait(): [%s] %s scheduler [%s], self->parent [%s]\n", 
        self->treepos,  str_coev_state[self->state],
        ts_scheduler.scheduler ? ts_scheduler.scheduler->treepos : "none", 
        self->parent ? self->parent->treepos : "none");

    if ((!ts_scheduler.scheduler) || (ts_scheduler.scheduler->state != CSTATE_RUNNABLE)) {
        coev_dprintf("ts_scheduler.scheduler %p, state %s\n",
            ts_scheduler.scheduler,
            ts_scheduler.scheduler ?  str_coev_state[ts_scheduler.scheduler->state] : "none");
        self->status = CSW_SCHEDULER_NEEDED;
        self->origin = self;
        return;
    }
    
    if (self == ts_scheduler.scheduler) {
        /* should be unpossible */
        self->status = CSW_TARGET_SELF;
        self->origin = self;
        return;
    }

    /* check that there's nothing on watchers */
    if (    ev_is_active(&self->watcher)
         || ev_is_pending(&self->watcher)
         || ev_is_active(&self->io_timer)
         || ev_is_pending(&self->io_timer)
         || ev_is_active(&self->sleep_timer)
         || ev_is_pending(&self->sleep_timer) ) {
        coev_dprintf("coev_wait(%d, %d, %f): inconsistent event watchers' status:\n"
            "    watcher: %c%c\n    io_timer: %c%c\n    sleep_timer: %c%c\n",
             fd, revents, timeout,
             ev_is_active(&self->watcher) ? 'A' : 'a',
             ev_is_pending(&self->watcher) ? 'P' : 'p',
             ev_is_active(&self->io_timer) ? 'A' : 'a',
             ev_is_pending(&self->io_timer) ? 'P' : 'p',
             ev_is_active(&self->sleep_timer) ? 'A' : 'a',
             ev_is_pending(&self->sleep_timer) ? 'P' : 'p' );
        _fm.abort("coev_wait(): inconsistent event watchers' status.");
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
        ev_io_init(&self->watcher, io_callback, fd, revents);
        ev_io_start(ts_scheduler.loop, &self->watcher);
        _fm.c_waits++;
        self->state = CSTATE_IOWAIT;
    }
    
    ts_scheduler.waiters += 1;
    
    coev_dprintf("coev_wait(): switching to scheduler\n");
    ts_scheduler.scheduler->state = CSTATE_CURRENT;
    ts_scheduler.scheduler->status = CSW_VOLUNTARY;
    ts_scheduler.scheduler->origin = self;
    ts_current = ts_scheduler.scheduler;
    
    cstk_dump("before swapcontext\n");
    if (swapcontext(&self->ctx, &ts_scheduler.scheduler->ctx) == -1)
        _fm.abort("coev_scheduled_switch(): swapcontext() failed.");
    cstk_dump("after swapcontext\n");
    
    /* we're here either because scheduler switched back
       or someone is being rude. */
    
    if (   (ts_current->status != CSW_EVENT)
	&& (ts_current->status != CSW_WAKEUP)
        && (ts_current->status != CSW_TIMEOUT)) {
	/* someone's being rude. */
        coev_dprintf("coev_wait(): [%s]/%s is being rude to [%s] %s %s\n",
            self->origin->treepos, str_coev_state[self->origin->state],
            self->treepos, str_coev_state[self->state], 
            str_coev_status[self->status]);
        _fm.abort("unscheduled switch into event-waiting coroutine");
    }
    coev_dprintf("coev_wait(): [%s] switch back from [%s] %s CSW: %s\n", 
        self->treepos, self->origin->treepos,
        str_coev_state[self->origin->state],  str_coev_status[self->status]);
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
    
    returns:
        current scheduler if there is one, 
        NULL after there's nothing to schedule left, 
        NULL if it was interrupted by coev_unloop 
            (ts_scheduler.stop_flag is true in this case)

*/

coev_t *
coev_loop(void) {
    coev_dprintf("coev_loop(): scheduler entered.\n");
    
    if (ts_scheduler.scheduler)
        return ts_scheduler.scheduler;
    
    ts_scheduler.scheduler = (coev_t*)ts_current;
    ts_scheduler.stop_flag = 0;
    
    do {
	coev_t *target, *runq_head;
        
        
	dump_runqueue("coev_loop(): runqueue before running it");
        coev_dprintf("coev_loop(): %d waiters\n", ts_scheduler.waiters);
	
        /* guard against infinite loop in scheduler in case something 
           schedules itself over and over */
	runq_head = ts_scheduler.runq_head;
        ts_scheduler.runq_head = ts_scheduler.runq_tail = NULL;
        
        coev_dprintf("coev_loop(): running the queue.\n");
        
	while ((target = runq_head)) {
            coev_dprintf("coev_loop(): runqueue run: target %p head %p next %p\n", target, runq_head, target->rq_next);
	    runq_head = target->rq_next;
            if (runq_head == target)
                _fm.abort("coev_loop(): runqueue loop detected");
	    target->rq_next = NULL;
            
            if ((target->state != CSTATE_RUNNABLE) && (target->state != CSTATE_SCHEDULED)) {
                coev_dprintf("coev_scheduler(): [%s] is %s, skipping.\n",
                        target->treepos, str_coev_state[target->state]);
                continue;
            }
            coev_dprintf("coev_scheduler(): switching to [%s] %s %s\n",
                    target->treepos, str_coev_state[target->state], str_coev_status[target->status]);
            
            ts_current->state = CSTATE_RUNNABLE;
            target->origin = (coev_t *) ts_current;
            target->state = CSTATE_CURRENT;
            ts_current = target;
            
            cstk_dump("before swapcontext");
            cstk_dprintf("target's sp %p origin's sp %p\n", target->ctx.uc_stack.ss_sp,
                target->origin->ctx.uc_stack.ss_sp);

            if (swapcontext(&target->origin->ctx, &target->ctx) == -1)
                _fm.abort("coev_scheduler(): swapcontext() failed.");
            
            cstk_dump("after swapcontext\n");
            cstk_dprintf("current sp %p origin's sp %p\n", ts_current->ctx.uc_stack.ss_sp,
                ts_current->origin->ctx.uc_stack.ss_sp);
            
            switch (ts_current->status) {
                case CSW_VOLUNTARY:
                    coev_dprintf("coev_loop(): yield from %p [%s]\n", ts_current->origin, ts_current->origin->treepos);
                    break;
                case CSW_SIGCHLD:
                    coev_dprintf("coev_loop(): sigchld from %p [%s] ignored.\n", ts_current->origin, ts_current->origin->treepos);
                    break;
                default:
                    coev_dprintf("Unexpected switch to scheduler (i'm [%s])\n", ts_current->treepos);
                    coev_dump("origin", ts_current->origin); 
                    coev_dump("self", (coev_t *)ts_current);
                    _fm.abort("unexpected switch to scheduler");
            }
	}
        
	dump_runqueue("coev_loop(): runqueue after running it");
        coev_dprintf("coev_loop(): %d waiters\n", ts_scheduler.waiters);
        
	if (ts_scheduler.runq_head != NULL) 
	    ev_loop(ts_scheduler.loop, EVLOOP_NONBLOCK);
	else
            if (ts_scheduler.waiters > 0)
                ev_loop(ts_scheduler.loop, EVLOOP_ONESHOT);
            else 
                break;
    } while (!ts_scheduler.stop_flag);
    
    ts_scheduler.scheduler = NULL;
    coev_dprintf("coev_loop(): scheduler exited.\n");
    return NULL;
}

void
coev_unloop(void) {
    /* ts_scheduler.runq_tail = NULL; if we're totally aborting the scheduler */
    ev_unloop(ts_scheduler.loop, EVUNLOOP_ALL);
    ts_scheduler.stop_flag = 1;
    coev_dprintf("coev_unloop(): ev_unloop called.\n");
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
            colo_dprintf("            <%p>: owner [%s] count %d bunch %p\n", lc, 
                lc->owner != NULL ? lc->owner->treepos : "(nil)", 
                lc->count, lc->bunch);
            lc = lc->next;
            i++;
        }
        colo_dprintf("            TOTAL %d\n", i);
        colo_dprintf("        AVAIL DUMP:\n");
        lc = p->avail;
        i = 0;
        while (lc != NULL) {
            colo_dprintf("            <%p>: owner [%s] count %d bunch %p\n", lc, 
                lc->owner != NULL ? lc->owner->treepos : "(nil)", 
                lc->count, lc->bunch);
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
    bunch->next = NULL;
    bunch->area = _fm.malloc(sizeof(colock_t) * COLOCK_PREALLOCATE);
    
    if (bunch->area == NULL)
	_fm.abort("ENOMEM allocating lock area");	
    
    memset(bunch->area, 0, sizeof(colock_t) * COLOCK_PREALLOCATE);
    bunch->allocated =  COLOCK_PREALLOCATE;
    {
        int i;
        
        for(i=1; i < COLOCK_PREALLOCATE; i++) {
            bunch->area[i-1].next = &(bunch->area[i]);
            bunch->area[i-1].bunch = bunch;
        }
        bunch->area[COLOCK_PREALLOCATE-1].bunch = bunch;
    }
    bunch->avail = bunch->area;
    bunch->used = NULL;
    
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
    lock->owner = NULL;

    colo_dprintf("colock_allocate(): [%s] allocates %p\n", ts_current->treepos, lock);
    colo_dump(ts_rootlockbunch);
    return (void *) lock;
}

void 
colock_free(colock_t *lock) {
    colock_t *prev;
    colbunch_t *bunch = lock->bunch;
    
    lock->owner = NULL; 
    
    colo_dprintf("colock_free(%p): [%s] deallocates [%s]'s %p (of bunch %p)\n", lock, ts_current->treepos, 
        lock->owner != NULL ? lock->owner->treepos : "(nil)",  lock, bunch);
    
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
        if (p->count > 0) {
            colo_dprintf("colock_acquire(%p, %d): [%s] fails to acquire lock belongs to [%s] %d times\n", 
                p, wf, ts_current->treepos, p->owner->treepos, p->count);
            return 0;
        }
    }
    
    while (p->owner) {
        if (p->owner == ts_current) {
            p->count ++;
            colo_dprintf("colock_acquire(%p, %d): [%s] acquires lock for %dth time\n", 
                p, wf, ts_current->treepos, p->count);
            return 1;
        }
        if (p->count == 0) {
            colo_dprintf("colock_acquire(%p, %d): lock has owner [%s] and count=0, unpossible!\n",
                    p, wf, p->owner->treepos);
            _fm.abort("owned lock has count = 0");
        }
	/* the promised dire insults */
	colo_dprintf("colock_acquire(%p, %d): [%s] attemtps to acquire lock that was not released by [%s]\n",
	    p, wf, ts_current->treepos, p->owner->treepos);
        if (wf == 0)
            return 0;
        
	_fm.abort("haha busy-wait");
	coev_stall();
    }
    p->owner = (coev_t *)ts_current;
    p->count = 1;
    colo_dprintf("colock_acquire(%p, %d): [%s] successfully acquires lock.\n", p, wf, ts_current->treepos);
    
    return 1;
}

void 
colock_release(colock_t *p) {
    if (p->count == 0)
        colo_dprintf("colock_release(%p): [%s] releases a lock that has no owner\n", p, ts_current->treepos);
    if (p->count >0)
        p->count--;
    if (p->owner != ts_current)
        colo_dprintf("colock_release(%p): [%s] releases lock that was acquired by [%s], new count=%d\n", 
            p, ts_current->treepos, p->owner != NULL ? p->owner->treepos : "(nil)", p->count);
    else
        colo_dprintf("colock_release(%p): [%s] releases lock, new count=%d.\n", 
            p, ts_current->treepos, p->count);
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
    self->in_limit = CNRBUF_MAGIC;
    self->iop_timeout = timeout;
    self->in_buffer = _fm.malloc(self->in_allocated);
    self->fd = fd;
    self->err_no = 0;
    
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
    ssize_t top_free, total_free, bottom_free, used_start_off, used_end_off;

    top_free = self->in_position - self->in_buffer;
    total_free = self->in_allocated - self->in_used;
    bottom_free = total_free - top_free;
    used_start_off = self->in_position - self->in_buffer;
    used_end_off = used_start_off + self->in_used;

    _fm.dprintf("buffer metadata:\n"
    "\tbuf=%p pos=%p used offsets %zd  - %zd \n"
    "\tallocated=%zd used=%zd limit=%zd\n"
    "\ttop_free=%zd bottom_free=%zd\ttotal_free=%zd\n",
	self->in_buffer, self->in_position, used_start_off, used_end_off,
	self->in_allocated, self->in_used, self->in_limit,
	top_free, bottom_free, total_free);
    assert(used_start_off <= self->in_allocated);
    assert(used_end_off <= self->in_allocated);
    
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
        ssize_t posn_offset;
	ssize_t newsize = (self->in_used + needed + 2*CNRBUF_MAGIC) & (~(CNRBUF_MAGIC-1));
        if (newsize > self->in_limit)
            self->in_limit = newsize;
        
        posn_offset = self->in_position - self->in_buffer;
	self->in_buffer = _fm.realloc(self->in_buffer, newsize);
        
        if (!self->in_buffer) {
            errno = ENOMEM;
            return -1; /* no memory */
        }
        self->in_allocated = newsize;
        self->in_position = self->in_buffer + posn_offset;
        cnrb_dprintf("sf_reshuffle_buffer(*,%zd): realloc successful: newsize=%zd\n", 
            needed, self->in_allocated);
    }
    /* we're still have 2*CNRBUF_MAGIC bytes more than needed */
    /* OR we just reallocated the buffer and let's move */
    /* the used bytes to the top anyway */
    
    memmove(self->in_buffer, self->in_position, self->in_used);
    self->in_position = self->in_buffer;

    cnrb_dprintf("sf_reshuffle_buffer(*,%zd): after realloc and/or move\n", needed);
    cnrb_dump(self);
    
    return 0;
}

ssize_t 
cnrbuf_read(cnrbuf_t *self, void **p, ssize_t sizehint) {
    ssize_t rv, readen, to_read;
    
    if (self->owner && (self->owner != ts_current)) {
        errno = EBUSY;
        return -1;
    }
    
    if ((self->err_no != 0) && (self->in_used == 0)) {
        /* we had error, but returned buffer 
           contents up to it. Return error now */
        errno = self->err_no;
        return -1;
    }
    
    cnrb_dprintf("cnrbuf_read(): fd=%d sizehint %zd bytes buflimit %zd bytes\n", 
        self->fd, sizehint, self->in_limit);
    
    if (sizehint > self->in_limit)
        self->in_limit = sizehint;

    do {
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
    
        if ( sf_reshuffle_buffer(self, to_read) ) {
            self->err_no = ENOMEM;
            return 0;
        }
rerecv:
	readen = recv(self->fd, self->in_position + self->in_used, to_read, 0);
        cnrb_dprintf("cnrbuf_read(): %zd bytes read into %p, reqd len %zd\n", 
            readen, self->in_position + self->in_used, to_read);
        cnrb_dump(self);
        
	if (readen == -1) {
	    if (errno == EAGAIN) {
		coev_wait(self->fd, COEV_READ, self->iop_timeout);
                if (ts_current->status == CSW_EVENT)
                    goto rerecv;
                
		if (ts_current->status == CSW_TIMEOUT)
                    self->err_no = ETIMEDOUT;
                else
                    _fm.abort("cnrbuf_read(): unpossible status after wait");
            } else {
                self->err_no = errno;
            }
	} else {
            self->in_used += readen;
        }
        
    } while (readen > 0);
    
    
    /* error and no data to return */
    if ((self->in_used == 0) && (self->err_no != 0)) {
        errno = self->err_no;
        return -1;
    }
    
    /* return whatever we managed to read, error or shutdown. */
    *p = self->in_position;
    rv = self->in_used;
    self->in_used = 0;
    return rv;
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
    ssize_t rv, to_read, readen;
    
    if ((self->err_no != 0) && (self->in_used == 0)) {
        /* we had error, but returned buffer 
           contents up to it. Return error now */
        errno = self->err_no;
        return -1;
    }
    
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
        if ( sizehint )
            to_read = sizehint - self->in_used;
        else
            if ( self->in_used + 2 * CNRBUF_MAGIC < self->in_limit )
                to_read = 2 * CNRBUF_MAGIC;
            else 
                to_read = self->in_limit - self->in_used;
            
        if ( sf_reshuffle_buffer(self, to_read) ) {
            self->err_no = ENOMEM;
            return 0;
        }
rerecv:
	readen = recv(self->fd, self->in_position + self->in_used, to_read, 0);
        cnrb_dprintf("cnrbuf_readline: %zd bytes read into %p, reqd len %zd errno %s\n", 
                readen, self->in_position + self->in_used, to_read, 
                readen==-1? strerror(errno): "none");
        if (readen > 0) {
            /* woo we read something */
            char *old_position = self->in_position + self->in_used;
            self->in_used += readen;
            cnrb_dump(self);
	    rv = sf_extract_line(self, old_position, p, sizehint);
            if ( rv > 0 )
                return rv;
        }
        
	if (readen == -1) {
	    if (errno == EAGAIN) {
		coev_wait(self->fd, COEV_READ, self->iop_timeout);
                if (ts_current->status == CSW_EVENT) {
                    cnrb_dprintf("cnrbuf_readline: CSW_EVENT after wait, continuing\n");
                    goto rerecv;
                }
                
		if (ts_current->status == CSW_TIMEOUT)
                    self->err_no = ETIMEDOUT;
                else
                    _fm.abort("cnrbuf_read(): unpossible status after wait");
            } else {
                self->err_no = errno;
            }
	}

    } while(readen > 0);
    cnrb_dprintf("cnrbuf_readline: readen==0\n");
    
    /* error and no data to return */
    if ((self->in_used == 0) && (self->err_no != 0)) {
        errno = self->err_no;
        return -1;
    }
    /* no more data : return whatever there is */
    *p = self->in_position;
    rv = self->in_used;
    self->in_used = 0;
    self->in_position = self->in_buffer;
    return rv;    
}

int
coev_send(int fd, const void *data, ssize_t len, ssize_t *rv, double timeout) {
    ssize_t wrote, to_write, written;

    written = 0;
    to_write = len;
    
    cnrb_dprintf("coev_send(): fd=%d len=%zd bytes\n", fd, to_write);
    while (to_write){ 
	wrote = send(fd, (char *)data + written, to_write, MSG_NOSIGNAL);
	cnrb_dprintf("coev_send(): fd=%d wrote=%zd bytes\n", fd, wrote);
	if (wrote == -1) {
	    if (errno == EAGAIN) {
		coev_wait(fd, COEV_WRITE, timeout);
		if (ts_current->status == CSW_EVENT)
		    continue;
                if (ts_current->status == CSW_TIMEOUT) {
                    errno = ETIMEDOUT;
                    *rv = written;
                    return -1;
                }
                _fm.abort("coev_send() unpossible status after wait()");
	    }
	    break;
	}
	written += wrote;
	to_write -= wrote;
    }
    *rv = written;
    return to_write == 0 ? 0 : -1;
}

const char const *coev_stat_names[] = {
    "switches",
    "waits",
    "sleeps",
    "stacks.free",
    "stacks.used",
    "coevs.free",
    "coevs.used",
    "coevs.dead",
};

void
coev_getstats(uint64_t *ptr) {
    ptr[0] = _fm.c_switches;
    ptr[1] = _fm.c_waits;
    ptr[2] = _fm.c_sleeps;
    ptr[3] = _fm.stacks_free;
    ptr[4] = _fm.stacks_used;
    ptr[5] = _fm.coevs_free;
    ptr[6] = _fm.coevs_used;
    ptr[7] = _fm.coevs_dead;
}

void
coev_setdebug(int debug) {
    _fm.debug = debug;
}

int 
coev_setparent(coev_t *target, coev_t *newparent) {
    coev_t *p;
    
    if (newparent->state == CSTATE_ZERO)
        /* wrong trousers */
        return -1;
    
    p = _coev_sweep(newparent);

    if (!p)
        _fm.abort("everyone's dead, how come?");
    
    target->parent->child_count --;
    
    _coev_sweep(target->parent);
    
    newparent->child_count ++;
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
    _fm.c_waits = 0;
    _fm.c_sleeps = 0;
    _fm.stacks_free = 0;
    _fm.stacks_used = 0;
    _fm.coevs_free = 0;
    _fm.coevs_used = 0;
    _fm.coevs_dead = 0; /* active = used - dead */
    
    ts_scheduler.loop = ev_default_loop(0);
    ts_scheduler.scheduler = NULL;
    ts_scheduler.runq_head = NULL;
    ts_scheduler.runq_tail = NULL;
    ts_scheduler.waiters = 0;
    
    ts_cls_last_key = 1L;
    
    if (_fm.inthdlr) {
        ev_signal_init(&ts_scheduler.intsig, intsig_cb, SIGINT);
        ev_signal_start(ts_scheduler.loop, &ts_scheduler.intsig);
        ev_unref(ts_scheduler.loop);
    }
    
    ts_rootlockbunch = NULL;
    colock_bunch_init(&ts_rootlockbunch);
    
    memset(&ts_stack_bunch, 0, sizeof(struct _coev_stack_bunch));
    memset(&ts_coev_bunch, 0, sizeof(struct _coev_t_bunch));
    
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
    _free_coevs(); /* yep. worse than the above. */
}

void coev_fork_notify(void) {
    ev_default_fork();
}