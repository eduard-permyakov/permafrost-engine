/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#include <Python.h> /* Must be first */
#include <frameobject.h>

#include "py_task.h"
#include "../task.h"
#include "../sched.h"
#include "../main.h"
#include "../lib/public/khash.h"

#include <stdint.h>

typedef struct {
    PyObject_HEAD
    PyObject *runfunc;
    uint32_t tid;
    enum{
        PYTASK_STATE_NOT_STARTED,
        PYTASK_STATE_RUNNING,
        PYTASK_STATE_FINISHED,
    }state;
}PyTaskObject;

KHASH_MAP_INIT_INT(task, PyTaskObject*)

static PyObject *PyTask_getattro(PyTaskObject *self, PyObject *name);

static PyObject *PyTask_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyTask_dealloc(PyTaskObject *self);

static PyObject *PyTask_pickle(PyTaskObject *self);
static PyObject *PyTask_unpickle(PyObject *cls, PyObject *args);

static PyObject *PyTask_run(PyTaskObject *self);
static PyObject *PyTask_wait(PyTaskObject *self, PyObject *args);
static PyObject *PyTask_yield(PyTaskObject *self);
static PyObject *PyTask_send(PyTaskObject *self, PyObject *args);
static PyObject *PyTask_receive(PyTaskObject *self, PyObject *args);
static PyObject *PyTask_reply(PyTaskObject *self, PyObject *args);
static PyObject *PyTask_await_event(PyTaskObject *self, PyObject *args);
static PyObject *PyTask_sleep(PyTaskObject *self, PyObject *args);
static PyObject *PyTask_register(PyTaskObject *self, PyObject *args);
static PyObject *PyTask_who_is(PyTaskObject *self, PyObject *args, PyObject *kwargs);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyMethodDef PyTask_methods[] = {
    {"__pickle__", 
    (PyCFunction)PyTask_pickle, METH_NOARGS,
    "Serialize a Permafrost Engine task object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyTask_unpickle, METH_VARARGS | METH_CLASS,
    "Create a new pf.Task instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {"run", 
    (PyCFunction)PyTask_run, METH_NOARGS,
    "Start the task, invoking its' __run__ method in a fiber context."},

    {"wait", 
    (PyCFunction)PyTask_wait, METH_VARARGS,
    "Block until the completion of another pf.Task instance."},

    {"yield_", 
    (PyCFunction)PyTask_yield, METH_NOARGS,
    "Yield to the scheduler, allowing other tasks to run. The task may be suspended until "
    "the next frame. Yielding periodically is a means to make sure that long-running tasks "
    "don't exceed the frame's time budget."},

    {"send", 
    (PyCFunction)PyTask_send, METH_VARARGS,
    "Send a message to another pf.Task instance, becoming blocked until it replies."},

    {"receive", 
    (PyCFunction)PyTask_receive, METH_VARARGS,
    "Become blocked, waiting until a message is received from the specified task."},

    {"reply", 
    (PyCFunction)PyTask_reply, METH_VARARGS,
    "Respond to a sent message from another task, unblocking it."},

    {"await_event", 
    (PyCFunction)PyTask_await_event, METH_VARARGS,
    "Become blocked unitl a particular event takes place."},

    {"sleep", 
    (PyCFunction)PyTask_sleep, METH_VARARGS,
    "Become blocked for a period of time specified in milliseconds."},

    {"register", 
    (PyCFunction)PyTask_register, METH_VARARGS,
    "Register this task for a specific name."},

    {"who_is", 
    (PyCFunction)PyTask_who_is, METH_VARARGS | METH_KEYWORDS,
    "Look up a task for a specific name"},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyTask_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.Task",               /* tp_name */
    sizeof(PyTaskObject),    /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)PyTask_dealloc,/* tp_dealloc */
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
    (getattrofunc)PyTask_getattro, /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "Permafrost Engine runnable task.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyTask_methods,            /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    PyTask_new,                /* tp_new */
};

static PyThreadState *s_main_thread_state;
static khash_t(task) *s_tid_task_map;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct result py_task(void *arg)
{
    PyTaskObject *self = (PyTaskObject*)arg;

    ASSERT_IN_MAIN_THREAD();
    ASSERT_IN_CTX(self->tid);

    /* Create a new PyThreadState for each fiber. Since we only run it in the 
     * main thread in a fiber which cannot be pre-empted and yields control at
     * known boundaries, there is no need to take the GIL before switching to it.
     */
    PyInterpreterState *interp = PyThreadState_Get()->interp;
    PyThreadState *ts = PyThreadState_New(interp);
    s_main_thread_state = PyThreadState_Swap(ts);

    // TODO: when we resume a saved session, we're going to need to use the frame API
    // to begin execution mid-way...
    PyObject *ret = PyEval_EvalCodeEx(
        (PyCodeObject *)PyFunction_GET_CODE(self->runfunc),
        PyFunction_GET_GLOBALS(self->runfunc), 
        NULL,
        (PyObject**)&self, 1,
        NULL, 0, 
        NULL, 0,
        PyFunction_GET_CLOSURE(self->runfunc)
    );
    Py_XDECREF(ret);

    PyThreadState_Swap(s_main_thread_state);
    PyThreadState_Delete(ts);

    khiter_t k = kh_get(task, s_tid_task_map, self->tid);
    assert(k != kh_end(s_tid_task_map));
    kh_del(task, s_tid_task_map, k);

    self->state = PYTASK_STATE_FINISHED;
    Py_CLEAR(self->runfunc);
    Py_DECREF(self);

    Task_Unregister();
    return NULL_RESULT;
}

static PyObject *PyTask_getattro(PyTaskObject *self, PyObject *name)
{
    if(PyString_Check(name) && !strcmp(PyString_AS_STRING(name), "__run__")) {
        PyErr_SetString(PyExc_AttributeError, "The __run__ method of the task cannot be accessed directly. "
            "Invoke in in a fiber context with 'run'.");
        return NULL;
    }
    return PyObject_GenericGetAttr((PyObject*)self, name);
}

static PyObject *PyTask_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyTaskObject *self = (PyTaskObject*)type->tp_alloc(type, 0);
    if(!self)
        goto fail_alloc;

    PyObject *runname = PyString_InternFromString("__run__");
    if(!runname)
        goto fail_run;

    PyObject *func = _PyType_Lookup(type, runname); /* borrowed */
    Py_DECREF(runname);

    if(!func) {
        PyErr_SetString(PyExc_RuntimeError, "Task class must implement a __run__ method.");
        goto fail_run;
    }

    if(!PyFunction_Check(func)) {
        PyErr_SetString(PyExc_RuntimeError, "The task class's __run__ attribute must be a function.");
        goto fail_run;
    }

    Py_INCREF(func);
    self->runfunc = func;
    self->state = PYTASK_STATE_NOT_STARTED;
    return (PyObject*)self;

fail_run:
    Py_DECREF(self);
fail_alloc:
    assert(PyErr_Occurred());
    return NULL;
}

static void PyTask_dealloc(PyTaskObject *self)
{

}

static PyObject *PyTask_pickle(PyTaskObject *self)
{
    return NULL;
}

static PyObject *PyTask_unpickle(PyObject *cls, PyObject *args)
{
    return NULL;
}

static PyObject *PyTask_run(PyTaskObject *self)
{
    self->tid = Sched_Create(16, py_task, self, NULL, TASK_MAIN_THREAD_PINNED);
    self->state = PYTASK_STATE_RUNNING;

    int status;
    khiter_t k = kh_put(task, s_tid_task_map, self->tid, &status);
    assert(status != -1 && status != 0);
    kh_value(s_tid_task_map, k) = self;
    /* Retain a running task object until it finishes */
    Py_INCREF(self);

    Py_RETURN_NONE;
}

static PyObject *PyTask_wait(PyTaskObject *self, PyObject *args)
{
    return NULL;
}

static PyObject *PyTask_yield(PyTaskObject *self)
{
    ASSERT_IN_MAIN_THREAD();

    if(self->state != PYTASK_STATE_RUNNING || Sched_ActiveTID() != self->tid) {
        PyErr_SetString(PyExc_RuntimeError, 
            "The 'yield_' method can only be called from the context of the __run__ method.");
        return NULL;
    }

    assert(s_main_thread_state);
    PyThreadState *ts = PyThreadState_Swap(s_main_thread_state);
    Task_Yield();
    s_main_thread_state = PyThreadState_Swap(ts);

    Py_RETURN_NONE;
}

static PyObject *PyTask_send(PyTaskObject *self, PyObject *args)
{
    return NULL;
}

static PyObject *PyTask_receive(PyTaskObject *self, PyObject *args)
{
    return NULL;
}

static PyObject *PyTask_reply(PyTaskObject *self, PyObject *args)
{
    return NULL;
}

static PyObject *PyTask_await_event(PyTaskObject *self, PyObject *args)
{
    ASSERT_IN_MAIN_THREAD();

    if(self->state != PYTASK_STATE_RUNNING || Sched_ActiveTID() != self->tid) {
        PyErr_SetString(PyExc_RuntimeError, 
            "The 'await_event' method can only be called from the context of the __run__ method.");
        return NULL;
    }

    int event;
    if(!PyArg_ParseTuple(args, "i", &event)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one integer argument (event to wait on)");
        return NULL;
    }

    assert(s_main_thread_state);
    PyThreadState *ts = PyThreadState_Swap(s_main_thread_state);
    Task_AwaitEvent(event);
    s_main_thread_state = PyThreadState_Swap(ts);

    Py_RETURN_NONE;
}

static PyObject *PyTask_sleep(PyTaskObject *self, PyObject *args)
{
    ASSERT_IN_MAIN_THREAD();

    if(self->state != PYTASK_STATE_RUNNING || Sched_ActiveTID() != self->tid) {
        PyErr_SetString(PyExc_RuntimeError, 
            "The 'await_event' method can only be called from the context of the __run__ method.");
        return NULL;
    }

    int ms;
    if(!PyArg_ParseTuple(args, "i", &ms)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one integer argument (number of milliseconds)");
        return NULL;
    }

    assert(s_main_thread_state);
    PyThreadState *ts = PyThreadState_Swap(s_main_thread_state);
    Task_Sleep(ms);
    s_main_thread_state = PyThreadState_Swap(ts);

    Py_RETURN_NONE;
}

static PyObject *PyTask_register(PyTaskObject *self, PyObject *args)
{
    ASSERT_IN_MAIN_THREAD();

    if(self->state != PYTASK_STATE_RUNNING || Sched_ActiveTID() != self->tid) {
        PyErr_SetString(PyExc_RuntimeError, 
            "The 'register' method can only be called from the context of the __run__ method.");
        return NULL;
    }

    const char *name;
    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one string argument (the name to register under)");
        return NULL;
    }

    assert(s_main_thread_state);
    PyThreadState *ts = PyThreadState_Swap(s_main_thread_state);
    Task_Register(name);
    s_main_thread_state = PyThreadState_Swap(ts);

    Py_RETURN_NONE;
}

static PyObject *PyTask_who_is(PyTaskObject *self, PyObject *args, PyObject *kwargs)
{
    ASSERT_IN_MAIN_THREAD();

    if(self->state != PYTASK_STATE_RUNNING || Sched_ActiveTID() != self->tid) {
        PyErr_SetString(PyExc_RuntimeError, 
            "The 'who_is' method can only be called from the context of the __run__ method.");
        return NULL;
    }

    static char *kwlist[] = {"name", "blocking", NULL};
    int blocking = true;
    const char *name;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "s|i", kwlist, &name, &blocking)) {
        assert(PyErr_Occurred());
        return NULL;
    }

    assert(s_main_thread_state);
    PyThreadState *ts = PyThreadState_Swap(s_main_thread_state);
    uint32_t tid = Task_WhoIs(name, blocking);
    s_main_thread_state = PyThreadState_Swap(ts);

    if(tid == NULL_TID) {
        Py_RETURN_NONE;    
    }

    khiter_t k = kh_get(task, s_tid_task_map, tid);
    if(k == kh_end(s_tid_task_map)) {
        Py_RETURN_NONE;    
    }

    PyTaskObject *ret = kh_value(s_tid_task_map, k);
    Py_INCREF(ret);
    return (PyObject*)ret;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void S_Task_PyRegister(PyObject *module)
{
    if(PyType_Ready(&PyTask_type) < 0)
        return;
    Py_INCREF(&PyTask_type);
    PyModule_AddObject(module, "Task", (PyObject*)&PyTask_type);
}

bool S_Task_Init(void)
{
    s_tid_task_map = kh_init(task);
    if(!s_tid_task_map)
        return false;
    return true;
}

void S_Task_Shutdown(void)
{
    kh_destroy(task, s_tid_task_map);
}

