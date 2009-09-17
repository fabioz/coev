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
 
#ifndef COEV_H
#define COEV_H
#ifdef __cplusplus
extern "C" {
#endif

#ifdef THREADING_MADNESS
#include <pthread.h>
#endif
#include <stdint.h>
#include <unistd.h>
#include <ucontext.h>

#include <ev.h>

/* coev_t::state */
#define CSTATE_INIT          0 /* not initialized */
#define CSTATE_CURRENT       1 /* currently executing */
#define CSTATE_RUNNABLE      2 /* switched out voluntarily */
#define CSTATE_SCHEDULED     3 /* in runqueue */
#define CSTATE_IOWAIT        4 /* waiting on an fd */
#define CSTATE_SLEEP         5 /* sleeping */
#define CSTATE_DEAD          6 /* dead */

/* coev_t::status */
#define CSW_NONE               0 /* there was no switch */
#define CSW_VOLUNTARY          1 /* explicit switch, not from the scheduler */
#define CSW_EVENT              2 /* io-event fired */
#define CSW_WAKEUP             3 /* sleep elapsed */
#define CSW_TIMEOUT            4 /* io-event timed out */
#define CSW_SIGCHLD            5 /* child died */
#define CSW_YIELD              6 /* switck back after explicit yield (aka scheduled) */
/* below are immediate (no actual switch) error return values */
#define CSW_NOWHERE_TO_SWITCH  7 /* wait or sleep w/o scheduler and no one to ask for one */
#define CSW_SCHEDULER_NEEDED   8 /* wait or sleep w/o scheduler: please run one (to a parent) */
#define CSW_WAIT_IN_SCHEDULER  9 /* wait or sleep in the scheduler */
#define CSW_SWITCH_TO_SELF    10 /* switch to self attempted. */


struct _coev;
typedef struct _coev coev_t;
typedef void (*coev_runner_t)(coev_t *);

struct _coev {
    ucontext_t ctx;     /* the context */
    unsigned int id;    /* serial, to build debug representations / show tree position */
    
    coev_t *parent;     /* report death here */
    coev_t *origin;     /* switched from here last time */

    int state;          /* CSTATE_* -- state of this coroutine */
    int status;         /* CSW_*  -- status of last switch into this coroutine */
    
    coev_runner_t run;  /* entry point into the coroutine, NULL if the coro has already started. */
    
    struct ev_io watcher; /* IO watcher */
    struct ev_timer io_timer; /* IO timeout timer. */
    struct ev_timer sleep_timer; /* sleep timer */
    
    coev_t *next;         /* runqueue list pointer */
    int ran_out_of_order; /* already ran flag */
    char *treepos;      /* position in the tree */
    
#ifdef THREADING_MADNESS
    pthread_t thread;
#endif    
};


/*** METHOD OF OPERATION
    
    The run function pointer points to the function that would be 
    executed when a coroutine is first switch()-ed into.

    Prototype:
        void *run(coev_t *self, void *p);

    Parameters:
        self - pointer to this coroutine just in case 
        p - parameter given to the initial coev_switch().

    The coroutine can and should call coev_switch() inside the run()
    function in order to switch to some other coroutine. 

    ISTHISSO? If the coroutine that is being switch()-ed into was never 
    run, it will be assigned as a child.
    
    Return value of this function can be
     - a parameter passed by some other coroutine calling coev_switch()
     - return value of run() function of a child coroutine 
       that happened to die.
    
    To facilitate distinction between these two cases, an optional hook is provided,
    which will be called in context of dying coroutine, just after run() returns.
    It will be supplied run() return value, and must return a value of same type.
    

    IO SCHEDULER

    A coroutine also can and should call coev_wait() function. This function
    will set up a wait for given event and then switch to the io-scheduler
    coroutine. 
    
    A coroutine in a wait state can not be switch()-ed into. 
    
    The io-scheduler coroutine is any regular coroutine, blessed with a
    call of coev_set_scheduler(), so that it is known where to switch
    upon colo_wait() calls.
    
    Such coroutine's run() function is expected to call coev_ev_loop() to listen
    for events and dispatch by switching. 
    
    In the absence of designated scheduler coroutine, switches are perfomed to 'root'
    coroutine (created at the time of coev_initialize())
    
    FIXME: explicit scheduling rework.
    
    THREAD SAFETY
    
    In order to be safe, do not use threads.
    Switches are possible only in context of a single thread. 
    Failure to observe that will be reported with crossthread_fail callback.
    
*/


/* memory management + error reporting to use */
typedef struct _coev_framework_methods {
    /* memory management */
    void *(*malloc)(size_t);
    void *(*realloc)(void *, size_t); 
    void  (*free)(void *);
    
    /* total failure: must not return. */
    void (*abort)(const char *);
    
    /* same, but look at errno. */
    void (*eabort)(const char *, int);

    /* handle SIGINT (or NULL to ignore) */
    void (*inthdlr)(void);
    
    /* debug output collector */
    int (*dprintf)(const char *format, ...);
    
    /* 1=do the debug output */
    int debug_output;
    /* 1=dump coev structures when appropriate */
    int dump_coevs;
    
    /* statistics: */
    volatile uint64_t c_switches;
    volatile uint64_t c_bytes_copied;
    volatile uint64_t c_waits;
    volatile uint64_t c_sleeps;
    
} coev_frameth_t;

void coev_libinit(const coev_frameth_t *fm, coev_t *root);

void coev_init(coev_t *child, coev_runner_t runner, size_t stacksize);
void coev_free(coev_t *corpse);
void coev_switch(coev_t *target);
coev_t *coev_current(void);
/* returns 0 on success or -1 if a cycle would result */
int coev_setparent(coev_t *target, coev_t *newparent);

/* return tree traverse from root to given greenlet. 
   memory is not to be touched by caller */
const char *coev_treepos(coev_t *coio);
const char *coev_state(coev_t *);
const char *coev_status(coev_t *);

#define COEV_READ       EV_READ
#define COEV_WRITE      EV_WRITE

/* wait for an IO event, switch back to caller's coroutine when event fires, 
   event times out or voluntary switch occurs. 
   if ((fd == -1) && (revents == 0)), sleep for the specified time. */
void coev_wait(int fd, int revents, ev_tstamp timeout);

/* wrapper around the above. */
void coev_sleep(ev_tstamp timeout);


#define CSCHED_NOERROR          0  /* no error */
#define CSCHED_DEADMEAT         1  /* attempt to schedule dead coroutin */
#define CSCHED_ALREADY          2  /* attempt to schedule already scheduled coroutine */
#define CSCHED_NOSCHEDULER      3  /* attempt to yield, but no scheduler to switch to */

/* schedule a switch to the waiter */
int coev_schedule(coev_t *waiter);

/* must be called for IO scheduling to begin. */
void coev_loop(void);

/* can be called anywhere to stop and return from the coev_loop().
does not perform a switch to scheduler. */
void coev_unloop(void);

void coev_getstats(uint64_t *switches, uint64_t *waits, uint64_t *sleeps, uint64_t *bytes_copied);
void coev_setdebug(int de, int du);

#define COEV_STARTED(op)    ((op)->stack_stop != NULL)
#define COEV_ACTIVE(op)     ((op)->stack_start != NULL)
#define COEV_DEAD(op)       (((op)->stack_stop != NULL) && ((op)->stack_start == NULL))

#define COEV_GET_PARENT(op) ((op)->parent)

#ifdef __cplusplus
}
#endif
#endif /* COEV_H */