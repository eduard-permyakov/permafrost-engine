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

#include <Python.h> /* must be first */
#include "entity_script.h" 
#include "../entity.h"
#include "../asset_load.h"
#include "../anim/public/anim.h"
#include "../game/public/game.h"
#include "../event/public/event.h"

typedef struct {
    PyObject_HEAD
    struct entity *ent;
}PyEntityObject;

typedef struct {
    PyEntityObject super; 
}PyAnimEntityObject;

static PyObject *PyEntity_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyEntity_dealloc(PyEntityObject *self);
static PyObject *PyEntity_get_name(PyEntityObject *self, void *closure);
static int       PyEntity_set_name(PyEntityObject *self, PyObject *value, void *closure);
static PyObject *PyEntity_get_pos(PyEntityObject *self, void *closure);
static int       PyEntity_set_pos(PyEntityObject *self, PyObject *value, void *closure);
static PyObject *PyEntity_get_scale(PyEntityObject *self, void *closure);
static int       PyEntity_set_scale(PyEntityObject *self, PyObject *value, void *closure);
static PyObject *PyEntity_activate(PyEntityObject *self);
static PyObject *PyEntity_register(PyEntityObject *self, PyObject *args);
static PyObject *PyEntity_unregister(PyEntityObject *self, PyObject *args);
static PyObject *PyEntity_notify(PyEntityObject *self, PyObject *args);

static int       PyAnimEntity_init(PyAnimEntityObject *self, PyObject *args, PyObject *kwds);
static PyObject *PyAnimEntity_play_anim(PyAnimEntityObject *self, PyObject *args);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyMethodDef PyEntity_methods[] = {
    {"activate", 
    (PyCFunction)PyEntity_activate, METH_NOARGS,
    "Add the entity to the game world, making it visible and allowing other entities "
    "to interact with it in the simulation. The activated entity will be removed from "
    "the game world when no more references to it remain in scope. (ex: Using 'del' "
    "when you have a single reference)"},

    {"register", 
    (PyCFunction)PyEntity_register, METH_VARARGS,
    "Registers the specified callable to be invoked when an event of the specified type "
    "is sent to this entity." },

    {"unregister", 
    (PyCFunction)PyEntity_unregister, METH_VARARGS,
    "Unregisters a callable previously registered to be invoked on the specified event."},

    {"notify", 
    (PyCFunction)PyEntity_notify, METH_VARARGS,
    "Send a specific event to an entity in order to invoke the entity's event handlers."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyEntity_getset[] = {
    {"name",
    (getter)PyEntity_get_name, (setter)PyEntity_set_name,
    "Custom name given to this enitty.",
    NULL},
    {"pos",
    (getter)PyEntity_get_pos, (setter)PyEntity_set_pos,
    "The XYZ position in worldspace coordinates.",
    NULL},
    {"scale",
    (getter)PyEntity_get_scale, (setter)PyEntity_set_scale,
    "The XYZ scaling factors.",
    NULL},
    {NULL}  /* Sentinel */
};

static PyMethodDef PyAnimEntity_methods[] = {
    {"play_anim", 
    (PyCFunction)PyAnimEntity_play_anim, METH_VARARGS,
    "Play the animation clip with the specified name." },
    {NULL}  /* Sentinel */
};


static PyTypeObject PyEntity_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.Entity",               /* tp_name */
    sizeof(PyEntityObject),    /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)PyEntity_dealloc,/* tp_dealloc */
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
    "Permafrost Engine generic game entity.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyEntity_methods,          /* tp_methods */
    0,                         /* tp_members */
    PyEntity_getset,           /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    PyEntity_new,              /* tp_new */
};

static PyTypeObject PyAnimEntity_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "pf.AnimEntity",
    .tp_basicsize = sizeof(PyAnimEntityObject), 
    .tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc       = "Permafrost Engine animated entity.",
    .tp_methods   = PyAnimEntity_methods,
    .tp_base      = &PyEntity_type,
    .tp_init      = (initproc)PyAnimEntity_init,
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static PyObject *PyEntity_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyEntityObject *self;
    const char *dirpath, *filename, *name;

    self = (PyEntityObject*)type->tp_alloc(type, 0);

    if(!PyArg_ParseTuple(args, "sss", &dirpath, &filename, &name)) {
        Py_DECREF(self); 
        return NULL;
    }

    extern const char *g_basepath;
    char entity_path[512];
    strcpy(entity_path, g_basepath);
    strcat(entity_path, dirpath);

    self->ent = AL_EntityFromPFObj(entity_path, filename, name);
    if(!self->ent) {
        Py_DECREF(self); 
        return NULL;
    }

    return (PyObject*)self;
}

static void PyEntity_dealloc(PyEntityObject *self)
{
    assert(self->ent);

    G_RemoveEntity(self->ent);
    AL_EntityFree(self->ent);

    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *PyEntity_get_name(PyEntityObject *self, void *closure)
{
    return Py_BuildValue("s", self->ent->name);
}

static int PyEntity_set_name(PyEntityObject *self, PyObject *value, void *closure)
{
    if(!PyObject_IsInstance(value, (PyObject*)&PyString_Type)){
        PyErr_SetString(PyExc_TypeError, "Argument must be a string.");
        return -1;
    }

    const char *s = PyString_AsString(value);
    if(strlen(s) >= ENTITY_NAME_LEN){
        PyErr_SetString(PyExc_TypeError, "Name string is too long.");
        return -1;
    }

    strcpy(self->ent->name, s);
    return 0;
}

static PyObject *PyEntity_get_pos(PyEntityObject *self, void *closure)
{
    return Py_BuildValue("[f,f,f]", self->ent->pos.x, self->ent->pos.y, self->ent->pos.z);
}

static int PyEntity_set_pos(PyEntityObject *self, PyObject *value, void *closure)
{
    if(!PyList_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a list.");
        return -1;
    }
    
    Py_ssize_t len = PyList_Size(value);
    if(len != 3) {
        PyErr_SetString(PyExc_TypeError, "Argument must have a size of 3."); 
        return -1;
    }

    for(int i = 0; i < len; i++) {

        PyObject *item = PyList_GetItem(value, i);
        if(!PyFloat_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "List items must be floats.");
            return -1;
        }

        self->ent->pos.raw[i] = PyFloat_AsDouble(item);
    }

    return 0;
}

static PyObject *PyEntity_get_scale(PyEntityObject *self, void *closure)
{
    return Py_BuildValue("[f,f,f]", self->ent->scale.x, self->ent->scale.y, self->ent->scale.z);
}

static int PyEntity_set_scale(PyEntityObject *self, PyObject *value, void *closure)
{
    if(!PyList_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a list.");
        return -1;
    }
    
    Py_ssize_t len = PyList_Size(value);
    if(len != 3) {
        PyErr_SetString(PyExc_TypeError, "Argument must have a size of 3."); 
        return -1;
    }

    for(int i = 0; i < len; i++) {

        PyObject *item = PyList_GetItem(value, i);
        if(!PyFloat_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "List items must be floats.");
            return -1;
        }

        self->ent->scale.raw[i] = PyFloat_AsDouble(item);
    }

    return 0;
}

static PyObject *PyEntity_activate(PyEntityObject *self)
{
    assert(self->ent);
    G_AddEntity(self->ent);
    Py_RETURN_NONE;
}

static PyObject *PyEntity_register(PyEntityObject *self, PyObject *args)
{
    enum eventtype event;
    PyObject *callable, *user_arg;

    if(!PyArg_ParseTuple(args, "iOO", &event, &callable, &user_arg)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a tuple of an integer and two objects.");
        return NULL;
    }

    if(!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "Second argument must be callable.");
        return NULL;
    }

    Py_INCREF(callable);
    Py_INCREF(user_arg);

    bool ret = E_Entity_ScriptRegister(event, self->ent->uid, callable, user_arg);
    assert(ret == true);
    Py_RETURN_NONE;
}

static PyObject *PyEntity_unregister(PyEntityObject *self, PyObject *args)
{
    enum eventtype event;
    PyObject *callable;

    if(!PyArg_ParseTuple(args, "iO", &event, &callable)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a tuple of an integer and one object.");
        return NULL;
    }

    if(!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "Second argument must be callable.");
        return NULL;
    }

    bool ret = E_Entity_ScriptUnregister(event, self->ent->uid, callable);
    Py_RETURN_NONE;
}

static PyObject *PyEntity_notify(PyEntityObject *self, PyObject *args)
{
    enum eventtype event;
    PyObject *arg;

    if(!PyArg_ParseTuple(args, "iO", &event, &arg)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a tuple of an integer and one object.");
        return NULL;
    }

    Py_INCREF(arg);

    E_Entity_Notify(event, self->ent->uid, arg, ES_SCRIPT);
    Py_RETURN_NONE;
}

static int PyAnimEntity_init(PyAnimEntityObject *self, PyObject *args, PyObject *kwds)
{
    const char *dirpath, *filename, *name, *clipname;
    if(!PyArg_ParseTuple(args, "ssss", &dirpath, &filename, &name, &clipname)) {
        return -1;
    }

    A_InitCtx(self->super.ent, clipname, 24);
    return 0;
}

static PyObject *PyAnimEntity_play_anim(PyAnimEntityObject *self, PyObject *args)
{
    const char *clipname;
    if(!PyArg_ParseTuple(args, "s", &clipname)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string.");
        return NULL;
    }

    A_SetActiveClip(self->super.ent, clipname, ANIM_MODE_LOOP, 24);
    Py_RETURN_NONE;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void S_Entity_PyRegister(PyObject *module)
{
    if(PyType_Ready(&PyEntity_type) < 0)
        return;
    Py_INCREF(&PyEntity_type);
    PyModule_AddObject(module, "Entity", (PyObject*)&PyEntity_type);

    if(PyType_Ready(&PyAnimEntity_type) < 0)
        return;
    Py_INCREF(&PyAnimEntity_type);
    PyModule_AddObject(module, "AnimEntity", (PyObject*)&PyAnimEntity_type);
}

