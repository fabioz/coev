#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "structmember.h"

#include <sys/types.h>
#include <sys/socket.h>

#include "coev.h"


/* mod.local: a type. 

   constructor returns a plain PyObject bound to current coroutine.

*/

struct _coroutine;
typedef struct _coroutine PyCoroutine;

struct _coroutine {
    PyObject_HEAD
    coev_t coev;
    PyObject *run;
    PyObject *cldata;
    char *treepos;
    PyCoroutine *parent;
    int switch_veto;
};

#define TLS_ATTR  __thread

static TLS_ATTR
struct _coroutine_stats {
    uint64_t created;
    uint64_t destroyed;
} module_stats;

static TLS_ATTR PyCoroutine *coro_main;
static TLS_ATTR int debug_flag;

#define COEV2CORO(c) ((PyCoroutine *) ( ((char *)(c)) - offsetof(PyCoroutine, coev) ))
#define CURCORO ((PyCoroutine *) ( ((char *)(coev_current())) - offsetof(PyCoroutine, coev) ))

/* require Python >= 2.5 */
#if (PY_VERSION_HEX < 0x02050000)
#error Python version >= 2.5 is required.
#endif

/* define some 2.6 stuff that is missing from 2.5 */
#ifndef Py_TYPE
#define Py_TYPE(ob)                (((PyObject*)(ob))->ob_type)
#endif
#ifndef PyVarObject_HEAD_INIT
#define PyVarObject_HEAD_INIT(type, size) PyObject_HEAD_INIT(type) size,
#endif

static PyObject* PyExc_CoroError;
static PyObject* PyExc_CoroExit;
static PyObject* PyExc_CoroWakeUp;
static PyObject* PyExc_CoroTimeout;
static PyObject* PyExc_CoroNoSchedInRoot;
static PyObject* PyExc_CoroSwitchDenied;
static PyObject* PyExc_CoroSocketError;

static struct _exc_def {
    PyObject **exc;
    PyObject **parent;
    char *name;
    char *shortname;
    const char *doc;
} _exc_tab[] = {
    {
        &PyExc_CoroError, &PyExc_Exception,
        "coev.Error", "Error",
        "unspecified coroutine error"
    },
    {
        &PyExc_CoroWakeUp, &PyExc_CoroError,
        "coev.WakeUp", "WakeUp",
        "voluntary switch into waiting or sleeping coroutine"
    },
    {
        &PyExc_CoroTimeout, &PyExc_CoroError,
        "coev.Timeout", "Timeout",
        "timeout on wait"
    },
    {
        &PyExc_CoroNoSchedInRoot, &PyExc_CoroError,
        "coev.NoSchedInRoot", "NoSchedInRoot",
        "no scheduler while wait/sleep called in root coroutine"
    },
    {
        &PyExc_CoroExit, &PyExc_CoroError,
        "coev.Exit", "Exit",
        "coev.Exit\n\
This special exception does not propagate to the parent coroutine; it\n\
can be used to kill a single coroutine.\n"
    },
    {
        &PyExc_CoroSwitchDenied, &PyExc_CoroError,
        "coev.SwitchDenied", "SwitchDenied",
        "Switch (wait, sleep) is denied. Please remember that no switches\n"
        "are allowed in constructors of classes derived from coev.local\n"
    },
    {
        &PyExc_CoroSocketError, &PyExc_CoroError,
        "coev.SocketError", "SocketError",
        "ask Captain Obvious\n"
    },
    { 0 }
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
    PyCoroutine *child, *current;
    
    child = (PyCoroutine *)type->tp_alloc(type, 0);
    
    if (!child)
        return NULL;
    
    current = CURCORO;

    coev_init(&child->coev, &coro_runner);
    child->parent = current;
    Py_INCREF(current);
    child->run = Py_None;
    Py_INCREF(Py_None);
    child->treepos = coev_treepos(&child->coev);
    child->switch_veto = 0;
    module_stats.created += 1;
    
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
    on a coroutine object reaches zero 

    At this stage, coroutine must be dead,
    that is, its run() function returned.

*/
static void 
coro_dealloc(PyObject *po) {
    PyCoroutine *self = (PyCoroutine *)po;
    
    coro_dprintf("coro_dealloc(): deallocation of [%s] ensues\n", 
        self->treepos);    
    
    if (!COEV_DEAD(&self->coev))
        Py_FatalError("deallocation of non-dead coroutine object");

    /* let go of referenced stuff */
    po->ob_type->tp_clear(po);

    /* release treepos string */
    if (self->treepos)
        PyMem_Free(self->treepos);
    
    /* release coev internal resources */
    coev_free(&self->coev);
    
    /* dealloc self */
    po->ob_type->tp_free(po);
    
    module_stats.destroyed += 1;
}

/* GC support: tp_traverse */
static int
coro_traverse(PyObject *po, visitproc visit, void *arg) {
    PyCoroutine *self = (PyCoroutine *)po;
    
    coro_dprintf("coro_traverse in [%s]\n", self->treepos);
    
    if(self->cldata)
        Py_VISIT(self->cldata);
    Py_VISIT(self->run);
    Py_VISIT(self->parent);
    return 0;
}

/* GC support: tp_clear */
static int 
coro_clear(PyObject *po) {
    PyCoroutine *self = (PyCoroutine *)po;
    
    coro_dprintf("coro_clear() %p run %p\n", self, self->run);
    
    Py_CLEAR(self->cldata);
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
Switch execution to this coroutine.\n\
\n\
If this coroutine has never been run, then this coroutine\n\
will be switched to using the body of self.run(*args).\n\
\n\
If the coroutine is active (has been run, but was switch()'ed\n\
out before leaving its run function), then this coroutine will\n\
be resumed and the return value to its switch call will be\n\
None if no arguments are given, the given argument if one\n\
argument is given, or the args tuple if multiple arguments\n\
are given\n\
\n\
If the coroutine is dead, or is the current coroutine then this\n\
function will simply return the args using the same rules as\n\
above.");

static PyObject* 
coro_switch(PyCoroutine *self, PyObject *args) {
    PyObject *result;
    PyObject *swreturn;
    coerv_t rv;
    
    if (CURCORO->switch_veto) {
        PyErr_SetString(PyExc_CoroSwitchDenied, "switch(): prohibited.");
        return NULL;
    }
    
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
"throw(typ[,val[,tb]]) -> raise exception in coroutine, return value passed "
"when switching back");
/** this basically switches to a coroutine with p=NULL
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
    
    if (CURCORO->switch_veto) {
        PyErr_SetString(PyExc_CoroSwitchDenied, "coro_throw(): switch prohibited.");
        return NULL;
    }    
    
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
                        "after the start of the coroutine");
        return -1;
    }
    
    if (debug_flag) {
        PyObject *r = PyObject_Repr(nrun);
        coro_dprintf("coro_setrun(): [%s] to %s \n", 
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
    { 0 }
};

static PyGetSetDef coro_getsets[] = {
    {"run",     (getter) coro_getrun,
                (setter) coro_setrun },
    {"parent",  (getter) coro_getparent,
                (setter) coro_setparent },
    {"dead",    (getter) coro_getdead },
    {"posn",    (getter) coro_getpos },
    { 0 }
};

static PyNumberMethods coro_as_number = {
    NULL,                       /* nb_add */
    NULL,                       /* nb_subtract */
    NULL,                       /* nb_multiply */
    NULL,                       /* nb_divide */
    NULL,                       /* nb_remainder */
    NULL,                       /* nb_divmod */
    NULL,                       /* nb_power */
    NULL,                       /* nb_negative */
    NULL,                       /* nb_positive */
    NULL,                       /* nb_absolute */
    (inquiry) coro_nonzero,     /* nb_nonzero */
};

PyDoc_STRVAR(coroutine_doc,
"coroutine([run=None, [parent=None]]) -> coroutine object\n\n\
Create a new coroutine object (without running it). \n\
run -- the callable to invoke \n\
parent -- the parent coroutine, defaults to the current coroutine.");

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
    coroutine_doc,                      /* tp_doc */
    coro_traverse,                      /* tp_traverse */
    coro_clear,	                        /* tp_clear */
    0,					/* tp_richcompare */
    0,	                                /* tp_weaklistoffset */
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
    0,          			/* tp_free */
    coro_is_gc,	                        /* tp_is_gc */
};


/** coroutine-local data interface: threading.local analog:

coroutine.local type. 

its instance's __dict__ is bound to the coroutine object that created it. 

cldata: { id(coroutine.local instance): its __dict__ }

coroutine inheritance:
  When an existing coroutine.local object is accessed in a coroutine,
  the tree is traversed to root to find the coroutine whose cldata contains 
  the __dict__ for the object. Iff it is not found, new __dict__ is attached
  to the current coroutine.
  
  When a new coroutine.local object is constructed, it is attached to the current
  coroutine.
   
  Constructors of coroutine.local subclasses are prohibited from performing switches.
  Performing a switch() causes Py_FatalError(). 

*/

typedef struct {
    PyObject_HEAD
    PyObject *key;
    PyObject *args;
    PyObject *kw;
    PyObject *dict;
} CoroLocalData;

PyDoc_STRVAR(corolocal_doc,
"local() -> None\n\n\
Coroutine-local data.\n\n\
Closely modelled on (and implementation morphed from) standard library's\n\
threading._local class.\n\n\
Notice: while subclassing is allowed, no coroutine switches are allowed\n\
in subclasses' constructor. Attempt to switch will result in SwitchDenied\n\
exception.");

static PyObject *
corolocal_new(PyTypeObject *type, PyObject *args, PyObject *kw) {
    CoroLocalData *self;

    if (type->tp_init == PyBaseObject_Type.tp_init
        && ((args && PyObject_IsTrue(args))
        || (kw && PyObject_IsTrue(kw)))) {
        PyErr_SetString(PyExc_TypeError, 
                        "Initialization arguments are not supported");
        return NULL;
    }

    self = (CoroLocalData *)type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;
    
    Py_XINCREF(args);
    self->args = args;
    Py_XINCREF(kw);
    self->kw = kw;
    self->dict = NULL;	/* making sure */
    self->key = PyString_FromFormat("coroutine.local.%p", self);
    if (self->key == NULL) 
        goto err;

    self->dict = PyDict_New();
    if (self->dict == NULL)
        goto err;

    if (CURCORO->cldata == NULL) { /* lazy allocation */
        CURCORO->cldata = PyDict_New();
        if (CURCORO->cldata == NULL)
            goto err;
    }

    if (PyDict_SetItem(CURCORO->cldata, self->key, self->dict) < 0)
        goto err;

    return (PyObject *)self;

err:
    Py_DECREF(self);
    return NULL;
}

static int
corolocal_traverse(CoroLocalData *self, visitproc visit, void *arg) {
    Py_VISIT(self->args);
    Py_VISIT(self->kw);
    Py_VISIT(self->dict);
    return 0;
}

static int 
corolocal_clear(CoroLocalData *self) {
    Py_CLEAR(self->key);
    Py_CLEAR(self->args);
    Py_CLEAR(self->kw);
    Py_CLEAR(self->dict);
    return 0;
}

static void
corolocal_dealloc(CoroLocalData *self) {
    /* WTF do here?? */
    /* threading.local code visits every threadlocal dict and 
       deletes the reference to the self.

       we need some global list of references to cldata dicts.
       this may be either global flat list of PyCoroutine objects
       in existence, or of cldata members that have been initialized.

       this is not workable, because dying coroutines will kill
       associated dicts, thus making those references stale.
    
       as primary mode of operation for coroutines is spawn often, die often,
       we leave our data to be taken care of by coroutine destructors. For now.
    */

    corolocal_clear(self);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *
find_cldict(CoroLocalData *self) {
    PyObject *cldict;
    
    if (CURCORO->cldata == NULL) {
        /* lazily initialize coroutine's cldict */
        coro_dprintf("find_cldict(): CLD[%p] attaches new cldata dict to [%s]", self, CURCORO->treepos);
        CURCORO->cldata = PyDict_New(); 
        cldict = NULL;
    } else {
        cldict = PyDict_GetItem(CURCORO->cldata, self->key);
    }

    if (cldict == NULL) {
        cldict = PyDict_New(); /* we own ldict */

        if (cldict == NULL) {
            return NULL;
        } else {
            int i = PyDict_SetItem(CURCORO->cldata, self->key, cldict);
            Py_DECREF(cldict); /* now ldict is borrowed */
            if (i < 0) 
                return NULL;
        }

        Py_CLEAR(self->dict);
        Py_INCREF(cldict);
        self->dict = cldict; /* still borrowed */

        CURCORO->switch_veto = 1;
        if (Py_TYPE(self)->tp_init != PyBaseObject_Type.tp_init &&
            Py_TYPE(self)->tp_init((PyObject*)self, self->args, self->kw) < 0) {
            /* we need to get rid of cldict from coroutine so
               we create a new one the next time we do an attr
               access */ /* WTF is this ?? */
                PyDict_DelItem(CURCORO->cldata, self->key);
                cldict = NULL;
        }
    }
    CURCORO->switch_veto = 0;
    return cldict;
}
    

static int
corolocal_setattro(CoroLocalData *self, PyObject *name, PyObject *v) {
    PyObject *cldict;

    cldict = find_cldict(self);
    if (cldict == NULL) 
        return -1;

    return PyObject_GenericSetAttr((PyObject *)self, name, v);
}

static PyObject *
corolocal_getdict(CoroLocalData *self, void *closure) {
    if (self->dict == NULL) {
        PyErr_SetString(PyExc_AttributeError, "__dict__");
        return NULL;
    }

    Py_INCREF(self->dict);
    return self->dict;
}

static PyGetSetDef corolocal_getset[] = {
    {   "__dict__", 
        (getter) corolocal_getdict, 
        (setter) NULL,
        "local-data dictionary", 
        NULL 
    },
    { 0 }  
};

static PyObject *corolocal_getattro(CoroLocalData *, PyObject *);

static PyTypeObject CoroLocalData_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    /* tp_name           */ "coev.local",
    /* tp_basicsize      */ sizeof(CoroLocalData),
    /* tp_itemsize       */ 0,
    /* tp_dealloc        */ (destructor)corolocal_dealloc,
    /* tp_print          */ 0,
    /* tp_getattr        */ 0,
    /* tp_setattr        */ 0,
    /* tp_compare        */ 0,
    /* tp_repr           */ 0,
    /* tp_as_number      */ 0,
    /* tp_as_sequence    */ 0,
    /* tp_as_mapping     */ 0,
    /* tp_hash           */ 0,
    /* tp_call           */ 0,
    /* tp_str            */ 0,
    /* tp_getattro       */ (getattrofunc)corolocal_getattro,
    /* tp_setattro       */ (setattrofunc)corolocal_setattro,
    /* tp_as_buffer      */ 0,
    /* tp_flags          */ Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    /* tp_doc            */ corolocal_doc,
    /* tp_traverse       */ (traverseproc)corolocal_traverse,
    /* tp_clear          */ (inquiry)corolocal_clear,
    /* tp_richcompare    */ 0,
    /* tp_weaklistoffset */ 0,
    /* tp_iter           */ 0,
    /* tp_iternext       */ 0,
    /* tp_methods        */ 0,
    /* tp_members        */ 0,
    /* tp_getset         */ corolocal_getset,
    /* tp_base           */ 0,
    /* tp_dict           */ 0,
    /* tp_descr_get      */ 0,
    /* tp_descr_set      */ 0,
    /* tp_dictoffset     */ offsetof(CoroLocalData, dict),
    /* tp_init           */ 0,
    /* tp_alloc          */ 0,
    /* tp_new            */ corolocal_new
};

static PyObject *
corolocal_getattro(CoroLocalData *self, PyObject *name)
{
    PyObject *cldict, *value;

    cldict = find_cldict(self);
    if (cldict == NULL) 
        return NULL;

    if (Py_TYPE(self) != &CoroLocalData_Type)
        /* use generic lookup for subtypes */
        return PyObject_GenericGetAttr((PyObject *)self, name);

    /* Optimization: just look in dict ourselves */
    value = PyDict_GetItem(cldict, name);
    if (value == NULL) 
        /* Fall back on generic to get __class__ and __dict__ */
        return PyObject_GenericGetAttr((PyObject *)self, name);

    Py_INCREF(value);
    return value;
}

/** coev.socketfile - file-like interface to a network socket */

typedef struct {
    PyObject_HEAD
    int fd;
    char *in_buffer, *in_position;
    Py_ssize_t in_allocated, in_used, in_limit;
    double iop_timeout;
} CoroSocketFile;

PyDoc_STRVAR(socketfile_doc,
"socketfile(fd, timeout, [rlim]) -> socketfile object\n\n\
Coroutine-aware file-like interface to network sockets.\n\n\
fd -- integer fd to wrap around.\n\
timeout -- float timeout per IO operation.\n\
rlim -- read buffer size limit (default 4K).\n\
");

static PyObject *
socketfile_new(PyTypeObject *type, PyObject *args, PyObject *kw) {
    CoroSocketFile *self;
    static char *kwds[] = {  "fd", "timeout", "rlim", NULL };

    self = (CoroSocketFile *)type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;
    
    self->in_buffer = NULL;
    self->in_allocated = 4096;
    self->in_limit = 4096;
    
    if (!PyArg_ParseTupleAndKeywords(args, kw, "nd|n", kwds,
	    &self->fd, &self->iop_timeout, &self->in_limit)) {
	Py_DECREF(self);
	return NULL;
    }
    if (self->in_limit == 0) {
	PyErr_SetString(PyExc_ValueError, "Read buffer limit must be positive");
	Py_DECREF(self);
	return NULL;
    }
    self->in_buffer = PyMem_Malloc(self->in_allocated);
    
    if (!self->in_buffer) {
	Py_DECREF(self);
	return PyErr_NoMemory();
    }
    self->in_position = self->in_buffer;
    
    return (PyObject *)self;
}

static void
socketfile_dealloc(CoroSocketFile *self) {
    if (self->in_buffer)
	PyMem_Free(self->in_buffer);

    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *
mod_wait_bottom_half(int fd, int revents, ev_tstamp timeout);

#define INBUF_MAGIC (1<<12)
/** makes some space at the end of the read buffer 
by either moving occupied space or growing it by reallocating 
*/
static int
sf_reshuffle_buffer(CoroSocketFile *self, Py_ssize_t needed) {
    Py_ssize_t top_free, total_free;
    
    top_free = self->in_position - self->in_buffer;
    total_free = self->in_allocated - self->in_used;
    
    if (total_free - top_free > needed) 
	return 0;
    
    if (needed > total_free - 2 * INBUF_MAGIC) {
	/* reallocation imminent */
	Py_ssize_t newsize = INBUF_MAGIC + ((needed - total_free) & (INBUF_MAGIC - 1));
	char *newbuf = PyMem_Realloc(self->in_buffer, newsize);
	if (!newbuf) {
	/* memmove imminent */
	    newbuf = PyMem_Malloc(newsize);
	    if (!newbuf)
		return -1; /* no memory */
	    memmove(newbuf, self->in_position, self->in_used);
	    PyMem_Free(self->in_buffer);
	    self->in_position = self->in_buffer = newbuf;
	    self->in_allocated = newsize;
	    if (newsize > self->in_limit)
		self->in_limit = newsize;
	    return 0;
	}
    }
    /* we're still have 2*INBUF_MAGIC bytes more than needed */
    
    memmove(self->in_buffer, self->in_position, self->in_used);
    self->in_position = self->in_buffer;
    
    return 0;
}


PyDoc_STRVAR(socketfile_read_doc,
"read(size) -> bytestr\n\n\
Read at most size bytes or until EOF is reached.\n\
size -- size hint.\n\
");
static PyObject * 
socketfile_read(CoroSocketFile *self, PyObject* args) {
    Py_ssize_t len = 0, readen;
    PyObject *rv;
    
    if (!PyArg_ParseTuple(args, "n", &len))
	return NULL;

    if (len == 0) 
	return PyErr_SetString(PyExc_ValueError, "Read size must be positive"), NULL;
    
    /* check if we have enough contigous space in buffer */
    if (sf_reshuffle_buffer(self, len)) 
	return PyErr_NoMemory();
    
    do {
	/* check if we have enough data in buffer */
	if ( self->in_used >= len ) {
	    rv = PyString_FromStringAndSize(self->in_position, len);
	    self->in_used -= len;
	    self->in_position += len;
	    return rv;
	}
	
	readen = recv(self->fd, self->in_position + self->in_used, 
	              len - self->in_used, 0);
	if (readen == -1) {
	    if (errno == EAGAIN) {
		rv = mod_wait_bottom_half(self->fd, COEV_READ, self->iop_timeout);
		if (!rv)
		    return rv;
		Py_DECREF(rv);
		continue;
	    }
	    return PyErr_SetFromErrno(PyExc_CoroSocketError);
	}
	if (readen == 0)
	    len = self->in_used; /* return whatever we managed to read. */
	else
	    self->in_used += readen;
    } while (1);
}

static PyObject *
sf_extract_line(CoroSocketFile *self, const char *startfrom, ssize_t limit) {
    char *culprit;
    char *buffer_end;
    ssize_t len;
    PyObject *the_line;
    
    buffer_end = self->in_buffer + self->in_allocated;
    len = buffer_end - startfrom;
    
    culprit = memchr(startfrom, '\n', len);
    if ( (!culprit) && (self->in_used < limit) )
	return NULL; /* need more data */
    
    len = culprit ? culprit - self->in_position + 1 : limit;
    the_line = PyString_FromStringAndSize(self->in_position, len);
    
    self->in_used -= len;
    
    if (!self->in_used)
	self->in_position = self->in_buffer;
    else
	self->in_position += len;
    
    return the_line;
}

PyDoc_STRVAR(socketfile_readline_doc,
"readline(size) -> str\n\n\
Read at most size bytes or until LF or EOF are reached.\n\
Notice: if supplied size is larger than buffer growth limit,\n\
the latter is adjusted upwards.\n\
");
static PyObject* 
socketfile_readline(CoroSocketFile *self, PyObject* args) {
    Py_ssize_t limit = 0, readen;   
    PyObject *rv;
    char *old_position;
    
    if (!PyArg_ParseTuple(args, "n", &limit ))
	return NULL;
    
    if (!limit)
	return PyErr_SetString(PyExc_ValueError, "Line size limit must be positive"), NULL;
    
    if (limit > self->in_limit)
	self->in_limit = limit;
    
    /* look if we can return w/o syscalls */
    if ( (rv = sf_extract_line(self, self->in_position, limit)) )
	return rv;
    
    /* make space */
    if (sf_reshuffle_buffer(self, limit))
	return PyErr_NoMemory();
    
    do {
	readen = recv(self->fd, self->in_position + self->in_used, 
		      limit - self->in_used, 0);
	if (readen == 0) { 
	    /* no more data : return whatever there is */
	    rv = PyString_FromStringAndSize(self->in_position, self->in_used);
	    self->in_used = 0;
	    self->in_position = self->in_buffer;
	    return rv;
	}
	if (readen == -1) {
	    if (errno == EAGAIN) {
		rv = mod_wait_bottom_half(self->fd, COEV_READ, self->iop_timeout);
		if (!rv)
		    return rv;
		Py_DECREF(rv);
		continue;
	    }
	    return PyErr_SetFromErrno(PyExc_CoroSocketError);
	}
	    
	old_position = self->in_position + self->in_used;
	self->in_used += readen;
	if ( (rv = sf_extract_line(self, old_position, limit)) )
	    return rv;

    } while(1);
}

PyDoc_STRVAR(socketfile_write_doc,
"write(str) -> None\n\n\
Write the string to the fd. EPIPE results in an exception.\n\
");
static PyObject * 
socketfile_write(CoroSocketFile *self, PyObject* args) {
    PyObject *rv;
    const char *str;
    Py_ssize_t len, wrote, written, to_write;
    
    if (!PyArg_ParseTuple(args, "s#", &str, &len))
	return NULL;

    written = 0;
    do {
	to_write = len - written;
	wrote = send(self->fd, str + written, to_write, MSG_NOSIGNAL);
	if (wrote == -1) {
	    if (errno == EAGAIN) {
		rv = mod_wait_bottom_half(self->fd, COEV_WRITE, self->iop_timeout);
		if (!rv)
		    return rv;
		Py_DECREF(rv);
		continue;
	    }
	    return PyErr_SetFromErrno(PyExc_CoroSocketError);
	}
	written += wrote;

    } while (to_write);
    
    Py_RETURN_NONE;
}

static PyMethodDef socketfile_methods[] = {
    {"read",  (PyCFunction) socketfile_read,  METH_VARARGS, socketfile_read_doc},
    {"readline", (PyCFunction) socketfile_readline, METH_VARARGS, socketfile_readline_doc},
    {"write", (PyCFunction) socketfile_write, METH_VARARGS, socketfile_write_doc},
    { 0 }
};

static PyTypeObject CoroSocketFile_Type = {
    PyObject_HEAD_INIT(NULL)
    /* ob_size           */ 0,
    /* tp_name           */ "coev.socketfile",
    /* tp_basicsize      */ sizeof(CoroSocketFile),
    /* tp_itemsize       */ 0,
    /* tp_dealloc        */ (destructor)socketfile_dealloc,
    /* tp_print          */ 0,
    /* tp_getattr        */ 0,
    /* tp_setattr        */ 0,
    /* tp_compare        */ 0,
    /* tp_repr           */ 0,
    /* tp_as_number      */ 0,
    /* tp_as_sequence    */ 0,
    /* tp_as_mapping     */ 0,
    /* tp_hash           */ 0,
    /* tp_call           */ 0,
    /* tp_str            */ 0,
    /* tp_getattro       */ 0,
    /* tp_setattro       */ 0,
    /* tp_as_buffer      */ 0,
    /* tp_flags          */ Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    /* tp_doc            */ socketfile_doc,
    /* tp_traverse       */ 0,
    /* tp_clear          */ 0,
    /* tp_richcompare    */ 0,
    /* tp_weaklistoffset */ 0,
    /* tp_iter           */ 0,
    /* tp_iternext       */ 0,
    /* tp_methods        */ socketfile_methods,
    /* tp_members        */ 0,
    /* tp_getset         */ 0,
    /* tp_base           */ 0,
    /* tp_dict           */ 0,
    /* tp_descr_get      */ 0,
    /* tp_descr_set      */ 0,
    /* tp_dictoffset     */ 0,
    /* tp_init           */ 0,
    /* tp_alloc          */ 0,
    /* tp_new            */ socketfile_new
};

/** Module definition */
/* FIXME: wait/sleep can possibly leak reference to passed-in value */
/* FIXME: remember WTH I was thinking when I wrote the above */

PyDoc_STRVAR(mod_wait_doc,
"wait(fd, events, timeout) -> None\n\n\
Switch to scheduler until requested IO event or timeout happens.\n\
fd -- file descriptor (integer).\n\
revents -- bitmask.\n\
timeout -- in seconds.");

static PyObject *
mod_wait(PyObject *a, PyObject* args) {
    int fd, revents;
    double timeout;
    
    if (CURCORO->switch_veto) {
        PyErr_SetString(PyExc_CoroSwitchDenied, "mod_wait(): switch prohibited.");
        return NULL;
    }
    
    if (!PyArg_ParseTuple(args, "iid", &fd, &revents, &timeout))
	return NULL;
    
    return mod_wait_bottom_half(fd, revents, timeout);
}

static PyObject *
mod_wait_bottom_half(int fd, int revents, ev_tstamp timeout) {
    coerv_t rv;
    
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

PyDoc_STRVAR(mod_sleep_doc,
"sleep(amount) -> None\n\n\
Switch to scheduler until at least amount seconds pass.\n\
amount -- number of seconds to sleep.");

static PyObject *
mod_sleep(PyObject *a, PyObject *args) {
    double timeout;
    coerv_t rv;
    
    if (CURCORO->switch_veto) {
        PyErr_SetString(PyExc_CoroSwitchDenied, "mod_wait(): switch prohibited.");
        return NULL;
    }    
    
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

PyDoc_STRVAR(mod_scheduler_doc,
"scheduler() -> None\n\n\
Run scheduler: dispatch pending IO or timer events");

static PyObject *
mod_scheduler(PyObject *a) {
    
    if (CURCORO->switch_veto) {
        PyErr_SetString(PyExc_CoroSwitchDenied, "mod_wait(): switch prohibited.");
        return NULL;
    }    
    
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

PyDoc_STRVAR(mod_current_doc,
"current() -> coroutine\n\n\
Return currently executing coroutine object");

static PyObject* 
mod_current(PyObject *a) {
    PyObject *current = (PyObject *) CURCORO;
    Py_INCREF(current);
    return current; 
}

PyDoc_STRVAR(mod_stats_doc,
"stats() -> {...}\n\n\
Returns a dict of various counters");

static PyObject *
mod_stats(PyObject *a) {
    uint64_t sw, wa, sl, bc;
    
    coev_getstats(&sw, &wa, &sl, &bc);
    return Py_BuildValue("{s:K,s:K,s:K,s:K,s:K,s:K}", 
        "switches",     sw, 
        "waits",        wa, 
        "sleeps",       sl, 
        "bytes_copied", bc, 
        "created",      module_stats.created, 
        "destroyed",    module_stats.destroyed );
}

PyDoc_STRVAR(mod_setdebug_doc,
"setdebug([module=False, [library=False, [switchdump=False]]]) -> \n\n\
module -- enable module-level debug output.\n\
library -- enable library-level debug output \n\
switchdump -- dump coev_t structures too.\n");

static PyObject *
mod_setdebug(PyObject *a, PyObject *args, PyObject *kwargs) {
    static char *kwds[] = { "module", "library", "switchdump", NULL };
    int module = 0;
    int library = 0;
    int do_dump = 0;
    
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, 
            "|iii:setdebug", kwds, &module, &library, &do_dump))    
	return NULL;
    coev_setdebug(library, do_dump);
    debug_flag = module;
    Py_RETURN_NONE;   
}

static PyMethodDef CoevMethods[] = {
    {   "current", (PyCFunction)mod_current, METH_NOARGS, mod_current_doc },
    {   "scheduler", (PyCFunction)mod_scheduler, METH_NOARGS, mod_scheduler_doc },
    {   "wait", (PyCFunction)mod_wait, METH_VARARGS, mod_wait_doc },
    {   "sleep", (PyCFunction)mod_sleep, METH_VARARGS, mod_sleep_doc },
    {   "stats", (PyCFunction)mod_stats, METH_NOARGS, mod_stats_doc },
    {   "setdebug", (PyCFunction)mod_setdebug, 
        METH_VARARGS | METH_KEYWORDS, mod_setdebug_doc },
    { 0 }
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
    
    if (PyModule_AddStringConstant(m, "__version__", "0.3") < 0)
        return;

    if (PyModule_AddIntConstant(m, "READ", 1) < 0)
        return;
    
    if (PyModule_AddIntConstant(m, "WRITE", 2) < 0)
        return;
    
    if (PyType_Ready(&PyCoroutine_Type) < 0)
        return;

    if (PyType_Ready(&CoroLocalData_Type) < 0)
        return;

    if (PyType_Ready(&CoroSocketFile_Type) < 0)
        return;

    { /* add exceptions */
        PyObject* exc_obj;
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
            
            *(_exc_tab[i].exc) = PyErr_NewException(_exc_tab[i].name, *(_exc_tab[i].parent), exc_dict);
            Py_DECREF(exc_dict);
            
            exc_obj = *(_exc_tab[i].exc);
            
            if (exc_obj == NULL)
                return;
            
            Py_INCREF(exc_obj);
            PyModule_AddObject(m, _exc_tab[i].shortname, exc_obj);

        }
    }

    PyCoroutine_TypePtr = &PyCoroutine_Type;
    Py_INCREF(&PyCoroutine_Type);
    PyModule_AddObject(m, "coroutine", (PyObject*) &PyCoroutine_Type);
    
    Py_INCREF(&CoroLocalData_Type);
    PyModule_AddObject(m, "local", (PyObject*) &CoroLocalData_Type);
    
    Py_INCREF(&CoroSocketFile_Type);
    PyModule_AddObject(m, "socketfile", (PyObject*) &CoroSocketFile_Type);
    {
        /* initialize coev library */
        /* coro_main = PyObject_GC_New(PyCoroutine, &PyCoroutine_Type); */
        coro_main = (PyCoroutine *)PyCoroutine_Type.tp_alloc(&PyCoroutine_Type, 0);
        coev_initialize(&_cmf, &coro_main->coev);
        coro_main->treepos = coev_treepos(&coro_main->coev);
        coro_main->switch_veto = 0;
    }
    
}
