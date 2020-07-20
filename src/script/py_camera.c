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

#include "py_camera.h"
#include "../camera.h"
#include "../pf_math.h"
#include "../game/public/game.h"

#define CAM_DEFAULT_SPEED   (0.20f)
#define CAM_DEFAULT_SENS    (0.05f)


typedef struct {
    PyObject_HEAD
    struct camera *cam;
    enum cam_mode  mode;
}PyCameraObject;

static PyObject *PyCamera_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyCamera_dealloc(PyCameraObject *self);
static int       PyCamera_init(PyCameraObject *self, PyObject *args, PyObject *kwds);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyObject *s_active_cam = NULL;

static PyMethodDef PyCamera_methods[] = {
    {NULL}  /* Sentinel */
};

static PyGetSetDef PyCamera_getset[] = {
    {NULL}  /* Sentinel */
};

static PyTypeObject PyCamera_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.Camera",               /* tp_name */
    sizeof(PyCameraObject),    /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)PyCamera_dealloc, /* tp_dealloc */
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
    "Permafrost Engine camera object.                               \n" 
    "                                                               \n"
    "The camera takes the following (optional) keyword  arguments   \n"
    "in its' constructor:                                           \n"
    "                                                               \n"
    "  - mode {pf.CAM_MODE_RTS, pf.CAM_MODE_FPS, pf.CAM_MODE_FREE}  \n"
    "  - position (tuple of 3 floats)                               \n"
    "  - pitch (float)                                              \n"
    "  - yaw (float)                                                \n"
    "  - speed (float)                                              \n"
    "  - sensitivity (float)                                        \n"
    ,                          /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyCamera_methods,          /* tp_methods */
    0,                         /* tp_members */
    PyCamera_getset,           /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)PyCamera_init,   /* tp_init */
    0,                         /* tp_alloc */
    PyCamera_new,              /* tp_new */
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static PyObject *PyCamera_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    struct camera *cam = Camera_New();
    if(!cam) {
        PyErr_NoMemory();
        return NULL;
    }

    PyCameraObject *self = (PyCameraObject*)type->tp_alloc(type, 0);
    if(!self) {
        assert(PyErr_Occurred());
        return NULL;
    }

    static char *kwlist[] = {"mode", "position", "pitch", "yaw", "speed", "sensitivity", NULL};
    enum cam_mode mode = CAM_MODE_FREE;
    PyObject *obj;
    float a, b, c, d;

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "|iOffff", kwlist, &mode, &obj, &a, &b, &c, &d)) {
        assert(PyErr_Occurred());
        return NULL;
    }

    self->mode = mode;
    self->cam = cam;
    return (PyObject*)self;
}

static void PyCamera_dealloc(PyCameraObject *self)
{
    /* The active camera is owned and freed by the core gamestate */
    if(self->cam != G_GetActiveCamera()) {
        Camera_Free(self->cam);
    }
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int PyCamera_init(PyCameraObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"mode", "position", "pitch", "yaw", "speed", "sensitivity", NULL};

    PyObject *pos_tuple = NULL;
    vec3_t pos = Camera_GetPos(self->cam);

    int mode;
    float pitch = Camera_GetPitch(self->cam);
    float yaw = Camera_GetYaw(self->cam);
    float speed = CAM_DEFAULT_SPEED;
    float sens = CAM_DEFAULT_SENS;

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "|iOffff", kwlist, 
        &mode, &pos_tuple, &pitch, &yaw, &speed, &sens)) {
        assert(PyErr_Occurred());
        return -1;
    }

    if(pos_tuple 
    && !(PyTuple_Check(pos_tuple) 
        || PyTuple_GET_SIZE(pos_tuple) != 3 
        || !PyFloat_Check(PyTuple_GET_ITEM(pos_tuple, 0))
        || !PyFloat_Check(PyTuple_GET_ITEM(pos_tuple, 1))
        || !PyFloat_Check(PyTuple_GET_ITEM(pos_tuple, 2)))) {
        PyErr_SetString(PyExc_TypeError, "position keyword argument must be a tuple of 3 floats.");
        return -1;
    }

    if(pos_tuple) {
        pos = (vec3_t) {
            PyFloat_AS_DOUBLE(PyTuple_GET_ITEM(pos_tuple, 0)),
            PyFloat_AS_DOUBLE(PyTuple_GET_ITEM(pos_tuple, 1)),
            PyFloat_AS_DOUBLE(PyTuple_GET_ITEM(pos_tuple, 2)),
        };
    }

    Camera_SetPos(self->cam, pos);
    Camera_SetPitchAndYaw(self->cam, pitch, yaw);
    Camera_SetSpeed(self->cam, speed);
    Camera_SetSens(self->cam, sens);

    return 0;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void S_Camera_PyRegister(PyObject *module)
{
    if(PyType_Ready(&PyCamera_type) < 0)
        return;
    Py_INCREF(&PyCamera_type);
    PyModule_AddObject(module, "Camera", (PyObject*)&PyCamera_type);
}

bool S_Camera_Init(void)
{
    s_active_cam = PyCamera_type.tp_alloc(&PyCamera_type, 0);
    if(!s_active_cam) {
        assert(PyErr_Occurred());
        return false;
    }

    ((PyCameraObject*)s_active_cam)->cam = G_GetActiveCamera();
    ((PyCameraObject*)s_active_cam)->mode = G_GetCameraMode();

    return true;
}

void S_Camera_Shutdown(void)
{
    Py_DECREF(s_active_cam);
    s_active_cam = NULL;
}

PyObject *S_Camera_GetActive(void)
{
    Py_INCREF(s_active_cam);
    return s_active_cam;
}

bool S_Camera_SetActive(PyObject *cam)
{
    if(cam == s_active_cam)
        return true;

    if(!PyType_IsSubtype(cam->ob_type, &PyCamera_type)) {
        PyErr_SetString(PyExc_TypeError, "The active camera must be a subclass of the pf.Camera type");
        return false;
    }

    Py_DECREF(s_active_cam);
    s_active_cam = cam;
    Py_INCREF(s_active_cam);

    PyCameraObject *pycam = (PyCameraObject*)s_active_cam;
    G_SetActiveCamera(pycam->cam, pycam->mode);
    return true;
}

