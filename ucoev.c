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
#include <sys/mman.h>
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

/* The current greenlet in this thread state (holds a reference) */
static TLS_ATTR volatile coev_t *ts_current;

static TLS_ATTR volatile int ts_count;

static TLS_ATTR coev_t *ts_root;

static coev_frameth_t _fm;

static TLS_ATTR
struct _coev_scheduler_stuff {
    coev_t *scheduler;
    struct ev_loop *loop;
    struct ev_signal intsig;
    coev_t *runq_head;
    coev_t *runq_tail;
} ts_scheduler;

#define coev_dprintf(fmt, args...) do { if (_fm.debug_output) _fm.dprintf(fmt, ## args); } while(0)
#define coev_dump(msg, coev) do { if (_fm.dump_coevs) _coev_dump(msg, coev); } while(0)

static void update_treepos(coev_t *);
static void sleep_callback(struct ev_loop *, ev_timer *, int );
static void iotimeout_callback(struct ev_loop *, ev_timer *, int );

/** initialize a root coroutine for a thread */
static void
coev_init_root(coev_t *root) {
    void *sp;
    size_t ROOT_STACK_SIZE = 42*4096;
    
    if (ts_current != NULL) 
        _fm.abort("coev_init_root(): second initialization refused.");
    
    ts_current = root;
    ts_root = root;

    memset(root, 0, sizeof(coev_t));
    
    root->parent = NULL;
    root->run = NULL;
    root->id = 0;
    root->state = CSTATE_CURRENT;
    root->status = CSW_NONE;
    root->next = NULL;
    root->ran_out_of_order = 0;

    sp = mmap(NULL, ROOT_STACK_SIZE, PROT_EXEC|PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0); 
    
    if (sp == MAP_FAILED)
        _fm.eabort("coev_init_root(): mmap() stack allocation failed", errno);
    
    root->ctx.uc_stack.ss_sp = sp;
    root->ctx.uc_stack.ss_flags = 0;
    root->ctx.uc_stack.ss_size = ROOT_STACK_SIZE;
    
    
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
    void *sp;

    if (ts_current == NULL)
        _fm.abort("coev_init(): library not initialized");
    
    if (stacksize < SIGSTKSZ)
        _fm.abort("coev_init(): stack size too small (less than SIGSTKSZ)");
    
    sp = mmap(NULL, stacksize, PROT_EXEC|PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0); 
    
    if (sp == MAP_FAILED)
        _fm.eabort("coev_init(): mmap() stack allocation failed", errno);
    
    memset(child, 0, sizeof(coev_t));
    
    if (getcontext(&child->ctx))
	_fm.eabort("coev_init(): getcontext() failed", errno);
    
    child->ctx.uc_stack.ss_sp = sp;
    child->ctx.uc_stack.ss_flags = 0;
    child->ctx.uc_stack.ss_size = stacksize;
    child->ctx.uc_link = &(((coev_t*)ts_current)->ctx);
    
    makecontext(&child->ctx, coev_initialstub, 0);
    
    child->id = ts_count++;
    child->parent = (coev_t*)ts_current;
    update_treepos(child);
    child->run = runner;
    child->state = CSTATE_INIT;
    child->status = CSW_NONE;
    child->next = NULL;
    child->ran_out_of_order = 0;
    
    ev_timer_init(&child->io_timer, iotimeout_callback, 23., 42.);
    ev_timer_init(&child->sleep_timer, sleep_callback, 23., 42.);
}

void
coev_free(coev_t *corpse) {
    if (corpse->ctx.uc_stack.ss_sp)
        if (munmap(corpse->ctx.uc_stack.ss_sp, corpse->ctx.uc_stack.ss_size))
            _fm.abort("coev_free(): munmap failed.");
    if (corpse->treepos)
	_fm.free(corpse->treepos);
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
    "INIT     ",
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
    "NOWHERE_TO_SWITCH",
    "SCHEDULER_NEEDED ",
    "WAIT_IN_SCHEDULER",
    "SWITCH_TO_SELF   "
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
        coev_dprintf("%s\n", m);
    coev_dprintf( "coev_t<%p> [%s] %s, %s (current<%p> root<%p>):\n"
            "    is_current: %d\n"
            "    is_root:    %d\n"
            "    is_sched:   %d\n"
            "    parent:     %p\n"
            "    run:        %p\n"
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
    
    if (_fm.debug_output) {
        coev_dprintf("coev_switch(): from [%s] to [%s]\n", 
	    origin->treepos, target->treepos);
        coev_dump("switch, origin", origin);
        coev_dump("switch, target", target);        
    }
    
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

    parent->status = CSW_SIGCHLD;
    parent->origin = self;
    
    /* that's it. */
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
        case CSTATE_INIT:
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
}

static void
iotimeout_callback(struct ev_loop *loop, ev_timer *w, int revents) {
    coev_t *waiter = (coev_t *) ( ((char *)w) - offsetof(coev_t, io_timer) );

    ev_io_stop(ts_scheduler.loop, &waiter->watcher);
    ev_timer_stop(ts_scheduler.loop, w);
    
    waiter->state = CSTATE_RUNNABLE;
    waiter->status = CSW_TIMEOUT; /* this is timeout */
    
    coev_runq_append(waiter);
}

static void
sleep_callback(struct ev_loop *loop, ev_timer *w, int revents) {
    coev_t *waiter = (coev_t *) ( ((char *)w) - offsetof(coev_t, sleep_timer) );

    waiter->state = CSTATE_RUNNABLE;
    waiter->status = CSW_WAKEUP; /* this is scheduled */

    coev_runq_append(waiter);
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
        } else {
            ev_io_init(&self->watcher, io_callback, fd, revents);
            ev_io_start(ts_scheduler.loop, &self->watcher);
        }
        _fm.c_waits++;
    
        self->state = CSTATE_IOWAIT;
    }

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

        ev_loop(ts_scheduler.loop, 0);
	
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
    
    if (_fm.inthdlr) {
        ev_signal_init(&ts_scheduler.intsig, intsig_cb, SIGINT);
        ev_signal_start(ts_scheduler.loop, &ts_scheduler.intsig);
        ev_unref(ts_scheduler.loop);
    }
    
    coev_init_root(root);
}
