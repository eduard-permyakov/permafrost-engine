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

#include "py_region.h"
#include "py_entity.h"
#include "py_pickle.h"
#include "public/script.h"
#include "../sched.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/khash.h"
#include "../lib/public/mem.h"
#include "../lib/public/SDL_vec_rwops.h"
#include "../game/public/game.h"

#include <assert.h>


#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))
#define CHK_TRUE(_pred, _label) do{ if(!(_pred)) goto _label; }while(0)

KHASH_MAP_INIT_STR(PyObject, PyObject*)

typedef struct {
    PyObject_HEAD
    enum region_type type;
    const char *name;
}PyRegionObject;

static PyObject *PyRegion_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyRegion_dealloc(PyRegionObject *self);

static PyObject *PyRegion_curr_ents(PyRegionObject *self);
static PyObject *PyRegion_contains(PyRegionObject *self, PyObject *args);
static PyObject *PyRegion_explore(PyRegionObject *self, PyObject *args);
static PyObject *PyRegion_pickle(PyRegionObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyRegion_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

static PyObject *PyRegion_get_pos(PyRegionObject *self, void *closure);
static int       PyRegion_set_pos(PyRegionObject *self, PyObject *value, void *closure);
static PyObject *PyRegion_get_shown(PyRegionObject *self, void *closure);
static int       PyRegion_set_shown(PyRegionObject *self, PyObject *value, void *closure);
static PyObject *PyRegion_get_name(PyRegionObject *self, void *closure);
static PyObject *PyRegion_get_type(PyRegionObject *self, void *closure);
static PyObject *PyRegion_get_parameters(PyRegionObject *self, void *closure);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyMethodDef PyRegion_methods[] = {
    {"curr_ents", 
    (PyCFunction)PyRegion_curr_ents, METH_NOARGS,
    "Get a list of all the entities currently within the region."},

    {"contains", 
    (PyCFunction)PyRegion_contains, METH_VARARGS,
    "Returns True if the specified entity is currently within the region."},

    {"explore", 
    (PyCFunction)PyRegion_explore, METH_VARARGS,
    "Explore the Fog of War in the region for the specified faction."},

    {"__pickle__", 
    (PyCFunction)PyRegion_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine region object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyRegion_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.Region instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyRegion_getset[] = {
    {"position",
    (getter)PyRegion_get_pos, (setter)PyRegion_set_pos,
    "The current worldspace position of the region", 
    NULL},
    {"shown",
    (getter)PyRegion_get_shown, (setter)PyRegion_set_shown,
    "Boolean to control whether the region is rendered on the map surface", 
    NULL},
    {"name",
    (getter)PyRegion_get_name, NULL,
    "The name of the region.", 
    NULL},
    {"type",
    (getter)PyRegion_get_type, NULL,
    "The type (pf.REGION_CIRCLE or pf.REGION_RECTANGLE) of the region.", 
    NULL},
    {"parameters",
    (getter)PyRegion_get_parameters, NULL,
    "Get a dictionary with the size parameters of the region, which vary depending on the region type", 
    NULL},
    {NULL}  /* Sentinel */
};

static PyTypeObject PyRegion_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.Region",               /* tp_name */
    sizeof(PyRegionObject),    /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)PyRegion_dealloc, /* tp_dealloc */
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
    "Permafrost Engine region object.                               \n" 
    "                                                               \n"
    "The regions takes the following (mandatory) keyword arguments  \n"
    "in its' constructor:                                           \n"
    "                                                               \n"
    "  - type {pf.REGION_CIRCLE, pf.REGION_RECTANGLE}               \n"
    "  - name (string)                                              \n"
    "  - position (tuple of 2 floats)                               \n"
    "                                                               \n"
    "In addition, it takes the following arguments depending on the \n"
    "type:                                                          \n"
    "                                                               \n"
    "  - radius (float) [circle regions only]                       \n"
    "  - dimensions (tuple of 2 floats) [rectangular regions only]  \n"
    ,                          /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyRegion_methods,          /* tp_methods */
    0,                         /* tp_members */
    PyRegion_getset,           /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    PyRegion_new,              /* tp_new */
};

static khash_t(PyObject) *s_name_pyobj_table;
static PyObject          *s_loaded;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static PyObject *PyRegion_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"type", "name", "position", "radius", "dimensions", NULL};

    enum region_type regtype;
    const char *name;
    vec2_t position;

    float radius;
    vec2_t dims;

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "is(ff)|f(ff)", kwlist, &regtype, &name, 
        &position.x, &position.z, &radius, &dims.x, &dims.z)) {
        assert(PyErr_Occurred());
        return NULL;
    }

    if(regtype != REGION_CIRCLE && regtype != REGION_RECTANGLE) {
        PyErr_SetString(PyExc_TypeError, 
            "regtype keyword argument must be one of {pf.REGION_CIRCLE, pf.REGION_RECTANGLE}.");
        return NULL;
    }

    if(regtype == REGION_CIRCLE 
    && (PyDict_GetItemString(kwds, "dimensions") || !PyDict_GetItemString(kwds, "radius"))) {
        PyErr_SetString(PyExc_TypeError, "CIRCLE regions must have a radius but no dimensions.");
        return NULL;
    }

    if(regtype == REGION_RECTANGLE
    && (!PyDict_GetItemString(kwds, "dimensions") || PyDict_GetItemString(kwds, "radius"))) {
        PyErr_SetString(PyExc_TypeError, "RECTANGLE regions must have dimensions but no radius.");
        return NULL;
    }

    PyRegionObject *self = (PyRegionObject*)type->tp_alloc(type, 0);
    if(!self) {
        assert(PyErr_Occurred());
        return NULL;
    }
    const char *copy = pf_strdup(name);
    if(!copy) {
        Py_DECREF(self);
        type->tp_free(self);
        return PyErr_NoMemory();
    }

    self->type = regtype;
    self->name = copy;

    bool status = false;
    switch(regtype) {
    case REGION_CIRCLE:
        status = G_Region_AddCircle(name, position, radius);
        break;
    case REGION_RECTANGLE:
        status = G_Region_AddRectangle(name, position, dims.x, dims.z);
        break;
    default: assert(0);
    }

    if(!status) {
        char errbuff[256];
        pf_snprintf(errbuff, sizeof(errbuff), "Unable to create region (%s) of type (%d)\n", 
            copy, regtype);
        PyErr_SetString(PyExc_RuntimeError, errbuff);

        PF_FREE(copy);
        type->tp_free(self);
        return NULL;
    }

    int ret;
    khiter_t k = kh_put(PyObject, s_name_pyobj_table, self->name, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_name_pyobj_table, k) = (PyObject*)self;

    return (PyObject*)self;
}

static void PyRegion_dealloc(PyRegionObject *self)
{
    khiter_t k = kh_get(PyObject, s_name_pyobj_table, self->name);
    assert(k != kh_end(s_name_pyobj_table));
    kh_del(PyObject, s_name_pyobj_table, k);

    G_Region_Remove(self->name);
    PF_FREE(self->name);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *PyRegion_curr_ents(PyRegionObject *self)
{
    assert(Sched_UsingBigStack());

    uint32_t ents[512];
    size_t nents = G_Region_GetEnts(self->name, ARR_SIZE(ents), ents);

    PyObject *ret = PyList_New(0);
    if(!ret)
        return NULL;

    for(int i = 0; i < nents; i++) {
        PyObject *ent = S_Entity_ObjForUID(ents[i]);
        if(!ent)
            continue;

        if(0 != PyList_Append(ret, ent)) {
            Py_DECREF(ret);
            return NULL;
        }
    }
    return ret;
}

static PyObject *PyRegion_contains(PyRegionObject *self, PyObject *args)
{
    PyObject *obj;

    if(!PyArg_ParseTuple(args, "O", &obj)
    || !S_Entity_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single pf.Entity instance.");
        return NULL;
    }

    uint32_t uid = 0;
    S_Entity_UIDForObj(obj, &uid);

    if(G_Region_ContainsEnt(self->name, uid)) {
        Py_RETURN_TRUE;
    }else {
        Py_RETURN_FALSE;
    }
}

static PyObject *PyRegion_explore(PyRegionObject *self, PyObject *args)
{
    int faction_id;
    if(!PyArg_ParseTuple(args, "i", &faction_id)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single integer (faction ID).");
        return NULL;
    }

    uint16_t factions = G_GetFactions(NULL, NULL, NULL);
    if(faction_id < 0 || faction_id >= MAX_FACTIONS || !(factions & (0x1 << faction_id))) {
        PyErr_SetString(PyExc_TypeError, "Invalid faction ID.");
        return NULL;
    }

    G_Region_ExploreFog(self->name, faction_id);
    Py_RETURN_NONE;
}

static PyObject *PyRegion_pickle(PyRegionObject *self, PyObject *args, PyObject *kwargs)
{
    bool status;
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);

    PyObject *type = PyInt_FromLong(self->type);
    CHK_TRUE(type, fail_pickle);
    status = S_PickleObjgraph(type, stream);
    Py_DECREF(type);
    CHK_TRUE(status, fail_pickle);

    PyObject *name = PyString_FromString(self->name);
    CHK_TRUE(name, fail_pickle);
    status = S_PickleObjgraph(name, stream);
    Py_DECREF(name);
    CHK_TRUE(status, fail_pickle);

    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));
fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    return ret;
}

static PyObject *PyRegion_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = NULL;
    PyRegionObject *reg = NULL;
    const char *str;
    Py_ssize_t len;
    char tmp;

    if(!PyArg_ParseTuple(args, "s#", &str, &len)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single string.");
        goto fail_args;
    }

    SDL_RWops *stream = SDL_RWFromConstMem(str, len);
    CHK_TRUE(stream, fail_args);

    PyObject *type = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *name = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    if(!type || !name) {
        PyErr_SetString(PyExc_RuntimeError, "Could not unpickle internal state of pf.Region instance");
        goto fail_unpickle;
    }

    if(!PyInt_Check(type)) {
        PyErr_SetString(PyExc_RuntimeError, "Unpickled 'type' field must be an integer type");
        goto fail_unpickle;
    }

    if(!PyString_Check(name)) {
        PyErr_SetString(PyExc_RuntimeError, "Unpickled 'name' field must be a string");
        goto fail_unpickle;
    }

    reg = (PyRegionObject*)((PyTypeObject*)cls)->tp_alloc((PyTypeObject*)cls, 0);
    if(!reg) {
        assert(PyErr_Occurred());
        goto fail_unpickle;
    }

    reg->type = PyInt_AS_LONG(type);
    reg->name = pf_strdup(PyString_AS_STRING(name));

    if(!reg->name) {

        PyErr_SetString(PyExc_MemoryError, "Unable to allocate string buffer");
        ((PyTypeObject*)cls)->tp_free(reg);
        goto fail_unpickle;
    }

    int status;
    khiter_t k = kh_put(PyObject, s_name_pyobj_table, reg->name, &status);
    if(status == -1 || status == 0) {

        PyErr_SetString(PyExc_RuntimeError, "Unable to allocate table storage");
        ((PyTypeObject*)cls)->tp_free(reg);
        goto fail_unpickle;
    }
    kh_value(s_name_pyobj_table, k) = (PyObject*)reg;

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", reg, (int)nread);
    Py_DECREF(reg);

fail_unpickle:
    Py_XDECREF(type);
    Py_XDECREF(name);
fail_args:
    return ret;
}

static PyObject *PyRegion_get_pos(PyRegionObject *self, void *closure)
{
    vec2_t pos = {0};
    G_Region_GetPos(self->name, &pos);
    return Py_BuildValue("ff", pos.x, pos.z);
}

static int PyRegion_set_pos(PyRegionObject *self, PyObject *value, void *closure)
{
    if(!PyTuple_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple.");
        return -1;
    }
    
    vec2_t newpos;
    if(!PyArg_ParseTuple(value, "ff", &newpos.x, &newpos.z)) {
        return -1;
    }

    G_Region_SetPos(self->name, newpos);
    return 0;
}

static PyObject *PyRegion_get_shown(PyRegionObject *self, void *closure)
{
    bool on = false;
    G_Region_GetShown(self->name, &on);
    if(on) {
        Py_RETURN_TRUE;
    }else {
        Py_RETURN_FALSE;
    }
}

static int PyRegion_set_shown(PyRegionObject *self, PyObject *value, void *closure)
{
    bool on = PyObject_IsTrue(value);
    bool status = G_Region_SetShown(self->name, on);
    assert(status);
    return 0;
}

static PyObject *PyRegion_get_name(PyRegionObject *self, void *closure)
{
    return PyString_FromString(self->name);
}

static PyObject *PyRegion_get_type(PyRegionObject *self, void *closure)
{
    return PyInt_FromLong(self->type);
}

static PyObject *PyRegion_get_parameters(PyRegionObject *self, void *closure)
{
    PyObject *dict = PyDict_New();
    if(!dict)
        return NULL;

    switch(self->type) {
    case REGION_CIRCLE: {
        float raw = 0.0f;
        G_Region_GetRadius(self->name, &raw);
        PyObject *radius = PyFloat_FromDouble(raw);
        if(!radius)
            goto fail_params;
        int status = PyDict_SetItemString(dict, "radius", radius);
        Py_DECREF(radius);
        if(status != 0)
            goto fail_params;
        break;
    }
    case REGION_RECTANGLE: {
        float rawx = 0.0f, rawz = 0.0f;
        G_Region_GetXLen(self->name, &rawx);
        G_Region_GetZLen(self->name, &rawz);
        PyObject *dims = Py_BuildValue("ff", rawx, rawz);
        if(!dims)
            goto fail_params;
        int status = PyDict_SetItemString(dict, "dimensions", dims);
        Py_DECREF(dims);
        if(status != 0)
            goto fail_params;
        break;
    }
    default: assert(0);
    }

    return dict;

fail_params:
    Py_DECREF(dict);
    return NULL;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void S_Region_PyRegister(PyObject *module)
{
    if(PyType_Ready(&PyRegion_type) < 0)
        return;
    Py_INCREF(&PyRegion_type);
    PyModule_AddObject(module, "Region", (PyObject*)&PyRegion_type);
}

bool S_Region_Init(void)
{
    s_loaded = PyList_New(0);
    if(!s_loaded)
        return false;

    s_name_pyobj_table = kh_init(PyObject);
    if(!s_name_pyobj_table) {
        Py_DECREF(s_loaded);
        return false;
    }
    return true;
}

void S_Region_Shutdown(void)
{
    kh_destroy(PyObject, s_name_pyobj_table);
}

void S_Region_Clear(void)
{
    Py_CLEAR(s_loaded);
}

void S_Region_NotifyContentsChanged(const char *name)
{
    khiter_t k = kh_get(PyObject, s_name_pyobj_table, name);
    if(k == kh_end(s_name_pyobj_table))
        return;

    PyObject *reg = kh_value(s_name_pyobj_table, k);
    if(!PyObject_HasAttrString(reg, "on_contents_changed"))
        return;

    PyObject *ret = PyObject_CallMethod(reg, "on_contents_changed", NULL);
    if(!ret) {
        S_ShowLastError();
    }
    Py_XDECREF(ret);
}

PyObject *S_Region_GetLoaded(void)
{
    PyObject *ret = s_loaded;
    if(!ret)
        NULL;

    s_loaded = PyList_New(0);
    assert(s_loaded);
    
    return ret;
}

script_opaque_t S_Region_ObjFromAtts(const char *name, int type, vec2_t pos, 
                                     float radius, float xlen, float zlen)
{
    if(type != REGION_CIRCLE && type != REGION_RECTANGLE)
        return NULL;

    PyObject *args = PyTuple_New(0), *kwargs = NULL;
    if(!args)
        return NULL;

    switch(type) {
    case REGION_CIRCLE:
        kwargs = Py_BuildValue("{s:i, s:s, s:(ff), s:f}",
            "type",         type,
            "name",         name,
            "position",     pos.x, pos.z,
            "radius",       radius);
        break;
    case REGION_RECTANGLE:
        kwargs = Py_BuildValue("{s:i, s:s, s:(ff), s:(ff)}",
            "type",         type,
            "name",         name,
            "position",     pos.x, pos.z,
            "dimensions",   xlen, zlen);
        break;
    default: assert(0);
    }

    if(!kwargs) {
        Py_DECREF(args);
        return NULL;
    }

    PyObject *ret = PyObject_Call((PyObject*)&PyRegion_type, args, kwargs);
    Py_DECREF(args);
    Py_DECREF(kwargs);

    if(ret) {
        PyList_Append(s_loaded, ret);
        Py_DECREF(ret);
    }
    return ret;
}

