/* adapter_float.c - psycoev pfloat type wrapper implementation
 *
 * Copyright (C) 2003-2009 Federico Di Gregorio <fog@debian.org>
 *
 * This file is part of psycoev.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>
#include <floatobject.h>
#include <math.h>

#define PSYCOPG_MODULE
#include "psycoev/config.h"
#include "psycoev/python.h"
#include "psycoev/psycoev.h"
#include "psycoev/adapter_pfloat.h"
#include "psycoev/microprotocols_proto.h"


/** the Float object **/

static PyObject *
pfloat_str(pfloatObject *self)
{
    double n = PyFloat_AsDouble(self->wrapped);
    if (isnan(n))
        return PyString_FromString("'NaN'::float");
    else if (isinf(n))
        return PyString_FromString("'Infinity'::float");
    else
        return PyObject_Str(self->wrapped);
}

static PyObject *
pfloat_getquoted(pfloatObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, "")) return NULL;
    return pfloat_str(self);
}

static PyObject *
pfloat_conform(pfloatObject *self, PyObject *args)
{
    PyObject *res, *proto;

    if (!PyArg_ParseTuple(args, "O", &proto)) return NULL;

    if (proto == (PyObject*)&isqlquoteType)
        res = (PyObject*)self;
    else
        res = Py_None;

    Py_INCREF(res);
    return res;
}

/** the Float object */

/* object member list */

static struct PyMemberDef pfloatObject_members[] = {
    {"adapted", T_OBJECT, offsetof(pfloatObject, wrapped), RO},
    {NULL}
};

/* object method table */

static PyMethodDef pfloatObject_methods[] = {
    {"getquoted", (PyCFunction)pfloat_getquoted, METH_VARARGS,
     "getquoted() -> wrapped object value as SQL-quoted string"},
    {"__conform__", (PyCFunction)pfloat_conform, METH_VARARGS, NULL},
    {NULL}  /* Sentinel */
};

/* initialization and finalization methods */

static int
pfloat_setup(pfloatObject *self, PyObject *obj)
{
    Dprintf("pfloat_setup: init pfloat object at %p, refcnt = "
        FORMAT_CODE_PY_SSIZE_T,
        self, ((PyObject *)self)->ob_refcnt
      );

    Py_INCREF(obj);
    self->wrapped = obj;

    Dprintf("pfloat_setup: good pfloat object at %p, refcnt = "
        FORMAT_CODE_PY_SSIZE_T,
        self, ((PyObject *)self)->ob_refcnt
      );
    return 0;
}

static int
pfloat_traverse(PyObject *obj, visitproc visit, void *arg)
{
    pfloatObject *self = (pfloatObject *)obj;

    Py_VISIT(self->wrapped);
    return 0;
}

static void
pfloat_dealloc(PyObject* obj)
{
    pfloatObject *self = (pfloatObject *)obj;

    Py_CLEAR(self->wrapped);

    Dprintf("pfloat_dealloc: deleted pfloat object at %p, refcnt = "
        FORMAT_CODE_PY_SSIZE_T,
        obj, obj->ob_refcnt
      );

    obj->ob_type->tp_free(obj);
}

static int
pfloat_init(PyObject *obj, PyObject *args, PyObject *kwds)
{
    PyObject *o;

    if (!PyArg_ParseTuple(args, "O", &o))
        return -1;

    return pfloat_setup((pfloatObject *)obj, o);
}

static PyObject *
pfloat_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    return type->tp_alloc(type, 0);
}

static void
pfloat_del(PyObject* self)
{
    PyObject_GC_Del(self);
}

static PyObject *
pfloat_repr(pfloatObject *self)
{
    return PyString_FromFormat("<psycoev._psycoev.Float object at %p>",
                                self);
}


/* object type */

#define pfloatType_doc \
"Float(str) -> new Float adapter object"

PyTypeObject pfloatType = {
    PyObject_HEAD_INIT(NULL)
    0,
    "psycoev._psycoev.Float",
    sizeof(pfloatObject),
    0,
    pfloat_dealloc, /*tp_dealloc*/
    0,          /*tp_print*/

    0,          /*tp_getattr*/
    0,          /*tp_setattr*/

    0,          /*tp_compare*/

    (reprfunc)pfloat_repr, /*tp_repr*/
    0,          /*tp_as_number*/
    0,          /*tp_as_sequence*/
    0,          /*tp_as_mapping*/
    0,          /*tp_hash */

    0,          /*tp_call*/
    (reprfunc)pfloat_str, /*tp_str*/

    0,          /*tp_getattro*/
    0,          /*tp_setattro*/
    0,          /*tp_as_buffer*/

    Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE|Py_TPFLAGS_HAVE_GC, /*tp_flags*/
    pfloatType_doc, /*tp_doc*/

    pfloat_traverse, /*tp_traverse*/
    0,          /*tp_clear*/

    0,          /*tp_richcompare*/
    0,          /*tp_weaklistoffset*/

    0,          /*tp_iter*/
    0,          /*tp_iternext*/

    /* Attribute descriptor and subclassing stuff */

    pfloatObject_methods, /*tp_methods*/
    pfloatObject_members, /*tp_members*/
    0,          /*tp_getset*/
    0,          /*tp_base*/
    0,          /*tp_dict*/

    0,          /*tp_descr_get*/
    0,          /*tp_descr_set*/
    0,          /*tp_dictoffset*/

    pfloat_init, /*tp_init*/
    0, /*tp_alloc  will be set to PyType_GenericAlloc in module init*/
    pfloat_new, /*tp_new*/
    (freefunc)pfloat_del, /*tp_free  Low-level free-memory routine */
    0,          /*tp_is_gc For PyObject_IS_GC */
    0,          /*tp_bases*/
    0,          /*tp_mro method resolution order */
    0,          /*tp_cache*/
    0,          /*tp_subclasses*/
    0           /*tp_weaklist*/
};


/** module-level functions **/

PyObject *
psyco_Float(PyObject *module, PyObject *args)
{
    PyObject *obj;

    if (!PyArg_ParseTuple(args, "O", &obj))
        return NULL;

    return PyObject_CallFunction((PyObject *)&pfloatType, "O", obj);
}
