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
#include "ui_script.h"
#include "tile_script.h"
#include "public/script.h"
#include "../entity.h"
#include "../game/public/game.h"
#include "../render/public/render.h"
#include "../event/public/event.h"
#include "../config.h"

#include <stdio.h>


static PyObject *PyPf_new_game(PyObject *self, PyObject *args);
static PyObject *PyPf_new_game_string(PyObject *self, PyObject *args);
static PyObject *PyPf_set_ambient_light_color(PyObject *self, PyObject *args);
static PyObject *PyPf_set_emit_light_color(PyObject *self, PyObject *args);
static PyObject *PyPf_set_emit_light_pos(PyObject *self, PyObject *args);

static PyObject *PyPf_register_event_handler(PyObject *self, PyObject *args);
static PyObject *PyPf_unregister_event_handler(PyObject *self, PyObject *args);
static PyObject *PyPf_global_event(PyObject *self, PyObject *args);

static PyObject *PyPf_activate_camera(PyObject *self, PyObject *args);
static PyObject *PyPf_prev_frame_ms(PyObject *self);
static PyObject *PyPf_get_resolution(PyObject *self);
static PyObject *PyPf_get_basedir(PyObject *self);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyMethodDef pf_module_methods[] = {

    {"new_game", 
    (PyCFunction)PyPf_new_game, METH_VARARGS,
    "Loads the specified map and creates an empty scene. Note that all "
    "references to existing _active_ entities _MUST_ be deleted before creating a "
    "new game."},

    {"new_game_string", 
    (PyCFunction)PyPf_new_game_string, METH_VARARGS,
    "The same as 'new_game' but takes the map contents string as an argument instead of "
    "a path and filename."},

    {"set_ambient_light_color", 
    (PyCFunction)PyPf_set_ambient_light_color, METH_VARARGS,
    "Sets the global ambient light color (specified as an RGB multiplier) for the scene."},

    {"set_emit_light_color", 
    (PyCFunction)PyPf_set_emit_light_color, METH_VARARGS,
    "Sets the color (specified as an RGB multiplier) for the global light source."},

    {"set_emit_light_pos", 
    (PyCFunction)PyPf_set_emit_light_pos, METH_VARARGS,
    "Sets the position (in XYZ worldspace coordinates)"},

    {"register_event_handler", 
    (PyCFunction)PyPf_register_event_handler, METH_VARARGS,
    "Adds a script event handler to be called when the specified global event occurs."},

    {"unregister_event_handler", 
    (PyCFunction)PyPf_unregister_event_handler, METH_VARARGS,
    "Removes a script event handler added by 'register_event_handler'."},

    {"global_event", 
    (PyCFunction)PyPf_global_event, METH_VARARGS,
    "Broadcast a global event so all handlers can get invoked."},

    {"activate_camera", 
    (PyCFunction)PyPf_activate_camera, METH_VARARGS,
    "Set the camera specified by the index to be the active camera, meaning the scene is "
    "being rendered from the camera's point of view. The second argument is teh camera "
    "control mode (0 = FPS, 1 = RTS). Note that the position of camera 0 is restricted "
    "to the map boundaries as it is expected to be the main RTS camera. The other cameras "
    "are unrestricted."},

    {"prev_frame_ms", 
    (PyCFunction)PyPf_prev_frame_ms, METH_NOARGS,
    "Get the duration of the previous game frame in milliseconds."},

    {"get_resolution", 
    (PyCFunction)PyPf_get_resolution, METH_NOARGS,
    "Get the currently set resolution of the game window."},

    {"get_basedir", 
    (PyCFunction)PyPf_get_basedir, METH_NOARGS,
    "Get the path to the top-level game resource folder (parent of 'assets')."},

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
    const char *dir, *pfmap;
    if(!PyArg_ParseTuple(args, "ss", &dir, &pfmap)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two strings.");
        return NULL;
    }

    G_NewGameWithMap(dir, pfmap);
    Py_RETURN_NONE;
}

static PyObject *PyPf_new_game_string(PyObject *self, PyObject *args)
{
    const char *mapstr;
    if(!PyArg_ParseTuple(args, "s", &mapstr)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a string.");
        return NULL;
    }

    G_NewGameWithMapString(mapstr);
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

static PyObject *PyPf_register_event_handler(PyObject *self, PyObject *args)
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

    bool ret = E_Global_ScriptRegister(event, callable, user_arg);
    assert(ret == true);
    Py_RETURN_NONE;
}

static PyObject *PyPf_unregister_event_handler(PyObject *self, PyObject *args)
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

    bool ret = E_Global_ScriptUnregister(event, callable);
    Py_RETURN_NONE;
}

static PyObject *PyPf_global_event(PyObject *self, PyObject *args)
{
    enum eventtype event;
    PyObject *arg;

    if(!PyArg_ParseTuple(args, "iO", &event, &arg)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a tuple of an integer and one object.");
        return NULL;
    }

    Py_INCREF(arg);

    E_Global_Notify(event, arg, ES_SCRIPT);
    Py_RETURN_NONE;
}

static PyObject *PyPf_activate_camera(PyObject *self, PyObject *args)
{
    int idx;
    enum cam_mode mode;

    if(!PyArg_ParseTuple(args, "ii", &idx, &mode)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a tuple of two integers.");
        return NULL;
    }

    G_ActivateCamera(idx, mode);
    Py_RETURN_NONE;
}

static PyObject *PyPf_prev_frame_ms(PyObject *self)
{
    extern unsigned g_last_frame_ms;
    return Py_BuildValue("i", g_last_frame_ms);
}

static PyObject *PyPf_get_resolution(PyObject *self)
{
    return Py_BuildValue("(i, i)", CONFIG_RES_X, CONFIG_RES_Y);
}

static PyObject *PyPf_get_basedir(PyObject *self)
{
    extern const char *g_basepath;
    return Py_BuildValue("s", g_basepath);
}

static bool s_sys_path_add_dir(const char *filename)
{
    if(strlen(filename) >= 512)
        return false;

    char copy[512];
    strcpy(copy, filename);

    char *end = copy + (strlen(copy) - 1);
    while(end > copy && *end != '/')
        end--;

    if(end == copy)
        return false;
    *end = '\0';

    PyObject *sys_path = PySys_GetObject("path");
    assert(sys_path);

    if(0 != PyList_Append(sys_path, Py_BuildValue("s", copy)) )
        return false;

    return true;
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
    S_UI_PyRegister(module);
    S_Tile_PyRegister(module);
}

bool S_Init(char *progname, const char *base_path, struct nk_context *ctx)
{
    Py_SetProgramName(progname);
    Py_Initialize();

    S_UI_Init(ctx);
    initpf();

    return true;
}

void S_Shutdown(void)
{
    S_UI_Shutdown();
    Py_Finalize();
}

bool S_RunFile(const char *path)
{
    FILE *script = fopen(path, "r");
    if(!script)
        return false;

    /* The directory of the script file won't be automatically added by 'PyRun_SimpleFile'.
     * We add it manually to sys.path ourselves. */
    if(!s_sys_path_add_dir(path))
        return false;

    PyObject *PyFileObject = PyFile_FromString((char*)path, "r");
    PyRun_SimpleFile(PyFile_AsFile(PyFileObject), path);

    fclose(script);
    return true;
}

void S_RunEventHandler(script_opaque_t callable, script_opaque_t user_arg, script_opaque_t event_arg)
{
    PyObject *args, *ret;
    assert(PyCallable_Check(callable));

    args = PyTuple_New(2);
    /* PyTuple_SetItem steals references! However, we wish to hold on
     * to the user_arg even when the tuple is destroyed. */
    Py_INCREF(user_arg);
    PyTuple_SetItem(args, 0, user_arg);
    PyTuple_SetItem(args, 1, event_arg);

    ret = PyObject_CallObject(callable, args);
    assert(ret);
    Py_DECREF(args);
}

void S_Release(script_opaque_t obj)
{
    Py_XDECREF(obj);
}

script_opaque_t S_WrapEngineEventArg(enum eventtype e, void *arg)
{
    switch(e) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            return Py_BuildValue("(i)", 
                ((SDL_Event*)arg)->key.keysym.scancode);
            break;
        case SDL_MOUSEMOTION:
            return Py_BuildValue("( (i,i), (i,i) )",
                ((SDL_Event*)arg)->motion.x,
                ((SDL_Event*)arg)->motion.y,
                ((SDL_Event*)arg)->motion.xrel,
                ((SDL_Event*)arg)->motion.xrel);
            break;
        default:
            Py_RETURN_NONE;
    }
}

