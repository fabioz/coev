/* vim:set noet ts=8 sw=8 : */

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

#include <coev.h>
#include <Python.h>

/* mod.local: a type. 

   constructor returns a plain PyObject bound to current coroutine.

*/

struct _coroutine;
typedef struct _coroutine PyCoroutine;

struct _coroutine {
    PyObject_HEAD
    coev_t coev;
    PyObject *weakreflist;
    PyObject *run;
    char *treepos;
    PyCoroutine *parent;
};
#define TLS_ATTR  __thread
static TLS_ATTR PyCoroutine *coro_main;
static TLS_ATTR int debug_flag;

#define COEV2CORO(c) ((PyCoroutine *) ( ((char *)(c)) - offsetof(PyCoroutine, coev) ))
#define CURCORO ((PyCoroutine *) ( ((char *)(coev_current())) - offsetof(PyCoroutine, coev) ))

/* require Python >= 2.5 */
#if (PY_VERSION_HEX < 0x02050000)
#error Python version >= 2.5 is required.
#endif

static PyObject* PyExc_CoroError;
static PyObject* PyExc_CoroExit;
static PyObject* PyExc_CoroWakeUp;
static PyObject* PyExc_CoroTimeout;
static PyObject* PyExc_CoroNoSchedInRoot;

static struct _exc_def {
    PyObject **exc;
    char *name;
    const char *doc;
} _exc_tab[] = {
    {
        &PyExc_CoroError,
        "coev.Error",
        "internal coroutine error"
    },
    {
        &PyExc_CoroWakeUp,
        "coev.WakeUp",
        "voluntary switch into waiting or sleeping coroutine"
    },
    {
        &PyExc_CoroTimeout,
        "coev.Timeout",
        "timeout on wait"
    },
    {
        &PyExc_CoroNoSchedInRoot,
        "coev.NoSchedInRoot",
        "no scheduler while wait/sleep called in root coroutine"
    },
    {
        &PyExc_CoroExit,
        "coev.Exit",
        "coev.Exit\n\
This special exception does not propagate to the parent coroutine; it\n\
can be used to kill a single coroutine.\n"
    },
    { 0,0,0 }
};

static int
_coro_dprintf(const char *fmt, ...) {
    va_list ap;
    int rv;

    va_start(ap, fmt);
    rv = vprintf(fmt, ap);
    va_end(ap);
    return rv;
}

#define coro_dprintf(fmt, args...) do { if (debug_flag) _coro_dprintf(fmt, ## args); } while(0)

static void *
coro_runner(coev_t *coev, void *p) {
    PyCoroutine *self = COEV2CORO(coev);
    PyObject *args = (PyObject *) p;
    PyObject *result;
    PyObject *t, *v, *tb, *r;

    /* we're implicitly referenced by running code */
    Py_INCREF(self); 
    
    coro_dprintf("coro_runner() [%s]: param %p; running.\n", 
        self->treepos, p);
    result = PyEval_CallObject(self->run, args);
    
    coro_dprintf("coro_runner() [%s]: result %p.\n", 
        self->treepos, result);
    
    Py_DECREF(args);
    
    coro_dprintf("coro_runner() [%s]: args released.\n", 
        self->treepos);

    PyErr_Fetch(&t, &v, &tb);
    
    if (t != NULL) {
        if (PyErr_GivenExceptionMatches(t, PyExc_CoroExit)) {
            /* we're being killed */
            coro_dprintf("coro_runner() [%s]: CoroExit raised, returning None.\n", 
                self->treepos);
            Py_DECREF(t);
            Py_XDECREF(v);
            Py_XDECREF(tb);
            Py_RETURN_NONE;
        }
        if (debug_flag) {
            if (v)
                r = PyObject_Repr(v);
            else
                r = PyObject_Repr(t);
            coro_dprintf("coro_runner() [%s]: returned %p, exception: %s\n", 
                self->treepos, result,
                PyString_AsString(r)
            );
            Py_DECREF(r);
        }
        PyErr_Restore(t, v, tb);
        
    } else 
        if (debug_flag) {
            if (result != NULL) {
                PyObject *r;
                r = PyObject_Repr(result);
                coro_dprintf("coro_runner() [%s]: returned %s, no exception.\n", 
                    self->treepos, PyString_AsString(r));
                Py_DECREF(r);
            } else {
                coro_dprintf("coro_runner() [%s]: NULL return w/o exception.\n", 
                    self->treepos);
            }
        }
    
    /* let go of callable and its implicit ref */
    Py_CLEAR(self->run);
    Py_DECREF(self);
    
    return (void *)result;
}

static PyObject * 
coro_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    PyCoroutine *child;
    
    child = PyObject_GC_New(PyCoroutine, type);
    
    if (!child)
        return NULL;
    
    PyCoroutine *current = CURCORO;

    coev_init(&child->coev, &coro_runner);
    child->weakreflist = NULL;
    child->parent = current;
    Py_INCREF(current);
    child->run = Py_None;
    Py_INCREF(Py_None);
    child->treepos = coev_treepos(&child->coev);
    
    PyObject_GC_Track(child);
    return (PyObject *) child;
}

static int coro_setrun(PyCoroutine* self, PyObject* nparent, void* c);
static int coro_setparent(PyCoroutine* self, PyObject* nparent, void* c);

static int 
coro_init(PyObject *po, PyObject *args, PyObject *kwds) {
    PyCoroutine *self = (PyCoroutine *)po;
    PyObject *run = NULL;
    PyObject *nparent = NULL;
    
    static char *kwlist[] = {"run", "parent", 0};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OO:green", kwlist,
                                     &run, &nparent))
        return -1;

    if (run != NULL)
        if (coro_setrun(self, run, NULL))
            return -1;
    
    if (nparent != NULL)
        if (coro_setparent(self, nparent, NULL))
            return -1;
    
    return 0;
}

/** this is called by Python, when refcount 
    on a greenlet object reaches zero 

    At this stage, greenlet must be dead,
    that is, its run() function returned.

*/
static void 
coro_dealloc(PyObject *po) {
    PyCoroutine *self = (PyCoroutine *)po;
    
    coro_dprintf("coro_dealloc(): deallocation of [%s] ensues\n", 
        self->treepos);    
    
    if (!COEV_DEAD(&self->coev))
        Py_FatalError("deallocation of non-dead coroutine object");

    PyObject_GC_UnTrack(self);

    /* clear weak references */
    if (self->weakreflist != NULL)
        PyObject_ClearWeakRefs((PyObject *)self);

    /* let go of parent */
    Py_CLEAR(self->parent);

    /* release treepos string */
    if (self->treepos)
        PyMem_Free(self->treepos);
    
    /* release coev internal resources */
    coev_free(&self->coev);
    
    /* dealloc self */
    PyObject_GC_Del(self);
}

/* GC support: tp_traverse */
static int
coro_traverse(PyObject *po, visitproc visit, void *arg) {
    PyCoroutine *self = (PyCoroutine *)po;
    
    coro_dprintf("coro_traverse in [%s]\n", self->treepos);
    
    Py_VISIT(self->run);
    Py_VISIT(self->parent);
    return 0;
}

/* GC support: tp_clear */
static int 
coro_clear(PyObject *po) {
    PyCoroutine *self = (PyCoroutine *)po;
    
    coro_dprintf("coro_clear() %p run %p\n", self, self->run);
    
    Py_CLEAR(self->run);
    Py_CLEAR(self->parent);
    return 0;
}

/* GC support: determine if this obj is collectable */
static int
coro_is_gc(PyObject *po) {
    PyCoroutine *self = (PyCoroutine *)po;
    if (self == coro_main) {
        coro_dprintf("coro_is_gc(): coro_main is never garbage [refcount = %d].\n",
        po->ob_refcnt);
        return 0;
    }
    if (!COEV_DEAD(&self->coev)) {
        coro_dprintf("coro_is_gc(): [%s] is active, refcount = %d\n",
            self->treepos, po->ob_refcnt);
        return 0;
    }
    coro_dprintf("coro_is_gc(): approving GC on [%s]\n", self->treepos);
    return 1;
}

PyDoc_STRVAR(coro_switch_doc,
"switch(*args)\n\
\n\
Switch execution to this greenlet.\n\
\n\
If this greenlet has never been run, then this greenlet\n\
will be switched to using the body of self.run(*args).\n\
\n\
If the greenlet is active (has been run, but was switch()'ed\n\
out before leaving its run function), then this greenlet will\n\
be resumed and the return value to its switch call will be\n\
None if no arguments are given, the given argument if one\n\
argument is given, or the args tuple if multiple arguments\n\
are given\n\
\n\
If the greenlet is dead, or is the current greenlet then this\n\
function will simply return the args using the same rules as\n\
above.");

static PyObject* 
coro_switch(PyCoroutine *self, PyObject *args) {
    PyObject *result;
    PyObject *swreturn;
    coerv_t rv;
    
    if (args != NULL)
        Py_INCREF(args);
    
    /* switch into this object. */
    coro_dprintf("coro_switch: current [%s] target [%s] args %p\n", 
        CURCORO->treepos,
        self->treepos, 
        args);
    rv = coev_switch(&self->coev, args);
    
    /* switch back occured. */
    coro_dprintf("coro_switch: current [%s] target [%s] switch returned %d/%p\n", 
        CURCORO->treepos,
        self->treepos,
        rv.status, rv.value);
    
    switch (rv.status) {
        case COERV_VOLUNTARY:
            break;
        
        case COERV_SCHEDULER_NEEDED:
            Py_RETURN_NONE;
        
        case COERV_SWITCH_TO_SELF:
            PyErr_SetString(PyExc_CoroError,
		    "switch(): attempt to switch to self");
            return NULL;
        
        case COERV_EVENT:
        case COERV_TIMEOUT:
        case COERV_NOWHERE_TO_SWITCH:
        case COERV_WAIT_IN_SCHEDULER:
        default:
            PyErr_SetString(PyExc_CoroError,
		    "switch(): unexpected switchback type");
        
            return NULL;
    }
        
    
    swreturn = (PyObject *) rv.value;
    
    if ( swreturn != NULL ) {
        if ( PyTuple_Check(swreturn) && PyTuple_GET_SIZE(swreturn) == 1) {
            /* unwrap 1-tuple */
            result = PyTuple_GET_ITEM(swreturn, 0);
            Py_INCREF(result);
            Py_DECREF(swreturn);
            return result;
        } else {
            /* single object already */
            return swreturn;
        }
    } else {
        if (COEV_DEAD(&self->coev)) {
            /* death by exception */
            coro_dprintf("swreturn [%s]: death by exception\n", self->treepos);
            return NULL;
        }

	/* propagate exception into caller */
	coro_dprintf("swreturn [%s]: exception injection\n", self->treepos);
	return NULL;
    }
}

PyDoc_STRVAR(throw_doc,
"throw(typ[,val[,tb]]) -> raise exception in greenlet, return value passed "
"when switching back");
/** this basically switches to a greenlet with p=NULL
    per Python conventions return of NULL from C extension
    function means an exception. 

    so we set up an exception and   
    we call switch with p=NULL

    this NULL winds up in target's coro_switch, which passes it
    to the interpreter, which raises the exception. */
static PyObject* 
coro_throw(PyCoroutine* self, PyObject* args) {
    PyObject *typ = PyExc_CoroExit;
    PyObject *val = NULL;
    PyObject *tb = NULL;
    
    if (!PyArg_ParseTuple(args, "|OOO:throw", &typ, &val, &tb))
        return NULL;

    /*  First, check the traceback argument, 
        replacing None with NULL. */
    if (tb == Py_None)
        tb = NULL;
    else if (tb != NULL && !PyTraceBack_Check(tb)) {
        PyErr_SetString(PyExc_TypeError,
            "throw() third argument must be a traceback object");
        return NULL;
    }

    Py_INCREF(typ);
    Py_XINCREF(val);
    Py_XINCREF(tb);

    if (PyExceptionClass_Check(typ)) {
        PyErr_NormalizeException(&typ, &val, &tb);
    } else if (PyExceptionInstance_Check(typ)) {
        /* Raising an instance.  The value should be a dummy. */
        if (val && val != Py_None) {
            PyErr_SetString(PyExc_TypeError,
              "instance exception may not have a separate value");
            goto failed_throw;
        } else {
            /* Normalize to raise <class>, <instance> */
            Py_XDECREF(val);
            val = typ;
            typ = PyExceptionInstance_Class(typ);
            Py_INCREF(typ);
        }
    } else {
        /* Not something you can raise.  throw() fails. */
        PyErr_Format(PyExc_TypeError,
                     "exceptions must be classes, or instances, not %s",
                     typ->ob_type->tp_name);
        goto failed_throw;
    }

    PyErr_Restore(typ, val, tb);
    
    return coro_switch(self, NULL);

failed_throw:
    /* Didn't use our arguments, so restore their original refcounts */
    Py_DECREF(typ);
    Py_XDECREF(val);
    Py_XDECREF(tb);
    return NULL;
}

static int 
coro_nonzero(PyCoroutine* self) {
    return COEV_ACTIVE(&self->coev);
}

static PyObject *
coro_getdead(PyCoroutine* self, void* c) {
    PyObject* res;
    if (COEV_DEAD(&self->coev))
        res = Py_False;
    else
        res = Py_True;
    Py_INCREF(res);
    return res;
}

static PyObject *
coro_getrun(PyCoroutine* self, void* c) {
    if (COEV_STARTED(&self->coev) || self->run == NULL) {
        PyErr_SetString(PyExc_AttributeError, "run");
        return NULL;
    }
    Py_INCREF(self->run);
    return self->run;
}

static int 
coro_setrun(PyCoroutine* self, PyObject* nrun, void* c) {
    PyObject* o;
    if (COEV_STARTED(&self->coev)) {
        PyErr_SetString(PyExc_AttributeError,
                        "run cannot be set "
                        "after the start of the greenlet");
        return -1;
    }
    
    if (debug_flag) {
        PyObject *r = PyObject_Repr(nrun);
        coro_dprintf("[%s] setrun to %s \n", 
            self->treepos, PyString_AsString(r));
        Py_DECREF(r);
    }

    o = self->run;
    self->run = nrun;
    Py_XINCREF(nrun);
    Py_XDECREF(o);
    return 0;
}

static PyObject* 
coro_getparent(PyCoroutine* self, void* c) {
    PyObject* result = self->parent ? (PyObject*) self->parent : Py_None;
    Py_INCREF(result);
    return result;
}

static PyObject* 
coro_getpos(PyCoroutine* self, void* c) {
    PyObject* result;
    if (self->treepos == NULL) 
        self->treepos = coev_treepos(&self->coev);
    
    result = PyString_FromString(self->treepos);

    Py_INCREF(result);
    return result;
}

static PyTypeObject *PyCoroutine_TypePtr;
static int 
coro_setparent(PyCoroutine* self, PyObject* nparent, void* c) {
    PyCoroutine* p;
    if (nparent == NULL) {
        PyErr_SetString(PyExc_AttributeError, "can't delete attribute");
        return -1;
    }
    if (!PyObject_TypeCheck(nparent, PyCoroutine_TypePtr)) {
        PyErr_SetString(PyExc_TypeError, "parent must be a coroutine");
        return -1;
    }
    for (p=(PyCoroutine*) nparent; p; p=p->parent) {
        if (p == self) {
            PyErr_SetString(PyExc_ValueError, "cyclic parent chain");
            return -1;
        }
    }
    
    if (self->treepos)
        PyMem_Free(self->treepos);
    self->treepos = coev_treepos(&self->coev);
    
    p = self->parent;
    self->parent = (PyCoroutine*) nparent;
    Py_INCREF(nparent);
    Py_DECREF(p);
    return 0;
}

static PyMethodDef coro_methods[] = {
	{"throw",  (PyCFunction) coro_throw,  METH_VARARGS, throw_doc},
	{"switch", (PyCFunction) coro_switch, METH_VARARGS, coro_switch_doc},
	{NULL,     NULL}		/* sentinel */
};

static PyGetSetDef coro_getsets[] = {
	{"run",    (getter) coro_getrun,
		   (setter) coro_setrun, /*XXX*/ NULL},
	{"parent", (getter) coro_getparent,
		   (setter) coro_setparent, /*XXX*/ NULL},
	{"dead",   (getter) coro_getdead,
	             NULL, /*XXX*/ NULL},
	{"posn",   (getter) coro_getpos,
		     NULL, /*XXX*/ NULL},
	{NULL}
};

static PyNumberMethods coro_as_number = {
	NULL,		                /* nb_add */
	NULL,		                /* nb_subtract */
	NULL,		                /* nb_multiply */
	NULL,		                /* nb_divide */
	NULL,		                /* nb_remainder */
	NULL,		                /* nb_divmod */
	NULL,		                /* nb_power */
	NULL,		                /* nb_negative */
	NULL,		                /* nb_positive */
	NULL,		                /* nb_absolute */
	(inquiry) coro_nonzero,        /* nb_nonzero */
};

/* is this needed at all ?
#include "structmember.h"
*/

PyTypeObject PyCoroutine_Type = {
    PyObject_HEAD_INIT(NULL)
    0,					/* ob_size */
    "coev.coroutine",		        /* tp_name */
    sizeof(PyCoroutine),		/* tp_basicsize */
    0,					/* tp_itemsize */
    /* methods */
    coro_dealloc,		        /* tp_dealloc */
    0,					/* tp_print */
    0,					/* tp_getattr */
    0,					/* tp_setattr */
    0,					/* tp_compare */
    0,					/* tp_repr */
    &coro_as_number,			/* tp_as _number*/
    0,					/* tp_as _sequence*/
    0,					/* tp_as _mapping*/
    0, 					/* tp_hash */
    0,					/* tp_call */
    0,					/* tp_str */
    0,					/* tp_getattro */
    0,					/* tp_setattro */
    0,					/* tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC,	/* tp_flags */
    "coroutine(run=None, parent=None)\n"
    "Create a new coroutine object (without running it).  \"run\" is the\n"
    "callable to invoke, and \"parent\" is the parent coroutine, which\n"
    "defaults to the current coroutine.",	/* tp_doc */
    coro_traverse,                      /* tp_traverse */
    coro_clear,	                        /* tp_clear */
    0,					/* tp_richcompare */
    offsetof(PyCoroutine, weakreflist),	/* tp_weaklistoffset */
    0,					/* tp_iter */
    0,					/* tp_iternext */
    coro_methods,			/* tp_methods */
    0,					/* tp_members */
    coro_getsets,			/* tp_getset */
    0,					/* tp_base */
    0,					/* tp_dict */
    0,					/* tp_descr_get */
    0,					/* tp_descr_set */
    0,					/* tp_dictoffset */
    coro_init,		                /* tp_init */
    0,                                  /* tp_alloc */
    coro_new,				/* tp_new */
    PyObject_GC_Del,			/* tp_free */
    coro_is_gc,	                        /* tp_is_gc */
};

/** Module definition */
/* FIXME: wait/sleep can possibly leak reference to passed-in value */
static PyObject *
mod_wait(PyObject* self, PyObject* args) {
    int fd, revents;
    double timeout;
    coerv_t rv;
    
    if (!PyArg_ParseTuple(args, "iid", &fd, &revents, &timeout))
	return NULL;
    coro_dprintf("mod_wait() caller [%s]\n", CURCORO->treepos);
    rv = coev_wait(fd, revents, timeout);
    coro_dprintf("mod_wait() switchback to [%s] rv.status=%d rv.value=%p\n", 
        CURCORO->treepos, rv.status, rv.value);
    switch (rv.status) {
        case COERV_EVENT:
            Py_RETURN_NONE;
        case COERV_VOLUNTARY:
            /* raise volswitch exception */
            PyErr_SetString(PyExc_CoroWakeUp,
		    "voluntary switch into waiting coroutine");        
            return NULL;
        case COERV_TIMEOUT:
            /* raise timeout exception */
            PyErr_SetString(PyExc_CoroTimeout,
		    "IO timeout");        
            return NULL;
        case COERV_NOWHERE_TO_SWITCH:
            /* raise nowhere2switch exception */
            PyErr_SetString(PyExc_CoroNoSchedInRoot,
		    "request to wait for IO in root coroutine without a scheduler");
            return NULL;
        default:
            /* raise timeout exception */
            PyErr_SetString(PyExc_CoroError,
		    "wait(): unknown switchback type");
            return NULL;
        
    }
}

static PyObject *
mod_sleep(PyObject* self, PyObject* args) {
    double timeout;
    coerv_t rv;
    
    if (!PyArg_ParseTuple(args, "d", &timeout))
	return NULL;
    coro_dprintf("mod_sleep() caller [%s]\n", CURCORO->treepos);
    rv = coev_sleep(timeout);
    switch (rv.status) {
        case COERV_EVENT:
            Py_RETURN_NONE;
        case COERV_VOLUNTARY:
            /* raise volswitch exception */
            PyErr_SetString(PyExc_CoroWakeUp,
		    "PyExc_CoroWakeUp");        
            return NULL;
        case COERV_TIMEOUT:
            /* raise timeout exception */
            PyErr_SetString(PyExc_CoroTimeout,
		    "PyExc_CoroTimeout");        
            return NULL;
        case COERV_NOWHERE_TO_SWITCH:
            /* raise nowhere2switch exception */
            PyErr_SetString(PyExc_CoroNoSchedInRoot,
		    "PyExc_CoroNoSchedInRoot");        
            return NULL;
        default:
            /* raise timeout exception */
            PyErr_SetString(PyExc_CoroError,
		    "wait(): unknown switchback type");        
            return NULL;
        
    }
}

static PyObject *
mod_scheduler(PyObject *a, PyObject *b) {
    coro_dprintf("coev.scheduler(): calling coev_loop() (cur=[%s]).\n", 
        CURCORO->treepos);
    coev_loop(0);
    /* this returns iff an ev_unloop() has been called. 
    either with coev_unloop() or in interrupt handler. */
    coro_dprintf("coev.scheduler(): coev_loop() returned (cur=[%s]).\n", 
        CURCORO->treepos);
    /* return NULL iff this module set up an exception. */
    if (PyErr_Occurred() != NULL)
        return NULL;
    Py_RETURN_NONE;
}

static PyObject* 
mod_current(PyObject *a, PyObject *b) {
    PyObject *current = (PyObject *) CURCORO;
    Py_INCREF(current);
    return current; 
}

static PyObject *
mod_stats(PyObject *a, PyObject *b) {
    uint64_t sw, wa, sl, bc;
    
    coev_getstats(&sw, &wa, &sl, &bc);
    return Py_BuildValue("KKKK", sw, wa, sl, bc);
}

static PyObject *
mod_setdebug(PyObject *a, PyObject *args) {
    int de, du;
    if (!PyArg_ParseTuple(args, "ii", &de, &du))
	return NULL;
    coev_setdebug(de, du);
    debug_flag = de;
    Py_RETURN_NONE;   
}

static PyMethodDef CoevMethods[] = {
    {   "current", 
        mod_current, 
        METH_NOARGS, 
        "coev.current()\n"
        "Returns the current coroutine (i.e. the one which called this\n"
        "function)."
    },
    {   "scheduler", 
        mod_scheduler, 
        METH_NOARGS,
        "coev.scheduler()\n"
        "Switches to coroutines which called self.wait() "
        "and have pending IO events"
    },
    {   "wait", 
        mod_wait, 
        METH_VARARGS,
        "coev.wait(fd, revents, timeout) "
        "Sets up a wait for io at fd with switchback to current coro "
        "and switches to the scheduler if it is running"
    },
    {   "sleep", 
        mod_sleep, 
        METH_VARARGS,
        "coev.sleep(amount) "
        "Sets up a sleep with switchback to current coro "
        "and switches to the scheduler if it is running"
    },
    {   "stats", 
        mod_stats, 
        METH_NOARGS, 
        "coev.stats() "
        "Returns 3-tuple of (switches, bytes copied, waits) "
        "counters"
    },
    {   "setdebug", 
        mod_setdebug, 
        METH_VARARGS, 
        "coev.setdebug(debug, dump) "
        "debug: print debug; "
        "dump: dump coev_t structures too."
    },
    {NULL, NULL}	/* Sentinel */
};

/* coev framework */

static void *
python_augmented_malloc(size_t size) {
    void *rv;
    
    rv = PyMem_Malloc(size);
    if (rv == NULL)
	PyErr_NoMemory();
    return rv;
    
}

static void *
python_augmented_realloc(void *ptr, size_t size) {
    void *rv;
    
    rv = PyMem_Realloc(ptr, size);
    if (rv == NULL)
	PyErr_NoMemory();
    return rv;
}

static void
python_augmented_free(void *ptr) {
    PyMem_Free(ptr);
}

static void 
set_xthread_exc(coev_t *a, coev_t *b, void *p) {
    PyErr_SetString(PyExc_CoroError,
		    "cannot switch to a different thread");
    Py_XDECREF((PyObject *)p);
}

static void
set_s2self_exc(coev_t *a, void *p) {
    PyErr_Format(PyExc_CoroError,
        "cannot switch to itself ([%s])",
        COEV2CORO(a)->treepos );
    Py_XDECREF((PyObject *)p);
}

static void
python_augmented_inthdlr(void) {
    coev_unloop();
    PyErr_SetNone(PyExc_KeyboardInterrupt);
}

coev_frameth_t _cmf = {
    python_augmented_malloc,    /* malloc */ 
    python_augmented_realloc,   /* realloc */
    python_augmented_free,      /* free */
    set_xthread_exc,            /* crossthread_fail */
    set_s2self_exc,             /* switch_to_itself fail */
    NULL,                       /* death - not used. */
    Py_FatalError,              /* abort */
    python_augmented_inthdlr,   /* unloop at SIGINT */
    /* debug */
    NULL,                       /* switch_notify */
    _coro_dprintf,              /* debug sink */
    0                           /* dump coev_t */
};

void 
initcoev(void) {
    PyObject* m;

    m = Py_InitModule("coev", CoevMethods);
    if (m == NULL)
        return;
    
    if (PyModule_AddStringConstant(m, "__version__", "0.2") < 0)
        return;

    if (PyModule_AddIntConstant(m, "READ", 1) < 0)
        return;
    
    if (PyModule_AddIntConstant(m, "WRITE", 2) < 0)
        return;
    
    if (PyType_Ready(&PyCoroutine_Type) < 0)
        return;

    { /* add exceptions */
        PyObject* exc_dict;
        PyObject* exc_doc;
        int i, e;
        
        for (i = 0; _exc_tab[i].exc; i++) {
            if (!(exc_dict = PyDict_New()))
                return;
            if (!(exc_doc = PyString_FromString(_exc_tab[i].doc))) {
                Py_DECREF(exc_dict);
                return;
            }
            e = PyDict_SetItemString(exc_dict, "__doc__", exc_doc);
            Py_DECREF(exc_doc);
            if (e == -1) {
                Py_DECREF(exc_dict);
                return;
            }
            *(_exc_tab[i].exc) = PyErr_NewException(_exc_tab[i].name, NULL, exc_dict);
            Py_DECREF(exc_dict);
            if (*(_exc_tab[i].exc) == NULL)
                return;
        }
    }

    PyCoroutine_TypePtr = &PyCoroutine_Type;
    Py_INCREF(&PyCoroutine_Type);
    PyModule_AddObject(m, "coroutine", (PyObject*) &PyCoroutine_Type);
    Py_INCREF(PyExc_CoroError);
    PyModule_AddObject(m, "Error", PyExc_CoroError);
    Py_INCREF(PyExc_CoroExit);
    PyModule_AddObject(m, "Exit", PyExc_CoroExit);

    Py_INCREF(PyExc_CoroWakeUp);
    PyModule_AddObject(m, "WakeUp", PyExc_CoroWakeUp);
    Py_INCREF(PyExc_CoroTimeout);
    PyModule_AddObject(m, "Timeout", PyExc_CoroTimeout);
    Py_INCREF(PyExc_CoroNoSchedInRoot);
    PyModule_AddObject(m, "NoSchedInRoot", PyExc_CoroNoSchedInRoot);
    
    {
        /* initialize coev library */
        coro_main = PyObject_GC_New(PyCoroutine, &PyCoroutine_Type);
        coev_initialize(&_cmf, &coro_main->coev);
        coro_main->treepos = coev_treepos(&coro_main->coev);
    }
    
}
