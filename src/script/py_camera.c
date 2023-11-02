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

#include "py_camera.h"
#include "py_pickle.h"
#include "../camera.h"
#include "../pf_math.h"
#include "../map/public/map.h"
#include "../lib/public/SDL_vec_rwops.h"
#include "../game/public/game.h"

#define CAM_DEFAULT_SPEED   (0.20f)
#define CAM_DEFAULT_SENS    (0.05f)

#define CHK_TRUE(_pred, _label) do{ if(!(_pred)) goto _label; }while(0)


typedef struct {
    PyObject_HEAD
    struct camera *cam;
    enum cam_mode  mode;
}PyCameraObject;

static PyObject *PyCamera_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyCamera_dealloc(PyCameraObject *self);
static int       PyCamera_init(PyCameraObject *self, PyObject *args, PyObject *kwds);

static PyObject *PyCamera_center_over_location(PyCameraObject *self, PyObject *args);
static PyObject *PyCamera_pickle(PyCameraObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyCamera_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

static PyObject *PyCamera_get_mode(PyCameraObject *self, void *closure);
static PyObject *PyCamera_get_pos(PyCameraObject *self, void *closure);
static int       PyCamera_set_pos(PyCameraObject *self, PyObject *value, void *closure);
static PyObject *PyCamera_get_dir(PyCameraObject *self, void *closure);
static PyObject *PyCamera_get_pitch(PyCameraObject *self, void *closure);
static int       PyCamera_set_pitch(PyCameraObject *self, PyObject *value, void *closure);
static PyObject *PyCamera_get_yaw(PyCameraObject *self, void *closure);
static int       PyCamera_set_yaw(PyCameraObject *self, PyObject *value, void *closure);
static PyObject *PyCamera_get_speed(PyCameraObject *self, void *closure);
static int       PyCamera_set_speed(PyCameraObject *self, PyObject *value, void *closure);
static PyObject *PyCamera_get_sensitivity(PyCameraObject *self, void *closure);
static int       PyCamera_set_sensitivity(PyCameraObject *self, PyObject *value, void *closure);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyObject *s_active_cam = NULL;

static PyMethodDef PyCamera_methods[] = {
    {"center_over_location", 
    (PyCFunction)PyCamera_center_over_location, METH_VARARGS,
    "Position the camera over the specified (X, Z) map location, by only shifting it in the XZ plane."},

    {"__pickle__", 
    (PyCFunction)PyCamera_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine camera object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyCamera_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.Camera instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyCamera_getset[] = {
    {"mode",
    (getter)PyCamera_get_mode, NULL,
    "The mode determines which controller is installed when the camera is activated. "
    "Can be one of pf.CAM_MODE_RTS, pf.CAM_MODE_FPS, pf.CAM_MODE_FREE",
    NULL},
    {"position",
    (getter)PyCamera_get_pos, (setter)PyCamera_set_pos,
    "The current worldspace position of the camera", 
    NULL},
    {"direction",
    (getter)PyCamera_get_dir, NULL,
    "The current worldspace direction of the camera", 
    NULL},
    {"pitch",
    (getter)PyCamera_get_pitch, (setter)PyCamera_set_pitch,
    "The camera's pitch (in degrees)", 
    NULL},
    {"yaw",
    (getter)PyCamera_get_yaw, (setter)PyCamera_set_yaw,
    "The camera's yaw (in degrees)",
    NULL},
    {"speed",
    (getter)PyCamera_get_speed, (setter)PyCamera_set_speed,
    "The camera's speed (in OpenGL units / ms)",
    NULL},
    {"sensitivity",
    (getter)PyCamera_get_sensitivity, (setter)PyCamera_set_sensitivity,
    "The camera's sensitivity (how fast direction can be changed)",
    NULL},
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

static PyObject *PyCamera_center_over_location(PyCameraObject *self, PyObject *args)
{
    vec2_t target;
    if(!PyArg_ParseTuple(args, "(ff)", &target.x, &target.z)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of two floats (XZ map position).");
        return NULL;
    }

    vec3_t dir = Camera_GetDir(self->cam);
    vec3_t pos = Camera_GetPos(self->cam);
    vec3_t map_intersect;

    bool hit = M_Raycast_CameraIntersecCoord(self->cam, &map_intersect);
    if(!hit) {
        /* in this case, find the intersection with the Y=0 plane */

        if(dir.y >= 0.0f) {
            PyErr_SetString(PyExc_RuntimeError, "The camera is facing upwards. Unable ot center over map position.");
            return NULL;
        }
        float t = fabs(pos.y / dir.y);
        map_intersect = (vec3_t) {
            pos.x + t * dir.x,
            0.0f,
            pos.z + t * dir.z
        };
    }

    vec2_t delta_xz;
    vec2_t map_intersect_xz = (vec2_t){map_intersect.x, map_intersect.z};
    PFM_Vec2_Sub(&target, &map_intersect_xz, &delta_xz);

    vec3_t newpos;
    vec3_t delta = (vec3_t){delta_xz.x, 0.0f, delta_xz.z};
    PFM_Vec3_Add(&pos, &delta, &newpos);

    Camera_SetPos(self->cam, newpos);
    Py_RETURN_NONE;
}

static PyObject *PyCamera_pickle(PyCameraObject *self, PyObject *args, PyObject *kwargs)
{
    bool status;
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);

    PyObject *active = PyInt_FromLong(!!((PyObject*)self == s_active_cam));
    CHK_TRUE(active, fail_pickle);
    status = S_PickleObjgraph(active, stream);
    Py_DECREF(active);
    CHK_TRUE(status, fail_pickle);

    PyObject *mode = PyInt_FromLong(self->mode);
    CHK_TRUE(mode, fail_pickle);
    status = S_PickleObjgraph(mode, stream);
    Py_DECREF(mode);
    CHK_TRUE(status, fail_pickle);

    vec3_t cam_pos = Camera_GetPos(self->cam);
    PyObject *position = Py_BuildValue("(fff)", cam_pos.x, cam_pos.y, cam_pos.z);
    CHK_TRUE(position, fail_pickle);
    status = S_PickleObjgraph(position, stream);
    Py_DECREF(position);
    CHK_TRUE(status, fail_pickle);

    PyObject *pitch = PyFloat_FromDouble(Camera_GetPitch(self->cam));
    CHK_TRUE(pitch, fail_pickle);
    status = S_PickleObjgraph(pitch, stream);
    Py_DECREF(pitch);
    CHK_TRUE(status, fail_pickle);

    PyObject *yaw = PyFloat_FromDouble(Camera_GetYaw(self->cam));
    CHK_TRUE(yaw, fail_pickle);
    status = S_PickleObjgraph(yaw, stream);
    Py_DECREF(yaw);
    CHK_TRUE(status, fail_pickle);

    PyObject *speed = PyFloat_FromDouble(Camera_GetSpeed(self->cam));
    CHK_TRUE(speed, fail_pickle);
    status = S_PickleObjgraph(speed, stream);
    Py_DECREF(speed);
    CHK_TRUE(status, fail_pickle);

    PyObject *sens = PyFloat_FromDouble(Camera_GetSens(self->cam));
    CHK_TRUE(sens, fail_pickle);
    status = S_PickleObjgraph(sens, stream);
    Py_DECREF(sens);
    CHK_TRUE(status, fail_pickle);

    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));
fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    return ret;
}

static PyObject *PyCamera_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = NULL;
    PyObject *cam_obj = NULL;
    const char *str;
    Py_ssize_t len;
    char tmp;

    if(!PyArg_ParseTuple(args, "s#", &str, &len)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single string.");
        goto fail_args;
    }

    SDL_RWops *stream = SDL_RWFromConstMem(str, len);
    CHK_TRUE(stream, fail_args);

    PyObject *active = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *mode = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *position = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *pitch = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *yaw = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *speed = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *sensitivity = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    if(!active
    || !mode
    || !position
    || !pitch
    || !yaw
    || !speed
    || !sensitivity) {
        PyErr_SetString(PyExc_RuntimeError, "Could not unpickle internal state of pf.Camera instance");
        goto fail_unpickle;
    }

    if(!PyInt_Check(active)) {
        PyErr_SetString(PyExc_RuntimeError, "Unpickled 'active' field must be an integer type");
        goto fail_unpickle;
    }

    if(PyInt_AS_LONG(active)) {
        cam_obj = s_active_cam;
        Py_INCREF(cam_obj);
    }else{

        PyObject *cam_args = PyTuple_New(0);
        if(!cam_args)
            goto fail_unpickle;

        PyObject *cam_kwargs = Py_BuildValue("{s:O,s:O,s:O,s:O,s:O,s:O}", 
            "mode",         mode,
            "position",     position,
            "pitch",        pitch,
            "yaw",          yaw,
            "speed",        speed,
            "sensitivity",  sensitivity);
        if(!cam_kwargs) {
            Py_DECREF(cam_args);
            goto fail_unpickle;
        }

        cam_obj = PyObject_Call((PyObject*)&PyCamera_type, cam_args, cam_kwargs);
        Py_DECREF(cam_args);
        Py_DECREF(cam_kwargs);
    }

    if(!cam_obj)
        goto fail_unpickle;

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", cam_obj, (int)nread);

fail_unpickle:
    Py_XDECREF(active);
    Py_XDECREF(mode);
    Py_XDECREF(position);
    Py_XDECREF(pitch);
    Py_XDECREF(yaw);
    Py_XDECREF(sensitivity);
    Py_XDECREF(cam_obj);
    SDL_RWclose(stream);
fail_args:
    return ret;
}

static bool camera_check_readonly(PyCameraObject *self)
{
    if(self->mode != CAM_MODE_FREE) {
        PyErr_SetString(PyExc_RuntimeError, "This attribute is readonly except for cameras in the pf.CAM_MODE_FREE mode");
        return true;
    }
    return false;
}

static PyObject *PyCamera_get_mode(PyCameraObject *self, void *closure)
{
    return PyInt_FromLong(self->mode);
}

static PyObject *PyCamera_get_pos(PyCameraObject *self, void *closure)
{
    vec3_t pos = Camera_GetPos(self->cam);
    return Py_BuildValue("(fff)", pos.x, pos.y, pos.z);
}

static int PyCamera_set_pos(PyCameraObject *self, PyObject *value, void *closure)
{
    if(camera_check_readonly(self))
        return -1;

    if(!PyTuple_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple.");
        return false;
    }

    vec3_t newpos;
    if(!PyArg_ParseTuple(value, "fff", 
        &newpos.raw[0], &newpos.raw[1], &newpos.raw[2])) {
        return -1;
    }

    Camera_SetPos(self->cam, newpos);
    return 0;
}

static PyObject *PyCamera_get_dir(PyCameraObject *self, void *closure)
{
    vec3_t dir = Camera_GetDir(self->cam);
    return Py_BuildValue("(fff)", dir.x, dir.y, dir.z);
}

static PyObject *PyCamera_get_pitch(PyCameraObject *self, void *closure)
{
    return PyFloat_FromDouble(Camera_GetPitch(self->cam));
}

static int PyCamera_set_pitch(PyCameraObject *self, PyObject *value, void *closure)
{
    if(camera_check_readonly(self))
        return -1;

    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a float.");
        return -1;
    }

    Camera_SetPitchAndYaw(self->cam, PyFloat_AS_DOUBLE(value), Camera_GetYaw(self->cam));
    return 0;
}

static PyObject *PyCamera_get_yaw(PyCameraObject *self, void *closure)
{
    return PyFloat_FromDouble(Camera_GetYaw(self->cam));
}

static int PyCamera_set_yaw(PyCameraObject *self, PyObject *value, void *closure)
{
    if(camera_check_readonly(self))
        return -1;

    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a float.");
        return -1;
    }

    Camera_SetPitchAndYaw(self->cam, Camera_GetPitch(self->cam), PyFloat_AS_DOUBLE(value));
    return 0;
}

static PyObject *PyCamera_get_speed(PyCameraObject *self, void *closure)
{
    return PyFloat_FromDouble(Camera_GetSpeed(self->cam));
}

static int PyCamera_set_speed(PyCameraObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a float.");
        return -1;
    }

    Camera_SetSpeed(self->cam, PyFloat_AS_DOUBLE(value));
    return 0;
}

static PyObject *PyCamera_get_sensitivity(PyCameraObject *self, void *closure)
{
    return PyFloat_FromDouble(Camera_GetSens(self->cam));
}

static int PyCamera_set_sensitivity(PyCameraObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a float.");
        return -1;
    }

    Camera_SetSens(self->cam, PyFloat_AS_DOUBLE(value));
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
    ((PyCameraObject*)s_active_cam)->mode = CAM_MODE_RTS; /* default */

    return true;
}

void S_Camera_Shutdown(void)
{
    /* No-op for now */
}

void S_Camera_Clear(void)
{
    Py_CLEAR(s_active_cam);
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

