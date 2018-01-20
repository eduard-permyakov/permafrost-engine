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

#include "entity_script.h"
#include "public/script.h"
#include "../entity.h"
#include "../game/public/game.h"
#include "../render/public/render.h"

#include <stdio.h>


static PyObject *PyPf_new_game(PyObject *self, PyObject *args);
static PyObject *PyPf_set_ambient_light_color(PyObject *self, PyObject *args);
static PyObject *PyPf_set_emit_light_color(PyObject *self, PyObject *args);
static PyObject *PyPf_set_emit_light_pos(PyObject *self, PyObject *args);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyMethodDef pf_module_methods[] = {

    {"new_game", 
    (PyCFunction)PyPf_new_game, METH_VARARGS,
    "Loads the specified map and creates an empty scene. Note that all "
    "references to existing _active_ entities _MUST_ be deleted before creating a "
    "new game."},

    {"set_ambient_light_color", 
    (PyCFunction)PyPf_set_ambient_light_color, METH_VARARGS,
    "Sets the global ambient light color (specified as an RGB multiplier) for the scene."},

    {"set_emit_light_color", 
    (PyCFunction)PyPf_set_emit_light_color, METH_VARARGS,
    "Sets the color (specified as an RGB multiplier) for the global light source."},

    {"set_emit_light_pos", 
    (PyCFunction)PyPf_set_emit_light_pos, METH_VARARGS,
    "Sets the position (in XYZ worldspace coordinates)"},

    {NULL}  /* Sentinel */
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool s_vec3_from_pylist_arg(PyObject *list, vec3_t *out)
{
    vec3_t ret;

    if(!PyList_Check(list)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a list.");
        return false;
    }
    
    Py_ssize_t len = PyList_Size(list);
    if(len != 3) {
        PyErr_SetString(PyExc_TypeError, "Argument must have a size of 3."); 
        return false;
    }

    for(int i = 0; i < len; i++) {

        PyObject *item = PyList_GetItem(list, i);
        if(!PyFloat_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "List items must be floats.");
            return false;
        }

        out->raw[i] = PyFloat_AsDouble(item);
    }

    return true;
}

static PyObject *PyPf_new_game(PyObject *self, PyObject *args)
{
    const char *dir, *pfmap, *pfmat;
    if(!PyArg_ParseTuple(args, "sss", &dir, &pfmap, &pfmat)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a tuple of three strings.");
        return NULL;
    }

    G_NewGameWithMap(dir, pfmap, pfmat);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_ambient_light_color(PyObject *self, PyObject *args)
{
    PyObject *list;
    vec3_t color;

    if(!PyArg_ParseTuple(args, "O!", &PyList_Type, &list))
        return NULL;

    if(!s_vec3_from_pylist_arg(list, &color))
        return NULL;

    R_GL_SetAmbientLightColor(color);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_emit_light_color(PyObject *self, PyObject *args)
{
    PyObject *list;
    vec3_t color;

    if(!PyArg_ParseTuple(args, "O!", &PyList_Type, &list))
        return NULL;

    if(!s_vec3_from_pylist_arg(list, &color))
        return NULL;

    R_GL_SetLightEmitColor(color);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_emit_light_pos(PyObject *self, PyObject *args)
{
    PyObject *list;
    vec3_t pos;

    if(!PyArg_ParseTuple(args, "O!", &PyList_Type, &list))
        return NULL;

    if(!s_vec3_from_pylist_arg(list, &pos))
        return NULL;

    R_GL_SetLightPos(pos);
    Py_RETURN_NONE;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

PyMODINIT_FUNC initpf(void)
{
    PyObject *module;
    module = Py_InitModule("pf", pf_module_methods);
    if(!module)
        return;

    S_Entity_PyRegister(module);
}

bool S_Init(char *progname, const char *base_path)
{
    Py_SetProgramName(progname);
    Py_Initialize();

    initpf();

    return true;
}

void S_Shutdown(void)
{
    Py_Finalize();
}

bool S_RunFile(const char *path)
{
    FILE *script = fopen(path, "r");
    if(!script)
        return false;

    PyRun_SimpleFile(script, path); 
    fclose(script);
    return true;
}

