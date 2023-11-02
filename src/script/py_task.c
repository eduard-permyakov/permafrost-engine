/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2023 Eduard Permyakov 
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
#include <opcode.h>

#include "py_task.h"
#include "py_pickle.h"
#include "public/script.h"
#include "../task.h"
#include "../sched.h"
#include "../main.h"
#include "../event.h"
#include "../perf.h"
#include "../game/public/game.h"
#include "../lib/public/khash.h"
#include "../lib/public/SDL_vec_rwops.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/mem.h"

#include <stdint.h>
#include <stdlib.h>


#define CALL_FLAG_VAR   1
#define CALL_FLAG_KW    2

#define CHK_TRUE(_pred, _label)         \
    do{                                 \
        if(!(_pred))                    \
            goto _label;                \
    }while(0)

struct pyrequest{
    PyObject *args;
    PyObject *kwargs;
    enum{
        PYREQ_WAIT,
        PYREQ_YIELD,
        PYREQ_SEND,
        PYREQ_RECEIVE,
        PYREQ_REPLY,
        PYREQ_AWAIT_EVENT,
        PYREQ_SLEEP,
        PYREQ_REGISTER,
        PYREQ_WHOIS,
    }type;
};

typedef struct {
    PyObject_HEAD
    PyObject *runfunc;
    /* When restoring a saved task, it can get a new TID. So, from 
     * the client code point-of-view, the TID can change suddenly
     * at any point. Thus, don't expose it. It's an implementation 
     * detail. */
    uint32_t tid;
    enum{
        PYTASK_STATE_NOT_STARTED,
        PYTASK_STATE_RUNNING,
        PYTASK_STATE_FINISHED,
    }state;
    struct pyrequest req;
    size_t stack_depth;
    PyThreadState *ts;
    const char *regname;
    uint32_t sleep_elapsed;
    bool small_stack;
}PyTaskObject;

KHASH_MAP_INIT_INT(task, PyTaskObject*)

typedef PyObject *(*ternaryfunc_t)(PyTaskObject*, PyObject*, PyObject*);
typedef PyObject *(*binaryfunc_t)(PyTaskObject*, PyObject*);
typedef PyObject *(*unaryfunc_t)(PyTaskObject*);

static PyObject *PyTask_call(PyTaskObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyTask_getattro(PyTaskObject *self, PyObject *name);

static PyObject *PyTask_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyTask_dealloc(PyTaskObject *self);

static PyObject *PyTask_pickle(PyTaskObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyTask_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

static PyObject *PyTask_run(PyTaskObject *self);
static PyObject *PyTask_wait(PyTaskObject *self, PyObject *args);
static PyObject *PyTask_yield(PyTaskObject *self);
static PyObject *PyTask_send(PyTaskObject *self, PyObject *args);
static PyObject *PyTask_receive(PyTaskObject *self);
static PyObject *PyTask_reply(PyTaskObject *self, PyObject *args);
static PyObject *PyTask_await_event(PyTaskObject *self, PyObject *args);
static PyObject *PyTask_sleep(PyTaskObject *self, PyObject *args);
static PyObject *PyTask_register(PyTaskObject *self, PyObject *args);
static PyObject *PyTask_who_is(PyTaskObject *self, PyObject *args, PyObject *kwargs);

static PyObject *PyTask_get_completed(PyTaskObject *self, void *closure);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyGetSetDef PyTask_getset[] = {
    {"completed",
    (getter)PyTask_get_completed, NULL,
    "Returns True if the task has ran to completion",
    NULL},
    {NULL}  /* Sentinel */
};

static PyMethodDef PyTask_methods[] = {
    {"__pickle__", 
    (PyCFunction)PyTask_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine task object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyTask_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
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
    (PyCFunction)PyTask_receive, METH_NOARGS,
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

PyTypeObject PyTask_type = {
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
    (ternaryfunc)PyTask_call,  /* tp_call */
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
    PyTask_getset,             /* tp_getset */
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
static uint32_t       s_pause_tick;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/* Like PyThreadState_New, but don't add it to the circular list managed by the 
 * interpreter core. The returned pointer must be free'd with pytask_ts_delete'. */
static PyThreadState *pytask_ts_new(PyInterpreterState *interp)
{
    PyThreadState *tstate = calloc(1, sizeof(PyThreadState));
    if(tstate) {
        tstate->interp = interp;
    }
    return tstate;
}

static void pytask_ts_delete(PyThreadState *tstate)
{
    PyThreadState_Clear(tstate);
    free(tstate);
}

static PyObject *pytask_call_method(void *func, PyTaskObject *self, PyObject *args, PyObject *kwargs)
{
    if(kwargs) {
        return ((ternaryfunc_t)func)(self, args, kwargs);
    }else if(args) {
        return ((binaryfunc_t)func)(self, args);
    }else{
        return ((unaryfunc_t)func)(self);
    }
}

static PyObject *pytask_resume_request(PyTaskObject *self)
{
    PyObject *args = self->req.args;
    PyObject *kwargs = self->req.kwargs;
    int reqtype = self->req.type;

    PyObject *ret = NULL;
    self->req = (struct pyrequest){0};

    switch(reqtype) {
    case PYREQ_WAIT:
        ret = pytask_call_method(PyTask_wait, self, args, kwargs);
        break;
    case PYREQ_YIELD:
        ret = pytask_call_method(PyTask_yield, self, args, kwargs);
        break;
    case PYREQ_SEND:
        ret = pytask_call_method(PyTask_send, self, args, kwargs);
        break;
    case PYREQ_RECEIVE:
        ret = pytask_call_method(PyTask_receive, self, args, kwargs);
        break;
    case PYREQ_REPLY:
        ret = pytask_call_method(PyTask_reply, self, args, kwargs);
        break;
    case PYREQ_AWAIT_EVENT:
        ret = pytask_call_method(PyTask_await_event, self, args, kwargs);
        break;
    case PYREQ_SLEEP:
        ret = pytask_call_method(PyTask_sleep, self, args, kwargs);
        break;
    case PYREQ_REGISTER:
        ret = pytask_call_method(PyTask_register, self, args, kwargs);
        break;
    case PYREQ_WHOIS:
        ret = pytask_call_method(PyTask_who_is, self, args, kwargs);
        break;
    }

    Py_XDECREF(args);
    Py_XDECREF(kwargs);
    return ret;
}

static int pytask_tracefunc(PyTaskObject *self, PyFrameObject *frame, int what, PyObject *arg)
{
    if(what == PyTrace_OPCODE) {
        assert(frame->f_stacktop);
        size_t stack_depth = (size_t)(frame->f_stacktop - frame->f_valuestack);
        self->stack_depth = stack_depth;
    }
    return 0;
}

static void pytask_push_ctx(PyTaskObject *self)
{
    s_main_thread_state = PyThreadState_Swap(self->ts);

    /* During frame evaluation, CPython NULLs out the current frame's
     * f_stacktop member. As such, there is no reliable way to get the 
     * size of the current evaluation stack of a frame. One exception
     * is that CPython sets f_stacktop to the correct value for trace
     * calls. We use this hook to capture the evaluation stack size
     * before every opcode execution. The evaluation stack size is 
     * normally a hidden implementation detail, but we use to save and 
     * restore running tasks.
     */
    PyEval_SetTrace((Py_tracefunc)pytask_tracefunc, (PyObject*)self);
}

static void pytask_pop_ctx(PyTaskObject *self)
{
    PyEval_SetTrace(NULL, NULL);
    assert(s_main_thread_state);
    PyThreadState *ts = PyThreadState_Swap(s_main_thread_state);
    assert(ts == self->ts);
}

static struct result py_task(void *arg)
{
    PyTaskObject *self = (PyTaskObject*)arg;

    ASSERT_IN_MAIN_THREAD();
    ASSERT_IN_CTX(self->tid);

    pytask_push_ctx(self);
    PyObject *ret = NULL;

    if(self->state == PYTASK_STATE_RUNNING) {

        if(self->regname) {
            Task_Register(self->regname);
        }

        PyObject *req_result = pytask_resume_request(self);
        assert(self->ts->frame);

        if(req_result) {

            /* Implementation details from Python/ceval.c
             */
            const unsigned char *bytecode = (unsigned char *)PyString_AS_STRING(self->ts->frame->f_code->co_code);
            const int lasti = self->ts->frame->f_lasti;
            PyObject_GC_UnTrack(self->ts->frame);

            int opcode = bytecode[lasti];
            int oparg = (bytecode[lasti + 2] << 8) + bytecode[lasti + 1];
            self->ts->frame->f_lasti += 2;

            assert(opcode == CALL_FUNCTION
                || opcode == CALL_FUNCTION_VAR
                || opcode == CALL_FUNCTION_KW
                || opcode == CALL_FUNCTION_VAR_KW);

            int na = oparg & 0xff;
            int nk = (oparg >> 8) & 0xff;
            int n = na + 2 * nk;

            if(opcode != CALL_FUNCTION) {
                int flags = (opcode - CALL_FUNCTION) & 3;
                if (flags & CALL_FLAG_VAR)
                    n++;
                if (flags & CALL_FLAG_KW)
                    n++;
            }

            PyObject **pfunc = (self->ts->frame->f_stacktop) - n - 1;

            /* Clear the stack of the function object. Also removes
             * the arguments in case they weren't consumed already.  
             */
            while((self->ts->frame->f_stacktop) > pfunc) {
                PyObject *w = *(--self->ts->frame->f_stacktop);
                Py_DECREF(w);
            }
            assert(self->ts->frame->f_stacktop >= self->ts->frame->f_valuestack);

            /* Push the result of the function call onto the evaluation stack. 
             */
            Py_INCREF(req_result);
            *(self->ts->frame->f_stacktop++) = req_result;

            if(!_PyObject_GC_IS_TRACKED(self->ts->frame))
                PyObject_GC_Track(self->ts->frame);
            ret = PyEval_EvalFrameEx(self->ts->frame, 0);
        }else{
            /* We've failed to resume the request. There are a couple of legitimate 
             * cases when this can occur: when the task was send-blocked or reply-blocked 
             * on another task that has subsequently finished running. Effectively this puts 
             * the resumed task into a state where it can never be unblocked. Such a "hung"
             * task is definitely an example of sloppy scripting but, nonetheless, shouldn't 
             * prevent us from resuming our session.
             */
            PyErr_Clear();
        }
        Py_CLEAR(self->ts->frame);
        Py_XDECREF(req_result);

    }else{

        assert(self->state == PYTASK_STATE_NOT_STARTED);
        assert(!self->ts->frame);
        assert(self->ts->recursion_depth == 0);

        self->state = PYTASK_STATE_RUNNING;

        ret = PyEval_EvalCodeEx(
            (PyCodeObject *)PyFunction_GET_CODE(self->runfunc),
            PyFunction_GET_GLOBALS(self->runfunc), 
            NULL,
            (PyObject**)&self, 1,
            NULL, 0, 
            NULL, 0,
            PyFunction_GET_CLOSURE(self->runfunc)
        );
    }

    if(ret) {
        PyObject *rval = Py_BuildValue("(OO)", (PyObject*)self, ret);
        E_Global_Notify(EVENT_SCRIPT_TASK_FINISHED, rval, ES_SCRIPT);
        Py_DECREF(ret);
    }else{
        E_Global_Notify(EVENT_SCRIPT_TASK_FINISHED, Py_BuildValue("(OO)", 
            (PyObject*)self, self->ts->curexc_type ? self->ts->curexc_type : Py_None), ES_SCRIPT);
    }

    /* Allow catching of the task exceptions for easier debugging */
    if(PyErr_Occurred()) {
        PyObject *exc_info = Py_BuildValue("OOOO",
            self,
            self->ts->curexc_type      ? self->ts->curexc_type      : Py_None,
            self->ts->curexc_value     ? self->ts->curexc_value     : Py_None,
            self->ts->curexc_traceback ? self->ts->curexc_traceback : Py_None
        );
        E_Global_Notify(EVENT_SCRIPT_TASK_EXCEPTION, exc_info, ES_SCRIPT);
        S_ShowLastError();
    }

    assert(!self->ts->frame);
    pytask_pop_ctx(self);

    khiter_t k = kh_get(task, s_tid_task_map, self->tid);
    assert(k != kh_end(s_tid_task_map));
    kh_del(task, s_tid_task_map, k);

    self->state = PYTASK_STATE_FINISHED;
    Py_CLEAR(self->runfunc);
    Py_DECREF(self);

    Task_Unregister();
    return NULL_RESULT;
}

static void pytask_req_set(PyTaskObject *task, PyObject *args, PyObject *kwargs, int type)
{
    Py_XINCREF(args);
    Py_XINCREF(kwargs);
    task->req = (struct pyrequest){args, kwargs, type};
}

static void pytask_req_clear(PyTaskObject *task)
{
    Py_XDECREF(task->req.args);
    Py_XDECREF(task->req.kwargs);
    task->req = (struct pyrequest){0};
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

static PyObject *PyTask_call(PyTaskObject *self, PyObject *args, PyObject *kwargs)
{
    PyEval_SetTrace((Py_tracefunc)pytask_tracefunc, (PyObject*)self);
    PyObject *ret = PyObject_Call((PyObject*)self, args, kwargs);
    PyEval_SetTrace(NULL, NULL);
    return ret;
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

    /* Create a new PyThreadState for each task. Since we only run it in the 
     * main thread in a fiber which cannot be pre-empted and yields control at
     * known boundaries, there is no need to take the GIL before switching to it.
     */
    PyInterpreterState *interp = PyThreadState_Get()->interp;
    self->ts = pytask_ts_new(interp);
    if(!self->ts) {
        assert(PyErr_Occurred());
        goto fail_run;
    }

    if(kwds) {
        PyObject *smallstack = PyDict_GetItemString(kwds, "small_stack");
        self->small_stack = (smallstack && PyObject_IsTrue(smallstack));
    }else{
        self->small_stack = false;
    }

    Py_INCREF(func);
    self->runfunc = func;
    self->state = PYTASK_STATE_NOT_STARTED;
    self->req = (struct pyrequest){0};
    self->stack_depth = 0;
    self->regname = NULL;
    self->sleep_elapsed = 0;
    return (PyObject*)self;

fail_run:
    Py_DECREF(self);
fail_alloc:
    assert(PyErr_Occurred());
    return NULL;
}

static void PyTask_dealloc(PyTaskObject *self)
{
    assert(self->state != PYTASK_STATE_RUNNING);
    assert(PyThreadState_Get() != self->ts);

    Py_CLEAR(self->runfunc);
    Py_CLEAR(self->ts->curexc_type);
    Py_CLEAR(self->ts->curexc_value);
    Py_CLEAR(self->ts->curexc_traceback);

    PF_FREE(self->regname);
    pytask_ts_delete(self->ts);
    self->ts = NULL;
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *PyTask_pickle(PyTaskObject *self, PyObject *args, PyObject *kwargs)
{
    bool status;
    PyObject *ret = NULL;
    size_t og_refcount = self->ob_refcnt;

    assert(self->state >= PYTASK_STATE_NOT_STARTED && self->state <= PYTASK_STATE_FINISHED);
    assert(self->ts->c_tracefunc == NULL);

    static char *kwlist[] = {"__ctx__", NULL};
    struct py_pickle_ctx *ctx = NULL;
    int len;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "|s#", kwlist, (char*)&ctx, &len)) {
        assert(PyErr_Occurred());
        return NULL;
    }
    if(!ctx) {
        PyErr_SetString(PyExc_TypeError, "Expecting __ctx__ keyword argument with pointer to pickling context.");
        return NULL;
    }
    assert(len == sizeof(struct py_pickle_ctx));

    PyObject *state = PyInt_FromLong(self->state);
    CHK_TRUE(state, fail_pickle);
    status = ctx->pickle_obj(ctx->private_ctx, state, ctx->stream);
    ctx->deferred_free(ctx->private_ctx, state);
    CHK_TRUE(status, fail_pickle);

    PyObject **old = NULL;
    if(self->state == PYTASK_STATE_RUNNING) {
        assert(self->ts->frame);
        old = self->ts->frame->f_stacktop;
        self->ts->frame->f_stacktop = self->ts->frame->f_valuestack + self->stack_depth;
    }

    PyObject *ts = Py_BuildValue("OiOOOOOOO",
        self->ts->frame             ? (PyObject*)self->ts->frame    : Py_None,
        self->ts->recursion_depth,
        self->ts->curexc_type       ? self->ts->curexc_type         : Py_None,
        self->ts->curexc_value      ? self->ts->curexc_value        : Py_None,
        self->ts->curexc_traceback  ? self->ts->curexc_traceback    : Py_None,
        self->ts->exc_type          ? self->ts->exc_type            : Py_None,
        self->ts->exc_value         ? self->ts->exc_value           : Py_None,
        self->ts->exc_traceback     ? self->ts->exc_traceback       : Py_None,
        self->ts->dict              ? self->ts->dict                : Py_None
    );
    CHK_TRUE(ts, fail_pickle);
    status = ctx->pickle_obj(ctx->private_ctx, ts, ctx->stream);
    ctx->deferred_free(ctx->private_ctx, ts);

    if(self->state == PYTASK_STATE_RUNNING) {
        self->ts->frame->f_stacktop = old;
    }
    CHK_TRUE(status, fail_pickle);

    PyObject *request = Py_BuildValue("OOi",
        self->req.args      ? self->req.args    : Py_None,
        self->req.kwargs    ? self->req.kwargs  : Py_None,
        self->req.type
    );
    CHK_TRUE(request, fail_pickle);
    status = ctx->pickle_obj(ctx->private_ctx, request, ctx->stream);
    ctx->deferred_free(ctx->private_ctx, request);
    CHK_TRUE(status, fail_pickle);

    PyObject *func = self->runfunc ? self->runfunc : Py_None;
    status = ctx->pickle_obj(ctx->private_ctx, func, ctx->stream);
    CHK_TRUE(status, fail_pickle);

    if(!self->regname) {
        status = ctx->pickle_obj(ctx->private_ctx, Py_None, ctx->stream);
    }else{
        PyObject *str = PyString_FromString(self->regname);
        CHK_TRUE(str, fail_pickle);
        status = ctx->pickle_obj(ctx->private_ctx, str, ctx->stream);
        ctx->deferred_free(ctx->private_ctx, str);
    }
    CHK_TRUE(status, fail_pickle);

    PyObject *stack_depth = PyInt_FromLong(self->stack_depth);
    status = ctx->pickle_obj(ctx->private_ctx, stack_depth, ctx->stream);
    ctx->deferred_free(ctx->private_ctx, stack_depth);
    CHK_TRUE(status, fail_pickle);

    PyObject *sleep_elapsed = PyInt_FromLong(self->sleep_elapsed);
    status = ctx->pickle_obj(ctx->private_ctx, sleep_elapsed, ctx->stream);
    ctx->deferred_free(ctx->private_ctx, sleep_elapsed);
    CHK_TRUE(status, fail_pickle);

    PyObject *small_stack = PyInt_FromLong(self->small_stack);
    status = ctx->pickle_obj(ctx->private_ctx, small_stack, ctx->stream);
    ctx->deferred_free(ctx->private_ctx, small_stack);
    CHK_TRUE(status, fail_pickle);

    ret = PyString_FromString("");
fail_pickle:
    assert(self->ob_refcnt == og_refcount);
    return ret;
}

static PyObject *PyTask_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    PyTaskObject *ret = NULL;
    Py_ssize_t nread = 0;
    char *str;
    int status, slen;
    char tmp;

    static char *kwlist[] = {"str", "__ctx__", NULL};
    struct py_unpickle_ctx *ctx = NULL;
    int len;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "s#|s#", kwlist, &str, &slen, &ctx, &len)) {
        assert(PyErr_Occurred());
        goto fail_args;
    }

    if(!ctx) {
        PyErr_SetString(PyExc_TypeError, "Expecting __ctx__ keyword argument with pointer to unpickling context.");
        return NULL;
    }
    assert(len == sizeof(struct py_unpickle_ctx));

    if(vec_size(ctx->stack) < 8) {
        PyErr_SetString(PyExc_RuntimeError, "Stack underflow");
        goto fail_args;
    }

    PyObject *small_stack = vec_pobj_pop(ctx->stack);
    PyObject *sleep_elapsed = vec_pobj_pop(ctx->stack);
    PyObject *stack_depth = vec_pobj_pop(ctx->stack);
    PyObject *regname = vec_pobj_pop(ctx->stack);
    PyObject *func = vec_pobj_pop(ctx->stack);
    PyObject *request = vec_pobj_pop(ctx->stack);
    PyObject *ts = vec_pobj_pop(ctx->stack);
    PyObject *state = vec_pobj_pop(ctx->stack);

    CHK_TRUE(PyInt_Check(small_stack), fail_unpickle);
    CHK_TRUE(PyInt_Check(sleep_elapsed), fail_unpickle);
    CHK_TRUE(PyInt_Check(stack_depth), fail_unpickle);
    CHK_TRUE(regname == Py_None || PyString_Check(regname), fail_unpickle);
    CHK_TRUE(PyInt_Check(state), fail_unpickle);
    CHK_TRUE(PyInt_AS_LONG(state) >= PYTASK_STATE_NOT_STARTED 
          && PyInt_AS_LONG(state) <= PYTASK_STATE_FINISHED, fail_unpickle);
    CHK_TRUE(PyFunction_Check(func) || func == Py_None, fail_unpickle);

    PyObject *frame;
    int recursion_depth;
    PyObject *curexc_type, *curexc_value, *curexc_traceback;
    PyObject *exc_type, *exc_value, *exc_traceback;
    PyObject *dict;

    if(!PyTuple_Check(ts) || !PyArg_ParseTuple(ts, "OiOOOOOOO", &frame, &recursion_depth,
        &curexc_type, &curexc_value, &curexc_traceback,
        &exc_type, &exc_value, &exc_traceback, &dict)) {
        goto fail_unpickle;
    }

    if(PyInt_AS_LONG(state) == PYTASK_STATE_RUNNING
    && (frame == Py_None || recursion_depth == 0)) {
        goto fail_unpickle;
    }
    CHK_TRUE(dict == Py_None || PyDict_Check(dict), fail_unpickle);

    PyObject *reqargs;
    PyObject *reqkwargs;
    int reqtype;

    if(!PyTuple_Check(request) || !PyArg_ParseTuple(request, "OOi", &reqargs, &reqkwargs, &reqtype))
        goto fail_unpickle;

    if(reqargs == Py_None && reqkwargs != Py_None)
        goto fail_unpickle;

    if(!(reqargs == Py_None || PyTuple_Check(reqargs))
    || !(reqkwargs == Py_None || PyDict_Check(reqkwargs)))
        goto fail_unpickle;

    ret = (PyTaskObject*)((PyTypeObject*)cls)->tp_alloc((struct _typeobject*)cls, 0);
    CHK_TRUE(ret, fail_unpickle);

    if(regname == Py_None) {
        ret->regname = NULL;
    }else{
        assert(PyString_Check(regname));
        ret->regname = pf_strdup(PyString_AS_STRING(regname));
    }

    PyInterpreterState *interp = PyThreadState_Get()->interp;
    ret->ts = pytask_ts_new(interp);
    if(!ret->ts) {
        Py_CLEAR(ret);
        goto fail_unpickle;
    }
    ret->ts->frame = (frame == Py_None) ? NULL : (Py_INCREF(frame), (PyFrameObject*)frame);
    ret->ts->recursion_depth = recursion_depth;
    ret->ts->dict = (dict == Py_None) ? NULL : (Py_INCREF(dict), dict);

    ret->ts->curexc_type = (curexc_type == Py_None) ? NULL : (Py_INCREF(curexc_type), curexc_type);
    ret->ts->curexc_value = (curexc_value == Py_None) ? NULL : (Py_INCREF(curexc_value), curexc_value);
    ret->ts->curexc_traceback = (curexc_traceback == Py_None) ? NULL 
        : (Py_INCREF(curexc_traceback), curexc_traceback);

    ret->ts->exc_type = (exc_type == Py_None) ? NULL : (Py_INCREF(exc_type), exc_type);
    ret->ts->exc_value = (exc_value == Py_None) ? NULL : (Py_INCREF(exc_value), exc_value);
    ret->ts->exc_traceback = (exc_traceback == Py_None) ? NULL 
        : (Py_INCREF(exc_traceback), exc_traceback);

    ret->req.args = (reqargs == Py_None) ? NULL : (Py_INCREF(reqargs), reqargs);
    ret->req.kwargs = (reqkwargs == Py_None) ? NULL : (Py_INCREF(reqkwargs), reqkwargs);
    ret->req.type = reqtype;

    ret->state = PyInt_AS_LONG(state);
    ret->runfunc = func == Py_None ? NULL : (Py_INCREF(func), func);
    ret->sleep_elapsed = PyInt_AS_LONG(sleep_elapsed);
    ret->small_stack = PyInt_AS_LONG(small_stack);
    ret->stack_depth = PyInt_AS_LONG(stack_depth);

    if(ret->state == PYTASK_STATE_RUNNING) {

        int flags = TASK_MAIN_THREAD_PINNED;
        if(!ret->small_stack)
            flags |= TASK_BIG_STACK;
    
        assert(ret->ts->frame->f_stacktop);
        ret->tid = Sched_Create(16, py_task, ret, NULL, flags);

        if(ret->tid == NULL_TID) {
            PyErr_SetString(PyExc_RuntimeError, "Unable to start fiber for task.");
            Py_CLEAR(ret);
            goto fail_unpickle;
        }

        /* Retain a running task object until it finishes */
        Py_INCREF(ret);

        int status;
        khiter_t k = kh_put(task, s_tid_task_map, ret->tid, &status);
        assert(status != -1 && status != 0);
        kh_value(s_tid_task_map, k) = ret;

        bool success = Sched_RunSync(ret->tid);
        assert(success);
    }

    SDL_RWops *stream = SDL_RWFromConstMem(str, len);
    if(!stream) {
        Py_XDECREF(ret);
        goto fail_unpickle;
    }
    nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    SDL_RWclose(stream);

fail_unpickle:
    if(!ret && !PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError, "Could not unpickle pf.Task instance");
    }
    Py_XDECREF(small_stack);
    Py_XDECREF(sleep_elapsed);
    Py_XDECREF(stack_depth);
    Py_XDECREF(regname);
    Py_XDECREF(func);
    Py_XDECREF(request);
    Py_XDECREF(ts);
    Py_XDECREF(state);
fail_args:

    if(ret) {
        PyObject *rval = Py_BuildValue("(Oi)", ret, (int)nread);
        Py_DECREF(ret);
        return rval;
    }else{
        return NULL;
    }
}

static PyObject *PyTask_run(PyTaskObject *self)
{
    ASSERT_IN_MAIN_THREAD();

    int flags = TASK_MAIN_THREAD_PINNED;
    if(!self->small_stack)
        flags |= TASK_BIG_STACK;

    self->tid = Sched_Create(16, py_task, self, NULL, flags);

    if(self->tid == NULL_TID) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to start fiber for task.");
        return NULL;
    }

    /* Retain a running task object until it finishes */
    Py_INCREF(self);

    int status;
    khiter_t k = kh_put(task, s_tid_task_map, self->tid, &status);
    assert(status != -1 && status != 0);
    kh_value(s_tid_task_map, k) = self;

    Py_RETURN_NONE;
}

static PyObject *PyTask_wait(PyTaskObject *self, PyObject *args)
{
    ASSERT_IN_MAIN_THREAD();

    if(self->state != PYTASK_STATE_RUNNING || Sched_ActiveTID() != self->tid) {
        PyErr_SetString(PyExc_RuntimeError, 
            "The 'wait' method can only be called from the context of the __run__ method.");
        return NULL;
    }

    PyObject *task;
    if(!PyArg_ParseTuple(args, "O", &task)
    || !PyObject_IsInstance(task, (PyObject*)&PyTask_type)) {
        PyErr_SetString(PyExc_TypeError, 
            "Expecting one argument: a pf.Task instance (task to wait on).");
        return NULL;
    }

    if(((PyTaskObject*)task)->state == PYTASK_STATE_FINISHED) {
        PyErr_SetString(PyExc_RuntimeError, 
            "Cannot wait on a task that's already finished.");
        return NULL;
    }

    pytask_req_set(self, NULL, NULL, PYREQ_YIELD);
    pytask_pop_ctx(self);

    PyObject *arg;
    int source;
    do{
        arg = Task_AwaitEvent(EVENT_SCRIPT_TASK_FINISHED, &source);
        if(!PyTuple_Check(arg) || PyTuple_GET_SIZE(arg) != 2)
            continue;
    }while(PyTuple_GET_ITEM(arg, 0) != task);

    pytask_push_ctx(self);
    pytask_req_clear(self);

    PyObject *ret = PyTuple_GET_ITEM(arg, 1);
    Py_INCREF(ret);
    Py_DECREF(arg);
    return ret;
}

static PyObject *PyTask_yield(PyTaskObject *self)
{
    ASSERT_IN_MAIN_THREAD();

    if(self->state != PYTASK_STATE_RUNNING || Sched_ActiveTID() != self->tid) {
        PyErr_SetString(PyExc_RuntimeError, 
            "The 'yield_' method can only be called from the context of the __run__ method.");
        return NULL;
    }

    pytask_req_set(self, NULL, NULL, PYREQ_YIELD);
    pytask_pop_ctx(self);

    Task_Yield();

    pytask_push_ctx(self);
    pytask_req_clear(self);

    Py_RETURN_NONE;
}

static PyObject *PyTask_send(PyTaskObject *self, PyObject *args)
{
    ASSERT_IN_MAIN_THREAD();

    if(self->state != PYTASK_STATE_RUNNING || Sched_ActiveTID() != self->tid) {
        PyErr_SetString(PyExc_RuntimeError, 
            "The 'send' method can only be called from the context of the __run__ method.");
        return NULL;
    }

    PyObject *recepient, *message;
    if(!PyArg_ParseTuple(args, "OO", &recepient, &message)
    || !PyObject_IsInstance(recepient, (PyObject*)&PyTask_type)) {
        PyErr_SetString(PyExc_TypeError, 
            "Expecting two arguments: a pf.Task instance (repecient) and a message object.");
        return NULL;
    }

    if(((PyTaskObject*)recepient)->state != PYTASK_STATE_RUNNING) {
        PyErr_SetString(PyExc_RuntimeError, "Can only send messages to a running task.");
        return NULL;
    }

    uint32_t recepient_tid = ((PyTaskObject*)recepient)->tid;
    PyObject *reply = NULL;
    Py_INCREF(message); /* retain the message object */

    pytask_req_set(self, args, NULL, PYREQ_SEND);
    pytask_pop_ctx(self);

    Task_Send(recepient_tid, &message, sizeof(message), &reply, sizeof(reply));

    pytask_push_ctx(self);
    pytask_req_clear(self);

    return reply; /* steal the reply object reference */
}

static PyObject *PyTask_receive(PyTaskObject *self)
{
    ASSERT_IN_MAIN_THREAD();

    if(self->state != PYTASK_STATE_RUNNING || Sched_ActiveTID() != self->tid) {
        PyErr_SetString(PyExc_RuntimeError, 
            "The 'receive' method can only be called from the context of the __run__ method.");
        return NULL;
    }

    uint32_t from_tid;
    PyObject *message;

    pytask_req_set(self, NULL, NULL, PYREQ_RECEIVE);
    pytask_pop_ctx(self);

    Task_Receive(&from_tid, &message, sizeof(message));

    pytask_push_ctx(self);
    pytask_req_clear(self);

    khiter_t k = kh_get(task, s_tid_task_map, from_tid);
    assert(k != kh_end(s_tid_task_map));
    PyTaskObject *from = kh_value(s_tid_task_map, k);

    PyObject *ret = PyTuple_New(2);
    if(!ret) {
        Py_DECREF(message);
        return NULL;
    }

    Py_INCREF(from);
    PyTuple_SetItem(ret, 0, (PyObject*)from);
    PyTuple_SetItem(ret, 1, message); /* steal message ref */
    return ret;
}

static PyObject *PyTask_reply(PyTaskObject *self, PyObject *args)
{
    ASSERT_IN_MAIN_THREAD();

    if(self->state != PYTASK_STATE_RUNNING || Sched_ActiveTID() != self->tid) {
        PyErr_SetString(PyExc_RuntimeError, 
            "The 'reply' method can only be called from the context of the __run__ method.");
        return NULL;
    }

    PyObject *recepient, *response;
    if(!PyArg_ParseTuple(args, "OO", &recepient, &response)
    || !PyObject_IsInstance(recepient, (PyObject*)&PyTask_type)) {
        PyErr_SetString(PyExc_TypeError, 
            "Expecting two arguments: a pf.Task instance (repecient) and a message object.");
        return NULL;
    }

    if(((PyTaskObject*)recepient)->state != PYTASK_STATE_RUNNING) {
        PyErr_SetString(PyExc_RuntimeError, "Can only reply to a running task.");
        return NULL;
    }

    uint32_t recepient_tid = ((PyTaskObject*)recepient)->tid;
    Py_INCREF(response); /* retain response */

    pytask_req_set(self, args, NULL, PYREQ_REPLY);
    pytask_pop_ctx(self);

    Task_Reply(recepient_tid, &response, sizeof(response));

    pytask_push_ctx(self);
    pytask_req_clear(self);

    Py_RETURN_NONE;
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

    pytask_req_set(self, args, NULL, PYREQ_AWAIT_EVENT);
    pytask_pop_ctx(self);

    int source;
    void *arg = Task_AwaitEvent(event, &source);

    pytask_push_ctx(self);
    pytask_req_clear(self);

    if(source == ES_ENGINE) {
        return S_WrapEngineEventArg(event, arg);
    }else{
        return (PyObject*)arg; /* steal ref */
    }
}

static PyObject *PyTask_sleep(PyTaskObject *self, PyObject *args)
{
    ASSERT_IN_MAIN_THREAD();

    if(self->state != PYTASK_STATE_RUNNING || Sched_ActiveTID() != self->tid) {
        PyErr_SetString(PyExc_RuntimeError, 
            "The 'sleep' method can only be called from the context of the __run__ method.");
        return NULL;
    }

    int ms;
    if(!PyArg_ParseTuple(args, "i", &ms)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one integer argument (number of milliseconds)");
        return NULL;
    }

    pytask_req_set(self, args, NULL, PYREQ_SLEEP);
    pytask_pop_ctx(self);

    Task_Sleep(ms - self->sleep_elapsed);

    self->sleep_elapsed = 0;
    pytask_push_ctx(self);
    pytask_req_clear(self);

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

    self->regname = pf_strdup(name);

    pytask_req_set(self, args, NULL, PYREQ_REGISTER);
    pytask_pop_ctx(self);

    Task_Register(name);

    pytask_push_ctx(self);
    pytask_req_clear(self);

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

    pytask_req_set(self, args, kwargs, PYREQ_WHOIS);
    pytask_pop_ctx(self);

    uint32_t tid = Task_WhoIs(name, blocking);

    pytask_push_ctx(self);
    pytask_req_clear(self);

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

static PyObject *PyTask_get_completed(PyTaskObject *self, void *closure)
{
    if(self->state == PYTASK_STATE_FINISHED) {
        Py_RETURN_TRUE;
    }else{
        Py_RETURN_FALSE;
    }
}

static void on_update_start(void *user, void *event)
{
    uint32_t elapsed = Perf_LastFrameMS();
    PyTaskObject *curr;
    uint32_t key;
    (void)key;

    kh_foreach(s_tid_task_map, key, curr, {
        if(curr->req.type == PYREQ_SLEEP)
            curr->sleep_elapsed += elapsed;
    });
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
    E_Global_Register(EVENT_UPDATE_START, on_update_start, NULL, G_RUNNING);
    return true;
}

void S_Task_Shutdown(void)
{
    kh_destroy(task, s_tid_task_map);
    E_Global_Unregister(EVENT_UPDATE_START, on_update_start);
}

void S_Task_Clear(void)
{
    PyTaskObject *curr;
    kh_foreach(s_tid_task_map, (uint32_t){0}, curr, {
        Py_DECREF(curr);
    });
    kh_clear(task, s_tid_task_map);
}

PyObject *S_Task_GetAll(void)
{
    PyObject *ret = PyTuple_New(kh_size(s_tid_task_map));
    if(!ret)
        return NULL;

    uint32_t key;
    PyTaskObject *curr;
    (void)key;

    size_t nset = 0;
    kh_foreach(s_tid_task_map, key, curr, {
        Py_INCREF(curr);
        PyTuple_SetItem(ret, nset++, (PyObject*)curr);
    });

    assert(nset == PyTuple_GET_SIZE(ret));
    return ret;
}

void S_Task_MaybeExit(void)
{
    uint32_t tid = Sched_ActiveTID();
    khiter_t k= kh_get(task, s_tid_task_map, tid);
    if(k == kh_end(s_tid_task_map))
        return;
    PyTaskObject *self = kh_value(s_tid_task_map, k);
    pytask_req_set(self, NULL, NULL, PYREQ_YIELD);
    pytask_pop_ctx(self);
}

void S_Task_MaybeEnter(void)
{
    uint32_t tid = Sched_ActiveTID();
    khiter_t k= kh_get(task, s_tid_task_map, tid);
    if(k == kh_end(s_tid_task_map))
        return;
    PyTaskObject *self = kh_value(s_tid_task_map, k);
    pytask_push_ctx(self);
    pytask_req_clear(self);
}

void S_Task_Flush(void)
{
    uint32_t tid;
    PyTaskObject *curr;
    int nran = 0;

    do{
        nran = 0;
        kh_foreach(s_tid_task_map, tid, curr, {
            if(curr->state != PYTASK_STATE_FINISHED && Sched_IsReady(tid)) {
                Sched_RunSync(tid);
                nran++;
            }
        });
    
    }while(nran > 0);
}

