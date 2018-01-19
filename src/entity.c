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
#include "entity.h" 
#include "asset_load.h"


typedef struct {
    PyObject_HEAD
    struct entity *ent;
}PyEntityObject;

static PyObject *PyEntity_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyEntity_dealloc(PyEntityObject *self);
static PyObject *PyEntity_get_name(PyEntityObject *self, void *closure);
static int       PyEntity_set_name(PyEntityObject *self, PyObject *value, void *closure);
static PyObject *PyEntity_get_pos(PyEntityObject *self, void *closure);
static int       PyEntity_set_pos(PyEntityObject *self, PyObject *value, void *closure);
static PyObject *PyEntity_get_scale(PyEntityObject *self, void *closure);
static int       PyEntity_set_scale(PyEntityObject *self, PyObject *value, void *closure);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

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
    0,                         /* tp_methods */
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

/*****************************************************************************/
/* PYTHON HOOKS                                                              */
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

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void Entity_ModelMatrix(const struct entity *ent, mat4x4_t *out)
{
    mat4x4_t trans, scale;

    PFM_Mat4x4_MakeTrans(ent->pos.x, ent->pos.y, ent->pos.z, &trans);
    PFM_Mat4x4_MakeScale(ent->scale.x, ent->scale.y, ent->scale.z, &scale);
    PFM_Mat4x4_Mult4x4(&trans, &scale, out);
}

void Entity_PyRegister(PyObject *module)
{
    if(PyType_Ready(&PyEntity_type) < 0)
        return;

    Py_INCREF(&PyEntity_type);
    PyModule_AddObject(module, "Entity", (PyObject*)&PyEntity_type);
}

