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

#include <ev.h>

#define CSTATE_INIT     0
#define CSTATE_CURRENT  1
#define CSTATE_IDLE     2
#define CSTATE_IOWAIT   3
#define CSTATE_SLEEP    4
#define CSTATE_DEAD     5
    
struct _coev_greenlet;
typedef struct _coev_greenlet coev_t;

struct _coev_greenlet {
    unsigned int id; /* serial, to build debug representations / show tree position */
    char* stack_start;
    char* stack_stop;
    char* stack_copy;
    long stack_saved;
    struct _coev_greenlet *stack_prev;
    struct _coev_greenlet *parent;
    struct _coev_greenlet *switchback_target;
    
    int state;
    
    /* entry point into the coroutine */
    void *(*run)(coev_t *, void *); 
    
    /* IO watcher */
    struct ev_io watcher;
    
    /* IO timeout timer. */
    struct ev_timer io_timer;
    
    /* sleep timer */
    struct ev_timer sleep_timer;
    
#ifdef THREADING_MADNESS
    pthread_t thread;
#endif    
};

#define COERV_VOLUNTARY         0
#define COERV_EVENT             1
#define COERV_TIMEOUT           2
#define COERV_NOWHERE_TO_SWITCH 3
#define COERV_SCHEDULER_NEEDED  4
/* errors */
#define COERV_WAIT_IN_SCHEDULER 5
#define COERV_SWITCH_TO_SELF    6


struct _coev_switchback_value {
    coev_t *from;
    void   *value;
    int     status;
};

typedef struct _coev_switchback_value coerv_t;

/*** METHOD OF OPERATION
    
    The run function pointer points to the function that would be 
    executed when greenlet is first switch()-ed into.

    Prototype:
        void *run(coev_t *self, void *p);

    Parameters:
        self - pointer to this greenlet just in case 
        p - parameter given to the initial coev_switch().

    The greenlet can and should call coev_switch() inside the run()
    function in order to switch to some other greenlet. If the greenlet
    that is being switch()-ed into never run, it will be assigned as 
    a child.
    
    Return value of this function can be
     - a parameter passed by some other greenlet calling coev_switch()
     - return value of run() function of a child greenlet that happened to
          die.
    
    To facilitate distinction between these two cases, an optional hook is provided,
    which will be called in context of dying greenlet, just after run() returns.
    It will be supplied run() return value, and must return a value of same type.
    

    IO SCHEDULER

    A greenlet also can and should call coev_wait() function. This function
    will set up a wait for given event and then switch to the io-scheduler
    greenlet. 
    
    A greenlet in a wait state can not be switch()-ed into. 
    
    The io-scheduler greenlet is any regular greenlet, blessed with a
    call of coev_set_scheduler(), so that it is known where to switch
    upon colo_wait() calls.
    
    Such greenlet's run() function is expected to call coev_ev_loop() to listen
    for events and dispatch by switching. 
    
    In the absence of designated scheduler greenlet, switches are perfomed to 'root'
    greenlet (created at the time of coev_initialize())
    
    
    THREAD SAFETY
    
    In order to be safe, do not use threads.
    Switches are possible only in context of a single thread. Failure to observe that
    will be reported with crossthread_fail callback.
    
*/


/* memory management + error reporting to use */
typedef struct _coev_framework_methods {
    /* memory management */
    void *(*malloc)(size_t);
    void *(*realloc)(void *, size_t); 
    void  (*free)(void *);
    
    /* exceptions */
    /* prohibited cross-thread operation */
    void (*crossthread_fail)(coev_t *, coev_t *, void *);
    
    /* switch to self */
    void (*switch2self)(coev_t *, void *);
    
    /* called after run() returns */
    void (*death)(coev_t *);
    
    /* total failure: must not return. */
    void (*abort)(const char *);

    /* handle SIGINT (or NULL to ignore) */
    void (*inthdlr)(void);
    
    /* switch debug: called just before any switch */
    void (*switch_notify)(coev_t *, coev_t *, int);
    
    /* debug output collector */
    int (*dprintf)(const char *format, ...);
    
    /* 1=do the debug output */
    int debug_output;
    /* 1=dump coev structures when appropriate */
    int dump_coevs;
    
    /* statistics: */
    uint64_t c_switches;
    uint64_t c_bytes_copied;
    uint64_t c_waits;
    uint64_t c_sleeps;
    
} coev_frameth_t;

void coev_initialize(const coev_frameth_t *fm, coev_t *root);

void coev_init(coev_t *child, void *(*run)(coev_t *, void *));
void coev_free(coev_t *corpse);
coerv_t coev_switch(coev_t *target, void *p);
coev_t *coev_current(void);

/* return tree traverse from root to given greenlet. 
   memory is to be freed by caller */
char *coev_treepos(coev_t *coio);

#define COEV_READ       EV_READ
#define COEV_WRITE      EV_WRITE

/* wait for an IO event, switch back to caller's greenlet when event fires, 
   event times out or voluntary switch occurs. */
coerv_t coev_wait(int fd, int revents, ev_tstamp timeout);

/* schedule a switchback after timeout, yield to scheduler */
coerv_t coev_sleep(ev_tstamp timeout);

#define COEV_ONESHOT    EV_ONESHOT
#define COEV_NONBLOCK   EV_NONBLOCK

/* must be called for IO scheduling to begin. */
void  coev_loop(int flags);

/* can be called anywhere to stop and return from the coev_loop().
does not perform a switch to scheduler. */
void  coev_unloop(void);

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