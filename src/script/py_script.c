/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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
#include <frameobject.h>

#include "py_entity.h"
#include "py_ui.h"
#include "py_tile.h"
#include "py_constants.h"
#include "py_pickle.h"
#include "py_camera.h"
#include "py_task.h"
#include "py_region.h"
#include "py_error.h"
#include "public/script.h"
#include "../entity.h"
#include "../game/public/game.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../navigation/public/nav.h"
#include "../audio/public/audio.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../phys/public/phys.h"
#include "../lib/public/SDL_vec_rwops.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/mem.h"
#include "../event.h"
#include "../config.h"
#include "../scene.h"
#include "../settings.h"
#include "../main.h"
#include "../ui.h"
#include "../session.h"
#include "../perf.h"
#include "../cursor.h"
#include "../task.h"
#include "../sched.h"

#include <SDL.h>
#include <stdio.h>


#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))

struct script_arg{
    const char *path;
    int argc;
    const char **argv;
};

static PyObject *PyPf_load_map(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_load_map_string(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_set_ambient_light_color(PyObject *self, PyObject *args);
static PyObject *PyPf_set_emit_light_color(PyObject *self, PyObject *args);
static PyObject *PyPf_set_emit_light_pos(PyObject *self, PyObject *args);
static PyObject *PyPf_load_scene(PyObject *self, PyObject *args, PyObject *kwargs);

static PyObject *PyPf_register_event_handler(PyObject *self, PyObject *args);
static PyObject *PyPf_register_ui_event_handler(PyObject *self, PyObject *args);
static PyObject *PyPf_unregister_event_handler(PyObject *self, PyObject *args);
static PyObject *PyPf_global_event(PyObject *self, PyObject *args);
static PyObject *PyPf_get_ticks(PyObject *self);
static PyObject *PyPf_ticks_delta(PyObject *self, PyObject *args);
static PyObject *PyPf_flush_tasks(PyObject *self);

static PyObject *PyPf_get_active_camera(PyObject *self);
static PyObject *PyPf_set_active_camera(PyObject *self, PyObject *args);

static PyObject *PyPf_prev_frame_ms(PyObject *self);
static PyObject *PyPf_prev_frame_perfstats(PyObject *self);
static PyObject *PyPf_get_resolution(PyObject *self);
static PyObject *PyPf_get_native_resolution(PyObject *self);
static PyObject *PyPf_get_basedir(PyObject *self);
static PyObject *PyPf_get_render_info(PyObject *self);
static PyObject *PyPf_get_nav_perfstats(PyObject *self);
static PyObject *PyPf_get_mouse_pos(PyObject *self);
static PyObject *PyPf_mouse_over_ui(PyObject *self);
static PyObject *PyPf_ui_text_edit_has_focus(PyObject *self);
static PyObject *PyPf_get_active_window(PyObject *self);
static PyObject *PyPf_get_file_size(PyObject *self, PyObject *args);
static PyObject *PyPf_shift_pressed(PyObject *self);
static PyObject *PyPf_ctrl_pressed(PyObject *self);
static PyObject *PyPf_get_key_name(PyObject *self, PyObject *args);

static PyObject *PyPf_get_active_font(PyObject *self);
static PyObject *PyPf_set_active_font(PyObject *self, PyObject *args);

static PyObject *PyPf_show_regions(PyObject *self);
static PyObject *PyPf_hide_regions(PyObject *self);
static PyObject *PyPf_enable_fog_of_war(PyObject *self);
static PyObject *PyPf_disable_fog_of_war(PyObject *self);
static PyObject *PyPf_explore_map(PyObject *self, PyObject *args);
static PyObject *PyPf_enable_unit_selection(PyObject *self);
static PyObject *PyPf_disable_unit_selection(PyObject *self);
static PyObject *PyPf_clear_unit_selection(PyObject *self);
static PyObject *PyPf_get_unit_selection(PyObject *self);
static PyObject *PyPf_set_unit_selection(PyObject *self, PyObject *args);
static PyObject *PyPf_get_hovered_unit(PyObject *self);
static PyObject *PyPf_entities_for_tag(PyObject *self, PyObject *args);

static PyObject *PyPf_hide_healthbars(PyObject *self);
static PyObject *PyPf_show_healthbars(PyObject *self);

static PyObject *PyPf_get_resource_list(PyObject *self);
static PyObject *PyPf_get_resource_stored(PyObject *self, PyObject *args);
static PyObject *PyPf_get_resource_capacity(PyObject *self, PyObject *args);
static PyObject *PyPf_get_factions_list(PyObject *self);
static PyObject *PyPf_add_faction(PyObject *self, PyObject *args);
static PyObject *PyPf_remove_faction(PyObject *self, PyObject *args);
static PyObject *PyPf_update_faction(PyObject *self, PyObject *args);
static PyObject *PyPf_set_faction_controllable(PyObject *self, PyObject *args);
static PyObject *PyPf_set_diplomacy_state(PyObject *self, PyObject *args);
static PyObject *PyPf_get_diplomacy_state(PyObject *self, PyObject *args);

static PyObject *PyPf_get_tile(PyObject *self, PyObject *args);
static PyObject *PyPf_update_tile(PyObject *self, PyObject *args);
static PyObject *PyPf_set_map_highlight_size(PyObject *self, PyObject *args);
static PyObject *PyPf_get_minimap_position(PyObject *self, PyObject *args);
static PyObject *PyPf_set_minimap_position(PyObject *self, PyObject *args);
static PyObject *PyPf_set_minimap_resize_mask(PyObject *self, PyObject *args);
static PyObject *PyPf_get_minimap_size(PyObject *self);
static PyObject *PyPf_set_minimap_size(PyObject *self, PyObject *args);
static PyObject *PyPf_set_minimap_border_clr(PyObject *self, PyObject *args);
static PyObject *PyPf_set_minimap_render_all_ents(PyObject *self, PyObject *args);
static PyObject *PyPf_mouse_over_minimap(PyObject *self);
static PyObject *PyPf_map_height_at_point(PyObject *self, PyObject *args);
static PyObject *PyPf_map_nearest_pathable(PyObject *self, PyObject *args);
static PyObject *PyPf_map_pos_under_cursor(PyObject *self);
static PyObject *PyPf_draw_text(PyObject *self, PyObject *args);
static PyObject *PyPf_set_storage_site_ui_style(PyObject *self, PyObject *args);
static PyObject *PyPf_set_storage_site_ui_border_color(PyObject *self, PyObject *args);
static PyObject *PyPf_set_storage_site_ui_font_color(PyObject *self, PyObject *args);
static PyObject *PyPf_storage_site_show_ui(PyObject *self, PyObject *args);

static PyObject *PyPf_set_move_on_left_click(PyObject *self);
static PyObject *PyPf_set_attack_on_left_click(PyObject *self);
static PyObject *PyPf_set_build_on_left_click(PyObject *self);
static PyObject *PyPf_set_gather_on_left_click(PyObject *self);
static PyObject *PyPf_set_pick_up_on_left_click(PyObject *self);
static PyObject *PyPf_set_drop_off_on_left_click(PyObject *self);
static PyObject *PyPf_set_transport_on_left_click(PyObject *self);
static PyObject *PyPf_set_click_move_enabled(PyObject *self, PyObject *args);

static PyObject *PyPf_settings_get(PyObject *self, PyObject *args);
static PyObject *PyPf_settings_set(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_settings_create(PyObject *self, PyObject *args);
static PyObject *PyPf_settings_delete(PyObject *self, PyObject *args);
static PyObject *PyPf_settings_flush(PyObject *self);

static PyObject *PyPf_get_simstate(PyObject *self);
static PyObject *PyPf_set_simstate(PyObject *self, PyObject *args);

static PyObject *PyPf_set_system_cursor(PyObject *self, PyObject *args);
static PyObject *PyPf_set_named_cursor(PyObject *self, PyObject *args);
static PyObject *PyPf_activate_system_cursor(PyObject *self, PyObject *args);
static PyObject *PyPf_activate_named_cursor(PyObject *self, PyObject *args);
static PyObject *PyPf_set_cursor_rts_mode(PyObject *self, PyObject *args);
static PyObject *PyPf_get_cursor_rts_mode(PyObject *self);

static PyObject *PyPf_multiply_quaternions(PyObject *self, PyObject *args);
static PyObject *PyPf_rand(PyObject *self, PyObject *args);

static PyObject *PyPf_pickle_object(PyObject *self, PyObject *args);
static PyObject *PyPf_unpickle_object(PyObject *self, PyObject *args);

static PyObject *PyPf_save_session(PyObject *self, PyObject *args);
static PyObject *PyPf_load_session(PyObject *self, PyObject *args);

static PyObject *PyPf_exec(PyObject *self, PyObject *args);
static PyObject *PyPf_exec_push(PyObject *self, PyObject *args);
static PyObject *PyPf_exec_pop(PyObject *self, PyObject *args);
static PyObject *PyPf_exec_pop_to_root(PyObject *self, PyObject *args);
static PyObject *PyPf_session_stack_depth(PyObject *self, PyObject *args);

static PyObject *PyPf_nearest_ent(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_ents_in_circle(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_ents_in_rect(PyObject *self, PyObject *args, PyObject *kwargs);

static PyObject *PyPf_play_music(PyObject *self, PyObject *args);
static PyObject *PyPf_curr_music(PyObject *self);
static PyObject *PyPf_get_all_music(PyObject *self);
static PyObject *PyPf_play_effect(PyObject *self, PyObject *args);
static PyObject *PyPf_play_global_effect(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_spawn_projectile(PyObject *self, PyObject *args);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyMethodDef pf_module_methods[] = {

    {"load_map", 
    (PyCFunction)PyPf_load_map, METH_VARARGS | METH_KEYWORDS,
    "Loads the map from the specified file."},

    {"load_map_string", 
    (PyCFunction)PyPf_load_map_string, METH_VARARGS | METH_KEYWORDS,
    "Loads the map from the specified PFMAP string."},

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
    (PyCFunction)PyPf_load_scene, METH_VARARGS | METH_KEYWORDS,
    "Import list of entities from a PFSCENE file (specified as a path string). "
    "Returns a tuple of the following items: list of loaded entities, list of loaded regions."},

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

    {"get_ticks", 
    (PyCFunction)PyPf_get_ticks, METH_NOARGS,
    "Get the current number of game ticks (milliseconsd) - only useful in finding elapsed "
    "times in relation to other ticks."},

    {"ticks_delta", 
    (PyCFunction)PyPf_ticks_delta, METH_VARARGS,
    "Compute the MS delta between two tick values retried by 'get_ticks'. This handles unsigned overflow "
    "unlike regular Python integer arithmnetic."},

    {"flush_tasks", 
    (PyCFunction)PyPf_flush_tasks, METH_VARARGS,
    "Run every single active scripting task until it either gets blocked or completes. Useful mainly "
    "for serialization during initialization."},

    {"get_active_camera", 
    (PyCFunction)PyPf_get_active_camera, METH_NOARGS,
    "Get a pf.Camera object describing the active camera from whose point of view the scene is currently rendered."},

    {"set_active_camera", 
    (PyCFunction)PyPf_set_active_camera, METH_VARARGS,
    "Set a pf.Camera object to be the active camera from whose point of view the scene is currently rendered."},

    {"prev_frame_ms", 
    (PyCFunction)PyPf_prev_frame_ms, METH_NOARGS,
    "Get the duration of the previous game frame in milliseconds."},

    {"prev_frame_perfstats", 
    (PyCFunction)PyPf_prev_frame_perfstats, METH_NOARGS,
    "Get a dictionary of the performance data for the previous frame."},

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

    {"ui_text_edit_has_focus", 
    (PyCFunction)PyPf_ui_text_edit_has_focus, METH_NOARGS,
    "Returns True if the mouse cursor is currently in an editable text field of a UI window, "
    "such that the field records the keystrokes."},

    {"get_active_window", 
    (PyCFunction)PyPf_get_active_window, METH_NOARGS,
    "Get the currently active (having focus) window, or None."},

    {"get_file_size", 
    (PyCFunction)PyPf_get_file_size, METH_VARARGS,
    "Get the size (in bytes) of a Python file object."},

    {"shift_pressed", 
    (PyCFunction)PyPf_shift_pressed, METH_NOARGS,
    "Returns True if either of the SHIFT keys are currently pressed."},

    {"ctrl_pressed", 
    (PyCFunction)PyPf_ctrl_pressed, METH_NOARGS,
    "Returns True if either of the CTRL keys are currently pressed."},

    {"get_key_name", 
    (PyCFunction)PyPf_get_key_name, METH_VARARGS,
    "Returns the string name for an SDL_Keycode integer value."},

    {"get_active_font", 
    (PyCFunction)PyPf_get_active_font, METH_NOARGS,
    "Get the name of the current active font."},

    {"set_active_font", 
    (PyCFunction)PyPf_set_active_font, METH_VARARGS,
    "Set the current active font to that of the specified name."},

    {"show_regions", 
    (PyCFunction)PyPf_show_regions, METH_NOARGS,
    "Show region outlines and names over the map surface."},

    {"hide_regions", 
    (PyCFunction)PyPf_hide_regions, METH_NOARGS,
    "Hide region outlines and names."},

    {"enable_fog_of_war", 
    (PyCFunction)PyPf_enable_fog_of_war, METH_NOARGS,
    "Enable the fog of war."},

    {"disable_fog_of_war", 
    (PyCFunction)PyPf_disable_fog_of_war, METH_NOARGS,
    "Disable the fog of war."},

    {"explore_map", 
    (PyCFunction)PyPf_explore_map, METH_VARARGS,
    "Set the entire map as having being 'explored' for a particular faction."},

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

    {"set_unit_selection", 
    (PyCFunction)PyPf_set_unit_selection, METH_VARARGS,
    "Overwrites the list of objects currently selected by the player with the specified entity list."},

    {"get_hovered_unit", 
    (PyCFunction)PyPf_get_hovered_unit, METH_NOARGS,
    "Get the closest unit under the mouse cursor, or None."},

    {"entities_for_tag", 
    (PyCFunction)PyPf_entities_for_tag, METH_VARARGS,
    "Get a tuple of entities that have the specific tag."},

    {"hide_healthbars", 
    (PyCFunction)PyPf_hide_healthbars, METH_NOARGS,
    "Disable rendering of healthbars. Overrides the user-configurable dynamic setting."},

    {"show_healthbars", 
    (PyCFunction)PyPf_show_healthbars, METH_NOARGS,
    "Re-enable showing the healthbars after a 'hide_healthbars' call. Note that the healthbars will only "
    "be rendered if the corresponding user-configurable setting is set."},

    {"get_resource_list",
    (PyCFunction)PyPf_get_resource_list, METH_NOARGS,
    "Returns a list of the names of all the resources that are present (or have ever been present) in the current session."},

    {"get_resource_stored",
    (PyCFunction)PyPf_get_resource_stored, METH_VARARGS,
    "Returns the total amount of a particular resource between all player-controlled storage sites."},

    {"get_resource_capacity",
    (PyCFunction)PyPf_get_resource_capacity, METH_VARARGS,
    "Returns the total capacity for storing a particular resource between all player-controlled storage sites."},

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

    {"get_diplomacy_state",
    (PyCFunction)PyPf_get_diplomacy_state, METH_VARARGS,
    "Query the diplomacy state of the specified two faction IDs."},

    {"get_tile", 
    (PyCFunction)PyPf_get_tile, METH_VARARGS,
    "Get the pf.Tile object describing the tile at the specified coordinates."},

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
    "Set the center position of the minimap in virtual screen coordinates. "
    "A size of 0 fully hides the minimap."},

    {"set_minimap_border_clr", 
    (PyCFunction)PyPf_set_minimap_border_clr, METH_VARARGS,
    "Set the border color for the minimap."},

    {"set_minimap_render_all_ents", 
    (PyCFunction)PyPf_set_minimap_render_all_ents, METH_VARARGS,
    "Set a boolean to control whether all entities should be shown on the minimap, or just "
    "more relevant ones, such as movable ones and buildings."},

    {"mouse_over_minimap",
    (PyCFunction)PyPf_mouse_over_minimap, METH_NOARGS,
    "Returns true if the mouse cursor is over the minimap, false otherwise."},

    {"map_height_at_point",
    (PyCFunction)PyPf_map_height_at_point, METH_VARARGS,
    "Returns the Y-dimension map height at the specified XZ coordinate. Returns None if the "
    "specified coordinate is outside the map bounds."},

    {"map_nearest_pathable",
    (PyCFunction)PyPf_map_nearest_pathable, METH_VARARGS,
    "Returns the closest XZ map position that is pathable and not currently blocked."},

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

    {"set_build_on_left_click",
    (PyCFunction)PyPf_set_build_on_left_click, METH_NOARGS,
    "Set the cursor to target mode. The next left click will issue a build command to the building "
    "under the cursor."},

    {"set_gather_on_left_click",
    (PyCFunction)PyPf_set_gather_on_left_click, METH_NOARGS,
    "Set the cursor to target mode. The next left click will issue a gather command to the resource "
    "under the cursor."},

    {"set_pick_up_on_left_click",
    (PyCFunction)PyPf_set_pick_up_on_left_click, METH_NOARGS,
    "Set the cursor to target mode. The next left click will issue a 'pick up' command to the storage site "
    "under the cursor."},

    {"set_drop_off_on_left_click",
    (PyCFunction)PyPf_set_drop_off_on_left_click, METH_NOARGS,
    "Set the cursor to target mode. The next left click will issue a 'drop off' command to the storage site "
    "under the cursor."},

    {"set_transport_on_left_click",
    (PyCFunction)PyPf_set_transport_on_left_click, METH_NOARGS,
    "Set the cursor to target mode. The next left click will issue a 'transport' command to the storage site "
    "under the cursor."},

    {"set_click_move_enabled",
    (PyCFunction)PyPf_set_click_move_enabled, METH_VARARGS,
    "Enable or disable issuing move orders by right-clicking."},

    {"draw_text",
    (PyCFunction)PyPf_draw_text, METH_VARARGS,
    "Draw a text label with the specified bounds (X, Y, W, H) )and with the specified color (R, G, B, A). "
    "The label lasts for one frame, meaning this should be called every tick to keep the label fixed."},

    {"set_storage_site_ui_style",
    (PyCFunction)PyPf_set_storage_site_ui_style, METH_VARARGS,
    "Set the storage site UI background to the specified color (R, G, B, A) or image (string)."},

    {"set_storage_site_ui_border_color",
    (PyCFunction)PyPf_set_storage_site_ui_border_color, METH_VARARGS,
    "Set the storage site UI border color to the specified color (R, G, B, A)."},

    {"set_storage_site_ui_font_color",
    (PyCFunction)PyPf_set_storage_site_ui_font_color, METH_VARARGS,
    "Set the storage site UI text color to the specified color (R, G, B, A)."},

    {"storage_site_show_ui",
    (PyCFunction)PyPf_storage_site_show_ui, METH_VARARGS,
    "Set a flag controlling whether or not the UI windows are rendered over the storage sites."},

    {"settings_get",
    (PyCFunction)PyPf_settings_get, METH_VARARGS,
    "Returns the value of the setting with the specified name. Will throw an exception if the setting is "
    "not found."},

    {"settings_set",
    (PyCFunction)PyPf_settings_set, METH_VARARGS | METH_KEYWORDS,
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

    {"settings_flush",
    (PyCFunction)PyPf_settings_flush, METH_VARARGS,
    "Write the current settings to the settings file."},

    {"get_simstate",
    (PyCFunction)PyPf_get_simstate, METH_NOARGS,
    "Returns the current simulation state."},

    {"set_simstate",
    (PyCFunction)PyPf_set_simstate, METH_VARARGS,
    "Set the current simulation state."},

    {"set_system_cursor",
    (PyCFunction)PyPf_set_system_cursor, METH_VARARGS,
    "Set a BMP for one of the cursors used by the engine core."},

    {"set_named_cursor",
    (PyCFunction)PyPf_set_named_cursor, METH_VARARGS,
    "Load a BMP cursor and associate it with a particular name."},

    {"activate_system_cursor",
    (PyCFunction)PyPf_activate_system_cursor, METH_VARARGS,
    "Make a cursor (specified by CURSOR enum type) the currently displayed cursor."},

    {"activate_named_cursor",
    (PyCFunction)PyPf_activate_named_cursor, METH_VARARGS,
    "Make a cursor (specified by name) the currently displayed cursor."},

    {"set_cursor_rts_mode",
    (PyCFunction)PyPf_set_cursor_rts_mode, METH_VARARGS,
    "Activates or deactivates the cursor RTS mode which keeps the cursor icon updated based on which edge "
    "of the screen it is at."},

    {"get_cursor_rts_mode",
    (PyCFunction)PyPf_get_cursor_rts_mode, METH_NOARGS,
    "Returns the current state of the RTS cursor mode enablement."},

    {"multiply_quaternions",
    (PyCFunction)PyPf_multiply_quaternions, METH_VARARGS,
    "Returns the normalized result of multiplying 2 quaternions (specified as a list of 4 floats - XYZW order)."},

    {"rand",
    (PyCFunction)PyPf_rand, METH_VARARGS,
    "Return a pseudo-random number in the range of 0 to the integer argument."},

    {"pickle_object",
    (PyCFunction)PyPf_pickle_object, METH_VARARGS,
    "Returns an ASCII string holding the serialized representation of the object graph."},

    {"unpickle_object",
    (PyCFunction)PyPf_unpickle_object, METH_VARARGS,
    "Returns a new reference to an object built from its' serialized representation. The argument string must "
    "an earlier return value of 'pf.pickle_object'."},

    {"save_session",
    (PyCFunction)PyPf_save_session, METH_VARARGS,
    "Save the current state of the engine to the specified file. The session can then be loaded "
    "from the file with the 'load_session' call."},

    {"load_session",
    (PyCFunction)PyPf_load_session, METH_VARARGS,
    "Load a session previously saved with the 'save_session' call."},

    {"exec_",
    (PyCFunction)PyPf_exec, METH_VARARGS,
    "Replace the current subsession with one set up by the provided script. "
    "This is performed asynchronously. Failure is notified via an EVENT_SESSION_FAIL_LOAD event."},

    {"exec_push",
    (PyCFunction)PyPf_exec_push, METH_VARARGS,
    "Replace the current subsession with one set up by the provided script, saving the current subsession onto a stack. "
    "This is performed asynchronously. Failure is notified via an EVENT_SESSION_FAIL_LOAD event."},

    {"exec_pop",
    (PyCFunction)PyPf_exec_pop, METH_VARARGS,
    "Pop a previously saved subsession, using it to replace the current subsession. "
    "This is performed asynchronously. Failure is notified via an EVENT_SESSION_FAIL_LOAD event."},

    {"exec_pop_to_root",
    (PyCFunction)PyPf_exec_pop_to_root, METH_VARARGS,
    "Pop the root subsession, using it to replace the current subsession. "
    "This is performed asynchronously. Failure is notified via an EVENT_SESSION_FAIL_LOAD event."},

    {"session_stack_depth",
    (PyCFunction)PyPf_session_stack_depth, METH_VARARGS,
    "Returns the number of sessions currently on the sessin stack."},

    {"nearest_ent",
    (PyCFunction)PyPf_nearest_ent, METH_VARARGS | METH_KEYWORDS,
    "Returns the nearest entity to the specified 'position' - (X, Z) point or None. Takes an optional 'predicate' callable "
    "argument to filter the results and an optional 'max_range' (Float) argument."},

    {"ents_in_circle",
    (PyCFunction)PyPf_ents_in_circle, METH_VARARGS | METH_KEYWORDS,
    "Returns a list of entities in the specified circle (defined by (X, Z) 'position' and 'radius'). "
    "Takes an optional 'predicate' callable argument to filter the results."},

    {"ents_in_rect",
    (PyCFunction)PyPf_ents_in_rect, METH_VARARGS | METH_KEYWORDS,
    "Returns a list of entities in the specified rectangle (defined by two (X, Z) points - "
    "the 'minimum' and 'maximum' corners. Takes an optional 'predicate' callable argument to "
    "filter the results."},

    {"play_music",
    (PyCFunction)PyPf_play_music, METH_VARARGS,
    "Set the specified audio track to loop in the background. The argument must be a name of a WAV file in the "
    "'assets/music' folder, without the file extension."},

    {"curr_music",
    (PyCFunction)PyPf_curr_music, METH_VARARGS,
    "Return the name of the currently playing music track, or None."},

    {"get_all_music",
    (PyCFunction)PyPf_get_all_music, METH_NOARGS,
    "Get a list of all the currently loaded music track names."},

    {"play_effect",
    (PyCFunction)PyPf_play_effect, METH_VARARGS,
    "Play a specified audio effect at the specified (X, Y, Z) position. The name must be a name of a WAV file in the "
    "'assets/sounds' folder, without the file extension."},

    {"play_global_effect",
    (PyCFunction)PyPf_play_global_effect, METH_VARARGS | METH_KEYWORDS,
    "Play a specified audio effect with global range. The name must be a name of a WAV file in the "
    "'assets/sounds' folder, without the file extension. If another global effect is already playing, "
    "the effect will be ignored, unless 'interrupt' is set to True."},

    {"spawn_projectile",
    (PyCFunction)PyPf_spawn_projectile, METH_VARARGS,
    "Spawn a projectile with the specified parameters at a map location. The projectile will travel "
    "with the specified velocity until it leaves the map bounds or hits a unit. The hit event can be "
    "handled."},

    {NULL}  /* Sentinel */
};

static const char        *s_progname = NULL;
static struct py_err_ctx  s_err_ctx = {false,};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void s_err_clear(void)
{
    Py_CLEAR(s_err_ctx.type);
    Py_CLEAR(s_err_ctx.value);
    Py_CLEAR(s_err_ctx.traceback);
    s_err_ctx.occurred = false;
}

static void s_on_update(void *user, void *event)
{
    S_Error_Update(&s_err_ctx);
}

static PyObject *PyPf_load_map(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"dir", "pfmap", "update_navgrid", "absolute", NULL};
    const char *dir, *pfmap;
    int update_navgrid = true;
    int absolute = false;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "zs|ii", kwlist, &dir, &pfmap, &update_navgrid, &absolute)) {
        PyErr_SetString(PyExc_TypeError, "The first argument must be a string or NULL (base directory). "
            "The second argument must be a string (PFMAP filepath).");
        return NULL;
    }

    char pfmap_path[512] = { [0] = '\0' };
    if(!absolute) {
        pf_strlcat(pfmap_path, g_basepath, sizeof(pfmap_path));
        pf_strlcat(pfmap_path, "/", sizeof(pfmap_path));
    }
    if(dir && strlen(dir) > 0) {
        pf_strlcat(pfmap_path, dir, sizeof(pfmap_path));
        pf_strlcat(pfmap_path, "/", sizeof(pfmap_path));
    }
    pf_strlcat(pfmap_path, pfmap, sizeof(pfmap_path));

    SDL_RWops *stream = SDL_RWFromFile(pfmap_path, "r");
    if(!stream) {
        char errbuff[256];
        pf_snprintf(errbuff, sizeof(errbuff), "Unable to open PFMap file %s", pfmap_path);
        PyErr_SetString(PyExc_RuntimeError, errbuff);
        return NULL;
    }

    if(!G_LoadMap(stream, update_navgrid)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to load the specified map file.");
        SDL_RWclose(stream);
        return NULL; 
    }

    SDL_RWclose(stream);
    Py_RETURN_NONE;
}

static PyObject *PyPf_load_map_string(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"mapstr", "update_navgrid", NULL};
    const char *mapstr;
    int update_navgrid = true;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "s|i", kwlist, &mapstr, &update_navgrid)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a string.");
        return NULL;
    }

    SDL_RWops *stream = SDL_RWFromConstMem(mapstr, strlen(mapstr));
    assert(stream);

    if(!G_LoadMap(stream, update_navgrid)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to load the specified map.");
        SDL_RWclose(stream);
        return NULL;
    }

    SDL_RWclose(stream);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_ambient_light_color(PyObject *self, PyObject *args)
{
    vec3_t color;
    if(!PyArg_ParseTuple(args, "(fff)", &color.x, &color.y, &color.z)) {
        PyErr_SetString(PyExc_TypeError, "Arugment must be a tuple of 3 floats.");
        return NULL;
    }

    R_PushCmd((struct rcmd){
        .func = R_GL_SetAmbientLightColor,
        .nargs = 1,
        .args = { R_PushArg(&color, sizeof(color)) },
    });
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_emit_light_color(PyObject *self, PyObject *args)
{
    vec3_t color;
    if(!PyArg_ParseTuple(args, "(fff)", &color.x, &color.y, &color.z)) {
        PyErr_SetString(PyExc_TypeError, "Arugment must be a tuple of 3 floats.");
        return NULL;
    }

    R_PushCmd((struct rcmd){
        .func = R_GL_SetLightEmitColor,
        .nargs = 1,
        .args = { R_PushArg(&color, sizeof(color)) },
    });
    Py_RETURN_NONE;
}

static PyObject *PyPf_load_scene(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"path", "update_navgrid", "absolute", NULL};
    const char *relpath; 
    int update_navgrid = true;
    int absolute = false;
    PyObject *ents, *regs;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "s|ii", kwlist, &relpath, &update_navgrid, &absolute)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string.");
        return NULL;
    }

    char full_path[512];
    if(absolute) {
        pf_strlcpy(full_path, relpath, sizeof(full_path));
    }else{
        pf_snprintf(full_path, sizeof(full_path), "%s/%s", g_basepath, relpath);
    }

    if(!Scene_Load(full_path)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to load scene from the specified file.");
        goto fail_load;
    }

    if(update_navgrid) {
        G_BakeNavDataForScene();
    }

    PyObject *ret = PyTuple_New(2);
    if(!ret)
        goto fail_load;

    PyTuple_SET_ITEM(ret, 0, S_Entity_GetLoaded());
    PyTuple_SET_ITEM(ret, 1, S_Region_GetLoaded());
    return ret;

fail_load:
    ents = S_Entity_GetLoaded();
    regs = S_Region_GetLoaded();
    Py_DECREF(ents);
    Py_DECREF(regs);
    return NULL;
}

static PyObject *PyPf_set_emit_light_pos(PyObject *self, PyObject *args)
{
    vec3_t pos;
    if(!PyArg_ParseTuple(args, "(fff)", &pos.x, &pos.y, &pos.z)) {
        PyErr_SetString(PyExc_TypeError, "Arugment must be a tuple of 3 floats.");
        return NULL;
    }

    G_SetLightPos(pos);
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
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Could not register handler for event.");
        return NULL;
    }
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

static PyObject *PyPf_get_ticks(PyObject *self)
{
    return PyInt_FromLong(SDL_GetTicks());
}

static PyObject *PyPf_ticks_delta(PyObject *self, PyObject *args)
{
    unsigned long a, b;

    if(!PyArg_ParseTuple(args, "kk", &a, &b)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two integers (tick values).");
        return NULL;
    }

    uint32_t a32 = a, b32 = b;
    return PyInt_FromLong((uint32_t)(b - a));
}

static PyObject *PyPf_flush_tasks(PyObject *self)
{
    S_Task_Flush();
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_active_camera(PyObject *self)
{
    return S_Camera_GetActive();
}

static PyObject *PyPf_set_active_camera(PyObject *self, PyObject *args)
{
    PyObject *cam;
    if(!PyArg_ParseTuple(args, "O",  &cam)) {
        assert(PyErr_Occurred());
        return NULL;
    }

    if(!S_Camera_SetActive(cam)) {
        assert(PyErr_Occurred());
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_prev_frame_ms(PyObject *self)
{
    return Py_BuildValue("i", Perf_LastFrameMS());
}

static PyObject *PyPf_prev_frame_perfstats(PyObject *self)
{
    struct perf_info *infos[16];
    size_t nthreads = Perf_Report(ARR_SIZE(infos), (struct perf_info **)&infos);
    int status;

    PyObject *ret = PyDict_New();
    if(!ret)
        return NULL;

    for(int i = 0; i < nthreads; i++) {

        struct perf_info *curr_info = infos[i];
        //PyObject *parents[curr_info->nentries + 1];
        STALLOC(PyObject*, parents, curr_info->nentries + 1);

        PyObject *thread_dict = PyDict_New();
        if(!thread_dict)
            goto fail;
        if(0 != PyDict_SetItemString(ret, curr_info->threadname, thread_dict))
            goto fail;
        Py_DECREF(thread_dict);

        PyObject *children = PyList_New(0);
        if(!children)
            goto fail;

        if(0 != PyDict_SetItemString(thread_dict, "children", children))
            goto fail;
        Py_DECREF(children);

        parents[0] = thread_dict;
        for(int j = 0; j < curr_info->nentries; j++) {

            PyObject *newdict = PyDict_New();
            if(!newdict)
                goto fail;

            int parent_idx = (curr_info->entries[j].parent_idx == ~((uint32_t)0)) ? 0 : curr_info->entries[j].parent_idx + 1;
            PyObject *parent = parents[parent_idx];
            assert(parent && PyDict_Check(parent));

            if(0 != PyList_Append(PyDict_GetItemString(parent, "children"), newdict))
                goto fail;
            parents[j + 1] = newdict;
            Py_DECREF(newdict);

            PyObject *name = PyString_FromString(curr_info->entries[j].funcname);
            if(!name)
                goto fail;
            status = PyDict_SetItemString(newdict, "name", name);
            Py_DECREF(name);
            if(0 != status)
                goto fail;

            PyObject *ms_delta = PyFloat_FromDouble(curr_info->entries[j].ms_delta);
            if(!ms_delta)
                goto fail;
            status = PyDict_SetItemString(newdict, "ms_delta", ms_delta);
            Py_DECREF(ms_delta);
            if(0 != status)
                goto fail;

            PyObject *pc_delta = PyLong_FromUnsignedLongLong(curr_info->entries[j].pc_delta);
            if(!pc_delta)
                goto fail;
            status = PyDict_SetItemString(newdict, "pc_delta", pc_delta);
            Py_DECREF(pc_delta);
            if(0 != status)
                goto fail;

            PyObject *children = PyList_New(0);
            if(!children)
                goto fail;
            status = PyDict_SetItemString(newdict, "children", children);
            Py_DECREF(children);
            if(0 != status)
                goto fail;
        }
    }

    for(int i = 0; i < nthreads; i++) {
        PF_FREE(infos[i]);
    }
    return ret;

fail:
    Py_XDECREF(ret);
    for(int i = 0; i < nthreads; i++) {
        PF_FREE(infos[i]);
    }
    return NULL;
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
    rval |= PyDict_SetItemString(ret, "version",  Py_BuildValue("s", R_GetInfo(RENDER_INFO_VERSION)));
    rval |= PyDict_SetItemString(ret, "vendor",   Py_BuildValue("s", R_GetInfo(RENDER_INFO_VENDOR)));
    rval |= PyDict_SetItemString(ret, "renderer", Py_BuildValue("s", R_GetInfo(RENDER_INFO_RENDERER)));
    rval |= PyDict_SetItemString(ret, "shading_language_version", Py_BuildValue("s", R_GetInfo(RENDER_INFO_SL_VERSION)));
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
    rval |= PyDict_SetItemString(ret, "los_invalidated",    Py_BuildValue("i", stats.los_invalidated));
    rval |= PyDict_SetItemString(ret, "flow_used",          Py_BuildValue("i", stats.flow_used));
    rval |= PyDict_SetItemString(ret, "flow_max",           Py_BuildValue("i", stats.flow_max));
    rval |= PyDict_SetItemString(ret, "flow_hit_rate",      Py_BuildValue("f", stats.flow_hit_rate));
    rval |= PyDict_SetItemString(ret, "flow_invalidated",   Py_BuildValue("i", stats.flow_invalidated));
    rval |= PyDict_SetItemString(ret, "ffid_used",          Py_BuildValue("i", stats.ffid_used));
    rval |= PyDict_SetItemString(ret, "ffid_max",           Py_BuildValue("i", stats.ffid_max));
    rval |= PyDict_SetItemString(ret, "ffid_hit_rate",      Py_BuildValue("f", stats.ffid_hit_rate));
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

static PyObject *PyPf_ui_text_edit_has_focus(PyObject *self)
{
    if(S_UI_TextEditHasFocus())
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

static PyObject *PyPf_get_active_window(PyObject *self)
{
    PyObject *ret = S_UI_ActiveWindow();
    if(ret) {
        return ret;
    }else{
        Py_RETURN_NONE;
    }
}

static PyObject *PyPf_get_file_size(PyObject *self, PyObject *args)
{
    PyObject *file;

    if(!PyArg_ParseTuple(args, "O", &file)
	|| !PyFile_Check(file)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a file object.");
        return NULL;
    }

	SDL_RWops *rw = SDL_RWFromFP(((PyFileObject*)file)->f_fp, false);
	PyObject *ret = PyLong_FromLongLong(SDL_RWsize(rw));
	SDL_FreeRW(rw);

	return ret;
}

static PyObject *PyPf_shift_pressed(PyObject *self)
{
    const Uint8 *state = SDL_GetKeyboardState(NULL);
    if(state[SDL_SCANCODE_LSHIFT] || state[SDL_SCANCODE_RSHIFT]) {
        Py_RETURN_TRUE;
    }else{
        Py_RETURN_FALSE;
    }
}

static PyObject *PyPf_ctrl_pressed(PyObject *self)
{
    const Uint8 *state = SDL_GetKeyboardState(NULL);
    if(state[SDL_SCANCODE_LCTRL] || state[SDL_SCANCODE_RCTRL]) {
        Py_RETURN_TRUE;
    }else{
        Py_RETURN_FALSE;
    }
}

static PyObject *PyPf_get_key_name(PyObject *self, PyObject *args)
{
    int keysym;
    if(!PyArg_ParseTuple(args, "i", &keysym)) {
        PyErr_SetString(PyExc_TypeError, "Argument must an integer value (SDL_Keysym).");
        return NULL;
    }
    const char *ret = SDL_GetKeyName(keysym);
    return PyString_FromString(ret);
}

static PyObject *PyPf_get_active_font(PyObject *self)
{
    return PyString_FromString(UI_GetActiveFont());
}

static PyObject *PyPf_set_active_font(PyObject *self, PyObject *args)
{
    const char *name;

    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a string.");
        return NULL;
    }

    if(!UI_SetActiveFont(name)) {
        Py_RETURN_FALSE;
    }else{
        Py_RETURN_TRUE;
    }
}

static PyObject *PyPf_show_regions(PyObject *self)
{
    G_Region_SetRender(true);
    Py_RETURN_NONE;
}

static PyObject *PyPf_hide_regions(PyObject *self)
{
    G_Region_SetRender(false);
    Py_RETURN_NONE;
}

static PyObject *PyPf_enable_fog_of_war(PyObject *self)
{
    G_Fog_Enable();
    Py_RETURN_NONE;
}

static PyObject *PyPf_disable_fog_of_war(PyObject *self)
{
    G_Fog_Disable();
    Py_RETURN_NONE;
}

static PyObject *PyPf_explore_map(PyObject *self, PyObject *args)
{
    int faction_id;
    if(!PyArg_ParseTuple(args, "i", &faction_id)
    || (faction_id < 0 || faction_id >= MAX_FACTIONS)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a valid faction ID (integer).");
        return NULL;
    }

    G_Fog_ExploreMap(faction_id);
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
    const vec_entity_t *sel = G_Sel_Get(&sel_type);

    PyObject *ret = PyList_New(0);
    if(!ret)
        return NULL;

    for(int i = 0; i < vec_size(sel); i++) {
        PyObject *ent = S_Entity_ObjForUID(vec_AT(sel, i));
        if(ent) {
            PyList_Append(ret, ent);
        }
    }

    return ret;
}

static PyObject *PyPf_set_unit_selection(PyObject *self, PyObject *args)
{
    PyObject *list;
    if(!PyArg_ParseTuple(args, "O!", &PyList_Type, &list)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a list object.");
        return NULL;
    }

    //uint32_t ents[PyList_GET_SIZE(list)];
    STALLOC(uint32_t, ents, PyList_GET_SIZE(list));
    size_t nents = 0;

    for(int i = 0; i < PyList_GET_SIZE(list); i++) {
        PyObject *obj = PyList_GET_ITEM(list, i);
        if(!S_Entity_Check(obj)) {
            PyErr_SetString(PyExc_TypeError, "Argument must a list of pf.Entity objects.");
            return NULL;
        }
        uint32_t uid;
        S_Entity_UIDForObj(obj, &uid);
        ents[nents++] = uid;
    }

    G_Sel_Set(ents, nents);
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_hovered_unit(PyObject *self)
{
    uint32_t hovered  = G_Sel_GetHovered();
    if(G_EntityExists(hovered)) {
        PyObject *obj = S_Entity_ObjForUID(hovered);
        if(obj) {
            Py_INCREF(obj);
            return obj;
        }
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_entities_for_tag(PyObject *self, PyObject *args)
{
    assert(Sched_UsingBigStack());

    const char *tag;
    if(!PyArg_ParseTuple(args, "s", &tag)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a string (tag).");
        return NULL;
    }

    uint32_t uids[16384];
    size_t nents = Entity_EntsForTag(tag, ARR_SIZE(uids), uids);

    PyObject *ret = PyTuple_New(nents);
    if(!ret)
        return NULL;

    for(int i = 0; i < nents; i++) {
        PyObject *ent = S_Entity_ObjForUID(uids[i]);
        assert(ent);
        Py_INCREF(ent);
        PyTuple_SET_ITEM(ret, i, ent);
    }
    return ret;
}

static PyObject *PyPf_hide_healthbars(PyObject *self)
{
    G_SetHideHealthbars(true);
    Py_RETURN_NONE;
}

static PyObject *PyPf_show_healthbars(PyObject *self)
{
    G_SetHideHealthbars(false);
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_resource_list(PyObject *self)
{
    const char *names[64];
    int nres = G_Resource_GetAllNames(ARR_SIZE(names), names);

    PyObject *ret = PyList_New(nres);
    if(!ret)
        return NULL;

    for(int i = 0; i < nres; i++) {
        PyObject *str = PyString_FromString(names[i]);
        if(!str) {
            Py_DECREF(ret);
            return NULL;
        }
        PyList_SetItem(ret, i, str);
    }
    return ret;
}

static PyObject *PyPf_get_resource_stored(PyObject *self, PyObject *args)
{
    const char *name;
    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one arguments: resource name (string).");
        return NULL;
    }

    int stored = G_StorageSite_GetPlayerStored(name);
    return PyInt_FromLong(stored);
}

static PyObject *PyPf_get_resource_capacity(PyObject *self, PyObject *args)
{
    const char *name;
    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one arguments: resource name (string).");
        return NULL;
    }

    int cap = G_StorageSite_GetPlayerCapacity(name);
    return PyInt_FromLong(cap);
}

static PyObject *PyPf_get_factions_list(PyObject *self)
{
    char names[MAX_FACTIONS][MAX_FAC_NAME_LEN];
    vec3_t colors[MAX_FACTIONS];
    bool controllable[MAX_FACTIONS];

    uint16_t facs = G_GetFactions(names, colors, controllable);
    size_t num_facs = 0;
    for(uint16_t fcopy = facs; fcopy; fcopy >>= 1) {
        if(fcopy & 0x1)
            num_facs++;
    }

    PyObject *ret = PyList_New(num_facs);
    if(!ret)
        goto fail_list;

    size_t num_set = 0;
    for(int i = 0; facs; facs >>= 1, i++) {

        if(!(facs & 0x1))
            continue;

        PyObject *fac_dict = PyDict_New();
        if(!fac_dict)
            goto fail_dict;
        PyList_SetItem(ret, num_set++, fac_dict); /* ret now owns fac_dict */

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

        PyObject *id = PyInt_FromLong(i);
        if(!id)
            goto fail_dict;
        PyDict_SetItemString(fac_dict, "id", id);
        Py_DECREF(id);
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
    G_GetFactions(NULL, NULL, controllable);

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

    G_GetFactions(names, colors, NULL);

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

static PyObject *PyPf_get_diplomacy_state(PyObject *self, PyObject *args)
{
    int faca, facb;

    if(!PyArg_ParseTuple(args, "ii", &faca, &facb)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a two integers.");
        return NULL;
    }

    if(faca == facb) {
        return PyInt_FromLong(DIPLOMACY_STATE_PEACE);
    }
    enum diplomacy_state state;
    bool status = G_GetDiplomacyState(faca, facb, &state);
    if(status) {
        return PyInt_FromLong(state);
    }else{
        PyErr_SetString(PyExc_TypeError, "Invalid faction ID(s).");
        return NULL;
    }
}

static PyObject *PyPf_get_tile(PyObject *self, PyObject *args)
{
    struct tile_desc desc;
    if(!PyArg_ParseTuple(args, "(ii)(ii)O", &desc.chunk_r, &desc.chunk_c, &desc.tile_r, &desc.tile_c)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two tuples of two integers.");
        return NULL;
    }

    return S_Tile_New(&desc);
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
    if(!G_GetMinimapPos(&x, &y)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to get minimap position. A map must be loaded.");
        return NULL;
    }
    return Py_BuildValue("(f,f)", x, y);
}

static PyObject *PyPf_set_minimap_position(PyObject *self, PyObject *args)
{
    float x, y;

    if(!PyArg_ParseTuple(args, "ff", &x, &y)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two floats.");
        return NULL;
    }

    if(!G_SetMinimapPos(x, y)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to set minimap position. A map must be loaded.");
        return NULL;
    }
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

    if(!G_SetMinimapResizeMask(resize_mask)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to set minimap resize mask. A map must be loaded.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_minimap_size(PyObject *self)
{
    int size;
    if(!G_GetMinimapSize(&size)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to get minimap size. A map must be loaded.");
        return NULL;
    }
    return Py_BuildValue("i", size);
}

static PyObject *PyPf_set_minimap_size(PyObject *self, PyObject *args)
{
    int size;

    if(!PyArg_ParseTuple(args, "i", &size)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an integer.");
        return NULL;
    }

    if(!G_SetMinimapSize(size)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to set minimap size. A map must be loaded.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_minimap_border_clr(PyObject *self, PyObject *args)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(args, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of 4 integers (RGBA).");
        return NULL;
    }

    vec4_t rgba = (vec4_t){
        r / 255.0f,
        g / 255.0f,
        b / 255.0f,
        a / 255.0f,
    };
    M_MinimapSetBorderClr(rgba);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_minimap_render_all_ents(PyObject *self, PyObject *args)
{
    PyObject *val;
    if(!PyArg_ParseTuple(args, "O", &val)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single boolean expression.");
        return NULL;
    }
    bool on = PyObject_IsTrue(val);
    G_SetMinimapRenderAllEntities(on);
    Py_RETURN_NONE;
}

static PyObject *PyPf_mouse_over_minimap(PyObject *self)
{
    bool result = G_MouseOverMinimap();
    if(result) {
        Py_RETURN_TRUE;
    }else {
        Py_RETURN_FALSE;
    }
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
    if(!result) {
        Py_RETURN_NONE;
    }else {
        return Py_BuildValue("f", height);
    }
}

static PyObject *PyPf_map_nearest_pathable(PyObject *self, PyObject *args)
{
    float x, z;

    if(!PyArg_ParseTuple(args, "(ff)", &x, &z)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a tuple of two floats.");
        return NULL;
    }

    vec2_t ret;
    bool exists = G_MapClosestPathable((vec2_t){x, z}, &ret);
    if(!exists) {
        Py_RETURN_NONE;
    }else{
        return Py_BuildValue("ff", ret.x, ret.z);
    }
}

static PyObject *PyPf_map_pos_under_cursor(PyObject *self)
{
    vec3_t pos;
    if(M_Raycast_MouseIntersecCoord(&pos)) {
        return Py_BuildValue("(fff)", pos.x, pos.y, pos.z);
    }else {
        Py_RETURN_NONE;
    }
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

static PyObject *PyPf_set_build_on_left_click(PyObject *self)
{
    G_Builder_SetBuildOnLeftClick();
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_gather_on_left_click(PyObject *self)
{
    G_Harvester_SetGatherOnLeftClick();
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_pick_up_on_left_click(PyObject *self)
{
    G_Harvester_SetPickUpOnLeftClick();
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_drop_off_on_left_click(PyObject *self)
{
    G_Harvester_SetDropOffOnLeftClick();
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_transport_on_left_click(PyObject *self)
{
    G_Harvester_SetTransportOnLeftClick();
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_click_move_enabled(PyObject *self, PyObject *args)
{
    PyObject *arg;
    if(!PyArg_ParseTuple(args, "O", &arg)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a single boolean expression.");
        return NULL;
    }

    G_Move_SetClickEnabled(PyObject_IsTrue(arg));
    Py_RETURN_NONE;
}

static PyObject *PyPf_draw_text(PyObject *self, PyObject *args)
{
    const char *text;
    struct rect rect;
    int r, g, b, a;
    int tint_r = 0, tint_g = 0, tint_b = 0, tint_a = 255;

    if(!PyArg_ParseTuple(args, "s(iiii)(iiii)|(iiii)", &text, &rect.x, &rect.y, &rect.w, &rect.h, &r, &g, &b, &a,
        &tint_r, &tint_g, &tint_b, &tint_a)) {
        PyErr_SetString(PyExc_TypeError, 
            "Expecting 3 arguments: A text string, a tuple of 4 ints (bounds) and a tuple of 4 ints (RGBA color). "
            "Optionally an extra RGBA tuple can be supplied to customize the tint RGBA color.");
        return NULL;
    }

    int width, height;
    Engine_WinDrawableSize(&width, &height);

    vec2_t vres = UI_GetTextVres();
    vec2_t adj_vres = UI_ArAdjustedVRes(vres);

    vec2_t center = (vec2_t){
        rect.x + rect.w / 2.0f,
        rect.y + rect.h / 2.0f
    };
    vec2_t ndc = (vec2_t) {
        (center.x - width /2.0f) / (width /2.0f),
        (height/2.0f - center.y) / (height/2.0f)
    };
    vec2_t adj_center = (vec2_t){
        (ndc.x + 1.0f) * adj_vres.x/2.0f,
        adj_vres.y - ((ndc.y + 1.0f) * adj_vres.y/2.0f),
    };

    struct rect adj_rect = (struct rect){
        adj_center.x - rect.w/2.0f,
        adj_center.y - rect.h/2.0f,
        rect.w,
        rect.h
    };
    struct rect shifted_rect = (struct rect){
        adj_rect.x - 1,
        adj_rect.y - 1,
        adj_rect.w,
        adj_rect.h
    };

    UI_DrawText(text, shifted_rect, (struct rgba){tint_r, tint_g, tint_b, tint_a});
    UI_DrawText(text, adj_rect, (struct rgba){r, g, b, a});
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_storage_site_ui_style(PyObject *self, PyObject *args)
{
    struct nk_style_item style;
    PyObject *val;

    if(!PyArg_ParseTuple(args, "O", &val)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an (R, G, B, A) tuple or an image path.");
        return NULL;
    }

    if(PyTuple_Check(val) 
    && PyTuple_GET_SIZE(val) == 4
    && PyInt_Check(PyTuple_GET_ITEM(val, 0))
    && PyInt_Check(PyTuple_GET_ITEM(val, 1))
    && PyInt_Check(PyTuple_GET_ITEM(val, 2))
    && PyInt_Check(PyTuple_GET_ITEM(val, 3))) {

        style.type = NK_STYLE_ITEM_COLOR;
        style.data.color.r = PyInt_AS_LONG(PyTuple_GET_ITEM(val, 0));
        style.data.color.g = PyInt_AS_LONG(PyTuple_GET_ITEM(val, 1));
        style.data.color.b = PyInt_AS_LONG(PyTuple_GET_ITEM(val, 2));
        style.data.color.a = PyInt_AS_LONG(PyTuple_GET_ITEM(val, 3));

    }else if(PyString_Check(val)) {

        style.type = NK_STYLE_ITEM_TEXPATH;
        pf_strlcpy(style.data.texpath, PyString_AS_STRING(val),
            ARR_SIZE(style.data.texpath));

    }else{
        PyErr_SetString(PyExc_TypeError, "Argument must be an (R, G, B, A) tuple or an image path.");
        return NULL; 
    }

    G_StorageSite_SetBackgroundStyle(&style);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_storage_site_ui_border_color(PyObject *self, PyObject *args)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(args, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple or an image path.");
        return NULL;
    }

    struct nk_color clr = (struct nk_color){r, g, b, a};
    G_StorageSite_SetBorderColor(&clr);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_storage_site_ui_font_color(PyObject *self, PyObject *args)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(args, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple or an image path.");
        return NULL;
    }

    struct nk_color clr = (struct nk_color){r, g, b, a};
    G_StorageSite_SetFontColor(&clr);
    Py_RETURN_NONE;
}

static PyObject *PyPf_storage_site_show_ui(PyObject *self, PyObject *args)
{
    PyObject *obj;
    if(!PyArg_ParseTuple(args, "O", &obj)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a single argument (True or False expression)");
        return NULL;
    }

    bool show = PyObject_IsTrue(obj);
    G_StorageSite_SetShowUI(show);
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

static PyObject *PyPf_settings_set(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"name", "value", "persist", NULL};
    const char *sname;
    PyObject *nvobj;
    int persist = true;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "sO|i", kwlist, &sname, &nvobj, &persist)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string and an object.");
        return NULL;
    }

    struct sval newval;
    if(!sval_from_pyobj(nvobj, &newval)) {
        PyErr_SetString(PyExc_TypeError, "The new value is not one of the allowed types for settings.");
        return NULL;
    }

    ss_e status; 
    if(!persist) {
        status = Settings_SetNoPersist(sname, &newval);
    }else{
        status = Settings_Set(sname, &newval);
    }

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
        pf_snprintf(errstr, sizeof(errstr), "Could not delete setting. [err: %d]", status);
        PyErr_SetString(PyExc_RuntimeError, errstr);
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *PyPf_settings_flush(PyObject *self)
{
    ss_e status = Settings_SaveToFile();
    if(status != SS_OKAY) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to save the current settings to the settings file.");
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

static PyObject *PyPf_set_system_cursor(PyObject *self, PyObject *args)
{
    int type;
    const char *path;
    int hotx, hoty;

    if(!PyArg_ParseTuple(args, "isii", &type, &path, &hotx, &hoty)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an int (CURSOR type), a string (path), and two ints (hotx, hoty).");
        return NULL;
    }

    if(!Cursor_LoadBMP(type, path, hotx, hoty)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to load cursor image for the specified type.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_named_cursor(PyObject *self, PyObject *args)
{
    const char *name, *path;
    int hotx, hoty;

    if(!PyArg_ParseTuple(args, "ssii", &name, &path, &hotx, &hoty)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string (name), a string (path), and two ints (hotx, hoty).");
        return NULL;
    }

    if(!Cursor_NamedLoadBMP(name, path, hotx, hoty)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to load cursor image for the specified name.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_activate_system_cursor(PyObject *self, PyObject *args)
{
    int type;
    if(!PyArg_ParseTuple(args, "i", &type)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an int (CURSOR type).");
        return NULL;
    }

    if(type < 0 || type >= _CURSOR_MAX) {
        PyErr_SetString(PyExc_TypeError, "Invalid CURSOR type. It must be a CURSOR enum value.");
        return NULL;
    }

    Cursor_SetActive(type);
    Py_RETURN_NONE;
}

static PyObject *PyPf_activate_named_cursor(PyObject *self, PyObject *args)
{
    const char *name;
    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string.");
        return NULL;
    }

    if(!Cursor_NamedSetActive(name)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to activate cursor with the specified name.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_cursor_rts_mode(PyObject *self, PyObject *args)
{
    PyObject *value;
    if(!PyArg_ParseTuple(args, "O", &value)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a single argument: boolean expression");
        return NULL;
    }

    Cursor_SetRTSMode(PyObject_IsTrue(value));
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_cursor_rts_mode(PyObject *self)
{
    if(Cursor_GetRTSMode()) {
        Py_RETURN_TRUE;
    }else{
        Py_RETURN_FALSE;
    }
}

static PyObject *PyPf_multiply_quaternions(PyObject *self, PyObject *args)
{
    PyObject *q1_list, *q2_list;
    quat_t q1, q2, ret;

    if(!PyArg_ParseTuple(args, "O!O!", &PyTuple_Type, &q1_list, &PyTuple_Type, &q2_list)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two tuples.");
        return NULL;
    }

    if(PyTuple_GET_SIZE(q1_list) != 4
    || PyTuple_GET_SIZE(q2_list) != 4) {
        PyErr_SetString(PyExc_TypeError, "The argument tuples must be of size 4.");
        return NULL;
    }

    if(!PyArg_ParseTuple(q1_list, "ffff", &q1.raw[0], &q1.raw[1], &q1.raw[2], &q1.raw[3]))
        return NULL;
    if(!PyArg_ParseTuple(q2_list, "ffff", &q2.raw[0], &q2.raw[1], &q2.raw[2], &q2.raw[3]))
        return NULL;

    PFM_Quat_MultQuat(&q1, &q2, &ret);
    PFM_Quat_Normal(&ret, &ret);

    return Py_BuildValue("(ffff)", ret.x, ret.y, ret.z, ret.w);
}

static PyObject *PyPf_rand(PyObject *self, PyObject *args)
{
    int max;
    if(!PyArg_ParseTuple(args, "i", &max)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single integer.");
        return NULL;
    }
    int raw = rand();
    int ret = ((float)raw) / RAND_MAX * max;
    assert(ret >= 0 && ret <= max);
    return PyInt_FromLong(ret);
}

static PyObject *PyPf_pickle_object(PyObject *self, PyObject *args)
{
    PyObject *obj;
    if(!PyArg_ParseTuple(args, "O", &obj)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single object.");
        return NULL;
    }

    SDL_RWops *vops = PFSDL_VectorRWOps();
    bool success = S_PickleObjgraph(obj, vops);
    if(!success) {
        assert(PyErr_Occurred());
        vops->close(vops);
        return NULL;
    }

    PyObject *ret = PyString_FromString("");
    assert(ret);
    if(_PyString_Resize(&ret, vops->size(vops)-1) < 0) {
        vops->close(vops);
        return NULL;
    }

    vops->seek(vops, 0, RW_SEEK_SET);
    vops->read(vops, PyString_AS_STRING(ret), 1, vops->size(vops));
    vops->close(vops);
    return ret;
}

static PyObject *PyPf_unpickle_object(PyObject *self, PyObject *args)
{
    PyObject *obj_str = PyTuple_GET_ITEM(args, 0);

    const char *str;
    Py_ssize_t len;

    if(!PyArg_ParseTuple(args, "s#", &str, &len)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string.");
        return NULL;
    }

    SDL_RWops *cmops = SDL_RWFromConstMem(str, len);
    PyObject *ret = S_UnpickleObjgraph(cmops);
    if(!ret) {
        assert(PyErr_Occurred());
        cmops->close(cmops);
        return NULL;
    }

    cmops->close(cmops);
    return ret;
}

static PyObject *PyPf_save_session(PyObject *self, PyObject *args)
{
    const char *str;
    if(!PyArg_ParseTuple(args, "s", &str)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string (path of the file to save the session to).");
        return NULL;
    }

    Session_RequestSave(str);
    Py_RETURN_NONE;
}

static PyObject *PyPf_load_session(PyObject *self, PyObject *args)
{
    const char *str;
    if(!PyArg_ParseTuple(args, "s", &str)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string (path of the file to load the session from).");
        return NULL;
    }

    Session_RequestLoad(str);
    Py_RETURN_NONE;
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

static int s_tracefunc(PyObject *obj, struct _frame *frame, int what, PyObject *arg)
{
    char name[128];

    switch(what) {
    case PyTrace_CALL: {
        pf_snprintf(name, sizeof(name), "[Py] %s", PyString_AS_STRING(frame->f_code->co_name));
        Perf_Push(name);
        break;
    }
    case PyTrace_EXCEPTION: /* fallthrough */
    case PyTrace_RETURN: {
        Perf_Pop(NULL);
        break;
    }
    case PyTrace_C_CALL: {
        assert(PyCFunction_Check(arg));
        PyCFunctionObject *func = (PyCFunctionObject*)arg;

        pf_snprintf(name, sizeof(name), "[PyC] %s", func->m_ml->ml_name);
        Perf_Push(name);
        break;
    }
    case PyTrace_C_EXCEPTION: /* fallthrough */
    case PyTrace_C_RETURN: {
        Perf_Pop(NULL);
    }
    case PyTrace_LINE:
        break; /* no-op */
    default: assert(0);
    }
    return 0;
}

static bool bool_val_validate(const struct sval *new_val)
{
    return (new_val->type == ST_TYPE_BOOL);
}

static void on_event_start(void *user, void *event)
{
    bool new_val = (uintptr_t)user;
    if(new_val) {
        PyEval_SetProfile(s_tracefunc, Py_None);
    }else{
        PyEval_SetProfile(NULL, NULL);
    }
    E_Global_Unregister(EVENT_UPDATE_START, on_event_start);
}

static void trace_enable_commit(const struct sval *new_val)
{
    /* Only change the profile func at frame boundaries so that we're not left 
     * with unmatched Perf_{Push,Pop} calls. */
    E_Global_Register(EVENT_UPDATE_START, on_event_start, 
        (void*)((uintptr_t)new_val->as_bool), G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);
}

static void s_create_settings(void)
{
    ss_e status;
    (void)status;

    status = Settings_Create((struct setting){
        .name = "pf.debug.trace_python",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = trace_enable_commit,
    });
    assert(status == SS_OKAY);
}

static PyObject *s_wrap_argv(struct arg_desc *args)
{
    PyObject *ret = PyTuple_New(args->argc);
    if(!ret)
        goto fail;

    for(int i = 0; i < args->argc; i++) {
        PyObject *str = PyString_FromString(args->argv[i]);
        if(!str)
            goto fail;
        PyTuple_SET_ITEM(ret, i, str);
    }
    return ret;

fail:
    Py_XDECREF(ret);
    return NULL;
}

static PyObject *PyPf_exec(PyObject *self, PyObject *args)
{
    const char *scriptname;
    PyObject *sargs;

    if(!PyArg_ParseTuple(args, "sO", &scriptname, &sargs)
    || !PyTuple_Check(sargs)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string and a tuple or arguments.");
        return NULL;
    }

    int argc = PyTuple_GET_SIZE(sargs);
    char *argv[32];

    if(argc > ARR_SIZE(argv)) {
        PyErr_SetString(PyExc_RuntimeError, "Maximum number of arguments exceeded.");
        return NULL;
    }

    for(int i = 0; i < argc; i++) {
        PyObject *arg = PyTuple_GET_ITEM(sargs, i);
        if(!PyString_Check(arg)) {
            PyErr_SetString(PyExc_TypeError, "Script arguments must be strings");
            return NULL;
        }
        argv[i] = PyString_AS_STRING(arg);
    }

    Session_RequestExec(scriptname, argc, argv);
    Py_RETURN_NONE;
}

static PyObject *PyPf_exec_push(PyObject *self, PyObject *args)
{
    const char *scriptname;
    PyObject *sargs = NULL;

    if(!PyArg_ParseTuple(args, "s|O", &scriptname, &sargs)
    || (sargs && !PyTuple_Check(sargs))) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string (and optionally a tuple of arguments).");
        return NULL;
    }

    int argc = sargs ? PyTuple_GET_SIZE(sargs) : 0;
    char *argv[32];

    if(argc > ARR_SIZE(argv)) {
        PyErr_SetString(PyExc_RuntimeError, "Maximum number of arguments exceeded.");
        return NULL;
    }

    for(int i = 0; i < argc; i++) {
        PyObject *arg = PyTuple_GET_ITEM(sargs, i);
        if(!PyString_Check(arg)) {
            PyErr_SetString(PyExc_TypeError, "Script arguments must be strings");
            return NULL;
        }
        argv[i] = PyString_AS_STRING(arg);
    }

    Session_RequestPush(scriptname, argc, argv);
    Py_RETURN_NONE;
}

static PyObject *PyPf_exec_pop(PyObject *self, PyObject *args)
{
    PyObject *sargs = NULL;

    if(!PyArg_ParseTuple(args, "|O", &sargs)
    || (sargs && !PyTuple_Check(sargs))) {
        PyErr_SetString(PyExc_TypeError, "One optional arument: tuple of strings.");
        return NULL;
    }

    int argc = sargs ? PyTuple_GET_SIZE(sargs) : 0;
    char *argv[32];

    if(argc > ARR_SIZE(argv)) {
        PyErr_SetString(PyExc_RuntimeError, "Maximum number of arguments exceeded.");
        return NULL;
    }

    for(int i = 0; i < argc; i++) {
        PyObject *arg = PyTuple_GET_ITEM(sargs, i);
        if(!PyString_Check(arg)) {
            PyErr_SetString(PyExc_TypeError, "Script arguments must be strings");
            return NULL;
        }
        argv[i] = PyString_AS_STRING(arg);
    }

    Session_RequestPop(argc, argv);
    Py_RETURN_NONE;
}

static PyObject *PyPf_exec_pop_to_root(PyObject *self, PyObject *args)
{
    PyObject *sargs = NULL;

    if(!PyArg_ParseTuple(args, "|O", &sargs)
    || (sargs && !PyTuple_Check(sargs))) {
        PyErr_SetString(PyExc_TypeError, "One optional arument: tuple of strings.");
        return NULL;
    }

    int argc = sargs ? PyTuple_GET_SIZE(sargs) : 0;
    char *argv[32];

    if(argc > ARR_SIZE(argv)) {
        PyErr_SetString(PyExc_RuntimeError, "Maximum number of arguments exceeded.");
        return NULL;
    }

    for(int i = 0; i < argc; i++) {
        PyObject *arg = PyTuple_GET_ITEM(sargs, i);
        if(!PyString_Check(arg)) {
            PyErr_SetString(PyExc_TypeError, "Script arguments must be strings");
            return NULL;
        }
        argv[i] = PyString_AS_STRING(arg);
    }

    Session_RequestPopToRoot(argc, argv);
    Py_RETURN_NONE;
}

static PyObject *PyPf_session_stack_depth(PyObject *self, PyObject *args)
{
    int depth = Session_StackDepth();
    return PyInt_FromLong(depth);
}

static bool s_pred_callable(uint32_t ent, void *arg)
{
    PyObject *func = arg;
    PyObject *obj = S_Entity_ObjForUID(ent);
    if(!obj)
        return false;

    PyObject *result = PyObject_CallFunctionObjArgs(func, obj, NULL);
    bool ret = PyObject_IsTrue(result);
    Py_DECREF(result);
    return ret;
}

static bool s_pred_any(uint32_t ent, void *arg)
{
    return true;
}

static PyObject *PyPf_nearest_ent(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"position", "predicate", "max_range", NULL};
    float max_range = 0.0f;
    vec2_t xz_pos;
    PyObject *predicate = NULL;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "(ff)|Of", kwlist, &xz_pos.x, &xz_pos.z, &predicate, &max_range)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an (X, Z) float tuple and "
            "an optional callable object.");
        return NULL;
    }

    if(predicate && !PyCallable_Check(predicate)) {
        PyErr_SetString(PyExc_TypeError, "'predicate' argument must be callable.");
        return NULL;
    }

    uint32_t nearest = NULL_UID;
    if(predicate) {
        nearest = G_Pos_NearestWithPred(xz_pos, s_pred_callable, predicate, max_range);
    }else{
        nearest = G_Pos_NearestWithPred(xz_pos, s_pred_any, NULL, max_range);
    }

    if(G_EntityExists(nearest)) {
        PyObject *ret = S_Entity_ObjForUID(nearest);
        if(!ret) {
            Py_RETURN_NONE;
        }
        Py_INCREF(ret);
        return ret;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_ents_in_circle(PyObject *self, PyObject *args, PyObject *kwargs)
{
    assert(Sched_UsingBigStack());

    static char *kwlist[] = {"position", "radius", "predicate", NULL};
    float radius;
    vec2_t xz_pos;
    PyObject *predicate = NULL;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "(ff)f|O", kwlist, &xz_pos.x, &xz_pos.z, &radius, &predicate)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an (X, Z) float tuple, a float and "
            "an optional callable object.");
        return NULL;
    }

    if(predicate && !PyCallable_Check(predicate)) {
        PyErr_SetString(PyExc_TypeError, "'predicate' argument must be callable.");
        return NULL;
    }

    uint32_t inside[16384];
    size_t ninside;

    if(predicate) {
        ninside = G_Pos_EntsInCircleWithPred(xz_pos, radius, inside, ARR_SIZE(inside),
            s_pred_callable, predicate);
    }else{
        ninside = G_Pos_EntsInCircleWithPred(xz_pos, radius, inside, ARR_SIZE(inside),
            s_pred_any, NULL);
    }

    PyObject *list = PyList_New(ninside);
    if(!list)
        return NULL;

    size_t ninserted = 0;
    for(int i = 0; i < ninside; i++) {
        PyObject *obj = S_Entity_ObjForUID(inside[i]);
        if(!obj)
            continue;
        Py_INCREF(obj);
        PyList_SET_ITEM(list, ninserted++, obj);
    }

    if(ninserted == ninside || ninserted == 0)
        return list;

    PyObject *ret = PyList_GetSlice(list, 0, ninserted-1);
    Py_DECREF(list);
    return ret;
}

static PyObject *PyPf_ents_in_rect(PyObject *self, PyObject *args, PyObject *kwargs)
{
    assert(Sched_UsingBigStack());

    static char *kwlist[] = {"minimum", "maximum", "predicate", NULL};
    vec2_t xz_min, xz_max;
    PyObject *predicate = NULL;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "(ff)(ff)|O", kwlist, &xz_min.x, &xz_min.z, 
        &xz_max.x, &xz_max.z, &predicate)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two (X, Z) float tuples, and an "
            "optional callable object.");
        return NULL;
    }

    if(predicate && !PyCallable_Check(predicate)) {
        PyErr_SetString(PyExc_TypeError, "'predicate' argument must be callable.");
        return NULL;
    }

    uint32_t inside[16384];
    size_t ninside;

    if(predicate) {
        ninside = G_Pos_EntsInRectWithPred(xz_min, xz_max, inside, ARR_SIZE(inside),
            s_pred_callable, predicate);
    }else{
        ninside = G_Pos_EntsInRectWithPred(xz_min, xz_max, inside, ARR_SIZE(inside),
            s_pred_any, NULL);
    }

    PyObject *list = PyList_New(ninside);
    if(!list)
        return NULL;

    size_t ninserted = 0;
    for(int i = 0; i < ninside; i++) {
        PyObject *obj = S_Entity_ObjForUID(inside[i]);
        if(!obj)
            continue;
        Py_INCREF(obj);
        PyList_SET_ITEM(list, ninserted++, obj);
    }

    if(ninserted == ninside || ninserted == 0)
        return list;

    PyObject *ret = PyList_GetSlice(list, 0, ninserted-1);
    Py_DECREF(list);
    return ret;
}

static PyObject *PyPf_play_music(PyObject *self, PyObject *args)
{
    const char *name;
    if(!PyArg_ParseTuple(args, "z", &name)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a single argument - name of the WAV file or 'None'.");
        return NULL;
    }

    if(!Audio_PlayMusic(name)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to play the specified music track.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_curr_music(PyObject *self)
{
    const char *name = Audio_CurrMusic();
    if(!name) {
        Py_RETURN_NONE;
    }
    return PyString_FromString(name);
}

static PyObject *PyPf_get_all_music(PyObject *self)
{
    const char *tracks[512];
    size_t ntracks = Audio_GetAllMusic(ARR_SIZE(tracks), tracks);

    PyObject *ret = PyList_New(ntracks);
    if(!ret)
        return NULL;

    for(int i = 0; i < ntracks; i++) {
        PyObject *str = PyString_FromString(tracks[i]);
        if(!str) {
            Py_DECREF(str);
            return NULL;
        }
        PyList_SET_ITEM(ret, i, str);
    }
    return ret;
}

static PyObject *PyPf_play_effect(PyObject *self, PyObject *args)
{
    vec3_t pos;
    const char *name;

    if(!PyArg_ParseTuple(args, "s(fff)", &name, &pos.x, &pos.y, &pos.z)) {
        PyErr_SetString(PyExc_TypeError, "Arugment must be a string (name) and a tuple of 3 floats (position).");
        return NULL;
    }

    if(!Audio_Effect_Add(pos, name)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to play the specified effect at the specified position.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_play_global_effect(PyObject *self, PyObject *args, PyObject *kwargs)
{
    const char *name;
    int interrupt = 0;
    int channel = 0;
    static char *kwlist[] = {"name", "interrupt", "channel", NULL};

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "s|ii", kwlist, &name, &interrupt, &channel)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a string (effect name), an optional boolean (interrupt) argument, "
            "and an optional integer (channel) argument.");
        return NULL;
    }

    if(!Audio_PlayForegroundEffect(name, interrupt, channel)) {
        PyErr_SetString(PyExc_RuntimeError, "Unable to play the specified global effect.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_spawn_projectile(PyObject *self, PyObject *args)
{
    vec3_t origin;
    vec3_t velocity;
    uint32_t ent_parent;
    int faction_id;
    uint32_t cookie;
    int flags;
    struct proj_desc pd;

    if(!PyArg_ParseTuple(args, "(fff)(fff)IIII(ss(fff)f)", &origin.x, &origin.y, &origin.z,
        &velocity.x, &velocity.y, &velocity.z, &ent_parent, &faction_id, &cookie, &flags,
        &pd.basedir, &pd.pfobj, &pd.scale.x, &pd.scale.y, &pd.scale.z, &pd.speed)) {

        PyErr_SetString(PyExc_RuntimeError, "The following arugment list is expected: "
            "origin (tuple of 3 floats), "
            "velocity (tuple of 3 floats) "
            "ent_parent (int), "
            "faction_id (int), "
            "cookie (int), "
            "flags (int), "
            "a projectile descriptor (a tuple of 4 objects): "
            "[projectile model base directory (string), "
            "projectile model PFOBJ filename (string), "
            "projectile model scale (tuple of 3 floats), "
            "projectile speed (float)].");
        return NULL;
    }

    uint32_t ret = P_Projectile_Add(origin, velocity, ent_parent, faction_id, cookie, flags, pd);
    return PyInt_FromLong(ret);
}

static void script_task_destructor(void *arg)
{
    free(arg);
}

static struct result script_task(void *arg)
{
    ASSERT_IN_MAIN_THREAD();
    Task_SetDestructor(script_task_destructor, arg);

    struct script_arg *sarg = arg;
    bool result = S_RunFile(sarg->path, sarg->argc, (char**)sarg->argv);

    return (struct result) {
        .type = RESULT_BOOL,
        .val.as_bool = result
    };
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
    S_Camera_PyRegister(module);
    S_Task_PyRegister(module);
    S_Region_PyRegister(module);
    S_Constants_Expose(module); 
}

bool S_Init(const char *progname, const char *base_path, struct nk_context *ctx)
{
    Py_NoSiteFlag = 1;
    Py_SetProgramName((char*)progname);
    s_progname = progname;

    static char script_dir[512];
    pf_snprintf(script_dir, sizeof(script_dir), "%s/%s", g_basepath, "scripts"); 

    Py_SetPythonHome(script_dir); /* caches passed in pointer */
    Py_InitializeEx(0);

    if(!S_UI_Init(ctx))
        return false;
    if(!S_Entity_Init())
        return false;
    if(!S_Task_Init())
        return false;
    if(!S_Region_Init())
        return false;

    if(0 != PyList_Append(PySys_GetObject("path"), Py_BuildValue("s", script_dir)))
        return false;

    pf_snprintf(script_dir, sizeof(script_dir), "%s/%s", g_basepath, "scripts/stdlib");
    if(0 != PyList_Append(PySys_GetObject("path"), Py_BuildValue("s", script_dir)))
        return false;

    initpf();

    if(!S_Camera_Init())
        return false;

    PyObject *module = PyDict_GetItemString(PySys_GetObject("modules"), "pf");
    assert(module);
    /* Initialize the pickler after registering all the built-ins, so that they can
     * be indexed. */
    if(!S_Pickle_Init(module))
        return false;

    s_create_settings();

    E_Global_Register(EVENT_UPDATE_START, s_on_update, NULL, G_ALL);
    return true;
}

void S_Shutdown(void)
{
    E_Global_Unregister(EVENT_UPDATE_START, s_on_update);

    /* Free any globaly retained Python objects before finalizing 
     * the session */
    s_err_clear();
    S_Pickle_Clear();
    S_Camera_Clear();
    S_Region_Clear();
    S_Task_Clear();
    S_Entity_Clear();

    Py_Finalize();

    /* Free all memory allocated during initialization - this can only 
     * be done now as destructors can still be called during finalization */
    S_Pickle_Shutdown();
    S_Camera_Shutdown();
    S_Region_Shutdown();
    S_Task_Shutdown();
    S_Entity_Shutdown();
    S_UI_Shutdown();
}

bool S_RunFile(const char *path, int argc, char **argv)
{
    bool ret = false;
    FILE *script = fopen(path, "r");
    if(!script)
        return false;

    STALLOC(char*, cargv, argc + 1);
    cargv[0] = (char*)path;
    if(argc) {
        memcpy(cargv + 1, argv, sizeof(*argv) * argc);
    }

    /* The directory of the script file won't be automatically added by 'PyRun_SimpleFile'.
     * We add it manually to sys.path ourselves. */
    if (!s_sys_path_add_dir(path))
        goto done;

    PyObject *main_module = PyImport_AddModule("__main__"); /* borrowed */
    if (!main_module)
        goto done;
    
    PyObject *global_dict = PyModule_GetDict(main_module); /* borrowed */

    if(PyDict_GetItemString(global_dict, "__file__") == NULL) {
        PyObject *f = PyString_FromString(path);
        if(f == NULL)
            goto done;
        if(PyDict_SetItemString(global_dict, "__file__", f) < 0) {
            Py_DECREF(f);
            goto done;
        }
        Py_DECREF(f);
    }

    Sched_TryYield();

    PySys_SetArgvEx(argc + 1, cargv, 0);
    PyObject *result = PyRun_File(script, path, Py_file_input, global_dict, global_dict);
    ret = (result != NULL);
    Py_XDECREF(result);

    if(PyErr_Occurred()) {
        S_ShowLastError();
    }

done:
    fclose(script);
    return ret;
}

void S_RunFileAsync(const char *path, int argc, char **argv, struct future *result)
{
    struct script_arg *arg = malloc(sizeof(struct script_arg));
    if(!arg)
        return;

    arg->path = path;
    arg->argc = argc;
    arg->argv = (const char**)argv;

    uint32_t tid = Sched_Create(31, script_task, arg, result, 
        TASK_MAIN_THREAD_PINNED | TASK_BIG_STACK);
}

bool S_GetFilePath(char *out, size_t maxout)
{
    PyObject *main_module = PyImport_AddModule("__main__"); /* borrowed */
    if(!main_module)
        return false;

    PyObject *global_dict = PyModule_GetDict(main_module); /* borrowed */
    if(PyDict_GetItemString(global_dict, "__file__") == NULL)
        return false;

    PyObject *file = PyDict_GetItemString(global_dict, "__file__"); /* borrowed */
    if(!PyString_Check(file))
        return false;

    pf_strlcpy(out, PyString_AS_STRING(file), maxout);
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

    /* Make sure to retain the callable object - PyObject_CallObject will not do this for us.
     * The reason is that the invoked handler may unregister itself, thus removing the last 
     * living reference to it. */
    Py_INCREF(callable);
    ret = PyObject_CallObject(callable, args);
    Py_DECREF(callable);
    Py_DECREF(args);

    if(PyErr_Occurred()) {
        S_ShowLastError();
    }
    Py_XDECREF(ret);
}

void S_Retain(script_opaque_t obj)
{
    Py_XINCREF(obj);
}

void S_Release(script_opaque_t obj)
{
    Py_XDECREF(obj);
}

script_opaque_t S_WrapEngineEventArg(int eventnum, void *arg)
{
    switch(eventnum) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        return Py_BuildValue("(iii)", 
            ((SDL_Event*)arg)->key.keysym.scancode, ((SDL_Event*)arg)->key.keysym.sym, ((SDL_Event*)arg)->key.keysym.mod);

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

    case EVENT_SESSION_FAIL_LOAD:
        return PyString_FromString(arg);

    case EVENT_BUILD_TARGET_ACQUIRED: 
    case EVENT_HARVEST_TARGET_ACQUIRED:
    case EVENT_STORAGE_TARGET_ACQUIRED:
    case EVENT_TRANSPORT_TARGET_ACQUIRED:
    case EVENT_BUILDING_CONSTRUCTED: 
    case EVENT_ORDER_ISSUED:
    case EVENT_ENTITY_DIED: {
        PyObject *ent = S_Entity_ObjForUID((uintptr_t)arg);
        if(ent) {
            Py_INCREF(ent);
            return ent;
        }else{
            Py_RETURN_NONE;
        }
    }
    case EVENT_SESSION_POPPED:
        return s_wrap_argv(arg);

    case EVENT_STORAGE_SITE_AMOUNT_CHANGED: {
        struct ss_delta_event *event = arg;
        return Py_BuildValue("si", event->name, event->delta);
    }
    case EVENT_PROJECTILE_HIT: {
        struct proj_hit *hit = arg;
        PyObject *ent = S_Entity_ObjForUID(hit->ent_uid);
        if(!ent) {
            ent = Py_None;
        }
        Py_INCREF(ent);
        PyObject *parent = S_Entity_ObjForUID(hit->parent_uid);
        if(!parent) {
            parent = Py_None;
        }
        Py_INCREF(parent);
        return Py_BuildValue("OIOI", ent, hit->proj_uid, parent, hit->cookie);
    }
    case EVENT_ENTERED_REGION:
    case EVENT_EXITED_REGION: {
        return PyString_FromString(arg);
    }
    default:
        Py_RETURN_NONE;
    }
}

script_opaque_t S_UnwrapIfWeakref(script_opaque_t arg)
{
    assert(arg);
    if(PyWeakref_Check(arg)) {
        PyObject *ret = PyWeakref_GetObject(arg);
        Py_INCREF(ret);
        return ret;
    }
    Py_INCREF(arg);
    return arg;
}

bool S_WeakrefDied(script_opaque_t arg)
{
    return (PyWeakref_Check(arg) && (PyWeakref_GetObject(arg) == Py_None));
}

bool S_ObjectsEqual(script_opaque_t a, script_opaque_t b)
{
    return (1 == PyObject_RichCompareBool(a, b, Py_EQ));
}

void S_ClearState(void)
{
    S_Shutdown();
    S_Init(s_progname, g_basepath, UI_GetContext());
    PyGC_Collect(); /* quick sanity check */
}

bool S_SaveState(SDL_RWops *stream)
{
    PyGC_Collect();

    PyObject *modules_dict = PySys_GetObject("modules"); /* borrowed */
    assert(modules_dict);
    bool ret = false;

    const size_t max_handlers = 65536;
    struct script_handler *handlers = malloc(max_handlers * sizeof(struct script_handler));
    if(!handlers)
        return false;
    size_t nhandlers = E_GetScriptHandlers(max_handlers, handlers);

    PyObject *saved_handlers = PyTuple_New(nhandlers);
    if(!saved_handlers)
        goto fail_tuple;

    for(int i = 0; i < nhandlers; i++) {
        PyObject *val = Py_BuildValue("lllOO", handlers[i].event, handlers[i].id, 
            handlers[i].simmask, (PyObject*)handlers[i].handler, (PyObject*)handlers[i].arg);
        if(!val)
            goto fail_handlers;
        PyTuple_SET_ITEM(saved_handlers, i, val);
    }

    PyInterpreterState *interp = PyThreadState_Get()->interp;
    assert(interp);

    PyObject *tasks = S_Task_GetAll();
    if(!tasks)
        goto fail_tasks;

    PyObject *state = Py_BuildValue("OOOOOOOOO", 
        interp->modules, 
        interp->sysdict, 
        interp->builtins,
        interp->modules_reloading,
        interp->codec_search_path,
        interp->codec_search_cache,
        interp->codec_error_registry,
        saved_handlers,
        tasks
    );
    if(!state)
        goto fail_state;

    ret = S_PickleObjgraph(state, stream);
    Py_DECREF(state);
    SDL_RWwrite(stream, "\n", 1, 1);

fail_state:
    Py_DECREF(tasks);
fail_tasks:
fail_handlers:
    Py_DECREF(saved_handlers);
fail_tuple:
    PF_FREE(handlers);
    return ret;
}

bool S_LoadState(SDL_RWops *stream)
{
    PyInterpreterState *interp = PyThreadState_Get()->interp;
    assert(interp);
    bool ret = false;

    PyObject *state = S_UnpickleObjgraph(stream);
    if(!state)
        return false;

    if(!PyTuple_Check(state) || (PyTuple_GET_SIZE(state) != 9))
        goto fail;

    PyInterpreterState_Clear(interp);

    interp->modules = PyTuple_GET_ITEM(state, 0);
    interp->sysdict = PyTuple_GET_ITEM(state, 1);
    interp->builtins = PyTuple_GET_ITEM(state, 2);
    interp->modules_reloading = PyTuple_GET_ITEM(state, 3);
    interp->codec_search_path = PyTuple_GET_ITEM(state, 4);
    interp->codec_search_cache = PyTuple_GET_ITEM(state, 5);
    interp->codec_error_registry = PyTuple_GET_ITEM(state, 6);

    Py_INCREF(interp->modules);
    Py_INCREF(interp->sysdict);
    Py_INCREF(interp->builtins);
    Py_INCREF(interp->modules_reloading);
    Py_INCREF(interp->codec_search_path);
    Py_INCREF(interp->codec_search_cache);
    Py_INCREF(interp->codec_error_registry);

    PyObject *handlers = PyTuple_GET_ITEM(state, 7);
    if(!PyTuple_Check(handlers))
        goto fail;

    for(int i = 0; i < PyTuple_GET_SIZE(handlers); i++) {

        PyObject *entry = PyTuple_GET_ITEM(handlers, i);
        if(!PyTuple_Check(entry) || PyTuple_GET_SIZE(entry) != 5)
            goto fail;

        PyObject *event = PyTuple_GET_ITEM(entry, 0);
        PyObject *uid = PyTuple_GET_ITEM(entry, 1);
        PyObject *simmask = PyTuple_GET_ITEM(entry, 2);
        PyObject *handler = PyTuple_GET_ITEM(entry, 3);
        PyObject *arg = PyTuple_GET_ITEM(entry, 4);

        if(!PyInt_Check(event)
        || !PyInt_Check(uid)
        || !PyInt_Check(simmask)
        || !PyCallable_Check(handler))
            goto fail;

        int ievent = PyInt_AS_LONG(event);
        uint32_t iuid = PyInt_AS_LONG(uid);
        int isimmask = PyInt_AS_LONG(simmask);

        Py_INCREF(handler);
        Py_INCREF(arg);

        if(iuid == ~((uint32_t)0)) {
            E_Global_ScriptRegister(ievent, handler, arg, isimmask);
        }else{
            E_Entity_ScriptRegister(ievent, iuid, handler, arg, isimmask);
        }
    }
    ret = true;

    /* Tasks are already installed and retained during unpickling. We don't 
     * need to do anything more with them. */

    char tmp[1];
    SDL_RWread(stream, tmp, sizeof(tmp), 1); /* consume NULL byte */
    do{
        SDL_RWread(stream, tmp, sizeof(tmp), 1); 
    }while(tmp[0] != '\n'); /* and newline (preceded by optional carriage return) */

fail:
    Py_DECREF(state);
    return ret;
} 

void S_ShowLastError(void)
{
    s_err_ctx.occurred = PyErr_Occurred();
    PyErr_Fetch(&s_err_ctx.type, &s_err_ctx.value, &s_err_ctx.traceback);
    PyErr_NormalizeException(&s_err_ctx.type, &s_err_ctx.value, &s_err_ctx.traceback);

    if(s_err_ctx.occurred) {
        s_err_ctx.prev_state = G_GetSimState();
        G_SetSimState(G_PAUSED_FULL);
    }
}

uint64_t S_ScriptTypeID(uint32_t uid)
{
    PyObject *ent = S_Entity_ObjForUID(uid);
    if(!ent)
        return 0;

    uint64_t ret = (uintptr_t)(Py_TYPE(ent));
    return ret;
}

