/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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
 */

#include <Python.h> /* Must be included first */

#include "public/script.h"
#include "../lib/public/nuklear.h"
#include "../lib/public/kvec.h"
#include "../event/public/event.h"

#include <assert.h>

struct rect{
    int x, y, width, height;
};

typedef struct {
    PyObject_HEAD
    const char  *name;
    struct rect  rect;
    int          flags;
    bool         shown;
}PyWindowObject;

static int       PyWindow_init(PyWindowObject *self, PyObject *args);

static PyObject *PyWindow_layout_row_static(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_layout_row_dynamic(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_button_label(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_show(PyWindowObject *self);
static PyObject *PyWindow_hide(PyWindowObject *self);
static PyObject *PyWindow_update(PyWindowObject *self);
static PyObject *PyWindow_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct nk_context       *s_nk_ctx;
static kvec_t(PyWindowObject*)  s_active_windows;


static PyMethodDef PyWindow_methods[] = {
    {"layout_row_static", 
    (PyCFunction)PyWindow_layout_row_static, METH_VARARGS,
    "Add a row with a static layout."},

    {"layout_row_dynamic", 
    (PyCFunction)PyWindow_layout_row_dynamic, METH_VARARGS,
    "Add a row with a dynamic layout."},

    {"button_label", 
    (PyCFunction)PyWindow_button_label, METH_VARARGS,
    "Add a button with a label and action."},

    {"show", 
    (PyCFunction)PyWindow_show, METH_NOARGS,
    "Make the window visible."},

    {"hide", 
    (PyCFunction)PyWindow_hide, METH_NOARGS,
    "Make the window invisible."},

    {"update", 
    (PyCFunction)PyWindow_update, METH_NOARGS,
    "Handles layout and state changes of the window. Default implementation is empty. "
    "This method should be overridden by subclasses to customize the window look and behavior."},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyWindow_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.Window",               /* tp_name */
    sizeof(PyWindowObject),    /* tp_basicsize */
    0,                         /* tp_itemsize */
    0,                         /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "Permafrost Engine UI window.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyWindow_methods,          /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)PyWindow_init,   /* tp_init */
    0,                         /* tp_alloc */
    PyWindow_new,              /* tp_new */
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static int PyWindow_init(PyWindowObject *self, PyObject *args)
{
    const char *name;
    struct rect rect;
    int flags;

    if(!PyArg_ParseTuple(args, "s(iiii)i", &name, &rect.x, &rect.y, &rect.width, &rect.height, &flags)) {
        PyErr_SetString(PyExc_TypeError, "3 arguments expected: integer, tuple of 4 integers, and integer.");
        return -1;
    }

    self->name = name;
    self->rect = rect;
    self->flags = flags;
    self->shown = false;
    return 0;
}

static PyObject *PyWindow_layout_row_static(PyWindowObject *self, PyObject *args)
{
    int height, width, cols;

    if(!PyArg_ParseTuple(args, "iii", &height, &width, &cols)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be three integers.");
        return NULL;
    }
    nk_layout_row_static(s_nk_ctx, height, width, cols);
    Py_RETURN_NONE;
}

static PyObject *PyWindow_layout_row_dynamic(PyWindowObject *self, PyObject *args)
{
    int height, cols;

    if(!PyArg_ParseTuple(args, "ii", &height, &cols)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be three integers.");
        return NULL;
    }
    nk_layout_row_dynamic(s_nk_ctx, height, cols);
    Py_RETURN_NONE;
}

static PyObject *PyWindow_button_label(PyWindowObject *self, PyObject *args)
{
    const char *str;
    PyObject *callable;

    if(!PyArg_ParseTuple(args, "sO", &str, &callable)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string and an object.");
        return NULL;
    }

    if(!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "Second argument must be callable.");
        return NULL;
    }

    if(nk_button_label(s_nk_ctx, str)) {
        PyObject_CallObject(callable, NULL);
    }

    Py_RETURN_NONE;
}

static PyObject *PyWindow_show(PyWindowObject *self)
{
    if(self->shown)
        Py_RETURN_NONE;

    kv_push(PyWindowObject*, s_active_windows, self);
    self->shown = true;
    Py_RETURN_NONE;
}

static bool equal(PyWindowObject *const *a, PyWindowObject *const *b)
{
    return *a == *b;
}

static PyObject *PyWindow_hide(PyWindowObject *self)
{
    if(!self->shown)
        Py_RETURN_NONE;

    int idx;
    kv_indexof(PyWindowObject*, s_active_windows, self, equal, idx);
    kv_del(PyWindowObject*, s_active_windows, idx);
    self->shown = false;
    Py_RETURN_NONE;
}

static PyObject *PyWindow_update(PyWindowObject *self)
{
    Py_RETURN_NONE;
}

static PyObject *PyWindow_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *self = type->tp_alloc(type, 0);
    return self;
}

static void active_windows_update(void *user, void *event)
{
    (void)user;
    (void)event;

    for(int i = 0; i < kv_size(s_active_windows); i++) {
    
        PyWindowObject *win = kv_A(s_active_windows, i);

        if(nk_begin(s_nk_ctx, win->name, 
            nk_rect(win->rect.x, win->rect.y, win->rect.width, win->rect.height), win->flags)) {

            PyObject *ret = PyObject_CallMethod((PyObject*)win, "update", NULL); 
            Py_DECREF(ret);

        }
        nk_end(s_nk_ctx);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool S_UI_Init(struct nk_context *ctx)
{
    assert(ctx);
    s_nk_ctx = ctx;

    kv_init(s_active_windows);
    return E_Global_Register(EVENT_UPDATE_UI, active_windows_update, NULL);
}

void S_UI_Shutdown(void)
{
    E_Global_Unregister(EVENT_UPDATE_UI, active_windows_update);
    kv_destroy(s_active_windows);
}

void S_UI_PyRegister(PyObject *module)
{
    if(PyType_Ready(&PyWindow_type) < 0)
        return;
    Py_INCREF(&PyWindow_type);
    PyModule_AddObject(module, "Window", (PyObject*)&PyWindow_type);
}

