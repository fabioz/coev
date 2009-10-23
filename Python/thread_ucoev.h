#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include "ucoev.h"

/** 

    Python threading model is limiting coroutines in:
    
        1. no return values from finishing coroutines.
        2. kill exception is SystemExit.
        3. not very clean coroutine identification:
            Thread id is (long)(coev_t *).
        4. deallocation of coev_t structures is done in 
           modcoev's mod_wait_bottom_half, not here.
           
        It's ugly. Had to patch threadmodule.c :(.

  coev_t::{A, X, Y, S} usage in Python. 
  
  bootstrap (_wrapper())
      A - NULL
      X - (in) pointer to boostrap function
      Y - (in) parameter of bootstrap function
      S - NULL
      
  operation
      A - (in) switch()-ret-tuple or NULL in subsequent switches, 
      X - (in, A==NULL) exception type to raise, otherwise NULL
      Y - (in, A==NULL) exception value to raise, otherwise NULL
      S - (in, A==NULL) exception traceback to use in raise, otherwise NULL
      
  death
      A - callable ret-tuple or NULL if death is due to exception
      X - (A==NULL) exception type to raise, NULL on normal return or SystemExit.
      Y - (A==NULL) exception value to raise, NULL on normal return or SystemExit.
      S - (A==NULL) exception traceback to use in raise, NULL on normal return or SystemExit.
   
**/

static void /*** FIXME ***/
python_augmented_inthdlr(void) {
    coev_unloop();
    /* should switch to root coro and raise exception there. */
    /* PyErr_SetNone(PyExc_KeyboardInterrupt); */
}

static void
pY_FatalErrno(const char *msg, int e) {
    char *serr;

    serr = strerror(e);
    fprintf(stderr, "%s: [%d] %s", msg, e, serr);
    fflush(stderr);
    abort();
}
static time_t start_time;
#ifdef Py_DEBUG
#define tuco_dprintf(fmt, args...) _tuco_dprintf(fmt, ## args)
#else
#define tuco_dprintf(fmt, args...)
#endif

static int
_tuco_dprintf(const char *fmt, ...) {
    va_list ap;
    int rv, saved_errno;

    saved_errno = errno;
    fprintf(stderr, "[%d] ", (int) (time(NULL) - start_time));
    va_start(ap, fmt);
    rv = vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
    
    errno = saved_errno;
    return rv;
}


static coev_t coev_main;

static
coev_frameth_t _cmf = {
    malloc,    /* malloc */ 
    realloc,   /* realloc */
    free,      /* free */
    Py_FatalError,              /* abort */
    pY_FatalErrno,              /* abort with errno */
    python_augmented_inthdlr,   /* unloop at SIGINT */
    _tuco_dprintf,              /* debug sink */
    0,                          /* debug flags */
};

static int initialized = 0;
static size_t _stacksize = 2 * 1024 * 1024;

/*
 * Initialization.
 */
static void
PyThread__init_thread(void) {
    tuco_dprintf("PyThread__init_thread(): initialized=%d\n", initialized);
    if (!initialized) {
        
        coev_libinit(&_cmf, &coev_main);
        initialized = 1;
	start_time = time(NULL);
    }
}

/*
 * Thread support.
 */
typedef void (*func_t)(void *);
static void 
_wrapper(coev_t *c) {
    func_t func;
    void *arg;
    func = ( func_t ) (c->X);
    arg = c->Y;
    c->A = NULL;
    c->X = NULL;
    c->Y = NULL;
    c->S = NULL;
    func(arg);
}

long
PyThread_start_new_thread(func_t func, void *arg) {
    coev_t *c;
    
    if (!initialized)
        PyThread_init_thread();
        
    c = coev_new( _wrapper, _stacksize );
    c->A = NULL;
    c->X = func;
    c->Y = arg;
    c->S = NULL;
    coev_schedule(c);
    return (long) c;
}

long
PyThread_get_thread_ident(void) {
    if (!initialized)
        PyThread_init_thread();

    return (long) coev_current();
}

void
PyThread_exit_thread(void) {
    return;
}

/* called last thing before t_boostrap returns control to _wrapper, 
   which returns to coev_initialstub, and the coev_t is officially dead. */
void
PyThread__exit_thread(void) {
    return;
}

#ifndef NO_EXIT_PROG
static
void do_PyThread_exit_prog(int status, int no_cleanup) {
    tuco_dprintf("PyThread_exit_prog(%d) called\n", status);
    if (!initialized)
        if (no_cleanup)
            _exit(status);
        else
            exit(status);
}

void
PyThread_exit_prog(int status) {
    do_PyThread_exit_prog(status, 0);
}

void
PyThread__exit_prog(int status) {
    do_PyThread_exit_prog(status, 1);
}
#endif /* NO_EXIT_PROG */

/*
 * Lock support.
 */

PyThread_type_lock
PyThread_allocate_lock(void) {
    PyThread__init_thread();
    return (PyThread_type_lock) colock_allocate();
}

void
PyThread_free_lock(PyThread_type_lock a_lock) {
    colock_free((colock_t *) a_lock);
}

int
PyThread_acquire_lock(PyThread_type_lock a_lock, int waitflag) {
    return colock_acquire((colock_t *) a_lock, waitflag);
}

void
PyThread_release_lock(PyThread_type_lock a_lock) {
    colock_release((colock_t *) a_lock);
}

/*
 * TLS support
 */

#define Py_HAVE_NATIVE_TLS "For Great Justice!"

int
PyThread_create_key(void) {
    PyThread__init_thread();
    return cls_new();
}

/* Forget the associations for key across *all* threads. */
void
PyThread_delete_key(int key) {
    cls_drop_across(key);
}

/* Confusing:  If the current thread has an association for key,
 * value is ignored, and 0 is returned.  Else an attempt is made to create
 * an association of key to value for the current thread.  0 is returned
 * if that succeeds, but -1 is returned if there's not enough memory
 * to create the association.  value must not be NULL.
 */
int
PyThread_set_key_value(int key, void *value) {
    assert(value != NULL);
    return cls_set(key, value);
}

/* Retrieve the value associated with key in the current thread, or NULL
 * if the current thread doesn't have an association for key.
 */
void *
PyThread_get_key_value(int key) {
    return cls_get(key);
}

/* Forget the current thread's association for key, if any. */
void
PyThread_delete_key_value(int key) {
    cls_del(key);
}

/* Forget everything not associated with the current thread id.
 * This function is called from PyOS_AfterFork().  It is necessary
 * because other thread ids which were in use at the time of the fork
 * may be reused for new threads created in the forked process.
 */
void
PyThread_ReInitTLS(void) {
    coev_fork_notify();
    tuco_dprintf("PyThread_ReInitTLS(): called coev_fork_notify().\n");
}

/* set the thread stack size.
 * Return 0 if size is valid, -1 if size is invalid,
 * -2 if setting stack size is not supported.
 */
static int
_pythread_ucoev_set_stacksize(size_t size) {
    /* set to default */
    if (size == 0) {
        _stacksize =  23 * 4096;
        return 0;
    }

    if (size > SIGSTKSZ)
        return _stacksize = size, 0;
    return -1;

}

#define THREAD_SET_STACKSIZE(x)	_pythread_ucoev_set_stacksize(x)



