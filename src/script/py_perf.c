/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking this software statically or dynamically with other modules is making
 *  a combined work based on this software. Thus, the terms and conditions of
 *  the GNU General Public License cover the whole combination.
 *
 *  As a special exception, the copyright holders of Permafrost Engine give
 *  you permission to link Permafrost Engine with independent modules to produce
 *  an executable, regardless of the license terms of these independent
 *  modules, and to copy and distribute the resulting executable under
 *  terms of your choice, provided that you also meet, for each linked
 *  independent module, the terms and conditions of the license of that
 *  module. An independent module is a module which is not derived from
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may
 *  extend this exception to your version of Permafrost Engine, but you are not
 *  obliged to do so. If you do not wish to do so, delete this exception
 *  statement from your version.
 *
 */

#define MEM_FILE_SYS MEM_SYS_SCRIPT
#define MEM_FILE_SUB MEM_SUB_SCRIPT_PERF

#include "py_perf.h"
#include "../perf.h"
#include "../lib/public/mem.h"

#include <assert.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_SCRIPT, MEM_SUB_SCRIPT_PERF)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_SCRIPT, MEM_SUB_SCRIPT_PERF)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_SCRIPT, MEM_SUB_SCRIPT_PERF)


typedef struct{
    PyObject_HEAD
    struct perf_info *info;
}PyPerfInfoObject;


static PyObject *PyPerfInfo_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void       PyPerfInfo_dealloc(PyPerfInfoObject *self);
static Py_ssize_t PyPerfInfo_length(PyPerfInfoObject *self);

static PyObject *PyPerfInfo_funcname(PyPerfInfoObject *self, PyObject *args);
static PyObject *PyPerfInfo_ms_delta(PyPerfInfoObject *self, PyObject *args);
static PyObject *PyPerfInfo_pc_delta(PyPerfInfoObject *self, PyObject *args);
static PyObject *PyPerfInfo_parent_idx(PyPerfInfoObject *self, PyObject *args);
static PyObject *PyPerfInfo_hw_ipc(PyPerfInfoObject *self, PyObject *args);
static PyObject *PyPerfInfo_hw_br_miss(PyPerfInfoObject *self, PyObject *args);
static PyObject *PyPerfInfo_hw_l1d_miss(PyPerfInfoObject *self, PyObject *args);
static PyObject *PyPerfInfo_hw_llc_miss(PyPerfInfoObject *self, PyObject *args);

static PyObject *PyPerfInfo_get_threadname(PyPerfInfoObject *self, void *closure);
static PyObject *PyPerfInfo_get_nentries(PyPerfInfoObject *self, void *closure);

static PyObject *PyPerfInfo_pickle(PyPerfInfoObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPerfInfo_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyMethodDef PyPerfInfo_methods[] = {
    {"funcname",
    (PyCFunction)PyPerfInfo_funcname, METH_VARARGS,
    "Returns the name of the perf entry at the given index."},

    {"ms_delta",
    (PyCFunction)PyPerfInfo_ms_delta, METH_VARARGS,
    "Returns the wall-clock duration (in milliseconds) of the entry at the given index."},

    {"pc_delta",
    (PyCFunction)PyPerfInfo_pc_delta, METH_VARARGS,
    "Returns the raw performance counter delta of the entry at the given index."},

    {"parent_idx",
    (PyCFunction)PyPerfInfo_parent_idx, METH_VARARGS,
    "Returns the parent entry index for the entry at the given index, or a "
    "negative value if the entry is a root."},

    {"hw_ipc",
    (PyCFunction)PyPerfInfo_hw_ipc, METH_VARARGS,
    "Returns the instructions-per-cycle ratio of the entry at the given index."},

    {"hw_br_miss",
    (PyCFunction)PyPerfInfo_hw_br_miss, METH_VARARGS,
    "Returns the branch-miss rate (percent) of the entry at the given index."},

    {"hw_l1d_miss",
    (PyCFunction)PyPerfInfo_hw_l1d_miss, METH_VARARGS,
    "Returns the L1D-miss rate (percent) of the entry at the given index."},

    {"hw_llc_miss",
    (PyCFunction)PyPerfInfo_hw_llc_miss, METH_VARARGS,
    "Returns the LLC-miss rate (percent) of the entry at the given index."},

    {"__pickle__",
    (PyCFunction)PyPerfInfo_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine PerfInfo to a string."},

    {"__unpickle__",
    (PyCFunction)PyPerfInfo_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.PerfInfo instance from a string earlier returned from a __pickle__ method. "
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyPerfInfo_getset[] = {
    {"threadname",
    (getter)PyPerfInfo_get_threadname, NULL,
    "Name of the thread that produced this perf report.",
    NULL},

    {"nentries",
    (getter)PyPerfInfo_get_nentries, NULL,
    "Number of perf entries available in this report.",
    NULL},

    {NULL}  /* Sentinel */
};

static PySequenceMethods PyPerfInfo_as_sequence = {
    (lenfunc)PyPerfInfo_length,    /* sq_length */
    0,                             /* sq_concat */
    0,                             /* sq_repeat */
    0,                             /* sq_item */
    0,                             /* sq_slice */
    0,                             /* sq_ass_item */
    0,                             /* sq_ass_slice */
    0,                             /* sq_contains */
    0,                             /* sq_inplace_concat */
    0,                             /* sq_inplace_repeat */
};

static PyTypeObject PyPerfInfo_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.PerfInfo",                  /* tp_name */
    sizeof(PyPerfInfoObject),       /* tp_basicsize */
    0,                              /* tp_itemsize */
    (destructor)PyPerfInfo_dealloc, /* tp_dealloc */
    0,                              /* tp_print */
    0,                              /* tp_getattr */
    0,                              /* tp_setattr */
    0,                              /* tp_reserved */
    0,                              /* tp_repr */
    0,                              /* tp_as_number */
    &PyPerfInfo_as_sequence,        /* tp_as_sequence */
    0,                              /* tp_as_mapping */
    0,                              /* tp_hash  */
    0,                              /* tp_call */
    0,                              /* tp_str */
    0,                              /* tp_getattro */
    0,                              /* tp_setattro */
    0,                              /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "Zero-copy view over a struct perf_info produced by the engine's perf system. "
    "Field accessors read directly from the underlying C buffer; no per-entry "
    "Python objects are eagerly allocated. The buffer is freed when the wrapper "
    "is destroyed."
    ,                               /* tp_doc */
    0,                              /* tp_traverse */
    0,                              /* tp_clear */
    0,                              /* tp_richcompare */
    0,                              /* tp_weaklistoffset */
    0,                              /* tp_iter */
    0,                              /* tp_iternext */
    PyPerfInfo_methods,             /* tp_methods */
    0,                              /* tp_members */
    PyPerfInfo_getset,              /* tp_getset */
    0,                              /* tp_base */
    0,                              /* tp_dict */
    0,                              /* tp_descr_get */
    0,                              /* tp_descr_set */
    0,                              /* tp_dictoffset */
    0,                              /* tp_init */
    0,                              /* tp_alloc */
    PyPerfInfo_new,                 /* tp_new */
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static PyObject *PyPerfInfo_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyPerfInfoObject *self = (PyPerfInfoObject*)type->tp_alloc(type, 0);
    if(self) {
        self->info = NULL;
    }
    return (PyObject*)self;
}

static void PyPerfInfo_dealloc(PyPerfInfoObject *self)
{
    if(self->info) {
        PF_FREE(self->info);
    }
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static Py_ssize_t PyPerfInfo_length(PyPerfInfoObject *self)
{
    if(!self->info)
        return 0;
    return (Py_ssize_t)self->info->nentries;
}

static int s_resolve_index(PyPerfInfoObject *self, PyObject *args, size_t *out_idx)
{
    Py_ssize_t i;
    if(!PyArg_ParseTuple(args, "n", &i)) {
        return -1;
    }
    if(!self->info) {
        PyErr_SetString(PyExc_RuntimeError, "PerfInfo has no underlying data.");
        return -1;
    }
    if(i < 0 || (size_t)i >= self->info->nentries) {
        PyErr_SetString(PyExc_IndexError, "PerfInfo entry index out of range.");
        return -1;
    }
    *out_idx = (size_t)i;
    return 0;
}

static PyObject *PyPerfInfo_funcname(PyPerfInfoObject *self, PyObject *args)
{
    size_t i;
    if(s_resolve_index(self, args, &i) != 0)
        return NULL;
    const char *name = self->info->entries[i].funcname;
    return PyString_FromString(name ? name : "");
}

static PyObject *PyPerfInfo_ms_delta(PyPerfInfoObject *self, PyObject *args)
{
    size_t i;
    if(s_resolve_index(self, args, &i) != 0)
        return NULL;
    return PyFloat_FromDouble(self->info->entries[i].ms_delta);
}

static PyObject *PyPerfInfo_pc_delta(PyPerfInfoObject *self, PyObject *args)
{
    size_t i;
    if(s_resolve_index(self, args, &i) != 0)
        return NULL;
    return PyLong_FromUnsignedLongLong(self->info->entries[i].pc_delta);
}

static PyObject *PyPerfInfo_parent_idx(PyPerfInfoObject *self, PyObject *args)
{
    size_t i;
    if(s_resolve_index(self, args, &i) != 0)
        return NULL;
    return PyInt_FromLong(self->info->entries[i].parent_idx);
}

static PyObject *PyPerfInfo_hw_ipc(PyPerfInfoObject *self, PyObject *args)
{
    size_t i;
    if(s_resolve_index(self, args, &i) != 0)
        return NULL;
    return PyFloat_FromDouble(self->info->entries[i].hw_ipc);
}

static PyObject *PyPerfInfo_hw_br_miss(PyPerfInfoObject *self, PyObject *args)
{
    size_t i;
    if(s_resolve_index(self, args, &i) != 0)
        return NULL;
    return PyFloat_FromDouble(self->info->entries[i].hw_br_miss);
}

static PyObject *PyPerfInfo_hw_l1d_miss(PyPerfInfoObject *self, PyObject *args)
{
    size_t i;
    if(s_resolve_index(self, args, &i) != 0)
        return NULL;
    return PyFloat_FromDouble(self->info->entries[i].hw_l1d_miss);
}

static PyObject *PyPerfInfo_hw_llc_miss(PyPerfInfoObject *self, PyObject *args)
{
    size_t i;
    if(s_resolve_index(self, args, &i) != 0)
        return NULL;
    return PyFloat_FromDouble(self->info->entries[i].hw_llc_miss);
}

static PyObject *PyPerfInfo_get_threadname(PyPerfInfoObject *self, void *closure)
{
    if(!self->info)
        Py_RETURN_NONE;
    return PyString_FromString(self->info->threadname);
}

static PyObject *PyPerfInfo_get_nentries(PyPerfInfoObject *self, void *closure)
{
    if(!self->info)
        return PyInt_FromLong(0);
    return PyInt_FromSsize_t((Py_ssize_t)self->info->nentries);
}

static PyObject *PyPerfInfo_pickle(PyPerfInfoObject *self, PyObject *args, PyObject *kwargs)
{
    return PyString_FromStringAndSize("", 0);
}

static PyObject *PyPerfInfo_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    PyObject *new_args = PyTuple_New(0);
    if(!new_args)
        return NULL;
    PyPerfInfoObject *piobj = (PyPerfInfoObject*)((PyTypeObject*)cls)->tp_new(
        (PyTypeObject*)cls, new_args, NULL);
    Py_DECREF(new_args);
    if(!piobj)
        return NULL;
    PyObject *ret = Py_BuildValue("(Oi)", piobj, 0);
    Py_DECREF(piobj);
    return ret;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void S_PerfInfo_PyRegister(PyObject *module)
{
    if(PyType_Ready(&PyPerfInfo_type) < 0)
        return;
    Py_INCREF(&PyPerfInfo_type);
    PyModule_AddObject(module, "PerfInfo", (PyObject*)&PyPerfInfo_type);
}

PyObject *S_PerfInfo_New(struct perf_info *info)
{
    assert(info);
    PyPerfInfoObject *self = PyObject_New(PyPerfInfoObject, &PyPerfInfo_type);
    if(!self)
        return NULL;
    self->info = info;
    return (PyObject*)self;
}

