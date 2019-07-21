/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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

#include <Python.h> /* Must be included first */

#include "entity_script.h"
#include "ui_script.h"
#include "tile_script.h"
#include "script_constants.h"
#include "public/script.h"
#include "../entity.h"
#include "../game/public/game.h"
#include "../render/public/render.h"
#include "../navigation/public/nav.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../event.h"
#include "../config.h"
#include "../scene.h"
#include "../settings.h"
#include "../main.h"
#include "../ui.h"

#include <SDL.h>

#include <stdio.h>


static PyObject *PyPf_new_game(PyObject *self, PyObject *args);
static PyObject *PyPf_new_game_string(PyObject *self, PyObject *args);
static PyObject *PyPf_set_ambient_light_color(PyObject *self, PyObject *args);
static PyObject *PyPf_set_emit_light_color(PyObject *self, PyObject *args);
static PyObject *PyPf_set_emit_light_pos(PyObject *self, PyObject *args);
static PyObject *PyPf_load_scene(PyObject *self, PyObject *args);

static PyObject *PyPf_register_event_handler(PyObject *self, PyObject *args);
static PyObject *PyPf_register_ui_event_handler(PyObject *self, PyObject *args);
static PyObject *PyPf_unregister_event_handler(PyObject *self, PyObject *args);
static PyObject *PyPf_global_event(PyObject *self, PyObject *args);

static PyObject *PyPf_activate_camera(PyObject *self, PyObject *args);
static PyObject *PyPf_prev_frame_ms(PyObject *self);
static PyObject *PyPf_get_resolution(PyObject *self);
static PyObject *PyPf_get_native_resolution(PyObject *self);
static PyObject *PyPf_get_basedir(PyObject *self);
static PyObject *PyPf_get_render_info(PyObject *self);
static PyObject *PyPf_get_nav_perfstats(PyObject *self);
static PyObject *PyPf_get_mouse_pos(PyObject *self);
static PyObject *PyPf_mouse_over_ui(PyObject *self);

static PyObject *PyPf_enable_unit_selection(PyObject *self);
static PyObject *PyPf_disable_unit_selection(PyObject *self);
static PyObject *PyPf_clear_unit_selection(PyObject *self);
static PyObject *PyPf_get_unit_selection(PyObject *self);

static PyObject *PyPf_get_factions_list(PyObject *self);
static PyObject *PyPf_add_faction(PyObject *self, PyObject *args);
static PyObject *PyPf_remove_faction(PyObject *self, PyObject *args);
static PyObject *PyPf_update_faction(PyObject *self, PyObject *args);
static PyObject *PyPf_set_faction_controllable(PyObject *self, PyObject *args);
static PyObject *PyPf_set_diplomacy_state(PyObject *self, PyObject *args);

static PyObject *PyPf_update_tile(PyObject *self, PyObject *args);
static PyObject *PyPf_set_map_highlight_size(PyObject *self, PyObject *args);
static PyObject *PyPf_get_minimap_position(PyObject *self, PyObject *args);
static PyObject *PyPf_set_minimap_position(PyObject *self, PyObject *args);
static PyObject *PyPf_set_minimap_resize_mask(PyObject *self, PyObject *args);
static PyObject *PyPf_get_minimap_size(PyObject *self);
static PyObject *PyPf_set_minimap_size(PyObject *self, PyObject *args);
static PyObject *PyPf_mouse_over_minimap(PyObject *self);
static PyObject *PyPf_map_height_at_point(PyObject *self, PyObject *args);
static PyObject *PyPf_map_pos_under_cursor(PyObject *self);
static PyObject *PyPf_set_move_on_left_click(PyObject *self);
static PyObject *PyPf_set_attack_on_left_click(PyObject *self);

static PyObject *PyPf_settings_get(PyObject *self, PyObject *args);
static PyObject *PyPf_settings_set(PyObject *self, PyObject *args);
static PyObject *PyPf_settings_create(PyObject *self, PyObject *args);
static PyObject *PyPf_settings_delete(PyObject *self, PyObject *args);

static PyObject *PyPf_get_simstate(PyObject *self);
static PyObject *PyPf_set_simstate(PyObject *self, PyObject *args);

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

    {"set_ambient_light_color", 
    (PyCFunction)PyPf_set_ambient_light_color, METH_VARARGS,
    "Sets the global ambient light color (specified as an RGB multiplier) for the scene."},

    {"set_emit_light_color", 
    (PyCFunction)PyPf_set_emit_light_color, METH_VARARGS,
    "Sets the color (specified as an RGB multiplier) for the global light source."},

    {"set_emit_light_pos", 
    (PyCFunction)PyPf_set_emit_light_pos, METH_VARARGS,
    "Sets the position (in XYZ worldspace coordinates)"},

    {"load_scene", 
    (PyCFunction)PyPf_load_scene, METH_VARARGS,
    "Import list of entities from a PFSCENE file (specified as a path string)."},

    {"register_event_handler", 
    (PyCFunction)PyPf_register_event_handler, METH_VARARGS,
    "Adds a script event handler to be called when the specified global event occurs. "
    "Any weakref user arguments are automatically unpacked before being passed to the handler."},

    {"register_ui_event_handler", 
    (PyCFunction)PyPf_register_ui_event_handler, METH_VARARGS,
    "Same as 'register_event_handler' but the handler will also be run when the simulation state "
    "is pf.G_PAUSED_UI_RUNNING. This is for UI callbacks that should still be invoked even when "
    "the game is in a 'paused' state."},

    {"unregister_event_handler", 
    (PyCFunction)PyPf_unregister_event_handler, METH_VARARGS,
    "Removes a script event handler added by 'register_event_handler'."},

    {"global_event", 
    (PyCFunction)PyPf_global_event, METH_VARARGS,
    "Broadcast a global event so all handlers can get invoked. Any weakref argument is "
    "automatically unpacked before being sent to the handler."},

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

    {"get_native_resolution", 
    (PyCFunction)PyPf_get_native_resolution, METH_NOARGS,
    "Returns the native resolution of the active monitor."},

    {"get_basedir", 
    (PyCFunction)PyPf_get_basedir, METH_NOARGS,
    "Get the path to the top-level game resource folder (parent of 'assets')."},

    {"get_render_info", 
    (PyCFunction)PyPf_get_render_info, METH_NOARGS,
    "Returns a dictionary describing the renderer context. It will have the string keys "
    "'renderer', 'version', 'shading_language_version', and 'vendor'."},

    {"get_nav_perfstats", 
    (PyCFunction)PyPf_get_nav_perfstats, METH_NOARGS,
    "Returns a dictionary holding various performance couners for the navigation subsystem."},

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

    {"get_factions_list",
    (PyCFunction)PyPf_get_factions_list, METH_NOARGS,
    "Returns a list of descriptors (dictionaries) for each faction in the game."},

    {"add_faction",
    (PyCFunction)PyPf_add_faction, METH_VARARGS,
    "Add a new faction with the specified name and color. By default, this faction is mutually at peace with "
    "every other existing faction. By default, new factions are player-controllable."},

    {"remove_faction",
    (PyCFunction)PyPf_remove_faction, METH_VARARGS,
    "Remove the faction with the specified faction_id. This will remove all entities belonging to that faction. "
    "This may change the values of some other entities' faction_ids."},

    {"update_faction",
    (PyCFunction)PyPf_update_faction, METH_VARARGS,
    "Updates the name and color of the faction with the specified faction_id."},

    {"set_faction_controllable",
    (PyCFunction)PyPf_set_faction_controllable, METH_VARARGS,
    "Sets whether units of this faction can be controlled by the player or not."},

    {"set_diplomacy_state",
    (PyCFunction)PyPf_set_diplomacy_state, METH_VARARGS,
    "Symmetrically sets the diplomacy state between two distinct factions (passed in as IDs)."},

    {"update_tile", 
    (PyCFunction)PyPf_update_tile, METH_VARARGS,
    "Update the map tile at the specified coordinates to the new value."},

    {"set_map_highlight_size", 
    (PyCFunction)PyPf_set_map_highlight_size, METH_VARARGS,
    "Determines how many tiles around the currently hovered tile are highlighted. (0 = none, "
    "1 = single tile highlighted, 2 = 3x3 grid highlighted, etc.)"},

    {"get_minimap_position", 
    (PyCFunction)PyPf_get_minimap_position, METH_NOARGS,
    "Returns the current minimap position in virtual screen coordinates."},

    {"set_minimap_position", 
    (PyCFunction)PyPf_set_minimap_position, METH_VARARGS,
    "Set the center position of the minimap in virtual screen coordinates."},

    {"set_minimap_resize_mask", 
    (PyCFunction)PyPf_set_minimap_resize_mask, METH_VARARGS,
    "Set the anchor points for the minimap, to control its bounds as the screen resizes."},

    {"get_minimap_size", 
    (PyCFunction)PyPf_get_minimap_size, METH_NOARGS,
    "Get the center position of the minimap in virtual screen coordinates."},

    {"set_minimap_size", 
    (PyCFunction)PyPf_set_minimap_size, METH_VARARGS,
    "Set the center position of the minimap in virtual screen coordinates."},

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

    {"set_move_on_left_click",
    (PyCFunction)PyPf_set_move_on_left_click, METH_NOARGS,
    "Set the cursor to target mode. The next left click will issue a move command to the location "
    "under the cursor."},

    {"set_attack_on_left_click",
    (PyCFunction)PyPf_set_attack_on_left_click, METH_NOARGS,
    "Set the cursor to target mode. The next left click will issue an attack command to the location "
    "under the cursor."},

    {"settings_get",
    (PyCFunction)PyPf_settings_get, METH_VARARGS,
    "Returns the value of the setting with the specified name. Will throw an exception if the setting is "
    "not found."},

    {"settings_set",
    (PyCFunction)PyPf_settings_set, METH_VARARGS,
    "Updates the value of the setting with the specified name. Will throw an exception if the setting is "
    "not found or if the new value for the setting is invalid."},

    {"settings_create",
    (PyCFunction)PyPf_settings_create, METH_VARARGS,
    "Create a new setting, the value of which will be saved in the settings file and will be accessible "
    "in another session. Settings may hold the following types: int, float, string, bool, and tuple "
    "of 2 floats (vec2). Setting names beginning with 'pf' are reserved for the engine."},

    {"settings_delete",
    (PyCFunction)PyPf_settings_delete, METH_VARARGS,
    "Delete a setting with the specified name. Setting names beginning with 'pf' are reserved for the "
    "engine and may not be deleted."},

    {"get_simstate",
    (PyCFunction)PyPf_get_simstate, METH_NOARGS,
    "Returns the current simulation state."},

    {"set_simstate",
    (PyCFunction)PyPf_set_simstate, METH_VARARGS,
    "Set the current simulation state."},

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

    if(!G_NewGameWithMap(dir, pfmap)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to create new game with the specified map file.");
        return NULL; 
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_new_game_string(PyObject *self, PyObject *args)
{
    const char *mapstr;
    if(!PyArg_ParseTuple(args, "s", &mapstr)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a string.");
        return NULL;
    }

    if(!G_NewGameWithMapString(mapstr)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to create new game with the specified map file.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_ambient_light_color(PyObject *self, PyObject *args)
{
    PyObject *list;
    vec3_t color;

    if(!PyArg_ParseTuple(args, "O!", &PyList_Type, &list))
        return NULL; /* exception already set */

    if(!s_vec3_from_pylist_arg(list, &color))
        return NULL; /* exception already set */

    R_GL_SetAmbientLightColor(color);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_emit_light_color(PyObject *self, PyObject *args)
{
    PyObject *list;
    vec3_t color;

    if(!PyArg_ParseTuple(args, "O!", &PyList_Type, &list))
        return NULL; /* exception already set */

    if(!s_vec3_from_pylist_arg(list, &color))
        return NULL; /* exception already set */

    R_GL_SetLightEmitColor(color);
    Py_RETURN_NONE;
}

static PyObject *PyPf_load_scene(PyObject *self, PyObject *args)
{
    const char *path; 

    if(!PyArg_ParseTuple(args, "s", &path)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string.");
        return NULL;
    }

    if(!Scene_Load(path)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to load scene from the specified file.");
        return NULL;
    }

    G_MakeStaticObjsImpassable();
    return S_Entity_GetAllList();
}

static PyObject *PyPf_set_emit_light_pos(PyObject *self, PyObject *args)
{
    PyObject *list;
    vec3_t pos;

    if(!PyArg_ParseTuple(args, "O!", &PyList_Type, &list))
        return NULL; /* exception already set */

    if(!s_vec3_from_pylist_arg(list, &pos))
        return NULL; /* exception already set */

    R_GL_SetLightPos(pos);
    Py_RETURN_NONE;
}

static PyObject *register_handler(PyObject *self, PyObject *args, int simmask)
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

    bool ret = E_Global_ScriptRegister(event, callable, user_arg, simmask);
    assert(ret == true);
    Py_RETURN_NONE;
}

static PyObject *PyPf_register_event_handler(PyObject *self, PyObject *args)
{
    return register_handler(self, args, G_RUNNING);
}

static PyObject *PyPf_register_ui_event_handler(PyObject *self, PyObject *args)
{
    return register_handler(self, args, G_RUNNING | G_PAUSED_UI_RUNNING);
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
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Could not unregister the specified event handler.");
        return NULL;
    }
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
    struct sval res;
    ss_e status = Settings_Get("pf.video.resolution", &res);
    assert(status == SS_OKAY);

    return Py_BuildValue("(i, i)", (int)res.as_vec2.x, (int)res.as_vec2.y);
}

static PyObject *PyPf_get_native_resolution(PyObject *self)
{
    SDL_DisplayMode dm;
    SDL_GetDesktopDisplayMode(0, &dm);
    return Py_BuildValue("(i, i)", dm.w, dm.h);
}

static PyObject *PyPf_get_basedir(PyObject *self)
{
    extern const char *g_basepath;
    return Py_BuildValue("s", g_basepath);
}

static PyObject *PyPf_get_render_info(PyObject *self)
{
    PyObject *ret = PyDict_New();
    if(!ret) {
        return NULL;
    }

    int rval = 0;
    rval |= PyDict_SetItemString(ret, "version",  Py_BuildValue("s", R_GL_GetInfo(RENDER_INFO_VERSION)));
    rval |= PyDict_SetItemString(ret, "vendor",   Py_BuildValue("s", R_GL_GetInfo(RENDER_INFO_VENDOR)));
    rval |= PyDict_SetItemString(ret, "renderer", Py_BuildValue("s", R_GL_GetInfo(RENDER_INFO_RENDERER)));
    rval |= PyDict_SetItemString(ret, "shading_language_version", Py_BuildValue("s", R_GL_GetInfo(RENDER_INFO_SL_VERSION)));
    assert(0 == rval);

    return ret;
}

static PyObject *PyPf_get_nav_perfstats(PyObject *self)
{
    PyObject *ret = PyDict_New();
    if(!ret) {
        return NULL;
    }

    struct fc_stats stats;
    N_FC_GetStats(&stats);

    int rval = 0;
    rval |= PyDict_SetItemString(ret, "los_used",           Py_BuildValue("i", stats.los_used));
    rval |= PyDict_SetItemString(ret, "los_max",            Py_BuildValue("i", stats.los_max));
    rval |= PyDict_SetItemString(ret, "los_hit_rate",       Py_BuildValue("f", stats.los_hit_rate));
    rval |= PyDict_SetItemString(ret, "flow_used",          Py_BuildValue("i", stats.flow_used));
    rval |= PyDict_SetItemString(ret, "flow_max",           Py_BuildValue("i", stats.flow_max));
    rval |= PyDict_SetItemString(ret, "flow_hit_rate",      Py_BuildValue("f", stats.flow_hit_rate));
    rval |= PyDict_SetItemString(ret, "mapping_used",       Py_BuildValue("i", stats.mapping_used));
    rval |= PyDict_SetItemString(ret, "mapping_max",        Py_BuildValue("i", stats.mapping_max));
    rval |= PyDict_SetItemString(ret, "mapping_hit_rate",   Py_BuildValue("f", stats.mapping_hit_rate));
    rval |= PyDict_SetItemString(ret, "grid_path_used",     Py_BuildValue("i", stats.grid_path_used));
    rval |= PyDict_SetItemString(ret, "grid_path_max",      Py_BuildValue("i", stats.grid_path_max));
    rval |= PyDict_SetItemString(ret, "grid_path_hit_rate", Py_BuildValue("f", stats.grid_path_hit_rate));
    assert(0 == rval);

    return ret;
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
    enum selection_type sel_type;
    const pentity_kvec_t *sel = G_Sel_Get(&sel_type);

    PyObject *ret = PyList_New(0);
    if(!ret)
        return NULL;

    for(int i = 0; i < kv_size(*sel); i++) {
        PyObject *ent = S_Entity_ObjForUID(kv_A(*sel, i)->uid);
        if(ent) {
            PyList_Append(ret, ent);
        }
    }

    return ret;
}

static PyObject *PyPf_get_factions_list(PyObject *self)
{
    char names[MAX_FACTIONS][MAX_FAC_NAME_LEN];
    vec3_t colors[MAX_FACTIONS];
    bool controllable[MAX_FACTIONS];

    size_t num_facs = G_GetFactions(names, colors, controllable);

    PyObject *ret = PyList_New(num_facs);
    if(!ret)
        goto fail_list;

    for(int i = 0; i < num_facs; i++) {
        PyObject *fac_dict = PyDict_New();
        if(!fac_dict)
            goto fail_dict;
        PyList_SetItem(ret, i, fac_dict); /* ret now owns fac_dict */

        PyObject *name = PyString_FromString(names[i]);
        if(!name)
            goto fail_dict;
        PyDict_SetItemString(fac_dict, "name", name);
        Py_DECREF(name);

        PyObject *color = Py_BuildValue("(iiii)", (int)colors[i].x, (int)colors[i].y, (int)colors[i].z, 255);
        if(!color)
            goto fail_dict;
        PyDict_SetItemString(fac_dict, "color", color);
        Py_DECREF(color);

        PyObject *control = controllable[i] ? Py_True : Py_False;
        PyDict_SetItemString(fac_dict, "controllable", control);
    }

    return ret;

fail_dict:
    Py_DECREF(ret);
fail_list:
    return NULL;
}

static PyObject *PyPf_add_faction(PyObject *self, PyObject *args)
{
    const char *name;
    int color_ints[4];

    if(!PyArg_ParseTuple(args, "s(iiii)", &name, &color_ints[0], &color_ints[1], &color_ints[2], &color_ints[3])) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string and a tuple of 4 ints (RGBA color descriptor).");
        return NULL;
    }

    vec3_t color = {color_ints[0], color_ints[1], color_ints[2]};
    if(!G_AddFaction(name, color)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to add the specified faction."); 
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_remove_faction(PyObject *self, PyObject *args)
{
    int faction_id;

    if(!PyArg_ParseTuple(args, "i", &faction_id)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an integer.");
        return NULL;
    }

    if(!G_RemoveFaction(faction_id)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to remove the specified faction."); 
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_update_faction(PyObject *self, PyObject *args)
{
    int faction_id;
    const char *name;
    int color_ints[4];

    if(!PyArg_ParseTuple(args, "is(iiii)", &faction_id, &name, 
        &color_ints[0], &color_ints[1], &color_ints[2], &color_ints[3])) {

        PyErr_SetString(PyExc_TypeError, "Arguments must be a string and a tuple of 4 ints (RGBA color descriptor).");
        return NULL;
    }

    vec3_t color = {color_ints[0], color_ints[1], color_ints[2]};

    bool controllable[MAX_FACTIONS];
    size_t num_facs = G_GetFactions(NULL, NULL, controllable);

    if(!G_UpdateFaction(faction_id, name, color, controllable[faction_id])) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to update the specified faction."); 
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_faction_controllable(PyObject *self, PyObject *args)
{
    int faction_id;
    PyObject *new_controllable;

    if(!PyArg_ParseTuple(args, "iO", &faction_id, &new_controllable)) {

        PyErr_SetString(PyExc_TypeError, "Arguments must be an integer and a boolean expression.");
        return NULL;
    }

    char names[MAX_FACTIONS][MAX_FAC_NAME_LEN];
    vec3_t colors[MAX_FACTIONS];

    size_t num_facs = G_GetFactions(names, colors, NULL);

    if(!G_UpdateFaction(faction_id, names[faction_id], colors[faction_id], PyObject_IsTrue(new_controllable))) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to update the specified faction."); 
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_diplomacy_state(PyObject *self, PyObject *args)
{
    int fac_id_a, fac_id_b;
    enum diplomacy_state ds;

    if(!PyArg_ParseTuple(args, "iii", &fac_id_a, &fac_id_b, &ds)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a three integers.");
        return NULL;
    }

    if(!G_SetDiplomacyState(fac_id_a, fac_id_b, ds)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to set the diplomacy state: invalid arguments.");
        return NULL;
    }
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

    if(!G_UpdateTile(&desc, tile)) {
        PyErr_SetString(PyExc_RuntimeError, "Could not update tile.");
        return NULL;
    }

    if(!G_UpdateMinimapChunk(desc.chunk_r, desc.chunk_c)) {
        PyErr_SetString(PyExc_RuntimeError, "Could not update minimap chunk.");
        return NULL;
    }

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

static PyObject *PyPf_get_minimap_position(PyObject *self, PyObject *args)
{
    float x, y;    
    G_GetMinimapPos(&x, &y);
    return Py_BuildValue("(f,f)", x, y);
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

static PyObject *PyPf_set_minimap_resize_mask(PyObject *self, PyObject *args)
{
    int resize_mask;

    if(!PyArg_ParseTuple(args, "i", &resize_mask)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer.");
        return NULL;
    }

    if((resize_mask & ANCHOR_X_MASK) == 0
    || (resize_mask & ANCHOR_Y_MASK) == 0) {
        PyErr_SetString(PyExc_RuntimeError, "Invalid reisize mask: the window must have at least one anchor in each dimension.");
        return NULL;
    }

    G_SetMinimapResizeMask(resize_mask);
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_minimap_size(PyObject *self)
{
    return Py_BuildValue("i", G_GetMinimapSize());
}

static PyObject *PyPf_set_minimap_size(PyObject *self, PyObject *args)
{
    int size;

    if(!PyArg_ParseTuple(args, "i", &size)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an integer.");
        return NULL;
    }

    G_SetMinimapSize(size);
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

static PyObject *PyPf_set_move_on_left_click(PyObject *self)
{
    G_Move_SetMoveOnLeftClick();
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_attack_on_left_click(PyObject *self)
{
    G_Move_SetAttackOnLeftClick();
    Py_RETURN_NONE;
}

static PyObject *PyPf_settings_get(PyObject *self, PyObject *args)
{
    const char *sname;

    if(!PyArg_ParseTuple(args, "s", &sname)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string.");
        return NULL;
    }

    struct sval val;
    ss_e status = Settings_Get(sname, &val);
    if(status == SS_NO_SETTING) {
        PyErr_SetString(PyExc_RuntimeError, "The setting with the given name does not exist.");
        return NULL;
    }

    switch(val.type) {
    case ST_TYPE_STRING:    return Py_BuildValue("s", val.as_string);
    case ST_TYPE_FLOAT:     return Py_BuildValue("f", val.as_float);
    case ST_TYPE_INT:       return Py_BuildValue("i", val.as_int);
    case ST_TYPE_BOOL:      if(val.as_bool) Py_RETURN_TRUE; else Py_RETURN_FALSE;
    case ST_TYPE_VEC2:      return Py_BuildValue("(f, f)", val.as_vec2.x, val.as_vec2.y);
    default: assert(0);     Py_RETURN_NONE;
    }
}

static bool sval_from_pyobj(PyObject *obj, struct sval *out)
{
    if(PyString_Check(obj)) {

        out->type = ST_TYPE_STRING;
        strncpy(out->as_string, PyString_AS_STRING(obj), sizeof(out->as_string));
        out->as_string[sizeof(out->as_string)-1] = '\0';

    }else if(PyBool_Check(obj)) {
    
        out->type = ST_TYPE_BOOL;
        out->as_bool = PyObject_IsTrue(obj);

    }else if(PyInt_Check(obj)) {
    
        out->type = ST_TYPE_INT;
        out->as_int = PyInt_AS_LONG(obj);

    }else if(PyFloat_Check(obj)) {
    
        out->type = ST_TYPE_FLOAT;
        out->as_float = PyFloat_AS_DOUBLE(obj);

    }else if(PyArg_ParseTuple(obj, "ff", &out->as_vec2.x, &out->as_vec2.y)) {
    
        out->type = ST_TYPE_VEC2;

    }else {
        return false;
    }
    return true;
}

static PyObject *PyPf_settings_set(PyObject *self, PyObject *args)
{
    const char *sname;
    PyObject *nvobj;

    if(!PyArg_ParseTuple(args, "sO", &sname, &nvobj)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string and an object.");
        return NULL;
    }

    struct sval newval;
    if(!sval_from_pyobj(nvobj, &newval)) {
        PyErr_SetString(PyExc_TypeError, "The new value is not one of the allowed types for settings.");
        return NULL;
    }

    ss_e status = Settings_Set(sname, &newval);
    if(status == SS_NO_SETTING) {
        PyErr_SetString(PyExc_RuntimeError, "The setting with the given name does not exist.");
        return NULL;
    }else if(status == SS_INVALID_VAL) {
        PyErr_SetString(PyExc_RuntimeError, "The new value is not allowed for this setting.");
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *PyPf_settings_create(PyObject *self, PyObject *args)
{
    const char *name;
    PyObject *val;

    if(!PyArg_ParseTuple(args, "sO", &name, &val)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string (name) and an object (value).");
        return NULL;
    }

    if(0 == strncmp(name, "pf", 2)) {
        PyErr_SetString(PyExc_RuntimeError, "Settings beginning with 'pf' are reserved for the engine.");
        return NULL;
    }

    struct sval sett_val;
    if(!sval_from_pyobj(val, &sett_val)) {
        PyErr_SetString(PyExc_TypeError, "The new value is not one of the allowed types for settings.");
        return NULL;
    }

    struct setting new_sett = (struct setting) {
        .val = sett_val,
        .prio = 2,
        .validate = NULL,
        .commit = NULL
    };
    strncpy(new_sett.name, name, sizeof(new_sett.name));
    new_sett.name[sizeof(new_sett.name)-1] = '\0';

    ss_e status = Settings_Create(new_sett);
    if(status != SS_OKAY) {
        char errstr[256];
        sprintf(errstr, "Could not create setting. [err: %d]", status);
        PyErr_SetString(PyExc_RuntimeError, errstr);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *PyPf_settings_delete(PyObject *self, PyObject *args)
{
    const char *name;

    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string (name).");
        return NULL;
    }

    if(0 == strncmp(name, "pf", 2)) {
        PyErr_SetString(PyExc_RuntimeError, "Settings beginning with 'pf' are reserved for the engine.");
        return NULL;
    }

    ss_e status = Settings_Delete(name);
    if(status != SS_OKAY) {
        char errstr[256];
        sprintf(errstr, "Could not delete setting. [err: %d]", status);
        PyErr_SetString(PyExc_RuntimeError, errstr);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *PyPf_get_simstate(PyObject *self)
{
    return Py_BuildValue("i", G_GetSimState());
}

static PyObject *PyPf_set_simstate(PyObject *self, PyObject *args)
{
    enum simstate ss;

    if(!PyArg_ParseTuple(args, "i", &ss)
    || (ss != G_RUNNING && ss != G_PAUSED_FULL && ss != G_PAUSED_UI_RUNNING)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer (valid simulation state value)");
        return NULL;
    }

    G_SetSimState(ss);
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
        return NULL; /* exception already set */
    if(!s_quat_from_pylist_arg(q2_list, &q2))
        return NULL; /* exception already set */

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

/* Due to indeterminate order of object deletion in 'Py_Finalize', there may 
 * be issues with destructor calls of any remaining entities. This will flood
 * stderr with ugly warnings, which we cannot do anything about. We elect 
 * to perform an additional step of 'manual' garbage collection before handing 
 * off to 'Py_Finalize' to make sure all entity destructors are called and 
 * the interpreter can have a 'clean' shutdown. 
 */
static void s_gc_all_ents(void)
{
    PyObject *sys_mod_dict = PyImport_GetModuleDict();
    assert(sys_mod_dict);
    PyObject *modules = PyMapping_Values(sys_mod_dict);
    assert(modules);

    for(int i = 0; i < PyList_Size(modules); i++) {

        PyObject *curr_module = PyList_GetItem(modules, i);
        PyObject *module_dict = PyModule_GetDict(curr_module);
        if(!module_dict)
            continue;

        if(!strncmp(PyModule_GetName(curr_module), "__", 2)
        &&  strncmp(PyModule_GetName(curr_module), "__main__", strlen("__main__")))
            continue;

        PyObject *module_vals = PyMapping_Values(module_dict);
        PyObject *val_names = PyMapping_Keys(module_dict);
        assert(module_vals && val_names);

        for(int j = 0; j < PyList_Size(module_vals); j++) {
        
            PyObject *curr_val = PyList_GetItem(module_vals, j);
            PyObject *curr_name = PyList_GetItem(val_names, j);

            if(!strncmp(PyString_AsString(curr_name), "__", 2))
                continue;

            if(PyCallable_Check(curr_val) || PyModule_Check(curr_val))
                continue;

            PyMapping_DelItem(module_dict, curr_name);
        }
        Py_DECREF(module_vals);
        Py_DECREF(val_names);
    }
    Py_DECREF(modules);

    /* The list should own the last remaining references to living entities.
     * Free the list and its' entities. */
    PyObject *list = S_Entity_GetAllList();
    Py_DECREF(list);
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
    S_Constants_Expose(module); 
}

bool S_Init(char *progname, const char *base_path, struct nk_context *ctx)
{
    Py_SetProgramName(progname);
    Py_Initialize();

    if(!S_UI_Init(ctx))
        return false;
    if(!S_Entity_Init())
        return false;

    char script_dir[512];
    strcpy(script_dir, g_basepath);
    strcat(script_dir, "/scripts");
    if(0 != PyList_Append(PySys_GetObject("path"), Py_BuildValue("s", script_dir)))
        return false;

    initpf();
    return true;
}

void S_Shutdown(void)
{
    s_gc_all_ents();
    Py_Finalize();
    S_Entity_Shutdown();
    S_UI_Shutdown();
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
    /* PyTuple_SetItem steals references! However, we wish to hold on to the user_arg. The event_arg
     * is DECREF'd once after all the handlers for the event have been executed. */
    Py_INCREF(user_arg);
    Py_INCREF(event_arg);
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

    case EVENT_GAME_SIMSTATE_CHANGED:
        return Py_BuildValue("(i)", (intptr_t)arg);

    default:
        Py_RETURN_NONE;
    }
}

script_opaque_t S_UnwrapIfWeakref(script_opaque_t arg)
{
    assert(arg);
    if(PyWeakref_Check(arg)) {
        return PyWeakref_GetObject(arg); 
    }
    return arg;
}

bool S_ObjectsEqual(script_opaque_t a, script_opaque_t b)
{
    return (1 == PyObject_RichCompareBool(a, b, Py_EQ));
}

