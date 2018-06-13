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
#include "../map/public/map.h"
#include "../event.h"
#include "../config.h"

#include <SDL.h>

#include <stdio.h>


static PyObject *PyPf_new_game(PyObject *self, PyObject *args);
static PyObject *PyPf_new_game_string(PyObject *self, PyObject *args);
static PyObject *PyPf_set_map_render_mode(PyObject *self, PyObject *args);
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
static PyObject *PyPf_get_mouse_pos(PyObject *self);
static PyObject *PyPf_mouse_over_ui(PyObject *self);

static PyObject *PyPf_enable_unit_selection(PyObject *self);
static PyObject *PyPf_disable_unit_selection(PyObject *self);
static PyObject *PyPf_clear_unit_selection(PyObject *self);
static PyObject *PyPf_get_unit_selection(PyObject *self);

static PyObject *PyPf_update_chunk_materials(PyObject *self, PyObject *args);
static PyObject *PyPf_update_tile(PyObject *self, PyObject *args);
static PyObject *PyPf_set_map_highlight_size(PyObject *self, PyObject *args);
static PyObject *PyPf_set_minimap_position(PyObject *self, PyObject *args);
static PyObject *PyPf_mouse_over_minimap(PyObject *self);
static PyObject *PyPf_map_height_at_point(PyObject *self, PyObject *args);
static PyObject *PyPf_map_pos_under_cursor(PyObject *self);

static PyObject *PyPf_multiply_quaternions(PyObject *self, PyObject *args);

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

    {"set_map_render_mode", 
    (PyCFunction)PyPf_set_map_render_mode, METH_VARARGS, 
    "Sets the rendering mode for every chunk in the currently active map."},

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

    {"get_mouse_pos", 
    (PyCFunction)PyPf_get_mouse_pos, METH_NOARGS,
    "Get the (x, y) cursor position on the screen."},

    {"mouse_over_ui", 
    (PyCFunction)PyPf_mouse_over_ui, METH_NOARGS,
    "Returns True if the mouse cursor is within the bounds of any UI windows."},

    {"enable_unit_selection", 
    (PyCFunction)PyPf_enable_unit_selection, METH_NOARGS,
    "Make it possible to select units with the mouse. Enable drawing of a selection box when dragging the mouse."},

    {"disable_unit_selection", 
    (PyCFunction)PyPf_disable_unit_selection, METH_NOARGS,
    "Make it impossible to select units with the mouse. Disable drawing of a selection box when dragging the mouse."},

    {"clear_unit_selection", 
    (PyCFunction)PyPf_clear_unit_selection, METH_NOARGS,
    "Clear the current unit seleciton."},

    {"get_unit_selection", 
    (PyCFunction)PyPf_get_unit_selection, METH_NOARGS,
    "Returns a list of objects currently selected by the player."},

    {"update_chunk_materials", 
    (PyCFunction)PyPf_update_chunk_materials, METH_VARARGS,
    "Update the material list for a particular chunk. Expects a tuple of chunk coordinates "
    "and a PFMAP material section string as arguments."},

    {"update_tile", 
    (PyCFunction)PyPf_update_tile, METH_VARARGS,
    "Update the map tile at the specified coordinates to the new value."},

    {"set_map_highlight_size", 
    (PyCFunction)PyPf_set_map_highlight_size, METH_VARARGS,
    "Determines how many tiles around the currently hovered tile are highlighted. (0 = none, "
    "1 = single tile highlighted, 2 = 3x3 grid highlighted, etc.)"},

    {"set_minimap_position", 
    (PyCFunction)PyPf_set_minimap_position, METH_VARARGS,
    "Set the center position of the minimap in screen coordinates."},

    {"mouse_over_minimap",
    (PyCFunction)PyPf_mouse_over_minimap, METH_NOARGS,
    "Returns true if the mouse cursor is over the minimap, false otherwise."},

    {"map_height_at_point",
    (PyCFunction)PyPf_map_height_at_point, METH_VARARGS,
    "Returns the Y-dimension map height at the specified XZ coordinate. Returns None if the "
    "specified coordinate is outside the map bounds."},

    {"map_pos_under_cursor",
    (PyCFunction)PyPf_map_pos_under_cursor, METH_NOARGS,
    "Returns the XYZ coordinate of the point of the map underneath the cursor. Returns 'None' if "
    "the cursor is not over the map."},

    {"multiply_quaternions",
    (PyCFunction)PyPf_multiply_quaternions, METH_VARARGS,
    "Returns the normalized result of multiplying 2 quaternions (specified as a list of 4 floats - XYZW order)."},

    {NULL}  /* Sentinel */
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool s_vec3_from_pylist_arg(PyObject *list, vec3_t *out)
{
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

static bool s_quat_from_pylist_arg(PyObject *list, quat_t *out)
{
    if(!PyList_Check(list)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a list.");
        return false;
    }
    
    Py_ssize_t len = PyList_Size(list);
    if(len != 4) {
        PyErr_SetString(PyExc_TypeError, "Argument must have a size of 4."); 
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

static PyObject *PyPf_set_map_render_mode(PyObject *self, PyObject *args)
{
    int mode; 
    if(!PyArg_ParseTuple(args, "i", &mode)) {
        PyErr_SetString(PyExc_TypeError, "Argument must an integer.");
        return NULL;
    }

    G_SetMapRenderMode(mode);
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

static PyObject *PyPf_get_mouse_pos(PyObject *self)
{
    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    return Py_BuildValue("(i, i)", mouse_x, mouse_y);
}

static PyObject *PyPf_mouse_over_ui(PyObject *self)
{
    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    if(S_UI_MouseOverWindow(mouse_x, mouse_y))
        Py_RETURN_TRUE;
    else
        Py_RETURN_NONE;
}

static PyObject *PyPf_enable_unit_selection(PyObject *self)
{
    G_Sel_Enable();
    Py_RETURN_NONE;
}

static PyObject *PyPf_disable_unit_selection(PyObject *self)
{
    G_Sel_Disable();
    Py_RETURN_NONE;
}

static PyObject *PyPf_clear_unit_selection(PyObject *self)
{
    G_Sel_Clear();
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_unit_selection(PyObject *self)
{
    const pentity_kvec_t *sel = G_Sel_Get();

    PyObject *ret = PyList_New(0);
    if(!ret)
        return NULL;

    for(int i = 0; i < kv_size(*sel); i++) {
        PyObject *ent =  S_Entity_ObjForUID(kv_A(*sel, i)->uid);
        if(ent) {
            PyList_Append(ret, ent);
        }
    }

    return ret;
}

static PyObject *PyPf_update_chunk_materials(PyObject *self, PyObject *args)
{
    int chunk_r, chunk_c;
    const char *mats_str;

    if(!PyArg_ParseTuple(args, "(ii)s", &chunk_r, &chunk_c, &mats_str)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a tuple of two integers and a string.");
        return NULL;
    }

    bool result = G_UpdateChunkMats(chunk_r, chunk_c, mats_str);
    if(!result)
        return NULL;
    else
        Py_RETURN_NONE;
}

static PyObject *PyPf_update_tile(PyObject *self, PyObject *args)
{
    struct tile_desc desc;
    PyObject *tile_obj;
    const struct tile *tile;

    if(!PyArg_ParseTuple(args, "(ii)(ii)O", &desc.chunk_r, &desc.chunk_c, &desc.tile_r, &desc.tile_c, &tile_obj)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two tuples of two integers and a pf.Tile object.");
        return NULL;
    }

    if(NULL == (tile = S_Tile_GetTile(tile_obj))) {
        PyErr_SetString(PyExc_TypeError, "Last argument must be of type pf.Tile.");
        return NULL;
    }

    if(!G_UpdateTile(&desc, tile))
        return NULL;

    if(!G_UpdateMinimapChunk(desc.chunk_r, desc.chunk_c))
        return NULL;

    Py_RETURN_NONE;
}

static PyObject *PyPf_set_map_highlight_size(PyObject *self, PyObject *args)
{
    int size;

    if(!PyArg_ParseTuple(args, "i", &size)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an integer.");
        return NULL;
    }

    M_Raycast_SetHighlightSize(size);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_minimap_position(PyObject *self, PyObject *args)
{
    float x, y;

    if(!PyArg_ParseTuple(args, "ff", &x, &y)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two floats.");
        return NULL;
    }

    G_SetMinimapPos(x, y);
    Py_RETURN_NONE;
}

static PyObject *PyPf_mouse_over_minimap(PyObject *self)
{
    bool result = G_MouseOverMinimap();
    if(result) 
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

static PyObject *PyPf_map_height_at_point(PyObject *self, PyObject *args)
{
    float x, z;

    if(!PyArg_ParseTuple(args, "ff", &x, &z)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two floats.");
        return NULL;
    }

    float height;
    bool result = G_MapHeightAtPoint((vec2_t){x, z}, &height);
    if(!result)
        Py_RETURN_NONE;
    else
        return Py_BuildValue("f", height);
}

static PyObject *PyPf_map_pos_under_cursor(PyObject *self)
{
    vec3_t pos;
    if(M_Raycast_IntersecCoordinate(&pos))
        return Py_BuildValue("[fff]", pos.x, pos.y, pos.z);
    else
        Py_RETURN_NONE;
}

static PyObject *PyPf_multiply_quaternions(PyObject *self, PyObject *args)
{
    PyObject *q1_list, *q2_list;
    quat_t q1, q2, ret;

    if(!PyArg_ParseTuple(args, "OO", &q1_list, &q2_list)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two objects.");
        return NULL;
    }

    if(!s_quat_from_pylist_arg(q1_list, &q1))
        return NULL;
    if(!s_quat_from_pylist_arg(q2_list, &q2))
        return NULL;

    PFM_Quat_MultQuat(&q1, &q2, &ret);
    PFM_Quat_Normal(&ret, &ret);

    return Py_BuildValue("[ffff]", ret.x, ret.y, ret.z, ret.w);
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

    if(!S_UI_Init(ctx))
        return false;
    if(!S_Entity_Init())
        return false;

    initpf();
    return true;
}

void S_Shutdown(void)
{
    Py_Finalize();
    S_UI_Shutdown();
    S_Entity_Shutdown();
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
    assert(user_arg);
    assert(event_arg);

    args = PyTuple_New(2);
    /* PyTuple_SetItem steals references! However, we wish to hold on
     * to the user_arg even when the tuple is destroyed. */
    Py_INCREF(user_arg);
    PyTuple_SetItem(args, 0, user_arg);
    PyTuple_SetItem(args, 1, event_arg);

    ret = PyObject_CallObject(callable, args);
    Py_DECREF(args);

    Py_XDECREF(ret);
    if(!ret) {
        PyErr_Print();
        exit(EXIT_FAILURE);
    }
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

        case SDL_MOUSEMOTION:
            return Py_BuildValue("(i,i), (i,i)",
                ((SDL_Event*)arg)->motion.x,
                ((SDL_Event*)arg)->motion.y,
                ((SDL_Event*)arg)->motion.xrel,
                ((SDL_Event*)arg)->motion.xrel);

        case SDL_MOUSEBUTTONDOWN:
            return Py_BuildValue("(i, i)",
                ((SDL_Event*)arg)->button.button,
                ((SDL_Event*)arg)->button.state);

        case SDL_MOUSEBUTTONUP:
            return Py_BuildValue("(i, i)",
                ((SDL_Event*)arg)->button.button,
                ((SDL_Event*)arg)->button.state);

        case SDL_MOUSEWHEEL:
        {
            uint32_t dir = ((SDL_Event*)arg)->wheel.direction;
            return Py_BuildValue("(i, i)",
                ((SDL_Event*)arg)->wheel.x * (dir == SDL_MOUSEWHEEL_NORMAL ? 1 : -1),
                ((SDL_Event*)arg)->wheel.y * (dir == SDL_MOUSEWHEEL_NORMAL ? 1 : -1));
        }
        case EVENT_SELECTED_TILE_CHANGED:
            if(!arg)
                Py_RETURN_NONE;
            return Py_BuildValue("(i,i), (i,i)", 
                ((struct tile_desc*)arg)->chunk_r,
                ((struct tile_desc*)arg)->chunk_c,
                ((struct tile_desc*)arg)->tile_r,
                ((struct tile_desc*)arg)->tile_c);

        default:
            Py_RETURN_NONE;
    }
}

bool S_ObjectsEqual(script_opaque_t a, script_opaque_t b)
{
    return (1 == PyObject_RichCompareBool(a, b, Py_EQ));
}

