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

#define MEM_FILE_SYS MEM_SYS_SCRIPT
#define MEM_FILE_SUB MEM_SUB_SCRIPT_DISPATCH

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
#include "py_console.h"
#include "py_perf.h"
#include "public/script.h"
#include "../entity.h"
#include "../game/public/game.h"
#include "../game/duty.h"
#include "../game/automation.h"
#include "../game/population.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../render/gl_mem.h"
#include "../navigation/public/nav.h"
#include "../audio/public/audio.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../phys/public/phys.h"
#include "../phys/public/collision.h"
#include "../lib/public/SDL_vec_rwops.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/nk_file_browser.h"
#include "../mem.h"
#include "../lib/public/windows.h"
#include "../event.h"
#include "../config.h"
#include "../scene.h"
#include "../settings.h"
#include "../main.h"
#include "../ui.h"
#include "../session.h"
#include "../perf.h"
#include "../cursor.h"
#include "../cam_control.h"
#include "../task.h"
#include "../sched.h"
#include "../asset_load.h"
#include "../sprite.h"

#include <SDL.h>
#include <mimalloc-stats.h>
#include <stdio.h>
#include <time.h>

#if defined(__linux__)
#include <sys/stat.h>
#endif

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_SCRIPT, MEM_SUB_SCRIPT_DISPATCH)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_SCRIPT, MEM_SUB_SCRIPT_DISPATCH)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_SCRIPT, MEM_SUB_SCRIPT_DISPATCH)


#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))
#define SLOW_HANDLER_US (3000.0)

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
static PyObject *PyPf_map_add_splat(PyObject *self, PyObject *args);
static PyObject *PyPf_map_remove_splat(PyObject *self, PyObject *args);
static PyObject *PyPf_add_decal(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_remove_decal(PyObject *self, PyObject *args);
static PyObject *PyPf_update_decal(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_get_decal(PyObject *self, PyObject *args);
static PyObject *PyPf_decals_at(PyObject *self, PyObject *args);
static PyObject *PyPf_get_all_decals(PyObject *self);
static PyObject *PyPf_set_decal_order(PyObject *self, PyObject *args);
static PyObject *PyPf_set_decal_highlight(PyObject *self, PyObject *args);
static PyObject *PyPf_clear_decals(PyObject *self);
static PyObject *PyPf_decal_image_size(PyObject *self, PyObject *args);
static PyObject *PyPf_decals_in_screen_rect(PyObject *self, PyObject *args);
static PyObject *PyPf_set_decal_drag_box(PyObject *self, PyObject *args);
static PyObject *PyPf_road_at(PyObject *self, PyObject *args);
static PyObject *PyPf_road_for_tile(PyObject *self, PyObject *args);
static PyObject *PyPf_road_bounds(PyObject *self, PyObject *args);
static PyObject *PyPf_add_road(PyObject *self, PyObject *args);
static PyObject *PyPf_remove_road(PyObject *self, PyObject *args);
static PyObject *PyPf_link_roads(PyObject *self, PyObject *args);
static PyObject *PyPf_unlink_roads(PyObject *self, PyObject *args);
static PyObject *PyPf_get_road(PyObject *self, PyObject *args);
static PyObject *PyPf_set_road_kind(PyObject *self, PyObject *args);
static PyObject *PyPf_get_road_kind(PyObject *self, PyObject *args);
static PyObject *PyPf_road_speed_factor(PyObject *self, PyObject *args);
static PyObject *PyPf_get_all_roads(PyObject *self);
static PyObject *PyPf_clear_roads(PyObject *self);
static PyObject *PyPf_set_road_hover(PyObject *self, PyObject *args);
static PyObject *PyPf_road_buildable(PyObject *self, PyObject *args);
static PyObject *PyPf_road_tiles_under(PyObject *self, PyObject *args);
static PyObject *PyPf_set_road_refused(PyObject *self, PyObject *args);
static PyObject *PyPf_show_roads(PyObject *self);
static PyObject *PyPf_hide_roads(PyObject *self);
static PyObject *PyPf_show_placement_grid(PyObject *self, PyObject *args);
static PyObject *PyPf_set_cluster_preview(PyObject *self, PyObject *args);
static PyObject *PyPf_set_road_tileset(PyObject *self, PyObject *args);
static PyObject *PyPf_get_road_tileset(PyObject *self);
static PyObject *PyPf_load_scene(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_open_url(PyObject *self, PyObject *args);
static PyObject *PyPf_preload_pfobj(PyObject *self, PyObject *args);

static PyObject *PyPf_register_event_handler(PyObject *self, PyObject *args);
static PyObject *PyPf_register_ui_event_handler(PyObject *self, PyObject *args);
static PyObject *PyPf_unregister_event_handler(PyObject *self, PyObject *args);
static PyObject *PyPf_global_event(PyObject *self, PyObject *args);
static PyObject *PyPf_get_ticks(PyObject *self);
static PyObject *PyPf_perf_us(PyObject *self);
static PyObject *PyPf_ticks_delta(PyObject *self, PyObject *args);
static PyObject *PyPf_flush_tasks(PyObject *self);

static PyObject *PyPf_get_active_camera(PyObject *self);
static PyObject *PyPf_set_active_camera(PyObject *self, PyObject *args);

static PyObject *PyPf_prev_frame_ms(PyObject *self);
static PyObject *PyPf_prev_frame_perfstats(PyObject *self);
static PyObject *PyPf_prev_frame_memstats(PyObject *self);
static PyObject *PyPf_prev_frame_vramstats(PyObject *self);
static PyObject *PyPf_prev_frame_gpu_stats(PyObject *self);
static PyObject *PyPf_prev_frame_billboard_stats(PyObject *self);
static PyObject *PyPf_get_nav_tick_times(PyObject *self);
static PyObject *PyPf_get_frame_times(PyObject *self);
static PyObject *PyPf_prev_frame_mem_accounting(PyObject *self);
static PyObject *PyPf_prev_frame_gpu_mem_accounting(PyObject *self);
static PyObject *PyPf_mem_audit(PyObject *self);
static PyObject *PyPf_prev_frame_allocd_bytes(PyObject *self);
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
static PyObject *PyPf_get_files_in_dir(PyObject *self, PyObject *args);
static PyObject *PyPf_get_time_str(PyObject *self);
static PyObject *PyPf_ensure_directory(PyObject *self, PyObject *args);
static PyObject *PyPf_delete_user_file(PyObject *self, PyObject *args);
static PyObject *PyPf_shift_pressed(PyObject *self);
static PyObject *PyPf_ctrl_pressed(PyObject *self);
static PyObject *PyPf_get_key_name(PyObject *self, PyObject *args);

static PyObject *PyPf_get_active_font(PyObject *self);
static PyObject *PyPf_set_active_font(PyObject *self, PyObject *args);
static PyObject *PyPf_get_text_outline(PyObject *self);
static PyObject *PyPf_set_text_outline(PyObject *self, PyObject *args);
static PyObject *PyPf_set_skybox(PyObject *self, PyObject *args);

static PyObject *PyPf_show_regions(PyObject *self);
static PyObject *PyPf_hide_regions(PyObject *self);
static PyObject *PyPf_enable_fog_of_war(PyObject *self);
static PyObject *PyPf_disable_fog_of_war(PyObject *self);
static PyObject *PyPf_explore_map(PyObject *self, PyObject *args);
static PyObject *PyPf_enable_unit_selection(PyObject *self);
static PyObject *PyPf_disable_unit_selection(PyObject *self);
static PyObject *PyPf_clear_unit_selection(PyObject *self);
static PyObject *PyPf_get_unit_selection(PyObject *self);
static PyObject *PyPf_get_idle_units(PyObject *self);
static PyObject *PyPf_set_unit_selection(PyObject *self, PyObject *args);
static PyObject *PyPf_get_hovered_unit(PyObject *self);
static PyObject *PyPf_gather_blocked(PyObject *self);
static PyObject *PyPf_entities_for_tag(PyObject *self, PyObject *args);

static PyObject *PyPf_hide_healthbars(PyObject *self);
static PyObject *PyPf_show_healthbars(PyObject *self);
static PyObject *PyPf_hide_unit_icons(PyObject *self);
static PyObject *PyPf_show_unit_icons(PyObject *self);
static PyObject *PyPf_set_right_click_attack(PyObject *self, PyObject *args);
static PyObject *PyPf_get_right_click_attack(PyObject *self);

static PyObject *PyPf_get_resource_list(PyObject *self);
static PyObject *PyPf_get_resource_stored(PyObject *self, PyObject *args);
static PyObject *PyPf_get_resource_capacity(PyObject *self, PyObject *args);
static PyObject *PyPf_get_resource_produced(PyObject *self, PyObject *args);
static PyObject *PyPf_get_resource_consumed(PyObject *self, PyObject *args);
static PyObject *PyPf_set_resource_icon(PyObject *self, PyObject *args);
static PyObject *PyPf_get_resource_icon(PyObject *self, PyObject *args);
static PyObject *PyPf_get_render_settings(PyObject *self);
static PyObject *PyPf_get_factions_list(PyObject *self);
static PyObject *PyPf_add_faction(PyObject *self, PyObject *args);
static PyObject *PyPf_remove_faction(PyObject *self, PyObject *args);
static PyObject *PyPf_update_faction(PyObject *self, PyObject *args);
static PyObject *PyPf_set_faction_controllable(PyObject *self, PyObject *args);
static PyObject *PyPf_set_diplomacy_state(PyObject *self, PyObject *args);
static PyObject *PyPf_get_diplomacy_state(PyObject *self, PyObject *args);

static PyObject *PyPf_get_population(PyObject *self, PyObject *args);
static PyObject *PyPf_get_population_limit(PyObject *self, PyObject *args);
static PyObject *PyPf_get_allied_population(PyObject *self, PyObject *args);
static PyObject *PyPf_get_enemy_population(PyObject *self, PyObject *args);
static PyObject *PyPf_get_player_population(PyObject *self);
static PyObject *PyPf_get_player_population_limit(PyObject *self);

static PyObject *PyPf_get_tile(PyObject *self, PyObject *args);
static PyObject *PyPf_update_tile(PyObject *self, PyObject *args);
static PyObject *PyPf_tiles_under_obj(PyObject *self, PyObject *args);
static PyObject *PyPf_set_map_highlight_size(PyObject *self, PyObject *args);
static PyObject *PyPf_get_minimap_position(PyObject *self, PyObject *args);
static PyObject *PyPf_set_minimap_position(PyObject *self, PyObject *args);
static PyObject *PyPf_set_minimap_resize_mask(PyObject *self, PyObject *args);
static PyObject *PyPf_get_minimap_size(PyObject *self);
static PyObject *PyPf_set_minimap_size(PyObject *self, PyObject *args);
static PyObject *PyPf_set_minimap_border_clr(PyObject *self, PyObject *args);
static PyObject *PyPf_set_minimap_render_all_ents(PyObject *self, PyObject *args);
static PyObject *PyPf_mouse_over_minimap(PyObject *self);
static PyObject *PyPf_map_pos_over_water(PyObject *self, PyObject *args);
static PyObject *PyPf_map_pos_over_land(PyObject *self, PyObject *args);
static PyObject *PyPf_map_height_at_point(PyObject *self, PyObject *args);
static PyObject *PyPf_map_nearest_pathable(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_map_locations_reachable(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_map_nearest_pathable_water(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_map_nearest_pathable_air(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_map_pos_pathable(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_map_ground_open(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_obj_adjacent(PyObject *self, PyObject *args);
static PyObject *PyPf_map_tile_info(PyObject *self, PyObject *args);
static PyObject *PyPf_closest_adjacent_pos(PyObject *self, PyObject *args);
static PyObject *PyPf_debug_desc(PyObject *self, PyObject *args);
static PyObject *PyPf_map_reachable_now(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_map_pos_under_cursor(PyObject *self);
static PyObject *PyPf_draw_text(PyObject *self, PyObject *args);
static PyObject *PyPf_draw_text_outlined(PyObject *self, PyObject *args);
static PyObject *PyPf_dump_framebuffer(PyObject *self, PyObject *args);
static PyObject *PyPf_set_storage_site_ui_style(PyObject *self, PyObject *args);
static PyObject *PyPf_set_storage_site_ui_border_color(PyObject *self, PyObject *args);
static PyObject *PyPf_set_storage_site_ui_font_color(PyObject *self, PyObject *args);
static PyObject *PyPf_storage_site_show_ui(PyObject *self, PyObject *args);
static PyObject *PyPf_set_garrison_ui_font_color(PyObject *self, PyObject *args);
static PyObject *PyPf_set_garrison_ui_icon(PyObject *self, PyObject *args);
static PyObject *PyPf_set_garrison_ui_style(PyObject *self, PyObject *args);
static PyObject *PyPf_garrison_show_ui(PyObject *self, PyObject *args);

static PyObject *PyPf_set_move_on_left_click(PyObject *self);
static PyObject *PyPf_set_attack_on_left_click(PyObject *self);
static PyObject *PyPf_set_build_on_left_click(PyObject *self);
static PyObject *PyPf_set_gather_on_left_click(PyObject *self);
static PyObject *PyPf_set_pick_up_on_left_click(PyObject *self);
static PyObject *PyPf_set_drop_off_on_left_click(PyObject *self);
static PyObject *PyPf_set_transport_on_left_click(PyObject *self);
static PyObject *PyPf_set_evict_on_left_click(PyObject *self);
static PyObject *PyPf_set_position_rally_point_on_left_click(PyObject *self);
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
static PyObject *PyPf_set_rts_camera_zoom_enabled(PyObject *self, PyObject *args);
static PyObject *PyPf_get_rts_camera_zoom_enabled(PyObject *self);

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
static PyObject *PyPf_set_damage_table(PyObject *self, PyObject *args);
static PyObject *PyPf_get_damage_table(PyObject *self);
static PyObject *PyPf_formation_arrange(PyObject *self, PyObject *args);
static PyObject *PyPf_move_in_formation(PyObject *self, PyObject *args);
static PyObject *PyPf_attack_in_formation(PyObject *self, PyObject *args);
static PyObject *PyPf_formation_preferred_for_set(PyObject *self, PyObject *args);
static PyObject *PyPf_group_lock(PyObject *self, PyObject *args);
static PyObject *PyPf_group_unlock(PyObject *self, PyObject *args);
static PyObject *PyPf_group_for_set(PyObject *self, PyObject *args);
static PyObject *PyPf_group_members(PyObject *self, PyObject *args);
static PyObject *PyPf_cluster_members(PyObject *self, PyObject *args);
static PyObject *PyPf_cluster_anchor(PyObject *self, PyObject *args);
static PyObject *PyPf_cluster_colour(PyObject *self, PyObject *args);
static PyObject *PyPf_set_cluster_icon(PyObject *self, PyObject *args);
static PyObject *PyPf_district_at(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_show_districts(PyObject *self, PyObject *args);
static PyObject *PyPf_districts_shown(PyObject *self);
static PyObject *PyPf_show_district(PyObject *self, PyObject *args);
static PyObject *PyPf_district_shown(PyObject *self);
static PyObject *PyPf_district_gathered(PyObject *self, PyObject *args);
static PyObject *PyPf_set_district_preview(PyObject *self, PyObject *args);
static PyObject *PyPf_cluster_preview_join(PyObject *self, PyObject *args);
static PyObject *PyPf_provision_counts(PyObject *self, PyObject *args);
static PyObject *PyPf_provision_demand(PyObject *self, PyObject *args);
static PyObject *PyPf_resource_balance(PyObject *self, PyObject *args);
static PyObject *PyPf_clusters(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_good_weight(PyObject *self, PyObject *args);
static PyObject *PyPf_set_good_weight(PyObject *self, PyObject *args);
static PyObject *PyPf_stock_target(PyObject *self, PyObject *args);
static PyObject *PyPf_set_stock_target(PyObject *self, PyObject *args);
static PyObject *PyPf_gather_rate(PyObject *self, PyObject *args);
static PyObject *PyPf_set_gather_rate(PyObject *self, PyObject *args);
static PyObject *PyPf_gatherer_rate(PyObject *self, PyObject *args);
static PyObject *PyPf_district_weight(PyObject *self, PyObject *args);
static PyObject *PyPf_set_district_weight(PyObject *self, PyObject *args);
static PyObject *PyPf_workers_carry_out(PyObject *self, PyObject *args);
static PyObject *PyPf_open_requests(PyObject *self, PyObject *args);
static PyObject *PyPf_set_workers_carry_out(PyObject *self, PyObject *args);
static PyObject *PyPf_crew_size(PyObject *self, PyObject *args);
static PyObject *PyPf_set_crew_size(PyObject *self, PyObject *args);
static PyObject *PyPf_crew_busy(PyObject *self, PyObject *args);
static PyObject *PyPf_set_automation_option(PyObject *self, PyObject *args);
static PyObject *PyPf_set_good_class(PyObject *self, PyObject *args);
static PyObject *PyPf_set_overlay_span(PyObject *self, PyObject *args);
static PyObject *PyPf_get_automation_option(PyObject *self, PyObject *args);
static PyObject *PyPf_awaits_builders(PyObject *self, PyObject *args);
static PyObject *PyPf_set_workshop_posts(PyObject *self, PyObject *args);
static PyObject *PyPf_set_post_demand(PyObject *self, PyObject *args);
static PyObject *PyPf_set_locked_posts(PyObject *self, PyObject *args);
static PyObject *PyPf_workshop_posts(PyObject *self, PyObject *args);
static PyObject *PyPf_post_workshop(PyObject *self, PyObject *args);
static PyObject *PyPf_man_post(PyObject *self, PyObject *args);
static PyObject *PyPf_evict_post(PyObject *self, PyObject *args);
static PyObject *PyPf_leave_post(PyObject *self, PyObject *args);
static PyObject *PyPf_workshop_eating(PyObject *self, PyObject *args);
static PyObject *PyPf_end_duty(PyObject *self, PyObject *args);
static PyObject *PyPf_set_duty_by(PyObject *self, PyObject *args);
static PyObject *PyPf_set_group_ui_style(PyObject *self, PyObject *args);
static PyObject *PyPf_set_group_ui_font_color(PyObject *self, PyObject *args);
static PyObject *PyPf_set_group_ui_bonus_color(PyObject *self, PyObject *args);
static PyObject *PyPf_set_group_highlight(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_clear_group_highlight(PyObject *self);
static PyObject *PyPf_group_for_ent(PyObject *self, PyObject *args);
static PyObject *PyPf_group_bonuses(PyObject *self, PyObject *args);
static PyObject *PyPf_show_range_indicator(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyPf_hide_range_indicator(PyObject *self);
static PyObject *PyPf_show_console(PyObject *self);
static PyObject *PyPf_get_version_string(PyObject *self);

static PyObject *PyPf_spawn_sprite_static(PyObject *self, PyObject *args);
static PyObject *PyPf_spawn_sprite_animated(PyObject *self, PyObject *args);

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

    {"map_add_splat", 
    (PyCFunction)PyPf_map_add_splat, METH_VARARGS,
    "Set a secondary texture for a particular texture. Takes 2 integers, the base material index "
    "and the accent material index."},

    {"map_remove_splat", 
    (PyCFunction)PyPf_map_remove_splat, METH_VARARGS,
    "Remove a secondary texture for a particular texture. Takes 2 integers, the base material index "
    "and the accent material index."},

    {"add_decal", 
    (PyCFunction)PyPf_add_decal, METH_VARARGS | METH_KEYWORDS,
    "Place an image (specified as a path relative to the base directory) on the map surface. "
    "Takes the image path, the XZ position of its centre and its XZ dimensions, followed by an "
    "optional 'rotation' (degrees), 'alpha' and 'hide_foliage' (no foliage where the decal is "
    "mostly opaque). Returns the new decal's integer ID."},

    {"remove_decal", 
    (PyCFunction)PyPf_remove_decal, METH_VARARGS,
    "Remove the decal with the specified ID."},

    {"update_decal", 
    (PyCFunction)PyPf_update_decal, METH_VARARGS | METH_KEYWORDS,
    "Change the attributes of the decal with the specified ID. Takes any of the 'path', 'pos', "
    "'dims', 'rotation', 'alpha' and 'hide_foliage' keyword arguments."},

    {"get_decal", 
    (PyCFunction)PyPf_get_decal, METH_VARARGS,
    "Returns a dictionary of the attributes of the decal with the specified ID."},

    {"decals_at", 
    (PyCFunction)PyPf_decals_at, METH_VARARGS,
    "Returns a list of the IDs of the decals covering the specified XZ position, top-most first."},

    {"get_all_decals", 
    (PyCFunction)PyPf_get_all_decals, METH_NOARGS,
    "Returns a list of the IDs of all decals, in the order that they are drawn."},

    {"set_decal_order", 
    (PyCFunction)PyPf_set_decal_order, METH_VARARGS,
    "Move the decal with the specified ID to the specified index of the draw order."},

    {"set_decal_highlight", 
    (PyCFunction)PyPf_set_decal_highlight, METH_VARARGS,
    "Set whether the decal with the specified ID is drawn highlighted."},

    {"clear_decals", 
    (PyCFunction)PyPf_clear_decals, METH_NOARGS,
    "Remove all decals from the map."},

    {"decal_image_size", 
    (PyCFunction)PyPf_decal_image_size, METH_VARARGS,
    "Returns the (width, height) in pixels of the image at the specified path (relative to the "
    "base directory)."},

    {"decals_in_screen_rect", 
    (PyCFunction)PyPf_decals_in_screen_rect, METH_VARARGS,
    "Returns a list of the IDs of the decals whose centres lie within the screen rectangle "
    "spanned by two (x, y) corners, as seen by the active camera."},

    {"set_decal_drag_box", 
    (PyCFunction)PyPf_set_decal_drag_box, METH_VARARGS,
    "Draw a selection box from the specified (x, y) screen position to the cursor, or stop "
    "drawing it when passed None."},

    {"road_at", 
    (PyCFunction)PyPf_road_at, METH_VARARGS,
    "Returns the (row, column) of the road tile covering the specified XZ position, or None "
    "when the position is off the map. Road tiles exist whether or not a road is placed on them."},

    {"road_for_tile", 
    (PyCFunction)PyPf_road_for_tile, METH_VARARGS,
    "Returns the (row, column) of the road tile covering the specified map tile, given as "
    "((chunk_r, chunk_c), (tile_r, tile_c))."},

    {"road_bounds", 
    (PyCFunction)PyPf_road_bounds, METH_VARARGS,
    "Returns the ((min_x, min_z), (max_x, max_z)) world bounds of the specified road tile."},

    {"add_road", 
    (PyCFunction)PyPf_add_road, METH_VARARGS,
    "Place a road on the specified (row, column) road tile, linked to nothing. Returns False "
    "when the tile is off the map."},

    {"remove_road", 
    (PyCFunction)PyPf_remove_road, METH_VARARGS,
    "Remove the road from the specified (row, column) road tile, along with its links. Returns "
    "False when there is no road there."},

    {"link_roads", 
    (PyCFunction)PyPf_link_roads, METH_VARARGS,
    "Link two orthogonally adjacent (row, column) road tiles which both have a road. Returns "
    "False when they cannot be linked."},

    {"unlink_roads", 
    (PyCFunction)PyPf_unlink_roads, METH_VARARGS,
    "Remove the link between two orthogonally adjacent (row, column) road tiles. Returns False "
    "when they cannot be unlinked."},

    {"get_road", 
    (PyCFunction)PyPf_get_road, METH_VARARGS,
    "Returns the mask of ROAD_LINK_* flags of the road on the specified (row, column) road tile, "
    "or None when there is no road there."},

    {"set_road_kind", 
    (PyCFunction)PyPf_set_road_kind, METH_VARARGS,
    "Make the road on the specified (row, column) road tile one of the ROAD_KIND_* kinds. Returns "
    "False when there is no road there."},

    {"get_road_kind", 
    (PyCFunction)PyPf_get_road_kind, METH_VARARGS,
    "Returns the ROAD_KIND_* kind of the road on the specified (row, column) road tile, or None "
    "when there is no road there."},

    {"road_speed_factor", 
    (PyCFunction)PyPf_road_speed_factor, METH_VARARGS,
    "Returns how much faster a unit going its way along the roads moves at the specified XZ "
    "position: 1.0 off the roads."},

    {"get_all_roads", 
    (PyCFunction)PyPf_get_all_roads, METH_NOARGS,
    "Returns a list of (row, column, links) tuples, one for every road tile with a road."},

    {"clear_roads", 
    (PyCFunction)PyPf_clear_roads, METH_NOARGS,
    "Remove all roads from the map."},

    {"set_road_hover", 
    (PyCFunction)PyPf_set_road_hover, METH_VARARGS,
    "Outline the specified (row, column) road tile, or stop outlining when passed None."},

    {"road_buildable", 
    (PyCFunction)PyPf_road_buildable, METH_VARARGS,
    "Returns True if a road site of the specified faction may stand on the specified (row, column) "
    "road tile: its ground is passable and explored, with no building on it. Units on it do not matter."},

    {"road_tiles_under", 
    (PyCFunction)PyPf_road_tiles_under, METH_VARARGS,
    "Returns the list of (row, column) road tiles, built on or not, that the footprint of the building "
    "with the specified UID overlaps, which a building units walk over leaves alone."},

    {"set_road_refused", 
    (PyCFunction)PyPf_set_road_refused, METH_VARARGS,
    "Draw the specified list of (row, column) road tiles in red, as tiles no road can be laid on. "
    "An empty list or None stops drawing them."},

    {"show_roads", 
    (PyCFunction)PyPf_show_roads, METH_NOARGS,
    "Draw an overlay of the road tiles and their links."},

    {"hide_roads", 
    (PyCFunction)PyPf_hide_roads, METH_NOARGS,
    "Stop drawing the overlay of the road tiles and their links."},

    {"show_placement_grid", 
    (PyCFunction)PyPf_show_placement_grid, METH_VARARGS,
    "Takes a boolean and an optional (X, Z) point. Turns on or off a grid, the size of a road tile and "
    "lined up with the road tiles, drawn over the terrain about the cursor, or about the point if it is "
    "given."},

    {"set_road_tileset", 
    (PyCFunction)PyPf_set_road_tileset, METH_VARARGS,
    "Set the images drawn on the road tiles. Takes a list of 16 entries, indexed by the mask of "
    "ROAD_LINK_* flags of a tile, each an image path (relative to the base directory) or a list "
    "of up to 4 variant image paths. Each image covers a whole road tile, with its top edge "
    "towards ROAD_LINK_UP and its right edge towards ROAD_LINK_RIGHT. None stops drawing the "
    "roads."},

    {"get_road_tileset", 
    (PyCFunction)PyPf_get_road_tileset, METH_NOARGS,
    "Returns the road tileset as a list of 16 lists of image paths, or None when there is none."},

    {"load_scene", 
    (PyCFunction)PyPf_load_scene, METH_VARARGS | METH_KEYWORDS,
    "Import list of entities from a PFSCENE file (specified as a path string). "
    "Returns a tuple of the following items: list of loaded entities, list of loaded regions, "
    "list of loaded cameras."},

    {"open_url", 
    (PyCFunction)PyPf_open_url, METH_VARARGS,
    "Open the specified URL in the system's browser."},

    {"preload_pfobj", 
    (PyCFunction)PyPf_preload_pfobj, METH_VARARGS,
    "Load and cache the model and animation data from the specified PFOBJ file."},

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

    {"perf_us",
    (PyCFunction)PyPf_perf_us, METH_NOARGS,
    "The wall clock in microseconds, as a float, for timing code in probes."},

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
    "Get a tuple of the performance data for the previous frame."},

    {"prev_frame_memstats", 
    (PyCFunction)PyPf_prev_frame_memstats, METH_NOARGS,
    "Get a dictionary of the memory allocation and usage statistics for the previous frame."},

    {"prev_frame_vramstats", 
    (PyCFunction)PyPf_prev_frame_vramstats, METH_NOARGS,
    "Get a dictionary of the GPU video memory statistics (in KB) for the previous frame."},

    {"prev_frame_gpu_stats",
    (PyCFunction)PyPf_prev_frame_gpu_stats, METH_NOARGS,
    "Get a dictionary of GPU pipeline-statistics counters (vertices/primitives "
    "submitted, vertex/fragment-shader invocations, clipping input/output "
    "primitives) for the previous frame."},

    {"prev_frame_billboard_stats",
    (PyCFunction)PyPf_prev_frame_billboard_stats, METH_NOARGS,
    "Get a dictionary with the number of camera-visible entities rendered as "
    "billboards and the total number drawn for the previous frame."},

    {"get_nav_tick_times",
    (PyCFunction)PyPf_get_nav_tick_times, METH_NOARGS,
    "Get a list of (wall_us, serial_us, total_us, nwork, budget_us) tuples for recent "
    "completed navigation ticks, oldest-first, for the perf window's nav-tick stats."},

    {"get_frame_times",
    (PyCFunction)PyPf_get_frame_times, METH_NOARGS,
    "Get a list of (seq, ms) tuples for recently completed frames, oldest-first. "
    "The engine-side ring is deep enough for a 1Hz sampler task to never miss frames."},

    {"prev_frame_mem_accounting",
    (PyCFunction)PyPf_prev_frame_mem_accounting, METH_NOARGS,
    "Get a dictionary of per-system memory accounting for the previous frame. "
    "Keys are system name strings; values are dicts of {bytes, count, subsystems} "
    "where 'subsystems' maps subsystem id (int) to {bytes, count}."},

    {"prev_frame_gpu_mem_accounting",
    (PyCFunction)PyPf_prev_frame_gpu_mem_accounting, METH_NOARGS,
    "Get a dictionary of the engine's tracked GPU memory for the previous "
    "frame, keyed by render-file system; each value is {bytes, count, "
    "subsystems} where subsystems splits into texture/renderbuffer/bufferobject."},

    {"mem_audit",
    (PyCFunction)PyPf_mem_audit, METH_NOARGS,
    "Walks mimalloc's heap to count live bytes in tagged blocks per system. "
    "Returns {sys_name: alive_bytes}. Compare with prev_frame_mem_accounting() "
    "to find bare-free leaks: counter > alive means tagged pointers freed "
    "without going through Mem_Free."},

    {"prev_frame_allocd_bytes", 
    (PyCFunction)PyPf_prev_frame_allocd_bytes, METH_NOARGS,
    "Get the number of bytes that were allocated with malloc during the previous frame."},

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

    {"get_files_in_dir",
    (PyCFunction)PyPf_get_files_in_dir, METH_VARARGS,
    "Get a list of non-directory files in the specified directory (relative to the working "
    "directory)."},

    {"get_time_str",
    (PyCFunction)PyPf_get_time_str, METH_VARARGS,
    "Get a string of the current system date and time."},

    {"ensure_directory",
    (PyCFunction)PyPf_ensure_directory, METH_VARARGS,
    "Create a directory relative to the working directory, if it does not exist already."},

    {"delete_user_file",
    (PyCFunction)PyPf_delete_user_file, METH_VARARGS,
    "Delete a specific user-created file. The first argument is a path to a directory, which "
    "may not contain sub-directories, may not be one of the installation directories, and may "
    "not be empty. The second argument is the filename."},

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

    {"get_text_outline", 
    (PyCFunction)PyPf_get_text_outline, METH_NOARGS,
    "Get the outline drawn around label text, as an ((R, G, B, A), width) tuple."},

    {"set_text_outline", 
    (PyCFunction)PyPf_set_text_outline, METH_VARARGS,
    "Set the outline drawn around label text from an (R, G, B, A) color and a width. "
    "A width of 0 turns the outline off. The outline is as thick as the font baked it."},

    {"set_skybox", 
    (PyCFunction)PyPf_set_skybox, METH_VARARGS,
    "Set the image set to be used for the skybox."},

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

    {"get_idle_units", 
    (PyCFunction)PyPf_get_idle_units, METH_NOARGS,
    "Returns a list of currently idle units."},

    {"set_unit_selection", 
    (PyCFunction)PyPf_set_unit_selection, METH_VARARGS,
    "Overwrites the list of objects currently selected by the player with the specified entity list."},

    {"get_hovered_unit", 
    (PyCFunction)PyPf_get_hovered_unit, METH_NOARGS,
    "Get the closest unit under the mouse cursor, or None."},

    {"gather_blocked",
    (PyCFunction)PyPf_gather_blocked, METH_NOARGS,
    "Returns True if a right click now would be on a node the selected workers could gather, were it not "
    "out of season."},

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

    {"hide_unit_icons", 
    (PyCFunction)PyPf_hide_unit_icons, METH_NOARGS,
    "Disable rendering of icons on top of entities."},

    {"show_unit_icons", 
    (PyCFunction)PyPf_show_unit_icons, METH_NOARGS,
    "Enable rendering of icons on top of entities."},

    {"set_right_click_attack", 
    (PyCFunction)PyPf_set_right_click_attack, METH_VARARGS,
    "Set whether every right click issues an attack-move order rather than a move order. "
    "Saved with the session."},

    {"get_right_click_attack", 
    (PyCFunction)PyPf_get_right_click_attack, METH_NOARGS,
    "Returns whether every right click issues an attack-move order rather than a move order."},

    {"get_resource_list",
    (PyCFunction)PyPf_get_resource_list, METH_NOARGS,
    "Returns a list of the names of all the resources that are present (or have ever been present) in the current session."},

    {"get_resource_stored",
    (PyCFunction)PyPf_get_resource_stored, METH_VARARGS,
    "Returns the total amount of a particular resource between all player-controlled storage sites, "
    "or between the storage sites of the faction with the optional ID."},

    {"get_resource_capacity",
    (PyCFunction)PyPf_get_resource_capacity, METH_VARARGS,
    "Returns the total capacity for storing a particular resource between all player-controlled storage sites."},

    {"get_resource_produced",
    (PyCFunction)PyPf_get_resource_produced, METH_VARARGS,
    "Returns the total amount of a particular resource the player-controlled factions have gathered or made "
    "since the session began."},

    {"get_resource_consumed",
    (PyCFunction)PyPf_get_resource_consumed, METH_VARARGS,
    "Returns the total amount of a particular resource the player-controlled factions have used up "
    "since the session began."},

    {"set_resource_icon",
    (PyCFunction)PyPf_set_resource_icon, METH_VARARGS,
    "Specify an image path (string) to be used as the icon for a resource with the specified resource name."},

    {"get_resource_icon",
    (PyCFunction)PyPf_get_resource_icon, METH_VARARGS,
    "Returns an image path (stirng) that is currently set to be used as the icon for the specified resource name."},

    {"get_render_settings",
    (PyCFunction)PyPf_get_render_settings, METH_VARARGS,
    "Returns a dictionary with the current render settings."},

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

    {"get_population",
    (PyCFunction)PyPf_get_population, METH_VARARGS,
    "Returns the current population count for the specified faction ID."},

    {"get_population_limit",
    (PyCFunction)PyPf_get_population_limit, METH_VARARGS,
    "Returns the current population limit for the specified faction ID."},

    {"get_allied_population",
    (PyCFunction)PyPf_get_allied_population, METH_VARARGS,
    "Returns the total population of factions at peace with the specified faction ID, "
    "excluding the faction itself."},

    {"get_enemy_population",
    (PyCFunction)PyPf_get_enemy_population, METH_VARARGS,
    "Returns the total population of factions at war with the specified faction ID."},

    {"get_player_population",
    (PyCFunction)PyPf_get_player_population, METH_NOARGS,
    "Returns the total population of all player-controllable factions."},

    {"get_player_population_limit",
    (PyCFunction)PyPf_get_player_population_limit, METH_NOARGS,
    "Returns the total population limit of all player-controllable factions."},

    {"get_tile", 
    (PyCFunction)PyPf_get_tile, METH_VARARGS,
    "Get the pf.Tile object describing the tile at the specified coordinates."},

    {"update_tile",
    (PyCFunction)PyPf_update_tile, METH_VARARGS,
    "Update the map tile at the specified coordinates to the new value."},

    {"tiles_under_obj",
    (PyCFunction)PyPf_tiles_under_obj, METH_VARARGS,
    "Returns a list of ((chunk_r, chunk_c), (tile_r, tile_c)) coordinates for the tiles "
    "covered by the footprint of the entity with the supplied UID."},

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

    {"map_pos_over_water",
    (PyCFunction)PyPf_map_pos_over_water, METH_VARARGS,
    "Returns true if the XZ position is over water."},

    {"map_pos_over_land",
    (PyCFunction)PyPf_map_pos_over_land, METH_VARARGS,
    "Returns true if the XZ position is over land."},

    {"map_height_at_point",
    (PyCFunction)PyPf_map_height_at_point, METH_VARARGS,
    "Returns the Y-dimension map height at the specified XZ coordinate. Returns None if the "
    "specified coordinate is outside the map bounds."},

    {"map_nearest_pathable",
    (PyCFunction)PyPf_map_nearest_pathable, METH_VARARGS | METH_KEYWORDS,
    "Returns the closest XZ map position that is pathable and not currently blocked."},

    {"map_locations_reachable",
    (PyCFunction)PyPf_map_locations_reachable, METH_VARARGS | METH_KEYWORDS,
    "Returns whether two XZ map positions lie on the same island of the layer a unit of the "
    "optional 'radius' walks, ignoring blockers."},

    {"map_nearest_pathable_water",
    (PyCFunction)PyPf_map_nearest_pathable_water, METH_VARARGS | METH_KEYWORDS,
    "Returns the closest XZ map position that is on water, pathable and not currently blocked."},

    {"map_nearest_pathable_air",
    (PyCFunction)PyPf_map_nearest_pathable_air, METH_VARARGS | METH_KEYWORDS,
    "Returns the closest XZ map position that is not currently blocked by other air units."},

    {"map_pos_pathable",
    (PyCFunction)PyPf_map_pos_pathable, METH_VARARGS | METH_KEYWORDS,
    "Returns true if the XZ position is pathable terrain for a unit of the specified radius. "
    "Terrain cut out by static objects such as cliffs is not pathable. Other units standing "
    "at the position are not taken into account."},

    {"map_reachable_now",
    (PyCFunction)PyPf_map_reachable_now, METH_VARARGS | METH_KEYWORDS,
    "Returns whether a unit of the optional 'radius' at the XZ position can get beside the "
    "entity as the ground is now, past the buildings and standing units in the way."},

    {"obj_adjacent",
    (PyCFunction)PyPf_obj_adjacent, METH_VARARGS,
    "Returns whether the first entity stands beside the second, as the engine judges an arrival."},

    {"map_tile_info",
    (PyCFunction)PyPf_map_tile_info, METH_VARARGS,
    "Takes an (X, Z) position and an optional unit radius. Returns a (pathable, blockers, static blockers, "
    "in a slit, local island, global island) tuple for the tile under it on that unit's layer, or None off the map."},

    {"closest_adjacent_pos",
    (PyCFunction)PyPf_closest_adjacent_pos, METH_VARARGS,
    "Takes a unit and a building. Returns the (X, Z) spot the unit makes for to stand beside the building, "
    "as the navigation picks it, or None when there is none."},

    {"debug_desc",
    (PyCFunction)PyPf_debug_desc, METH_VARARGS,
    "Takes 'move', 'build', 'harvest' or 'garrison' and an entity. Returns a line on the entity's state in that "
    "part of the engine, for probes."},

    {"map_ground_open",
    (PyCFunction)PyPf_map_ground_open, METH_VARARGS | METH_KEYWORDS,
    "Returns true if the ground within 'radius' of the XZ position is pathable and no unit or "
    "building stands on it, on the layer a unit of 'unit_radius' walks. With 'off_road' set, "
    "it must also be clear of roads."},

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

    {"set_evict_on_left_click",
    (PyCFunction)PyPf_set_evict_on_left_click, METH_NOARGS,
    "Set the cursor to target mode. The next left click will issue an 'evict' command to the garrisonable "
    "units in the selection, to get as close as possible to the target and evict all units.."},

    {"set_position_rally_point_on_left_click",
    (PyCFunction)PyPf_set_position_rally_point_on_left_click, METH_NOARGS,
    "Set the cursor to target mode. The next left click will update the rally points of all "
    "currently selected buildings."},

    {"set_click_move_enabled",
    (PyCFunction)PyPf_set_click_move_enabled, METH_VARARGS,
    "Enable or disable issuing move orders by right-clicking."},

    {"draw_text",
    (PyCFunction)PyPf_draw_text, METH_VARARGS,
    "Draw a text label with the specified bounds (X, Y, W, H) )and with the specified color (R, G, B, A). "
    "The label lasts for one frame, meaning this should be called every tick to keep the label fixed."},

    {"draw_text_outlined",
    (PyCFunction)PyPf_draw_text_outlined, METH_VARARGS,
    "Draw a text label with the specified bounds (X, Y, W, H) and color (R, G, B, A), outlined in the "
    "specified color (R, G, B, A). The label lasts for one frame."},

    {"dump_framebuffer",
    (PyCFunction)PyPf_dump_framebuffer, METH_VARARGS,
    "Dump the current color framebuffer to a PPM (P6) file at the specified path. For debugging/automation."},

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

    {"set_garrison_ui_font_color",
    (PyCFunction)PyPf_set_garrison_ui_font_color, METH_VARARGS,
    "Set the garrison UI text color to the specified color (R, G, B, A)."},

    {"set_garrison_ui_icon",
    (PyCFunction)PyPf_set_garrison_ui_icon, METH_VARARGS,
    "Set the garrisonable UI icon to the specified image (specified as an image path string)."},

    {"set_garrison_ui_style",
    (PyCFunction)PyPf_set_garrison_ui_style, METH_VARARGS,
    "Set the garrisonable UI background to the specified color (R, G, B, A) or image (string)."},

    {"garrison_show_ui",
    (PyCFunction)PyPf_garrison_show_ui, METH_VARARGS,
    "Set a flag controlling whether or not the UI labels are rendered over the garrisonable entities."},

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

    {"set_rts_camera_zoom_enabled",
    (PyCFunction)PyPf_set_rts_camera_zoom_enabled, METH_VARARGS,
    "Enable or disable the RTS camera's mousewheel zoom. Disable it when the wheel is "
    "temporarily repurposed and re-enable it afterwards."},

    {"get_rts_camera_zoom_enabled",
    (PyCFunction)PyPf_get_rts_camera_zoom_enabled, METH_NOARGS,
    "Returns whether the RTS camera's mousewheel zoom is currently enabled."},

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

    {"set_damage_table",
    (PyCFunction)PyPf_set_damage_table, METH_VARARGS,
    "Set the damage type vs. armour type interaction table. Takes a sequence of at most "
    "pf.DAMAGE_TYPE_MAX equal-length sequences of at most pf.ARMOUR_TYPE_MAX floats, indexed "
    "[damage_type][armour_type]. Each entry scales the damage an attack of that damage type deals "
    "to a target of that armour type. Type IDs the table does not cover are left at 1.0, as are "
    "all entries by default."},

    {"get_damage_table",
    (PyCFunction)PyPf_get_damage_table, METH_NOARGS,
    "Returns the current damage type vs. armour type interaction table as a tuple of "
    "pf.DAMAGE_TYPE_MAX tuples of pf.ARMOUR_TYPE_MAX floats, regardless of the size of the table "
    "that was set."},

    {"formation_arrange",
    (PyCFunction)PyPf_formation_arrange, METH_VARARGS,
    "Arrange the specified units into a formation of the specified type."},

    {"move_in_formation",
    (PyCFunction)PyPf_move_in_formation, METH_VARARGS,
    "Order the specified units to arrange themselves at the target location and orientation."},

    {"attack_in_formation",
    (PyCFunction)PyPf_attack_in_formation, METH_VARARGS,
    "Order the specified units to arrange themselves at the target location and orientation, "
    "attacking any enemies along the way."},

    {"formation_preferred_for_set",
    (PyCFunction)PyPf_formation_preferred_for_set, METH_VARARGS,
    "Returns the preferred formation type for the specified set of entities."},

    {"group_lock",
    (PyCFunction)PyPf_group_lock, METH_VARARGS,
    "Lock the specified entities into a group. Selecting any unit of the group selects the "
    "entire group. Returns the ID of the new group, or 0 on failure."},

    {"group_unlock",
    (PyCFunction)PyPf_group_unlock, METH_VARARGS,
    "Disband the group with the specified ID, making its members individually selectable "
    "once again."},

    {"group_for_set",
    (PyCFunction)PyPf_group_for_set, METH_VARARGS,
    "Returns the ID of the locked group that all the entities in the specified list belong "
    "to, or 0 if they do not all belong to the same group."},

    {"group_members",
    (PyCFunction)PyPf_group_members, METH_VARARGS,
    "Returns a list of the entities locked to the group with the specified ID."},

    {"cluster_members",
    (PyCFunction)PyPf_cluster_members, METH_VARARGS,
    "Returns a list of the buildings in the cluster with the specified ID, oldest first."},

    {"cluster_anchor",
    (PyCFunction)PyPf_cluster_anchor, METH_VARARGS,
    "Returns the building that anchors the cluster with the specified ID, or None."},

    {"cluster_colour",
    (PyCFunction)PyPf_cluster_colour, METH_VARARGS,
    "Returns the (R, G, B) colour the cluster with the specified ID is told apart by, or None."},

    {"set_cluster_icon",
    (PyCFunction)PyPf_set_cluster_icon, METH_VARARGS,
    "Sets the image shown over the buildings of a cluster that has more than one."},

    {"provision_counts",
    (PyCFunction)PyPf_provision_counts, METH_VARARGS,
    "Takes a faction ID and returns a tuple of how many of its units are fed, hungry and starving."},

    {"resource_balance",
    (PyCFunction)PyPf_resource_balance, METH_VARARGS,
    "Takes a resource name and a faction ID. Returns a tuple of how much of the resource the faction's "
    "storage sites can give away, and how much they ask for and are not yet getting."},

    {"clusters",
    (PyCFunction)PyPf_clusters, METH_VARARGS | METH_KEYWORDS,
    "Returns a sorted list of the IDs of every cluster, or of those of the 'type' and the "
    "'faction' given as keywords."},

    {"good_weight",
    (PyCFunction)PyPf_good_weight, METH_VARARGS,
    "Takes a resource name and a faction ID. Returns how much the faction's automated workers "
    "favour gathering it: 1.0 by default, 0.0 for not at all."},

    {"set_good_weight",
    (PyCFunction)PyPf_set_good_weight, METH_VARARGS,
    "Takes a resource name, a faction ID and a weight: how much the faction's automated workers "
    "favour gathering it. Above 1.0 it also lets that many times more of them gather it."},

    {"stock_target",
    (PyCFunction)PyPf_stock_target, METH_VARARGS,
    "Takes a resource name and a faction ID. Returns the spare stock of it the faction's "
    "gatherers keep up beyond what is asked for."},

    {"set_stock_target",
    (PyCFunction)PyPf_set_stock_target, METH_VARARGS,
    "Takes a resource name, a faction ID and the spare stock of it the faction's gatherers "
    "keep up beyond what is asked for."},

    {"gather_rate",
    (PyCFunction)PyPf_gather_rate, METH_VARARGS,
    "Takes a resource name and a faction ID. Returns how much of it a minute the faction's "
    "gatherers are to bring in beyond filling its stock: 0.0 for nothing."},

    {"set_gather_rate",
    (PyCFunction)PyPf_set_gather_rate, METH_VARARGS,
    "Takes a resource name, a faction ID and how much of it a minute the faction's gatherers "
    "are to bring in beyond filling its stock. As many gather it as bring that in."},

    {"gatherer_rate",
    (PyCFunction)PyPf_gatherer_rate, METH_VARARGS,
    "Takes a resource name and a faction ID. Returns how much of it one of the faction's "
    "gatherers brings in a minute, as measured over the last few minutes."},

    {"district_weight",
    (PyCFunction)PyPf_district_weight, METH_VARARGS,
    "Takes a district ID and a resource name. Returns the district's own weight for gathering "
    "it, or None when the district follows its faction's."},

    {"set_district_weight",
    (PyCFunction)PyPf_set_district_weight, METH_VARARGS,
    "Takes a district ID, a resource name and the district's own weight for gathering it, "
    "or None to follow its faction's."},

    {"workers_carry_out",
    (PyCFunction)PyPf_workers_carry_out, METH_VARARGS,
    "Takes a district ID. Returns whether workers carry goods from its stores to other districts."},

    {"open_requests",
    (PyCFunction)PyPf_open_requests, METH_VARARGS,
    "Takes a faction ID. Returns how many requests of its stores wait for a load, and how many "
    "carriers it has."},

    {"set_workers_carry_out",
    (PyCFunction)PyPf_set_workers_carry_out, METH_VARARGS,
    "Takes a district ID and whether workers carry goods from its stores to other districts. "
    "Carts always do."},

    {"crew_size",
    (PyCFunction)PyPf_crew_size, METH_VARARGS,
    "Takes a faction ID. Returns how many of its workers of no district build at once."},

    {"set_crew_size",
    (PyCFunction)PyPf_set_crew_size, METH_VARARGS,
    "Takes a faction ID and how many of its workers of no district build at once."},

    {"set_automation_option",
    (PyCFunction)PyPf_set_automation_option, METH_VARARGS,
    "Takes a faction ID, the name of a setting of how its automation goes about the work (carry_share, "
    "build_share, reach, breadth, man_workshops, pull_to_build, roam_far) and its value, kept within bounds."},

    {"set_overlay_span",
    (PyCFunction)PyPf_set_overlay_span, METH_VARARGS,
    "Takes the left and right screen columns, in 1920 wide virtual coordinates, between which marks over "
    "the world show; (0, 0) shows them everywhere."},

    {"set_good_class",
    (PyCFunction)PyPf_set_good_class, METH_VARARGS,
    "Takes a class name and a list of up to 8 resource names the automation treats as one good, "
    "scored together and gathered at the nearest node of any of them."},

    {"get_automation_option",
    (PyCFunction)PyPf_get_automation_option, METH_VARARGS,
    "Takes a faction ID and the name of a setting of its automation. Returns its value."},

    {"crew_busy",
    (PyCFunction)PyPf_crew_busy, METH_VARARGS,
    "Takes a faction ID. Returns how many of its workers of no district are building or bound "
    "for a site."},

    {"awaits_builders",
    (PyCFunction)PyPf_awaits_builders, METH_VARARGS,
    "Takes a site. Returns a tuple of whether it waits for builders because its faction's crew "
    "is all busy, how many of the crew are, and the crew's size."},

    {"set_workshop_posts",
    (PyCFunction)PyPf_set_workshop_posts, METH_VARARGS,
    "Takes a building and the number of posts it has for workers to fill (at most 4), or 0 "
    "for none."},

    {"set_post_demand",
    (PyCFunction)PyPf_set_post_demand, METH_VARARGS,
    "Takes a building with posts and what they are wanted for: pf.POSTS_FILL to have them filled, "
    "pf.POSTS_KEEP to keep its holders but send no more, or pf.POSTS_IDLE to let them go."},

    {"set_locked_posts",
    (PyCFunction)PyPf_set_locked_posts, METH_VARARGS,
    "Takes a building with posts and how many of them are to be kept empty."},

    {"workshop_posts",
    (PyCFunction)PyPf_workshop_posts, METH_VARARGS,
    "Takes a building. Returns a tuple of its posts, those kept empty, its holders inside and "
    "those on their way, or None when it has no posts."},

    {"post_workshop",
    (PyCFunction)PyPf_post_workshop, METH_VARARGS,
    "Takes a unit. Returns the building whose post it holds or is on its way to, or None."},

    {"man_post",
    (PyCFunction)PyPf_man_post, METH_VARARGS,
    "Takes a building with posts and optionally a worker, and sends the worker to a post of it as the "
    "player's order, opening one kept empty if need be. With no worker, the one the automation would "
    "pick for its district is sent. Returns the worker sent, or None."},

    {"evict_post",
    (PyCFunction)PyPf_evict_post, METH_VARARGS,
    "Takes a building with posts and lets one of its holders go, keeping the post it leaves empty. "
    "Returns the worker let go, or None."},

    {"leave_post",
    (PyCFunction)PyPf_leave_post, METH_VARARGS,
    "Takes a unit and lets it go from its post, leaving the post open. Returns whether it held one."},

    {"workshop_eating",
    (PyCFunction)PyPf_workshop_eating, METH_VARARGS,
    "Takes a building with posts. Returns how many of its holders are out at a meal, their post kept."},

    {"end_duty",
    (PyCFunction)PyPf_end_duty, METH_VARARGS,
    "Takes a unit and ends the duty it is at, leaving it to find or be given new work."},

    {"set_duty_by",
    (PyCFunction)PyPf_set_duty_by, METH_VARARGS,
    "Takes a unit and one of 'player', 'city' or 'automation': who gives its duty, the one it holds "
    "or else the next it takes on."},

    {"provision_demand",
    (PyCFunction)PyPf_provision_demand, METH_VARARGS,
    "Takes a faction ID and returns a dictionary of the rations per minute its units eat, keyed by the "
    "ID of the district they are in."},

    {"cluster_preview_join",
    (PyCFunction)PyPf_cluster_preview_join, METH_VARARGS,
    "Takes a cluster type, a faction ID, an (X, Z) position and a link radius. Returns the ID of the "
    "cluster a building placed there would join, or 0 if it would start one."},

    {"set_cluster_preview",
    (PyCFunction)PyPf_set_cluster_preview, METH_VARARGS,
    "Takes a building being placed, or None, and a cluster ID. Shows the cluster's chain and colour "
    "over the building, large, while it would link up with that cluster."},

    {"district_at",
    (PyCFunction)PyPf_district_at, METH_VARARGS | METH_KEYWORDS,
    "Takes an (X, Z) position and a faction ID, and an optional 'water' flag. Returns a tuple of the "
    "ID of the hub cluster whose district the position is in (0 for none), the path cost to its hub, "
    "and whether the position is within the hub's own territory."},

    {"show_districts",
    (PyCFunction)PyPf_show_districts, METH_VARARGS,
    "Turns the district lens, which tints each district of the player's factions, on or off."},

    {"districts_shown",
    (PyCFunction)PyPf_districts_shown, METH_NOARGS,
    "Returns whether the district lens is on."},

    {"show_district",
    (PyCFunction)PyPf_show_district, METH_VARARGS,
    "Takes a district ID, or 0 for none. Tints that district alone, faintly, with its own territory "
    "inside a dotted border and all of it inside a solid one. The district lens is drawn over it."},

    {"district_shown",
    (PyCFunction)PyPf_district_shown, METH_NOARGS,
    "Returns the ID of the district shown alone, or 0."},

    {"district_gathered",
    (PyCFunction)PyPf_district_gathered, METH_VARARGS,
    "Returns a dictionary of how much of each good was gathered in the district with the specified "
    "ID since the session began."},

    {"set_district_preview",
    (PyCFunction)PyPf_set_district_preview, METH_VARARGS,
    "Takes a faction ID and an (X, Z) position, or None to clear it. Shows the cells a hub placed "
    "there would take over, and returns how many."},

    {"set_group_ui_style",
    (PyCFunction)PyPf_set_group_ui_style, METH_VARARGS,
    "Set the background style of the group UI windows to an (R, G, B, A) color, an image, or "
    "a (path, left, right, top, bottom) nine patch."},

    {"set_group_ui_font_color",
    (PyCFunction)PyPf_set_group_ui_font_color, METH_VARARGS,
    "Set the font color of the group UI windows to an (R, G, B, A) color."},

    {"set_group_ui_bonus_color",
    (PyCFunction)PyPf_set_group_ui_bonus_color, METH_VARARGS,
    "Set the color of the group bonus labels on the group banner to an (R, G, B, A) color."},

    {"set_group_highlight",
    (PyCFunction)PyPf_set_group_highlight, METH_VARARGS | METH_KEYWORDS,
    "Outline every member of the specified group in an (R, G, B) color, with an optional 'width'. "
    "The outlines follow the live membership. Only one group is highlighted at a time."},

    {"clear_group_highlight",
    (PyCFunction)PyPf_clear_group_highlight, METH_NOARGS,
    "Stop outlining a group's members."},

    {"group_for_ent",
    (PyCFunction)PyPf_group_for_ent, METH_VARARGS,
    "Returns the ID of the group the specified entity is locked into, or 0 if it is not in one."},

    {"group_bonuses",
    (PyCFunction)PyPf_group_bonuses, METH_VARARGS,
    "Returns the bonuses granted to the members of the group with the specified ID, as a list of "
    "(tag, icon, kind, amount, percent) tuples."},

    {"show_range_indicator",
    (PyCFunction)PyPf_show_range_indicator, METH_VARARGS | METH_KEYWORDS,
    "Draw a ring of the specified radius around an entity, for showing how far one of its "
    "abilities reaches. Takes the pf.Entity and the radius, with an optional (R, G, B) 'color' "
    "and (float) 'width'. Only one indicator is shown at a time."},

    {"hide_range_indicator",
    (PyCFunction)PyPf_hide_range_indicator, METH_NOARGS,
    "Stop drawing the range indicator ring."},

    {"show_console",
    (PyCFunction)PyPf_show_console, METH_NOARGS,
    "Brings up the interactive Python console."},

    {"get_version_string",
    (PyCFunction)PyPf_get_version_string, METH_NOARGS,
    "Returns the version of Permafrost Engine."},

    {"spawn_sprite_static",
    (PyCFunction)PyPf_spawn_sprite_static, METH_VARARGS,
    "Spawns a sprite for a fixed duration. The first parameter is a sprite sheet descriptor "
    "(4-tuple of a filename, the row count, the column count, and the frame count). "
    "The additional parameters are ws_pos (worldspace position), ws_size (worldspace size), "
    "and duration_ms (milliseconds)."},

    {"spawn_sprite_animated",
    (PyCFunction)PyPf_spawn_sprite_animated, METH_VARARGS,
    "Spawns a sprite with a specific animation. The first parameter is a sprite sheet descriptor "
    "(4-tuple of a filename, the row count, the column count, and the frame count). "
    "The additional parameters are ws_pos (worldspace position), ws_size (worldspace size), "
    "the fps (integer), and the repeat count (integer)."},

    {NULL}  /* Sentinel */
};

static const char        *s_progname = NULL;
static struct py_err_ctx  s_err_ctx = {false,};


/* An error left set by an earlier call would surface in the next Python code
 * run, wherever it happens to test for one (e.g. a %d of -1): report where it
 * was found and clear it. */
static void clear_stale_error(const char *where, PyObject *what)
{
    if(!PyErr_Occurred())
        return;
    PyObject *type, *value, *tb;
    PyErr_Fetch(&type, &value, &tb);
    PyObject *vr = value ? PyObject_Repr(value) : NULL;
    PyObject *wr = what ? PyObject_Repr(what) : NULL;
    fprintf(stderr, "[stale-exc] %s: %s %s left set before %s\n", where,
        type ? ((PyTypeObject*)type)->tp_name : "?",
        vr && PyString_Check(vr) ? PyString_AS_STRING(vr) : "?",
        wr && PyString_Check(wr) ? PyString_AS_STRING(wr) : "?");
    Py_XDECREF(vr);
    Py_XDECREF(wr);
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
    PyErr_Clear();
}

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

    Py_ssize_t arena_b = 0, arena_c = 0, raw_b = 0, raw_c = 0;
    _PyObject_MemStats(&arena_b, &arena_c, &raw_b, &raw_c);
    Mem_SetPythonStats((int64_t)arena_b, (int64_t)arena_c, (int64_t)raw_b, (int64_t)raw_c);
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

    G_SetAmbientLightColor(color);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_emit_light_color(PyObject *self, PyObject *args)
{
    vec3_t color;
    if(!PyArg_ParseTuple(args, "(fff)", &color.x, &color.y, &color.z)) {
        PyErr_SetString(PyExc_TypeError, "Arugment must be a tuple of 3 floats.");
        return NULL;
    }

    G_SetEmitLightColor(color);
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
        char errbuff[256];
        pf_snprintf(errbuff, sizeof(errbuff), "Unable to load scene from the sepcified file%s",
            PyErr_Occurred() ? ": " : ".");
        if(PyErr_Occurred()) {
            PyObject *type, *value, *traceback;
            PyErr_Fetch(&type, &value, &traceback);
            PyObject *str = PyObject_Repr(value);
            pf_strlcat(errbuff, PyString_AS_STRING(str), sizeof(errbuff));
            Py_XDECREF(str);
            Py_XDECREF(type);
            Py_XDECREF(value);
            Py_XDECREF(traceback);
        }
        PyErr_SetString(PyExc_RuntimeError, errbuff);
        goto fail_load;
    }

    if(update_navgrid) {
        G_BakeNavDataForScene();
    }

    PyObject *ret = PyTuple_New(3);
    if(!ret)
        goto fail_load;

    PyTuple_SET_ITEM(ret, 0, S_Entity_GetLoaded());
    PyTuple_SET_ITEM(ret, 1, S_Region_GetLoaded());
    PyTuple_SET_ITEM(ret, 2, S_Camera_GetLoaded());
    return ret;

fail_load:
    ents = S_Entity_GetLoaded();
    regs = S_Region_GetLoaded();
    Py_DECREF(ents);
    Py_DECREF(regs);
    return NULL;
}

static PyObject *PyPf_open_url(PyObject *self, PyObject *args)
{
    const char *url;
    if(!PyArg_ParseTuple(args, "s", &url)) {
        PyErr_SetString(PyExc_TypeError, "Arugment must be a string (URL)");
        return NULL;
    }

    int result = SDL_OpenURL(url);
    if(result == 0) {
        Py_RETURN_NONE;
    }else{
        char errbuff[256];
        pf_snprintf(errbuff, sizeof(errbuff), SDL_GetError());
        PyErr_SetString(PyExc_RuntimeError, errbuff);
        return NULL;
    }
}

static PyObject *PyPf_preload_pfobj(PyObject *self, PyObject *args)
{
    const char *dir, *filename;
    if(!PyArg_ParseTuple(args, "ss", &dir, &filename)) {
        PyErr_SetString(PyExc_TypeError, "Arugments must be 2 strings (PFObj directory and filename).");
        return NULL;
    }
    AL_PreloadPFObj(dir, filename);
    Sched_TryYield();
    Py_RETURN_NONE;
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

static PyObject *PyPf_map_add_splat(PyObject *self, PyObject *args)
{
    int base_mat_idx, accent_mat_idx;

    if(!PyArg_ParseTuple(args, "ii", &base_mat_idx, &accent_mat_idx)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of 2 integers (material indices).");
        return NULL;
    }

    G_MapAddSplat(base_mat_idx, accent_mat_idx);
    Py_RETURN_NONE;
}

static PyObject *PyPf_map_remove_splat(PyObject *self, PyObject *args)
{
    int base_mat_idx, accent_mat_idx;

    if(!PyArg_ParseTuple(args, "ii", &base_mat_idx, &accent_mat_idx)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of 2 integers (material indices).");
        return NULL;
    }

    G_MapRemoveSplat(base_mat_idx, accent_mat_idx);
    Py_RETURN_NONE;
}

static PyObject *PyPf_add_decal(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"path", "pos", "dims", "rotation", "alpha", "hide_foliage", NULL};
    const char *path;
    PyObject *hide_foliage = NULL;
    struct decal_desc desc = (struct decal_desc){
        .rotation = 0.0f,
        .alpha    = 1.0f,
    };

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "s(ff)(ff)|ffO", kwlist, &path, 
        &desc.pos.x, &desc.pos.z, &desc.dims.x, &desc.dims.z, &desc.rotation, &desc.alpha,
        &hide_foliage)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string, two tuples of two floats "
            "and optional 'rotation' and 'alpha' floats and 'hide_foliage' boolean.");
        return NULL;
    }
    desc.hide_foliage = hide_foliage && PyObject_IsTrue(hide_foliage);

    if(strlen(path) >= sizeof(desc.path)) {
        PyErr_SetString(PyExc_ValueError, "The path is too long.");
        return NULL;
    }
    pf_strlcpy(desc.path, path, sizeof(desc.path));

    uint32_t id = M_Decal_Add(&desc);
    if(id == NULL_DECAL_ID) {
        PyErr_Format(PyExc_RuntimeError, "Could not add the decal. A map must be loaded, '%s' "
            "must be a readable image, the dimensions positive and the alpha within [0, 1].", path);
        return NULL;
    }
    return Py_BuildValue("I", id);
}

static PyObject *PyPf_remove_decal(PyObject *self, PyObject *args)
{
    unsigned id;
    if(!PyArg_ParseTuple(args, "I", &id)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer (decal ID).");
        return NULL;
    }

    if(!M_Decal_Remove(id)) {
        PyErr_Format(PyExc_ValueError, "No decal with ID %u.", id);
        return NULL;
    }
    Py_RETURN_NONE;
}

static bool parse_vec2(PyObject *obj, vec2_t *out)
{
    return PyArg_ParseTuple(obj, "ff", &out->x, &out->z);
}

static PyObject *PyPf_update_decal(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"id", "path", "pos", "dims", "rotation", "alpha", "hide_foliage", NULL};
    unsigned id;
    const char *path = NULL;
    PyObject *pos = NULL, *dims = NULL;
    PyObject *rotation = NULL, *alpha = NULL, *hide_foliage = NULL;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "I|sOOOOO", kwlist, &id, 
        &path, &pos, &dims, &rotation, &alpha, &hide_foliage)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an integer (decal ID) and any of the "
            "'path', 'pos', 'dims', 'rotation', 'alpha' and 'hide_foliage' keyword arguments.");
        return NULL;
    }

    struct decal_desc desc;
    if(!M_Decal_Get(id, &desc)) {
        PyErr_Format(PyExc_ValueError, "No decal with ID %u.", id);
        return NULL;
    }

    if(path) {
        if(strlen(path) >= sizeof(desc.path)) {
            PyErr_SetString(PyExc_ValueError, "The path is too long.");
            return NULL;
        }
        pf_strlcpy(desc.path, path, sizeof(desc.path));
    }
    if(pos && !parse_vec2(pos, &desc.pos)) {
        PyErr_SetString(PyExc_TypeError, "'pos' must be a tuple of two floats.");
        return NULL;
    }
    if(dims && !parse_vec2(dims, &desc.dims)) {
        PyErr_SetString(PyExc_TypeError, "'dims' must be a tuple of two floats.");
        return NULL;
    }
    if(rotation) {
        desc.rotation = PyFloat_AsDouble(rotation);
        if(PyErr_Occurred())
            return NULL;
    }
    if(alpha) {
        desc.alpha = PyFloat_AsDouble(alpha);
        if(PyErr_Occurred())
            return NULL;
    }
    if(hide_foliage) {
        desc.hide_foliage = PyObject_IsTrue(hide_foliage);
    }

    if(!M_Decal_Update(id, &desc)) {
        PyErr_Format(PyExc_RuntimeError, "Could not update the decal. '%s' must be a readable "
            "image, the dimensions positive and the alpha within [0, 1].", desc.path);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_decal(PyObject *self, PyObject *args)
{
    unsigned id;
    if(!PyArg_ParseTuple(args, "I", &id)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer (decal ID).");
        return NULL;
    }

    struct decal_desc desc;
    if(!M_Decal_Get(id, &desc)) {
        PyErr_Format(PyExc_ValueError, "No decal with ID %u.", id);
        return NULL;
    }

    return Py_BuildValue("{s:s, s:(ff), s:(ff), s:f, s:f, s:N}", 
        "path",         desc.path,
        "pos",          desc.pos.x, desc.pos.z,
        "dims",         desc.dims.x, desc.dims.z,
        "rotation",     desc.rotation,
        "alpha",        desc.alpha,
        "hide_foliage", PyBool_FromLong(desc.hide_foliage));
}

static PyObject *decal_id_list(const uint32_t *ids, size_t count)
{
    PyObject *ret = PyList_New(count);
    if(!ret)
        return NULL;

    for(size_t i = 0; i < count; i++) {
        PyObject *id = Py_BuildValue("I", ids[i]);
        if(!id) {
            Py_DECREF(ret);
            return NULL;
        }
        PyList_SET_ITEM(ret, i, id);
    }
    return ret;
}

static PyObject *PyPf_decals_at(PyObject *self, PyObject *args)
{
    vec2_t pos;
    if(!PyArg_ParseTuple(args, "(ff)", &pos.x, &pos.z)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of two floats.");
        return NULL;
    }

    size_t max = M_Decal_Count();
    uint32_t *ids = PF_MALLOC(sizeof(uint32_t) * (max + 1));
    if(!ids)
        return PyErr_NoMemory();
    size_t count = M_Decal_At(pos, max, ids);

    PyObject *ret = decal_id_list(ids, count);
    PF_FREE(ids);
    return ret;
}

static PyObject *PyPf_get_all_decals(PyObject *self)
{
    size_t max = M_Decal_Count();
    uint32_t *ids = PF_MALLOC(sizeof(uint32_t) * (max + 1));
    if(!ids)
        return PyErr_NoMemory();
    size_t count = M_Decal_GetAll(max, ids);

    PyObject *ret = decal_id_list(ids, count);
    PF_FREE(ids);
    return ret;
}

static PyObject *PyPf_set_decal_order(PyObject *self, PyObject *args)
{
    unsigned id;
    int order;
    if(!PyArg_ParseTuple(args, "Ii", &id, &order)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two integers (decal ID and index).");
        return NULL;
    }

    if(!M_Decal_SetOrder(id, order)) {
        PyErr_Format(PyExc_ValueError, "No decal with ID %u.", id);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_decal_highlight(PyObject *self, PyObject *args)
{
    unsigned id;
    PyObject *on;
    if(!PyArg_ParseTuple(args, "IO", &id, &on)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an integer (decal ID) and a boolean.");
        return NULL;
    }

    if(!M_Decal_SetHighlight(id, PyObject_IsTrue(on))) {
        PyErr_Format(PyExc_ValueError, "No decal with ID %u.", id);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_clear_decals(PyObject *self)
{
    M_Decal_Clear();
    Py_RETURN_NONE;
}

static PyObject *PyPf_decal_image_size(PyObject *self, PyObject *args)
{
    const char *path;
    if(!PyArg_ParseTuple(args, "s", &path)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string (image path).");
        return NULL;
    }

    int w, h;
    if(!M_Decal_ImageSize(path, &w, &h)) {
        PyErr_Format(PyExc_RuntimeError, "Could not read the image '%s'.", path);
        return NULL;
    }
    return Py_BuildValue("(ii)", w, h);
}

static PyObject *PyPf_decals_in_screen_rect(PyObject *self, PyObject *args)
{
    vec2_t a, b;
    if(!PyArg_ParseTuple(args, "(ff)(ff)", &a.x, &a.z, &b.x, &b.z)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two tuples of two floats (screen corners).");
        return NULL;
    }

    size_t max = M_Decal_Count();
    uint32_t *ids = PF_MALLOC(sizeof(uint32_t) * (max + 1));
    if(!ids)
        return PyErr_NoMemory();
    size_t count = M_Decal_InScreenRect(G_GetActiveCamera(), a, b, max, ids);

    PyObject *ret = decal_id_list(ids, count);
    PF_FREE(ids);
    return ret;
}

static PyObject *PyPf_set_decal_drag_box(PyObject *self, PyObject *args)
{
    PyObject *start;
    if(!PyArg_ParseTuple(args, "O", &start)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an (x, y) tuple or None.");
        return NULL;
    }

    if(start == Py_None) {
        M_Decal_SetDragBox(NULL);
        Py_RETURN_NONE;
    }

    vec2_t pos;
    if(!PyArg_ParseTuple(start, "ff", &pos.x, &pos.z)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an (x, y) tuple or None.");
        return NULL;
    }
    M_Decal_SetDragBox(&pos);
    Py_RETURN_NONE;
}

static PyObject *road_desc_tuple(struct road_desc rd)
{
    return Py_BuildValue("(ii)", rd.r, rd.c);
}

static PyObject *PyPf_road_at(PyObject *self, PyObject *args)
{
    vec2_t pos;
    if(!PyArg_ParseTuple(args, "(ff)", &pos.x, &pos.z)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of two floats.");
        return NULL;
    }

    struct road_desc rd;
    if(!G_Roads_DescForPoint(pos, &rd))
        Py_RETURN_NONE;
    return road_desc_tuple(rd);
}

static PyObject *PyPf_road_for_tile(PyObject *self, PyObject *args)
{
    struct tile_desc td;
    if(!PyArg_ParseTuple(args, "((ii)(ii))", &td.chunk_r, &td.chunk_c, &td.tile_r, &td.tile_c)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of two tuples of two integers.");
        return NULL;
    }

    struct road_desc rd;
    if(!G_Roads_DescForTile(&td, &rd)) {
        PyErr_SetString(PyExc_ValueError, "The tile is off the map.");
        return NULL;
    }
    return road_desc_tuple(rd);
}

static PyObject *PyPf_road_bounds(PyObject *self, PyObject *args)
{
    struct road_desc rd;
    if(!PyArg_ParseTuple(args, "(ii)", &rd.r, &rd.c)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of two integers.");
        return NULL;
    }

    vec2_t min, max;
    if(!G_Roads_Bounds(rd, &min, &max)) {
        PyErr_SetString(PyExc_ValueError, "The road tile is off the map.");
        return NULL;
    }
    return Py_BuildValue("(ff)(ff)", min.x, min.z, max.x, max.z);
}

static PyObject *PyPf_add_road(PyObject *self, PyObject *args)
{
    struct road_desc rd;
    if(!PyArg_ParseTuple(args, "(ii)", &rd.r, &rd.c)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of two integers.");
        return NULL;
    }
    return PyBool_FromLong(G_Roads_Add(rd));
}

static PyObject *PyPf_remove_road(PyObject *self, PyObject *args)
{
    struct road_desc rd;
    if(!PyArg_ParseTuple(args, "(ii)", &rd.r, &rd.c)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of two integers.");
        return NULL;
    }
    return PyBool_FromLong(G_Roads_Remove(rd));
}

static PyObject *PyPf_link_roads(PyObject *self, PyObject *args)
{
    struct road_desc a, b;
    if(!PyArg_ParseTuple(args, "(ii)(ii)", &a.r, &a.c, &b.r, &b.c)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two tuples of two integers.");
        return NULL;
    }
    return PyBool_FromLong(G_Roads_Link(a, b));
}

static PyObject *PyPf_unlink_roads(PyObject *self, PyObject *args)
{
    struct road_desc a, b;
    if(!PyArg_ParseTuple(args, "(ii)(ii)", &a.r, &a.c, &b.r, &b.c)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two tuples of two integers.");
        return NULL;
    }
    return PyBool_FromLong(G_Roads_Unlink(a, b));
}

static PyObject *PyPf_get_road(PyObject *self, PyObject *args)
{
    struct road_desc rd;
    if(!PyArg_ParseTuple(args, "(ii)", &rd.r, &rd.c)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of two integers.");
        return NULL;
    }

    uint8_t links;
    if(!G_Roads_Get(rd, &links))
        Py_RETURN_NONE;
    return Py_BuildValue("i", links);
}

static PyObject *PyPf_set_road_kind(PyObject *self, PyObject *args)
{
    struct road_desc rd;
    int kind;
    if(!PyArg_ParseTuple(args, "(ii)i", &rd.r, &rd.c, &kind)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a tuple of two integers and an integer.");
        return NULL;
    }
    if(kind != ROAD_KIND_DIRT && kind != ROAD_KIND_PLANK) {
        PyErr_SetString(PyExc_ValueError, "The kind must be one of the ROAD_KIND_* constants.");
        return NULL;
    }
    return PyBool_FromLong(G_Roads_SetKind(rd, kind));
}

static PyObject *PyPf_get_road_kind(PyObject *self, PyObject *args)
{
    struct road_desc rd;
    if(!PyArg_ParseTuple(args, "(ii)", &rd.r, &rd.c)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of two integers.");
        return NULL;
    }

    enum road_kind kind;
    if(!G_Roads_GetKind(rd, &kind))
        Py_RETURN_NONE;
    return Py_BuildValue("i", kind);
}

static PyObject *PyPf_road_speed_factor(PyObject *self, PyObject *args)
{
    vec2_t pos;
    if(!PyArg_ParseTuple(args, "(ff)", &pos.x, &pos.z)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of two floats.");
        return NULL;
    }
    return PyFloat_FromDouble(G_Roads_SpeedFactor(pos));
}

static PyObject *PyPf_get_all_roads(PyObject *self)
{
    size_t max = G_Roads_Count();
    struct road_desc *roads = PF_MALLOC(sizeof(struct road_desc) * (max + 1));
    uint8_t *links = PF_MALLOC(sizeof(uint8_t) * (max + 1));
    if(!roads || !links) {
        PF_FREE(roads);
        PF_FREE(links);
        return PyErr_NoMemory();
    }
    size_t count = G_Roads_GetAll(max, roads, links);

    PyObject *ret = PyList_New(count);
    if(!ret)
        goto out;

    for(int i = 0; i < count; i++) {
        PyObject *road = Py_BuildValue("(iii)", roads[i].r, roads[i].c, links[i]);
        if(!road) {
            Py_CLEAR(ret);
            goto out;
        }
        PyList_SET_ITEM(ret, i, road);
    }

out:
    PF_FREE(roads);
    PF_FREE(links);
    return ret;
}

static PyObject *PyPf_clear_roads(PyObject *self)
{
    G_Roads_Clear();
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_road_hover(PyObject *self, PyObject *args)
{
    PyObject *obj;
    if(!PyArg_ParseTuple(args, "O", &obj)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of two integers or None.");
        return NULL;
    }

    if(obj == Py_None) {
        G_Roads_SetHover(NULL);
        Py_RETURN_NONE;
    }

    struct road_desc rd;
    if(!PyArg_ParseTuple(obj, "ii", &rd.r, &rd.c)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a tuple of two integers or None.");
        return NULL;
    }
    G_Roads_SetHover(&rd);
    Py_RETURN_NONE;
}

static PyObject *PyPf_road_buildable(PyObject *self, PyObject *args)
{
    struct road_desc rd;
    int faction_id;
    if(!PyArg_ParseTuple(args, "(ii)i", &rd.r, &rd.c, &faction_id)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a tuple of two integers and an integer.");
        return NULL;
    }
    if(faction_id < 0 || faction_id >= MAX_FACTIONS) {
        PyErr_SetString(PyExc_ValueError, "Invalid faction ID.");
        return NULL;
    }
    return PyBool_FromLong(G_Roads_Buildable(rd, faction_id));
}

static PyObject *PyPf_road_tiles_under(PyObject *self, PyObject *args)
{
    unsigned uid;
    if(!PyArg_ParseTuple(args, "I", &uid)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer.");
        return NULL;
    }
    if(!G_EntityExists(uid)) {
        PyErr_SetString(PyExc_RuntimeError, "No entity with the specified UID exists.");
        return NULL;
    }

    struct road_desc tiles[64];
    size_t ntiles = G_Roads_TilesUnder(uid, ARR_SIZE(tiles), tiles);
    PyObject *ret = PyList_New(ntiles);
    if(!ret)
        return NULL;
    for(size_t i = 0; i < ntiles; i++) {
        PyList_SET_ITEM(ret, i, Py_BuildValue("(ii)", tiles[i].r, tiles[i].c));
    }
    return ret;
}

static PyObject *PyPf_set_road_refused(PyObject *self, PyObject *args)
{
    PyObject *list;
    if(!PyArg_ParseTuple(args, "O", &list)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a list of tuples of two integers or None.");
        return NULL;
    }

    if(list == Py_None) {
        G_Roads_SetRefused(0, NULL);
        Py_RETURN_NONE;
    }

    if(!PyList_Check(list)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a list of tuples of two integers or None.");
        return NULL;
    }

    size_t ntiles = PyList_GET_SIZE(list);
    struct road_desc *tiles = PF_MALLOC(sizeof(struct road_desc) * (ntiles + 1));
    if(!tiles)
        return PyErr_NoMemory();

    for(int i = 0; i < ntiles; i++) {
        if(!PyArg_ParseTuple(PyList_GET_ITEM(list, i), "ii", &tiles[i].r, &tiles[i].c)) {
            PF_FREE(tiles);
            PyErr_SetString(PyExc_TypeError, "Argument must be a list of tuples of two integers or None.");
            return NULL;
        }
    }
    G_Roads_SetRefused(ntiles, tiles);
    PF_FREE(tiles);
    Py_RETURN_NONE;
}

static PyObject *PyPf_show_roads(PyObject *self)
{
    G_Roads_SetRender(true);
    Py_RETURN_NONE;
}

static PyObject *PyPf_hide_roads(PyObject *self)
{
    G_Roads_SetRender(false);
    Py_RETURN_NONE;
}

static PyObject *PyPf_show_placement_grid(PyObject *self, PyObject *args)
{
    PyObject *on;
    vec2_t centre;
    PyObject *point = NULL;
    if(!PyArg_ParseTuple(args, "O|O", &on, &point)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a boolean and optionally a tuple of two numbers.");
        return NULL;
    }
    if(point && point != Py_None && !PyArg_ParseTuple(point, "ff", &centre.x, &centre.z)) {
        PyErr_SetString(PyExc_TypeError, "The centre must be a tuple of two numbers.");
        return NULL;
    }
    G_Roads_SetPlacementGrid(PyObject_IsTrue(on), (point && point != Py_None) ? &centre : NULL);
    Py_RETURN_NONE;
}

static bool parse_tileset_entry(PyObject *entry, struct road_tileset *ts, int mask)
{
    if(PyString_Check(entry)) {
        if(PyString_Size(entry) >= MAX_DECAL_PATH_LEN)
            return false;
        pf_strlcpy(ts->paths[mask][0], PyString_AS_STRING(entry), MAX_DECAL_PATH_LEN);
        ts->nvariants[mask] = 1;
        return true;
    }

    if(!PySequence_Check(entry))
        return false;
    Py_ssize_t n = PySequence_Size(entry);
    if(n < 1 || n > MAX_ROAD_VARIANTS)
        return false;

    for(int i = 0; i < n; i++) {
        PyObject *path = PySequence_GetItem(entry, i);
        bool ok = path && PyString_Check(path) && PyString_Size(path) < MAX_DECAL_PATH_LEN;
        if(ok) {
            pf_strlcpy(ts->paths[mask][i], PyString_AS_STRING(path), MAX_DECAL_PATH_LEN);
        }
        Py_XDECREF(path);
        if(!ok)
            return false;
    }
    ts->nvariants[mask] = n;
    return true;
}

static PyObject *PyPf_set_road_tileset(PyObject *self, PyObject *args)
{
    PyObject *list;
    if(!PyArg_ParseTuple(args, "O", &list)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a list of 16 entries or None.");
        return NULL;
    }

    if(list == Py_None) {
        G_Roads_SetTileset(NULL);
        Py_RETURN_NONE;
    }

    if(!PySequence_Check(list) || PySequence_Size(list) != ROAD_LINK_ALL + 1) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a list of 16 entries or None.");
        return NULL;
    }

    struct road_tileset *ts = PF_MALLOC(sizeof(struct road_tileset));
    if(!ts)
        return PyErr_NoMemory();

    for(int m = 0; m <= ROAD_LINK_ALL; m++) {
        PyObject *entry = PySequence_GetItem(list, m);
        bool ok = entry && parse_tileset_entry(entry, ts, m);
        Py_XDECREF(entry);
        if(!ok) {
            PF_FREE(ts);
            PyErr_Format(PyExc_TypeError, "Entry %d must be an image path or a list of 1 to %d "
                "image paths.", m, MAX_ROAD_VARIANTS);
            return NULL;
        }
    }

    bool ok = G_Roads_SetTileset(ts);
    PF_FREE(ts);
    if(!ok) {
        PyErr_SetString(PyExc_RuntimeError, "Could not set the road tileset. Every path must be "
            "a readable image.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_road_tileset(PyObject *self)
{
    struct road_tileset *ts = PF_MALLOC(sizeof(struct road_tileset));
    if(!ts)
        return PyErr_NoMemory();

    if(!G_Roads_GetTileset(ts)) {
        PF_FREE(ts);
        Py_RETURN_NONE;
    }

    PyObject *ret = PyList_New(ROAD_LINK_ALL + 1);
    for(int m = 0; ret && m <= ROAD_LINK_ALL; m++) {
        PyObject *variants = PyList_New(ts->nvariants[m]);
        for(int v = 0; variants && v < ts->nvariants[m]; v++) {
            PyObject *path = PyString_FromString(ts->paths[m][v]);
            if(!path) {
                Py_CLEAR(variants);
                break;
            }
            PyList_SET_ITEM(variants, v, path);
        }
        if(!variants) {
            Py_CLEAR(ret);
            break;
        }
        PyList_SET_ITEM(ret, m, variants);
    }

    PF_FREE(ts);
    return ret;
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

static PyObject *PyPf_perf_us(PyObject *self)
{
    return PyFloat_FromDouble(SDL_GetPerformanceCounter() * 1e6 / SDL_GetPerformanceFrequency());
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
    struct perf_info *infos[32];
    size_t nthreads = Perf_Report(ARR_SIZE(infos), (struct perf_info **)&infos);

    PyObject *ret = PyTuple_New(nthreads);
    if(!ret) {
        for(size_t i = 0; i < nthreads; i++)
            PF_FREE(infos[i]);
        return NULL;
    }

    for(size_t i = 0; i < nthreads; i++) {
        PyObject *pi = S_PerfInfo_New(infos[i]);
        if(!pi) {
            for(size_t j = i; j < nthreads; j++)
                PF_FREE(infos[j]);
            Py_DECREF(ret);
            return NULL;
        }
        PyTuple_SET_ITEM(ret, i, pi);
    }
    return ret;
}

static PyObject *PyPf_prev_frame_memstats(PyObject *self)
{
    struct perf_mem_stats stats = {0};
    Perf_GetMemoryStats(&stats);

    return Py_BuildValue("{s:L,s:L,s:L,s:K,s:K}",
        "mi_malloc_normal_current", (long long)stats.mi_malloc_normal_current,
        "mi_pages_current",         (long long)stats.mi_pages_current,
        "mi_threads_current",       (long long)stats.mi_threads_current,
        "vm_rss_kb",                (unsigned long long)stats.vm_rss_kb,
        "vm_size_kb",               (unsigned long long)stats.vm_size_kb);
}

static PyObject *PyPf_prev_frame_vramstats(PyObject *self)
{
    struct vram_stats stats = {0};
    Perf_GetVramStats(&stats);

    return Py_BuildValue("{s:i,s:i,s:i,s:i,s:i}",
        "dedicated_kb",         stats.dedicated_kb,
        "total_available_kb",   stats.total_available_kb,
        "current_available_kb", stats.current_available_kb,
        "eviction_count",       stats.eviction_count,
        "evicted_kb",           stats.evicted_kb);
}

static PyObject *PyPf_prev_frame_gpu_stats(PyObject *self)
{
    struct gpu_frame_stats stats = {0};
    Perf_GetGpuFrameStats(&stats);

    return Py_BuildValue("{s:K,s:K,s:K,s:K,s:K,s:K}",
        "verts_submitted",  (unsigned long long)stats.verts_submitted,
        "prims_submitted",  (unsigned long long)stats.prims_submitted,
        "vs_invocations",   (unsigned long long)stats.vs_invocations,
        "clip_in_prims",    (unsigned long long)stats.clip_in_prims,
        "clip_out_prims",   (unsigned long long)stats.clip_out_prims,
        "frag_invocations", (unsigned long long)stats.frag_invocations);
}

static PyObject *PyPf_prev_frame_billboard_stats(PyObject *self)
{
    struct billboard_frame_stats stats = {0};
    Perf_GetBillboardStats(&stats);

    return Py_BuildValue("{s:I,s:I}",
        "nbillboard", stats.nbillboard,
        "ntotal",     stats.ntotal);
}

static PyObject *PyPf_get_nav_tick_times(PyObject *self)
{
    struct nav_tick_sample samples[PERF_NAV_TICK_HISTORY];
    size_t n = Perf_GetNavTickTimes(PERF_NAV_TICK_HISTORY, samples);

    PyObject *list = PyList_New(n);
    if(!list)
        return NULL;
    for(size_t i = 0; i < n; i++) {
        PyObject *item = Py_BuildValue("(IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII)",
            samples[i].dur_us, samples[i].serial_us,
            samples[i].total_us, samples[i].nwork, samples[i].budget_us,
            samples[i].tick,
            samples[i].inval_us, samples[i].los_peek_us, samples[i].los_build_us,
            samples[i].cpr_async_us, samples[i].cpr_serial_us,
            samples[i].dv_us, samples[i].vel_us, samples[i].upd_us,
            samples[i].main_us, samples[i].consume_us, samples[i].copy_gs_us,
            samples[i].submit_us, samples[i].map_update_us, samples[i].drain_us,
            samples[i].pivot_us, samples[i].cmds_us, samples[i].flock_us,
            samples[i].snaps_us,
            samples[i].nlos_builds, samples[i].nreq_rebuilds,
            samples[i].nenemy_built, samples[i].nzone_built,
            samples[i].nsurround_built, samples[i].ninval_enemy,
            samples[i].ninval_surround, samples[i].nsvc_sync,
            samples[i].nsvc_patch, samples[i].nastar, samples[i].nastar_memo,
            samples[i].npseek_built);
        if(!item) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, i, item);
    }
    return list;
}

static PyObject *PyPf_get_frame_times(PyObject *self)
{
    struct frame_time_sample samples[PERF_FRAME_HISTORY];
    size_t n = Perf_GetFrameTimes(PERF_FRAME_HISTORY, samples);

    PyObject *list = PyList_New(n);
    if(!list)
        return NULL;
    for(size_t i = 0; i < n; i++) {
        PyObject *item = Py_BuildValue("(II)", samples[i].seq, samples[i].ms);
        if(!item) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, i, item);
    }
    return list;
}

static PyObject *mem_accounting_to_dict(const struct mem_accounting *acc)
{
    PyObject *result = PyDict_New();
    if(!result)
        return NULL;

    for(int sys = 0; sys < MEM_SYS_COUNT; sys++) {
        PyObject *subs = PyDict_New();
        if(!subs)
            goto fail;

        for(int sub = 0; sub < MEM_SUB_MAX_PER_SYS; sub++) {
            if(acc->sub_bytes[sys][sub] == 0 && acc->sub_count[sys][sub] == 0)
                continue;
            PyObject *sub_entry = Py_BuildValue("{s:L,s:L}",
                "bytes", (long long)acc->sub_bytes[sys][sub],
                "count", (long long)acc->sub_count[sys][sub]);
            if(!sub_entry) {
                Py_DECREF(subs);
                goto fail;
            }
            const char *sub_name = Mem_SubName(sys, sub);
            int rc;
            if(sub_name) {
                rc = PyDict_SetItemString(subs, sub_name, sub_entry);
            }else{
                PyObject *key = PyLong_FromLong(sub);
                rc = (key ? PyDict_SetItem(subs, key, sub_entry) : -1);
                Py_XDECREF(key);
            }
            Py_DECREF(sub_entry);
            if(rc != 0) {
                Py_DECREF(subs);
                goto fail;
            }
        }

        PyObject *sys_entry = Py_BuildValue("{s:L,s:L,s:O}",
            "bytes",      (long long)acc->sys_bytes[sys],
            "count",      (long long)acc->sys_count[sys],
            "subsystems", subs);
        Py_DECREF(subs);
        if(!sys_entry)
            goto fail;

        int rc = PyDict_SetItemString(result, Mem_SysName(sys), sys_entry);
        Py_DECREF(sys_entry);
        if(rc != 0)
            goto fail;
    }
    return result;

fail:
    Py_DECREF(result);
    return NULL;
}

static PyObject *PyPf_prev_frame_mem_accounting(PyObject *self)
{
    struct mem_accounting acc;
    Perf_GetMemoryAccounting(&acc);
    return mem_accounting_to_dict(&acc);
}

static PyObject *gpu_mem_accounting_to_dict(const struct gpu_mem_accounting *acc)
{
    PyObject *result = PyDict_New();
    if(!result)
        return NULL;

    for(int sys = 0; sys < GPU_MEM_SYS_COUNT; sys++) {
        if(acc->sys_bytes[sys] == 0 && acc->sys_count[sys] == 0)
            continue;

        PyObject *subs = PyDict_New();
        if(!subs)
            goto fail;

        for(int sub = 0; sub < GPU_MEM_SUB_COUNT; sub++) {
            if(acc->sub_bytes[sys][sub] == 0 && acc->sub_count[sys][sub] == 0)
                continue;
            PyObject *sub_entry = Py_BuildValue("{s:L,s:L}",
                "bytes", (long long)acc->sub_bytes[sys][sub],
                "count", (long long)acc->sub_count[sys][sub]);
            if(!sub_entry) {
                Py_DECREF(subs);
                goto fail;
            }
            int rc = PyDict_SetItemString(subs, gpu_mem_sub_name(sub), sub_entry);
            Py_DECREF(sub_entry);
            if(rc != 0) {
                Py_DECREF(subs);
                goto fail;
            }
        }

        PyObject *sys_entry = Py_BuildValue("{s:L,s:L,s:O}",
            "bytes",      (long long)acc->sys_bytes[sys],
            "count",      (long long)acc->sys_count[sys],
            "subsystems", subs);
        Py_DECREF(subs);
        if(!sys_entry)
            goto fail;

        int rc = PyDict_SetItemString(result, gpu_mem_sys_name(sys), sys_entry);
        Py_DECREF(sys_entry);
        if(rc != 0)
            goto fail;
    }
    return result;

fail:
    Py_DECREF(result);
    return NULL;
}

static PyObject *PyPf_prev_frame_gpu_mem_accounting(PyObject *self)
{
    struct gpu_mem_accounting acc;
    Perf_GetGpuMemoryAccounting(&acc);
    return gpu_mem_accounting_to_dict(&acc);
}

static PyObject *PyPf_mem_audit(PyObject *self)
{
    struct mem_accounting acc;
    Mem_AuditTaggedBytes(&acc);
    return mem_accounting_to_dict(&acc);
}

static PyObject *PyPf_prev_frame_allocd_bytes(PyObject *self)
{
    uint64_t allocd = Perf_LastFrameAllocdBytes();
    return Py_BuildValue("l", allocd);
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
    G_GetFieldCacheStats(&stats);

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

static PyObject *PyPf_get_text_outline(PyObject *self)
{
    const struct nk_style_text *text = &UI_GetContext()->style.text;
    return Py_BuildValue("(iiii)f", 
        text->outline_color.r, 
        text->outline_color.g, 
        text->outline_color.b, 
        text->outline_color.a, 
        text->outline_width);
}

static PyObject *PyPf_set_text_outline(PyObject *self, PyObject *args)
{
    int r, g, b, a;
    float width;

    if(!PyArg_ParseTuple(args, "(iiii)f", &r, &g, &b, &a, &width)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an (R, G, B, A) tuple and a float.");
        return NULL;
    }

    struct nk_style_text *text = &UI_GetContext()->style.text;
    text->outline_color = nk_rgba(r, g, b, a);
    text->outline_width = width;
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_skybox(PyObject *self, PyObject *args)
{
    const char *dir, *extension;

    if(!PyArg_ParseTuple(args, "ss", &dir, &extension)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be 2 strings.");
        return NULL;
    }

    G_SetSkybox(dir, extension);
    Py_RETURN_NONE;
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
    G_ReseedFogVision();
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

static PyObject *PyPf_get_idle_units(PyObject *self)
{
    PyObject *ret = PyList_New(0);
    if(!ret)
        return NULL;

    vec_entity_t idle;
    vec_entity_init(&idle);
    G_Automation_GetIdle(&idle);

    for(int i = 0; i < vec_size(&idle); i++) {
        uint32_t uid = vec_AT(&idle, i);
        PyObject *ent = S_Entity_ObjForUID(uid);
        if(ent) {
            PyList_Append(ret, ent);
        }
    }

    vec_entity_destroy(&idle);
    return ret;
}

static PyObject *PyPf_set_unit_selection(PyObject *self, PyObject *args)
{
    PyObject *list;
    if(!PyArg_ParseTuple(args, "O!", &PyList_Type, &list)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a list object.");
        return NULL;
    }

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

static PyObject *PyPf_gather_blocked(PyObject *self)
{
    if(G_GatherBlockedNow())
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
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

static PyObject *PyPf_hide_unit_icons(PyObject *self)
{
    G_SetShowUnitIcons(false);
    Py_RETURN_NONE;
}

static PyObject *PyPf_show_unit_icons(PyObject *self)
{
    G_SetShowUnitIcons(true);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_right_click_attack(PyObject *self, PyObject *args)
{
    int on;
    if(!PyArg_ParseTuple(args, "i", &on)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a boolean.");
        return NULL;
    }
    G_SetAttackOnRightClick(on);
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_right_click_attack(PyObject *self)
{
    if(G_GetAttackOnRightClick())
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

static PyObject *PyPf_get_resource_list(PyObject *self)
{
    const char *names[128];
    int nres = G_Resource_GetAllNames(ARR_SIZE(names) / 2, names);

    /* Goods made in buildings are no resource on the map */
    const char *stored[64];
    int nstored = G_StorageSite_GetAllNames(ARR_SIZE(stored), stored);
    for(int i = 0; i < nstored; i++) {
        bool listed = false;
        for(int j = 0; j < nres && !listed; j++) {
            listed = (0 == strcmp(names[j], stored[i]));
        }
        if(!listed) {
            names[nres++] = stored[i];
        }
    }

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
    if(PyList_Sort(ret) < 0) {
        Py_DECREF(ret);
        return NULL;
    }
    return ret;
}

static PyObject *PyPf_get_resource_stored(PyObject *self, PyObject *args)
{
    const char *name;
    int faction = -1;
    if(!PyArg_ParseTuple(args, "s|i", &name, &faction)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a resource name (string) and an optional faction ID.");
        return NULL;
    }

    int stored = (faction < 0) ? G_StorageSite_GetPlayerStored(name)
                               : G_StorageSite_GetFactionStored(faction, name);
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

static PyObject *PyPf_get_resource_produced(PyObject *self, PyObject *args)
{
    const char *name;
    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one argument: resource name (string).");
        return NULL;
    }
    return PyInt_FromLong(G_StorageSite_GetPlayerProduced(name));
}

static PyObject *PyPf_get_resource_consumed(PyObject *self, PyObject *args)
{
    const char *name;
    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one argument: resource name (string).");
        return NULL;
    }
    return PyInt_FromLong(G_StorageSite_GetPlayerConsumed(name));
}

static PyObject *PyPf_set_resource_icon(PyObject *self, PyObject *args)
{
    const char *name, *path;
    if(!PyArg_ParseTuple(args, "ss", &name, &path)) {
        PyErr_SetString(PyExc_TypeError, "Expecting two arguments: resource name (string) and image path (string).");
        return NULL;
    }

    G_Resource_SetIcon(name, path);
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_resource_icon(PyObject *self, PyObject *args)
{
    const char *name;
    if(!PyArg_ParseTuple(args, "s", &name)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one arguments: resource name (string).");
        return NULL;
    }
    const char *path = G_Resource_GetIcon(name);
    if(!path)
        Py_RETURN_NONE;
    return PyString_FromString(path);
}

static PyObject *PyPf_get_render_settings(PyObject *self)
{
    PyObject *curr;
    vec3_t light_pos = G_GetLightPos();
    vec3_t ambient_clr = G_GetAmbientLightColor();
    vec3_t emit_clr = G_GetEmitLightColor();
    const char *skybox_dir, *skybox_ext;
    G_GetSkybox(&skybox_dir, &skybox_ext);

    PyObject *ret = PyDict_New();
    if(!ret)
        goto fail;

    curr = Py_BuildValue("(fff)", light_pos.x, light_pos.y, light_pos.z);
    if(!curr)
        goto fail;
    if(0 != PyDict_SetItemString(ret, "emit_light_pos", curr)) {
        Py_DECREF(curr);
        goto fail;
    }

    curr = Py_BuildValue("(fff)", ambient_clr.x, ambient_clr.y, ambient_clr.z);
    if(!curr)
        goto fail;
    if(0 != PyDict_SetItemString(ret, "ambient_light_color", curr)) {
        Py_DECREF(curr);
        goto fail;
    }

    curr = Py_BuildValue("(fff)", emit_clr.x, emit_clr.y, emit_clr.z);
    if(!curr)
        goto fail;
    if(0 != PyDict_SetItemString(ret, "emit_light_color", curr)) {
        Py_DECREF(curr);
        goto fail;
    }

    curr = Py_BuildValue("(ss)", skybox_dir, skybox_ext);
    if(!curr)
        goto fail;
    if(0 != PyDict_SetItemString(ret, "skybox", curr)) {
        Py_DECREF(curr);
        goto fail;
    }
    return ret;

fail:
    Py_XDECREF(ret);
    return NULL;
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
    if(!G_AddFaction(name, color, NULL)) {
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

static PyObject *PyPf_get_population(PyObject *self, PyObject *args)
{
    int faction_id;
    if(!PyArg_ParseTuple(args, "i", &faction_id)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer faction ID.");
        return NULL;
    }
    return PyInt_FromLong(G_Population_Get(faction_id));
}

static PyObject *PyPf_get_population_limit(PyObject *self, PyObject *args)
{
    int faction_id;
    if(!PyArg_ParseTuple(args, "i", &faction_id)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer faction ID.");
        return NULL;
    }
    return PyInt_FromLong(G_Population_GetLimit(faction_id));
}

static PyObject *PyPf_get_allied_population(PyObject *self, PyObject *args)
{
    int faction_id;
    if(!PyArg_ParseTuple(args, "i", &faction_id)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer faction ID.");
        return NULL;
    }
    return PyInt_FromLong(G_Population_GetAllied(faction_id));
}

static PyObject *PyPf_get_enemy_population(PyObject *self, PyObject *args)
{
    int faction_id;
    if(!PyArg_ParseTuple(args, "i", &faction_id)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer faction ID.");
        return NULL;
    }
    return PyInt_FromLong(G_Population_GetEnemy(faction_id));
}

static PyObject *PyPf_get_player_population(PyObject *self)
{
    return PyInt_FromLong(G_Population_GetPlayer());
}

static PyObject *PyPf_get_player_population_limit(PyObject *self)
{
    return PyInt_FromLong(G_Population_GetPlayerLimit());
}

static PyObject *PyPf_get_tile(PyObject *self, PyObject *args)
{
    struct tile_desc desc;
    if(!PyArg_ParseTuple(args, "(ii)(ii)", &desc.chunk_r, &desc.chunk_c, &desc.tile_r, &desc.tile_c)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two tuples of two integers.");
        return NULL;
    }

    return S_Tile_New(&desc);
}

static PyObject *PyPf_tiles_under_obj(PyObject *self, PyObject *args)
{
    int uid;
    if(!PyArg_ParseTuple(args, "i", &uid)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an entity UID (integer).");
        return NULL;
    }

    if(!G_EntityExists(uid)) {
        PyErr_SetString(PyExc_RuntimeError, "No entity with the supplied UID.");
        return NULL;
    }

    const struct map *map = G_GetPrevTickMap();
    if(!map) {
        PyErr_SetString(PyExc_RuntimeError, "No active map.");
        return NULL;
    }

    struct obb obb;
    Entity_CurrentOBB(uid, &obb, true);

    struct map_resolution res;
    M_GetResolution(map, &res);

    struct tile_desc tds[2048];
    size_t ntiles = M_Tile_AllUnderObj(M_GetPos(map), res, &obb, tds, ARR_SIZE(tds));

    PyObject *list = PyList_New(ntiles);
    if(!list)
        return NULL;

    for(size_t i = 0; i < ntiles; i++) {
        PyObject *coord = Py_BuildValue("((ii)(ii))",
            tds[i].chunk_r, tds[i].chunk_c, tds[i].tile_r, tds[i].tile_c);
        if(!coord) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, i, coord);
    }
    return list;
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

static PyObject *PyPf_map_pos_over_water(PyObject *self, PyObject *args)
{
    float x, z;
    if(!PyArg_ParseTuple(args, "ff", &x, &z)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two floats.");
        return NULL;
    }

    bool result = G_PointOverWater((vec2_t){x, z});
    return PyBool_FromLong(result);
}

static PyObject *PyPf_map_pos_over_land(PyObject *self, PyObject *args)
{
    float x, z;
    if(!PyArg_ParseTuple(args, "ff", &x, &z)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two floats.");
        return NULL;
    }

    bool result = G_PointOverLand((vec2_t){x, z});
    return PyBool_FromLong(result);
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

static PyObject *PyPf_map_nearest_pathable(PyObject *self, PyObject *args, PyObject *kwargs)
{
    float x, z;
    static char *kwlist[] = {"pos", "radius", NULL};
    float radius = 0.0f;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "(ff)|f", kwlist, &x, &z, &radius)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a tuple of two floats. "
            "An optional (float) 'radius' arugment is allowed.");
        return NULL;
    }

    uint32_t flags = 0;
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);

    vec2_t ret;
    bool exists = G_MapClosestPathable((vec2_t){x, z}, &ret, layer);
    if(!exists) {
        Py_RETURN_NONE;
    }else{
        return Py_BuildValue("ff", ret.x, ret.z);
    }
}

static PyObject *PyPf_map_locations_reachable(PyObject *self, PyObject *args, PyObject *kwargs)
{
    float ax, az, bx, bz;
    static char *kwlist[] = {"a", "b", "radius", NULL};
    float radius = 0.0f;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "(ff)(ff)|f", kwlist, &ax, &az, &bx, &bz, &radius)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two tuples of two floats. "
            "An optional (float) 'radius' argument is allowed.");
        return NULL;
    }

    uint32_t flags = 0;
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);
    if(G_MapLocationsReachable((vec2_t){ax, az}, (vec2_t){bx, bz}, layer)) {
        Py_RETURN_TRUE;
    }else{
        Py_RETURN_FALSE;
    }
}

static PyObject *PyPf_map_nearest_pathable_water(PyObject *self, PyObject *args, PyObject *kwargs)
{
    float x, z;
    static char *kwlist[] = {"pos", "radius", NULL};
    float radius = 0.0f;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "(ff)|f", kwlist, &x, &z, &radius)) {
        PyErr_Print();
        PyErr_SetString(PyExc_TypeError, "Arguments must be a tuple of two floats. "
            "An optional (float) 'radius' arugment is allowed.");
        return NULL;
    }

    uint32_t flags = ENTITY_FLAG_WATER;
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);

    vec2_t ret;
    bool exists = G_MapClosestPathable((vec2_t){x, z}, &ret, layer);
    if(!exists) {
        Py_RETURN_NONE;
    }else{
        return Py_BuildValue("ff", ret.x, ret.z);
    }
}

static PyObject *PyPf_map_nearest_pathable_air(PyObject *self, PyObject *args, PyObject *kwargs)
{
    float x, z;
    static char *kwlist[] = {"pos", "radius", NULL};
    float radius = 0.0f;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "(ff)|f", kwlist, &x, &z, &radius)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a tuple of two floats. "
            "An optional (float) 'radius' arugment is allowed.");
        return NULL;
    }

    uint32_t flags = ENTITY_FLAG_AIR;
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);

    vec2_t ret;
    bool exists = G_MapClosestPathable((vec2_t){x, z}, &ret, layer);
    if(!exists) {
        Py_RETURN_NONE;
    }else{
        return Py_BuildValue("ff", ret.x, ret.z);
    }
}

static PyObject *PyPf_map_pos_pathable(PyObject *self, PyObject *args, PyObject *kwargs)
{
    float x, z;
    static char *kwlist[] = {"pos", "radius", NULL};
    float radius = 0.0f;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "(ff)|f", kwlist, &x, &z, &radius)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a tuple of two floats. "
            "An optional (float) 'radius' argument is allowed.");
        return NULL;
    }

    uint32_t flags = 0;
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);

    bool result = G_MapPositionPathable((vec2_t){x, z}, layer);
    return PyBool_FromLong(result);
}

static PyObject *PyPf_map_reachable_now(PyObject *self, PyObject *args, PyObject *kwargs)
{
    float x, z;
    PyObject *obj;
    static char *kwlist[] = {"pos", "entity", "radius", NULL};
    float radius = 0.0f;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "(ff)O|f", kwlist, &x, &z, &obj, &radius)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a tuple of two floats and an entity. "
            "An optional (float) 'radius' argument is allowed.");
        return NULL;
    }

    if(!S_Entity_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "The second argument must be a pf.Entity object.");
        return NULL;
    }
    uint32_t uid;
    S_Entity_UIDForObj(obj, &uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(0, radius);
    return PyBool_FromLong(G_MapObjReachableNow((vec2_t){x, z}, uid, layer));
}

static PyObject *PyPf_obj_adjacent(PyObject *self, PyObject *args)
{
    PyObject *a, *b;
    if(!PyArg_ParseTuple(args, "OO", &a, &b) || !S_Entity_Check(a) || !S_Entity_Check(b)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two pf.Entity objects.");
        return NULL;
    }
    uint32_t uid, other;
    S_Entity_UIDForObj(a, &uid);
    S_Entity_UIDForObj(b, &other);
    return PyBool_FromLong(G_ObjAdjacent(uid, other));
}

static PyObject *PyPf_map_tile_info(PyObject *self, PyObject *args)
{
    float x, z, radius = 0.0f;
    if(!PyArg_ParseTuple(args, "(ff)|f", &x, &z, &radius)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a tuple of two floats and an optional unit radius.");
        return NULL;
    }
    struct nav_tile_report rep;
    if(!G_MapTileInfo((vec2_t){x, z}, Entity_NavLayerWithRadius(0, radius), &rep))
        Py_RETURN_NONE;
    return Py_BuildValue("(iiiiii)", (int)rep.pathable, rep.blockers, rep.static_blockers, (int)rep.slit,
        rep.local_island, rep.global_island);
}

static PyObject *PyPf_closest_adjacent_pos(PyObject *self, PyObject *args)
{
    PyObject *a, *b;
    if(!PyArg_ParseTuple(args, "OO", &a, &b) || !S_Entity_Check(a) || !S_Entity_Check(b)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two pf.Entity objects.");
        return NULL;
    }
    uint32_t uid, target;
    S_Entity_UIDForObj(a, &uid);
    S_Entity_UIDForObj(b, &target);
    vec2_t out;
    if(!G_ClosestAdjacentPos(uid, target, &out))
        Py_RETURN_NONE;
    return Py_BuildValue("(ff)", out.x, out.z);
}

/* A line on the unit's state in the engine's movement, building or harvesting, for a probe */
static PyObject *PyPf_debug_desc(PyObject *self, PyObject *args)
{
    PyObject *obj;
    const char *which;
    if(!PyArg_ParseTuple(args, "sO", &which, &obj) || !S_Entity_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string ('move', 'build', 'harvest' or 'garrison') "
            "and a pf.Entity object.");
        return NULL;
    }
    uint32_t uid;
    S_Entity_UIDForObj(obj, &uid);

    char buff[256];
    if(!strcmp(which, "move")) {
        G_Move_DebugDesc(uid, buff, sizeof(buff));
    }else if(!strcmp(which, "build")) {
        G_Builder_DebugDesc(uid, buff, sizeof(buff));
    }else if(!strcmp(which, "harvest")) {
        G_Harvester_DebugDesc(uid, buff, sizeof(buff));
    }else if(!strcmp(which, "garrison")) {
        G_Garrison_DebugDesc(uid, buff, sizeof(buff));
    }else{
        PyErr_SetString(PyExc_ValueError, "The first argument must be 'move', 'build', 'harvest' or 'garrison'.");
        return NULL;
    }
    return PyString_FromString(buff);
}

static PyObject *PyPf_map_ground_open(PyObject *self, PyObject *args, PyObject *kwargs)
{
    float x, z;
    static char *kwlist[] = {"pos", "radius", "off_road", "unit_radius", NULL};
    float radius = 0.0f;
    int off_road = false;
    float unit_radius = 0.0f;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "(ff)|fif", kwlist, &x, &z, &radius, &off_road,
        &unit_radius)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a tuple of two floats. "
            "Optional (float) 'radius', (bool) 'off_road' and (float) 'unit_radius' arguments "
            "are allowed.");
        return NULL;
    }

    enum nav_layer layer = Entity_NavLayerWithRadius(0, unit_radius);
    bool result = G_MapGroundOpen((vec2_t){x, z}, radius, layer, off_road);
    return PyBool_FromLong(result);
}

static PyObject *PyPf_map_pos_under_cursor(PyObject *self)
{
    if(!G_MapLoaded()) {
        Py_RETURN_NONE;
    }
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

static PyObject *PyPf_set_evict_on_left_click(PyObject *self)
{
    G_Garrison_SetEvictOnLeftClick();
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_position_rally_point_on_left_click(PyObject *self)
{
    G_Building_SetPositionRallyPointOnLeftClick();
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

static struct rect text_rect_for_labels(struct rect rect)
{
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

    return (struct rect){
        adj_center.x - rect.w/2.0f,
        adj_center.y - rect.h/2.0f,
        rect.w,
        rect.h
    };
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

    struct rect adj_rect = text_rect_for_labels(rect);
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

static PyObject *PyPf_draw_text_outlined(PyObject *self, PyObject *args)
{
    const char *text;
    struct rect rect;
    int r, g, b, a;
    int out_r, out_g, out_b, out_a;

    if(!PyArg_ParseTuple(args, "s(iiii)(iiii)(iiii)", &text, &rect.x, &rect.y, &rect.w, &rect.h, &r, &g,
        &b, &a, &out_r, &out_g, &out_b, &out_a)) {
        PyErr_SetString(PyExc_TypeError,
            "Expecting 4 arguments: A text string, a tuple of 4 ints (bounds) and two tuples of 4 ints "
            "(RGBA color and RGBA outline color).");
        return NULL;
    }
    UI_DrawTextOutlined(text, text_rect_for_labels(rect), (struct rgba){r, g, b, a},
        (struct rgba){out_r, out_g, out_b, out_a});
    Py_RETURN_NONE;
}

static PyObject *PyPf_dump_framebuffer(PyObject *self, PyObject *args)
{
    const char *path;
    if(!PyArg_ParseTuple(args, "s", &path)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a single string argument: the output PPM file path.");
        return NULL;
    }

    int width, height;
    Engine_WinDrawableSize(&width, &height);

    R_PushCmd((struct rcmd){
        .func = R_GL_DumpFBColor_PPM,
        .nargs = 3,
        .args = {
            R_PushArg(path, strlen(path) + 1),
            R_PushArg(&width, sizeof(width)),
            R_PushArg(&height, sizeof(height)),
        },
    });
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

    if(S_UI_ParseNinePatch(val, &style.data.slice_texpath) == 0) {

        style.type = NK_STYLE_ITEM_NINE_SLICE_TEXPATH;

    }else if(PyTuple_Check(val) 
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
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A).");
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

static PyObject *PyPf_set_garrison_ui_font_color(PyObject *self, PyObject *args)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(args, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return NULL;
    }

    struct nk_color clr = (struct nk_color){r, g, b, a};
    G_Garrison_SetFontColor(&clr);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_garrison_ui_icon(PyObject *self, PyObject *args)
{
    const char *path;
    if(!PyArg_ParseTuple(args, "s", &path)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a string.");
    }

    G_Garrison_SetIcon(path);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_garrison_ui_style(PyObject *self, PyObject *args)
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

    G_Garrison_SetBackgroundStyle(&style);
    Py_RETURN_NONE;
}

static PyObject *PyPf_garrison_show_ui(PyObject *self, PyObject *args)
{
    PyObject *obj;
    if(!PyArg_ParseTuple(args, "O", &obj)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a single argument (True or False expression)");
        return NULL;
    }

    bool show = PyObject_IsTrue(obj);
    G_Garrison_SetShowUI(show);
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
        Py_RETURN_NONE;
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
    pf_strlcpy(new_sett.name, name, sizeof(new_sett.name));
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

static PyObject *PyPf_set_rts_camera_zoom_enabled(PyObject *self, PyObject *args)
{
    PyObject *value;
    if(!PyArg_ParseTuple(args, "O", &value)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a single argument: boolean expression");
        return NULL;
    }

    CamControl_RTS_SetZoomEnabled(PyObject_IsTrue(value));
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_rts_camera_zoom_enabled(PyObject *self)
{
    if(CamControl_RTS_GetZoomEnabled()) {
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

static PyObject *PyPf_get_files_in_dir(PyObject *self, PyObject *args)
{
    const char *str;
    if(!PyArg_ParseTuple(args, "s", &str)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string (relative directory path).");
        return NULL;
    }
    char absdir[NK_MAX_PATH_LEN];
    pf_snprintf(absdir, sizeof(absdir), "%s/%s", g_basepath, str);

    size_t nfiles = 0;
    struct file *files = nk_file_list(absdir, &nfiles);

    PyObject *ret = PyList_New(0);
    if(!ret)
        return NULL;

    for(int i = 0; i < nfiles; i++) {
        struct file *curr = &files[i];
        if(curr->is_dir)
            continue;
        PyObject *str = PyString_FromString(curr->name);
        if(!str) {
            Py_DECREF(ret);
            return NULL;
        }
        PyList_Append(ret, str);
        Py_DECREF(str);
    }
    return ret;
}

static PyObject *PyPf_get_time_str(PyObject *self)
{
    time_t curr_time = time(NULL);
    if(curr_time == (time_t)-1) {
        PyErr_SetString(PyExc_RuntimeError, "Could not get the time.");
        return NULL;
    }
    struct tm *tm = localtime(&curr_time);
    if(!tm) {
        PyErr_SetString(PyExc_RuntimeError, "Could not get the local time.");
        return NULL;
    }

    char buff[256];
    if(!strftime(buff, sizeof(buff), "%Y-%m-%d [%a] %H:%M", tm)) {
        PyErr_SetString(PyExc_RuntimeError, "Could not format time.");
        return NULL;
    }

    return PyString_FromString(buff);
}

static PyObject *PyPf_ensure_directory(PyObject *self, PyObject *args)
{
    const char *str;
    if(!PyArg_ParseTuple(args, "s", &str)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string (relative directory name).");
        return NULL;
    }
    if(strchr(str, '/') || strchr(str, '\\') || strchr(str, '.')) {
        PyErr_SetString(PyExc_TypeError, "Directory name must not contain '/', '\\', or '.'.");
        return NULL;
    }

    char path[512];
    pf_snprintf(path, sizeof(path), "%s/%s", g_basepath, str);

#ifdef _WIN32

    size_t wlen;
    wchar_t wpath[512];
    mbstowcs_s(&wlen, wpath, sizeof(wpath)-1, path, strlen(path));

    if(CreateDirectory(wpath, NULL) 
    || ERROR_ALREADY_EXISTS == GetLastError()) {
        Py_RETURN_NONE;
    }
#else
    if((mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == -1)
    || errno == EEXIST) {
        Py_RETURN_NONE;
    }
#endif

    PyErr_SetString(PyExc_TypeError, "Cannot create directory.");
    return NULL;
}

static PyObject *PyPf_delete_user_file(PyObject *self, PyObject *args)
{
    const char *dir, *file;
    if(!PyArg_ParseTuple(args, "ss", &dir, &file)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string (relative directory name), "
            "and a filename.");
        return NULL;
    }

    if(strchr(dir, '/') || strchr(dir, '\\') || strchr(dir, '.')) {
        PyErr_SetString(PyExc_TypeError, "Directory name must not contain '/', '\\', or '.'.");
        return NULL;
    }
    if(strlen(dir) == 0) {
        PyErr_SetString(PyExc_TypeError, "Directory name not be empty.");
        return NULL;
    }
    const char *blacklist[] = {
        "assets",
        "scripts"
    };
    for(int i = 0; i < ARR_SIZE(blacklist); i++) {
        if(0 == strcmp(dir, blacklist[i])) {
            PyErr_SetString(PyExc_TypeError, "Cannot delete from a blacklisted directory.");
            return NULL;
        }
    }

    char path[512];
    pf_snprintf(path, sizeof(path), "%s/%s/%s", g_basepath, dir, file);

#ifdef _WIN32
    if(0 == _unlink(path)) {
        Py_RETURN_NONE;
    }
#else
    if(0 == unlink(path)) {
        Py_RETURN_NONE;
    }
#endif

    PyErr_SetString(PyExc_TypeError, "Unable to delete the specified file.");
    return NULL;
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

    status = Settings_Create((struct setting){
        .name = "pf.debug.log_python",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
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

    /* A predicate that raised leaves its exception pending for the caller */
    if(PyErr_Occurred())
        return false;

    PyObject *result = PyObject_CallFunctionObjArgs(func, obj, NULL);
    if(!result)
        return false;
    bool ret = (PyObject_IsTrue(result) == 1);
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
    if(PyErr_Occurred())
        return NULL;

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

    if(ninserted == ninside)
        return list;

    PyObject *ret = PyList_GetSlice(list, 0, ninserted);
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

    if(ninserted == ninside)
        return list;

    PyObject *ret = PyList_GetSlice(list, 0, ninserted);
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
            Py_DECREF(ret);
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
    uint32_t dmg_type = 0;

    if(!PyArg_ParseTuple(args, "(fff)(fff)IIII(ss(fff)f)|I", &origin.x, &origin.y, &origin.z,
        &velocity.x, &velocity.y, &velocity.z, &ent_parent, &faction_id, &cookie, &flags,
        &pd.basedir, &pd.pfobj, &pd.scale.x, &pd.scale.y, &pd.scale.z, &pd.speed, &dmg_type)) {

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
            "projectile speed (float)], "
            "damage type (int, optional).");
        return NULL;
    }

    uint32_t ret = P_Projectile_Add(origin, velocity, ent_parent, faction_id, cookie,
        dmg_type, flags, pd);
    return PyInt_FromLong(ret);
}

static bool s_parse_damage_table(PyObject *obj, float out[DAMAGE_TYPE_MAX][ARMOUR_TYPE_MAX],
                                 int *out_nrows, int *out_ncols)
{
    if(!PySequence_Check(obj) || PySequence_Size(obj) > DAMAGE_TYPE_MAX) {
        PyErr_Format(PyExc_TypeError, "The damage table must be a sequence of at most %d rows, "
            "one per damage type.", DAMAGE_TYPE_MAX);
        return false;
    }

    const int nrows = PySequence_Size(obj);
    int ncols = 0;

    for(int d = 0; d < nrows; d++) {

        PyObject *row = PySequence_GetItem(obj, d);
        if(!row)
            return false;

        if(!PySequence_Check(row) || PySequence_Size(row) > ARMOUR_TYPE_MAX) {
            Py_DECREF(row);
            PyErr_Format(PyExc_TypeError, "Row %d of the damage table must be a sequence of at "
                "most %d floats, one per armour type.", d, ARMOUR_TYPE_MAX);
            return false;
        }

        /* Every row covers the same armour types, so ragged input is a typo. */
        if(d == 0) {
            ncols = PySequence_Size(row);
        }else if(PySequence_Size(row) != ncols) {
            Py_DECREF(row);
            PyErr_Format(PyExc_TypeError, "Row %d of the damage table has %d entries; row 0 "
                "has %d. All rows must be the same length.",
                d, (int)PySequence_Size(row), ncols);
            return false;
        }

        for(int a = 0; a < ncols; a++) {

            PyObject *val = PySequence_GetItem(row, a);
            if(!val) {
                Py_DECREF(row);
                return false;
            }

            double mult = PyFloat_AsDouble(val);
            Py_DECREF(val);
            if(PyErr_Occurred()) {
                Py_DECREF(row);
                return false;
            }

            if(mult < 0.0 || mult > 100.0) {
                Py_DECREF(row);
                PyErr_Format(PyExc_ValueError, "The damage multiplier at [%d][%d] must be in "
                    "the range [0.0, 100.0].", d, a);
                return false;
            }
            out[d][a] = mult;
        }
        Py_DECREF(row);
    }

    *out_nrows = nrows;
    *out_ncols = ncols;
    return true;
}

static PyObject *PyPf_set_damage_table(PyObject *self, PyObject *args)
{
    PyObject *obj;
    if(!PyArg_ParseTuple(args, "O", &obj)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one (sequence) argument.");
        return NULL;
    }

    /* Parse the whole table before committing any of it, so that a table which
     * fails validation leaves the live one untouched. */
    float mult[DAMAGE_TYPE_MAX][ARMOUR_TYPE_MAX];
    int nrows, ncols;
    if(!s_parse_damage_table(obj, mult, &nrows, &ncols))
        return NULL;

    /* The parsed rows are strided by the capacity, not by ncols. */
    float packed[DAMAGE_TYPE_MAX * ARMOUR_TYPE_MAX];
    for(int d = 0; d < nrows; d++) {
        for(int a = 0; a < ncols; a++) {
            packed[d * ncols + a] = mult[d][a];
        }
    }

    G_Combat_SetDamageTable(packed, nrows, ncols);
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_damage_table(PyObject *self)
{
    float mult[DAMAGE_TYPE_MAX][ARMOUR_TYPE_MAX];
    G_Combat_GetDamageTable(&mult[0][0]);

    PyObject *ret = PyTuple_New(DAMAGE_TYPE_MAX);
    if(!ret)
        return NULL;

    for(int d = 0; d < DAMAGE_TYPE_MAX; d++) {

        PyObject *row = PyTuple_New(ARMOUR_TYPE_MAX);
        if(!row) {
            Py_DECREF(ret);
            return NULL;
        }

        for(int a = 0; a < ARMOUR_TYPE_MAX; a++) {

            PyObject *val = PyFloat_FromDouble(mult[d][a]);
            if(!val) {
                Py_DECREF(row);
                Py_DECREF(ret);
                return NULL;
            }
            PyTuple_SET_ITEM(row, a, val);
        }
        PyTuple_SET_ITEM(ret, d, row);
    }
    return ret;
}

static PyObject *PyPf_formation_arrange(PyObject *self, PyObject *args)
{
    enum formation_type type;
    vec_entity_t ents;
    vec_entity_init(&ents);

    PyObject *list;
    if(!PyArg_ParseTuple(args, "O!i", &PyList_Type, &list, &type)) {
        PyErr_SetString(PyExc_TypeError, 
            "Argument must a list object and a formation type (integer).");
        return NULL;
    }

    for(int i = 0; i < PyList_GET_SIZE(list); i++) {
        PyObject *obj = PyList_GET_ITEM(list, i);
        if(!S_Entity_Check(obj)) {
            PyErr_SetString(PyExc_TypeError,
                "The entity list must contain only pf.Entity objects.");
            vec_entity_destroy(&ents);
            return NULL;
        }
        uint32_t uid;
        S_Entity_UIDForObj(obj, &uid);
        vec_entity_push(&ents, uid);
    }

    G_Formation_Arrange(type, &ents);
    vec_entity_destroy(&ents);
    Py_RETURN_NONE;
}

static PyObject *PyPf_move_in_formation(PyObject *self, PyObject *args)
{
    enum formation_type type;
    vec2_t target, orientation;
    vec_entity_t ents;
    vec_entity_init(&ents);

    PyObject *list;
    if(!PyArg_ParseTuple(args, "O!i(ff)(ff)", &PyList_Type, &list, &type,
        &target.x, &target.z, &orientation.x, &orientation.z)) {
        PyErr_SetString(PyExc_TypeError, 
            "Argument must a list object, a formation type (integer), "
             "a target location (list of 2 floats), and a target orientation (list of 2 floats).");
        return NULL;
    }

    for(int i = 0; i < PyList_GET_SIZE(list); i++) {
        PyObject *obj = PyList_GET_ITEM(list, i);
        if(!S_Entity_Check(obj)) {
            PyErr_SetString(PyExc_TypeError, 
                "The entity list must contain only pf.Entity objects.");
            vec_entity_destroy(&ents);
            return NULL;
        }
        uint32_t uid;
        S_Entity_UIDForObj(obj, &uid);
        vec_entity_push(&ents, uid);
    }

    G_Move_ArrangeInFormation(&ents, target, orientation, type);
    vec_entity_destroy(&ents);
    Py_RETURN_NONE;
}

static PyObject *PyPf_attack_in_formation(PyObject *self, PyObject *args)
{
    enum formation_type type;
    vec2_t target, orientation;
    vec_entity_t ents;
    vec_entity_init(&ents);

    PyObject *list;
    if(!PyArg_ParseTuple(args, "O!i(ff)(ff)", &PyList_Type, &list, &type,
        &target.x, &target.z, &orientation.x, &orientation.z)) {
        PyErr_SetString(PyExc_TypeError, 
            "Argument must a list object, a formation type (integer), "
             "a target location (list of 2 floats), and a target orientation (list of 2 floats).");
        return NULL;
    }

    for(int i = 0; i < PyList_GET_SIZE(list); i++) {
        PyObject *obj = PyList_GET_ITEM(list, i);
        if(!S_Entity_Check(obj)) {
            PyErr_SetString(PyExc_TypeError, 
                "The entity list must contain only pf.Entity objects.");
            vec_entity_destroy(&ents);
            return NULL;
        }
        uint32_t uid;
        S_Entity_UIDForObj(obj, &uid);
        vec_entity_push(&ents, uid);
    }

    G_Move_AttackInFormation(&ents, target, orientation, type);
    vec_entity_destroy(&ents);
    Py_RETURN_NONE;
}

static PyObject *PyPf_formation_preferred_for_set(PyObject *self, PyObject *args)
{
    vec_entity_t ents;
    vec_entity_init(&ents);

    PyObject *list;
    if(!PyArg_ParseTuple(args, "O!", &PyList_Type, &list)) {
        PyErr_SetString(PyExc_TypeError, 
            "Argument must a list object.");
        return NULL;
    }

    for(int i = 0; i < PyList_GET_SIZE(list); i++) {
        PyObject *obj = PyList_GET_ITEM(list, i);
        if(!S_Entity_Check(obj)) {
            PyErr_SetString(PyExc_TypeError, "Argument must a list of pf.Entity objects.");
            vec_entity_destroy(&ents);
            return NULL;
        }
        uint32_t uid;
        S_Entity_UIDForObj(obj, &uid);
        if(!G_EntityExists(uid))
            continue;
        vec_entity_push(&ents, uid);
    }

    enum formation_type type = G_Formation_PreferredForSet(&ents);
    vec_entity_destroy(&ents);
    return PyInt_FromLong(type);
}

static bool s_uids_for_ent_list(PyObject *list, uint32_t *out, size_t *out_nents)
{
    size_t nents = 0;
    for(int i = 0; i < PyList_GET_SIZE(list); i++) {
        PyObject *obj = PyList_GET_ITEM(list, i);
        if(!S_Entity_Check(obj)) {
            PyErr_SetString(PyExc_TypeError, "Argument must a list of pf.Entity objects.");
            return false;
        }
        uint32_t uid;
        S_Entity_UIDForObj(obj, &uid);
        if(!G_EntityExists(uid))
            continue;
        out[nents++] = uid;
    }
    *out_nents = nents;
    return true;
}

static PyObject *PyPf_group_lock(PyObject *self, PyObject *args)
{
    PyObject *list;
    if(!PyArg_ParseTuple(args, "O!", &PyList_Type, &list)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a list object.");
        return NULL;
    }

    STALLOC(uint32_t, ents, PyList_GET_SIZE(list));
    size_t nents;
    if(!s_uids_for_ent_list(list, ents, &nents))
        return NULL;

    return PyInt_FromLong(G_Group_Lock(ents, nents));
}

static PyObject *PyPf_group_unlock(PyObject *self, PyObject *args)
{
    int group_id;
    if(!PyArg_ParseTuple(args, "i", &group_id)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer group ID.");
        return NULL;
    }
    G_Group_Unlock(group_id);
    Py_RETURN_NONE;
}

static PyObject *PyPf_group_for_set(PyObject *self, PyObject *args)
{
    PyObject *list;
    if(!PyArg_ParseTuple(args, "O!", &PyList_Type, &list)) {
        PyErr_SetString(PyExc_TypeError, "Argument must a list object.");
        return NULL;
    }

    STALLOC(uint32_t, ents, PyList_GET_SIZE(list));
    size_t nents;
    if(!s_uids_for_ent_list(list, ents, &nents))
        return NULL;

    return PyInt_FromLong(G_Group_ForSet(ents, nents));
}

static PyObject *PyPf_set_group_ui_style(PyObject *self, PyObject *args)
{
    struct nk_style_item style;
    PyObject *val;

    if(!PyArg_ParseTuple(args, "O", &val)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an (R, G, B, A) tuple or an image path.");
        return NULL;
    }

    if(S_UI_ParseNinePatch(val, &style.data.slice_texpath) == 0) {

        style.type = NK_STYLE_ITEM_NINE_SLICE_TEXPATH;

    }else if(PyTuple_Check(val)
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

    G_Group_SetBackgroundStyle(&style);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_group_ui_font_color(PyObject *self, PyObject *args)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(args, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A).");
        return NULL;
    }

    struct nk_color clr = (struct nk_color){r, g, b, a};
    G_Group_SetFontColor(&clr);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_group_ui_bonus_color(PyObject *self, PyObject *args)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(args, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A).");
        return NULL;
    }

    struct nk_color clr = (struct nk_color){r, g, b, a};
    G_Group_SetBonusColor(&clr);
    Py_RETURN_NONE;
}

static PyObject *PyPf_group_for_ent(PyObject *self, PyObject *args)
{
    PyObject *obj;
    if(!PyArg_ParseTuple(args, "O", &obj) || !S_Entity_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single pf.Entity instance.");
        return NULL;
    }

    uint32_t uid = 0;
    S_Entity_UIDForObj(obj, &uid);
    return PyInt_FromLong(G_Group_ForEnt(uid));
}

static PyObject *PyPf_group_bonuses(PyObject *self, PyObject *args)
{
    int group_id;
    if(!PyArg_ParseTuple(args, "i", &group_id)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer group ID.");
        return NULL;
    }

    struct group_bonus_desc bonuses[16];
    int nbonuses = G_Group_GetBonuses(group_id, bonuses, ARR_SIZE(bonuses));

    PyObject *ret = PyList_New(nbonuses);
    if(!ret)
        return NULL;

    for(int i = 0; i < nbonuses; i++) {
        PyObject *desc = Py_BuildValue("(ssifO)", bonuses[i].tag, bonuses[i].icon,
            (int)bonuses[i].kind, bonuses[i].amount, bonuses[i].percent ? Py_True : Py_False);
        if(!desc) {
            Py_DECREF(ret);
            return NULL;
        }
        PyList_SET_ITEM(ret, i, desc);
    }
    return ret;
}

static PyObject *PyPf_show_range_indicator(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"entity", "radius", "color", "width", NULL};
    PyObject *obj;
    float radius;
    float r = 1.0f, g = 0.0f, b = 0.0f;
    float width = 0.5f;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "Of|(fff)f", kwlist, &obj, &radius,
        &r, &g, &b, &width)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a pf.Entity and a float radius. An "
            "optional (R, G, B) 'color' and (float) 'width' are allowed.");
        return NULL;
    }

    if(!S_Entity_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "The first argument must be a pf.Entity instance.");
        return NULL;
    }

    uint32_t uid = 0;
    S_Entity_UIDForObj(obj, &uid);
    G_SetRangeIndicator(uid, radius, (vec3_t){r, g, b}, width);
    Py_RETURN_NONE;
}

static PyObject *PyPf_hide_range_indicator(PyObject *self)
{
    G_ClearRangeIndicator();
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_group_highlight(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"group_id", "color", "width", NULL};
    int group_id;
    float r, g, b;
    float width = 1.0f;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "i(fff)|f", kwlist, &group_id, &r, &g, &b,
        &width)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an integer group ID and an (R, G, B) "
            "tuple of floats. An optional (float) 'width' is allowed.");
        return NULL;
    }

    struct bonus_highlight hl = (struct bonus_highlight){
        .active = true,
        .color = (vec3_t){r, g, b},
        .width = width
    };
    G_Group_SetHighlight(group_id, &hl);
    Py_RETURN_NONE;
}

static PyObject *PyPf_clear_group_highlight(PyObject *self)
{
    struct bonus_highlight hl = (struct bonus_highlight){0};
    G_Group_SetHighlight(0, &hl);
    Py_RETURN_NONE;
}

static PyObject *PyPf_group_members(PyObject *self, PyObject *args)
{
    int group_id;
    if(!PyArg_ParseTuple(args, "i", &group_id)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer group ID.");
        return NULL;
    }

    const vec_entity_t *members = G_Group_Members(group_id);
    size_t nmembers = members ? vec_size(members) : 0;

    PyObject *list = PyList_New(nmembers);
    if(!list)
        return NULL;

    size_t ninserted = 0;
    for(int i = 0; i < nmembers; i++) {
        PyObject *obj = S_Entity_ObjForUID(vec_AT(members, i));
        if(!obj)
            continue;
        Py_INCREF(obj);
        PyList_SET_ITEM(list, ninserted++, obj);
    }

    if(ninserted == nmembers)
        return list;

    PyObject *ret = PyList_GetSlice(list, 0, ninserted);
    Py_DECREF(list);
    return ret;
}

static PyObject *PyPf_cluster_members(PyObject *self, PyObject *args)
{
    int cluster_id;
    if(!PyArg_ParseTuple(args, "i", &cluster_id)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer cluster ID.");
        return NULL;
    }

    uint32_t members[256];
    size_t nmembers = G_Cluster_Members(cluster_id, ARR_SIZE(members), members);

    PyObject *list = PyList_New(0);
    if(!list)
        return NULL;
    for(int i = 0; i < nmembers; i++) {
        PyObject *obj = S_Entity_ObjForUID(members[i]);
        if(!obj)
            continue;
        if(0 != PyList_Append(list, obj)) {
            Py_DECREF(list);
            return NULL;
        }
    }
    return list;
}

static PyObject *PyPf_cluster_anchor(PyObject *self, PyObject *args)
{
    int cluster_id;
    if(!PyArg_ParseTuple(args, "i", &cluster_id)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer cluster ID.");
        return NULL;
    }
    PyObject *obj = S_Entity_ObjForUID(G_Cluster_Anchor(cluster_id));
    if(!obj)
        Py_RETURN_NONE;
    Py_INCREF(obj);
    return obj;
}

static PyObject *PyPf_cluster_colour(PyObject *self, PyObject *args)
{
    int cluster_id;
    if(!PyArg_ParseTuple(args, "i", &cluster_id)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer cluster ID.");
        return NULL;
    }
    struct nk_color clr;
    if(!G_Cluster_Colour(cluster_id, &clr))
        Py_RETURN_NONE;
    return Py_BuildValue("(iii)", clr.r, clr.g, clr.b);
}

static PyObject *PyPf_set_cluster_icon(PyObject *self, PyObject *args)
{
    const char *path;
    if(!PyArg_ParseTuple(args, "s", &path)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string path.");
        return NULL;
    }
    G_Cluster_SetIcon(path);
    Py_RETURN_NONE;
}

static PyObject *PyPf_provision_counts(PyObject *self, PyObject *args)
{
    int faction;
    if(!PyArg_ParseTuple(args, "i", &faction) || faction < 0 || faction >= MAX_FACTIONS) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a faction ID.");
        return NULL;
    }
    int counts[PROVISION_NUM_TIERS];
    G_Provision_Counts(faction, counts);
    return Py_BuildValue("(iii)", counts[PROVISION_FED], counts[PROVISION_HUNGRY], counts[PROVISION_STARVING]);
}

static PyObject *PyPf_resource_balance(PyObject *self, PyObject *args)
{
    const char *name;
    int faction;
    if(!PyArg_ParseTuple(args, "si", &name, &faction) || faction < 0 || faction >= MAX_FACTIONS) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a resource name and a faction ID.");
        return NULL;
    }
    int surplus, shortfall;
    G_Automation_Balance(faction, name, &surplus, &shortfall);
    return Py_BuildValue("(ii)", surplus, shortfall);
}

static int compare_ints(const void *a, const void *b)
{
    return *(const int*)a - *(const int*)b;
}

static PyObject *PyPf_clusters(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"type", "faction", NULL};
    const char *type = NULL;
    int faction = -1;
    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "|zi", kwlist, &type, &faction)) {
        PyErr_SetString(PyExc_TypeError, "Optional keywords: 'type' (string) and 'faction' (integer).");
        return NULL;
    }
    int ids[1024];
    size_t nids = G_Cluster_GetAll(ARR_SIZE(ids), ids);
    qsort(ids, nids, sizeof(ids[0]), compare_ints);

    PyObject *ret = PyList_New(0);
    if(!ret)
        return NULL;
    for(int i = 0; i < nids; i++) {
        if(type && strcmp(G_Cluster_Type(ids[i]), type))
            continue;
        if(faction >= 0 && G_Cluster_Faction(ids[i]) != faction)
            continue;
        PyObject *id = PyInt_FromLong(ids[i]);
        if(!id || 0 != PyList_Append(ret, id)) {
            Py_XDECREF(id);
            Py_DECREF(ret);
            return NULL;
        }
        Py_DECREF(id);
    }
    return ret;
}

static bool parse_good_faction(PyObject *args, const char **out_name, int *out_faction)
{
    if(!PyArg_ParseTuple(args, "si", out_name, out_faction) 
    || *out_faction < 0 || *out_faction >= MAX_FACTIONS) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a resource name and a faction ID.");
        return false;
    }
    return true;
}

static PyObject *PyPf_good_weight(PyObject *self, PyObject *args)
{
    const char *name;
    int faction;
    if(!parse_good_faction(args, &name, &faction))
        return NULL;
    return PyFloat_FromDouble(G_Automation_GetGoodWeight(faction, name));
}

static PyObject *PyPf_set_good_weight(PyObject *self, PyObject *args)
{
    const char *name;
    int faction;
    float weight;
    if(!PyArg_ParseTuple(args, "sif", &name, &faction, &weight) 
    || faction < 0 || faction >= MAX_FACTIONS || weight < 0.0f) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a resource name, a faction ID and a weight of 0 or more.");
        return NULL;
    }
    G_Automation_SetGoodWeight(faction, name, weight);
    Py_RETURN_NONE;
}

static PyObject *PyPf_stock_target(PyObject *self, PyObject *args)
{
    const char *name;
    int faction;
    if(!parse_good_faction(args, &name, &faction))
        return NULL;
    return PyInt_FromLong(G_Automation_GetStockTarget(faction, name));
}

static PyObject *PyPf_set_stock_target(PyObject *self, PyObject *args)
{
    const char *name;
    int faction, target;
    if(!PyArg_ParseTuple(args, "sii", &name, &faction, &target) 
    || faction < 0 || faction >= MAX_FACTIONS || target < 0) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a resource name, a faction ID and a stock of 0 or more.");
        return NULL;
    }
    G_Automation_SetStockTarget(faction, name, target);
    Py_RETURN_NONE;
}

static PyObject *PyPf_gather_rate(PyObject *self, PyObject *args)
{
    const char *name;
    int faction;
    if(!parse_good_faction(args, &name, &faction))
        return NULL;
    return PyFloat_FromDouble(G_Automation_GetGatherRate(faction, name));
}

static PyObject *PyPf_set_gather_rate(PyObject *self, PyObject *args)
{
    const char *name;
    int faction;
    float rate;
    if(!PyArg_ParseTuple(args, "sif", &name, &faction, &rate) 
    || faction < 0 || faction >= MAX_FACTIONS || rate < 0.0f) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a resource name, a faction ID and a rate of 0 or more.");
        return NULL;
    }
    G_Automation_SetGatherRate(faction, name, rate);
    Py_RETURN_NONE;
}

static PyObject *PyPf_gatherer_rate(PyObject *self, PyObject *args)
{
    const char *name;
    int faction;
    if(!parse_good_faction(args, &name, &faction))
        return NULL;
    return PyFloat_FromDouble(G_Automation_GathererRate(faction, name));
}

static PyObject *PyPf_district_weight(PyObject *self, PyObject *args)
{
    int district;
    const char *name;
    if(!PyArg_ParseTuple(args, "is", &district, &name)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a district ID and a resource name.");
        return NULL;
    }
    float weight = G_Automation_GetDistrictWeight(district, name);
    if(weight < 0.0f)
        Py_RETURN_NONE;
    return PyFloat_FromDouble(weight);
}

static PyObject *PyPf_set_district_weight(PyObject *self, PyObject *args)
{
    int district;
    const char *name;
    PyObject *weight_obj;
    if(!PyArg_ParseTuple(args, "isO", &district, &name, &weight_obj)
    || district <= 0 || (weight_obj != Py_None && !PyNumber_Check(weight_obj))) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a district ID, a resource name and a weight or None.");
        return NULL;
    }
    float weight = -1.0f;
    if(weight_obj != Py_None) {
        weight = PyFloat_AsDouble(weight_obj);
        if(PyErr_Occurred())
            return NULL;
        if(weight < 0.0f) {
            PyErr_SetString(PyExc_ValueError, "A district's weight cannot be negative.");
            return NULL;
        }
    }
    G_Automation_SetDistrictWeight(district, name, weight);
    Py_RETURN_NONE;
}

static PyObject *PyPf_open_requests(PyObject *self, PyObject *args)
{
    int faction;
    if(!PyArg_ParseTuple(args, "i", &faction)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one argument: faction ID (int).");
        return NULL;
    }
    int open, carriers;
    G_Automation_OpenRequests(faction, &open, &carriers);
    return Py_BuildValue("(ii)", open, carriers);
}

static PyObject *PyPf_workers_carry_out(PyObject *self, PyObject *args)
{
    int district;
    if(!PyArg_ParseTuple(args, "i", &district)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a district ID.");
        return NULL;
    }
    if(G_Automation_GetWorkersCarryOut(district))
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

static PyObject *PyPf_set_workers_carry_out(PyObject *self, PyObject *args)
{
    int district, on;
    if(!PyArg_ParseTuple(args, "ii", &district, &on) || district <= 0) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a district ID and a boolean.");
        return NULL;
    }
    G_Automation_SetWorkersCarryOut(district, on);
    Py_RETURN_NONE;
}

static PyObject *PyPf_crew_size(PyObject *self, PyObject *args)
{
    int faction;
    if(!PyArg_ParseTuple(args, "i", &faction) || faction < 0 || faction >= MAX_FACTIONS) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a faction ID.");
        return NULL;
    }
    return PyInt_FromLong(G_Automation_GetCrewSize(faction));
}

static PyObject *PyPf_set_crew_size(PyObject *self, PyObject *args)
{
    int faction, size;
    if(!PyArg_ParseTuple(args, "ii", &faction, &size) 
    || faction < 0 || faction >= MAX_FACTIONS || size < 0) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a faction ID and a crew size of 0 or more.");
        return NULL;
    }
    G_Automation_SetCrewSize(faction, size);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_overlay_span(PyObject *self, PyObject *args)
{
    float left, right;
    if(!PyArg_ParseTuple(args, "ff", &left, &right)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be two numbers.");
        return NULL;
    }
    G_SetOverlaySpan(left, right);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_good_class(PyObject *self, PyObject *args)
{
    const char *name;
    PyObject *list;
    if(!PyArg_ParseTuple(args, "sO", &name, &list) || !PyList_Check(list)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a name (string) and a list of resource names.");
        return NULL;
    }
    const char *members[8];
    size_t n = PyList_Size(list);
    if(n == 0 || n > 8) {
        PyErr_SetString(PyExc_ValueError, "Expecting between 1 and 8 resource names.");
        return NULL;
    }
    for(int i = 0; i < n; i++) {
        PyObject *item = PyList_GetItem(list, i);
        if(!PyString_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "Every member must be a resource name (string).");
            return NULL;
        }
        members[i] = PyString_AS_STRING(item);
    }
    if(!G_Automation_SetGoodClass(name, members, n)) {
        PyErr_SetString(PyExc_RuntimeError, "Could not set the class.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_automation_option(PyObject *self, PyObject *args)
{
    int faction;
    const char *name;
    float value;
    if(!PyArg_ParseTuple(args, "isf", &faction, &name, &value)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a faction ID, a setting's name and its value.");
        return NULL;
    }
    if(!G_Automation_SetOption(faction, name, value)) {
        PyErr_SetString(PyExc_ValueError, "No such faction or automation setting.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_automation_option(PyObject *self, PyObject *args)
{
    int faction;
    const char *name;
    float value;
    if(!PyArg_ParseTuple(args, "is", &faction, &name)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a faction ID and a setting's name.");
        return NULL;
    }
    if(!G_Automation_GetOption(faction, name, &value)) {
        PyErr_SetString(PyExc_ValueError, "No such faction or automation setting.");
        return NULL;
    }
    return PyFloat_FromDouble(value);
}

static PyObject *PyPf_crew_busy(PyObject *self, PyObject *args)
{
    int faction;
    if(!PyArg_ParseTuple(args, "i", &faction) || faction < 0 || faction >= MAX_FACTIONS) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a faction ID.");
        return NULL;
    }
    return PyInt_FromLong(G_Automation_CrewBusy(faction));
}

static PyObject *PyPf_awaits_builders(PyObject *self, PyObject *args)
{
    PyObject *obj;
    if(!PyArg_ParseTuple(args, "O", &obj) || !S_Entity_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a pf.Entity object.");
        return NULL;
    }
    uint32_t uid;
    S_Entity_UIDForObj(obj, &uid);
    int busy, size;
    bool awaits = G_Automation_AwaitsBuilders(uid, &busy, &size);
    return Py_BuildValue("(Oii)", awaits ? Py_True : Py_False, busy, size);
}

static bool workshop_arg(PyObject *obj, uint32_t *out)
{
    if(!S_Entity_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "The first argument must be a pf.Entity object.");
        return false;
    }
    S_Entity_UIDForObj(obj, out);
    uint32_t flags = G_FlagsGet(*out);
    if(!(flags & ENTITY_FLAG_BUILDING) || (flags & ENTITY_FLAG_ZOMBIE)) {
        PyErr_SetString(PyExc_ValueError, "The entity must be a building that is not a zombie.");
        return false;
    }
    return true;
}

static PyObject *PyPf_set_workshop_posts(PyObject *self, PyObject *args)
{
    PyObject *obj;
    int nposts;
    uint32_t uid;
    if(!PyArg_ParseTuple(args, "Oi", &obj, &nposts)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a building and an integer.");
        return NULL;
    }
    if(!workshop_arg(obj, &uid))
        return NULL;
    if(nposts < 0 || nposts > 4) {
        PyErr_SetString(PyExc_ValueError, "A workshop has between 0 and 4 posts.");
        return NULL;
    }
    if(!G_Posts_SetPosts(uid, nposts)) {
        PyErr_SetString(PyExc_RuntimeError, "Could not set the workshop's posts.");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_post_demand(PyObject *self, PyObject *args)
{
    PyObject *obj;
    int demand;
    uint32_t uid;
    if(!PyArg_ParseTuple(args, "Oi", &obj, &demand)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a building and an integer.");
        return NULL;
    }
    if(!workshop_arg(obj, &uid))
        return NULL;
    if(demand != POSTS_IDLE && demand != POSTS_KEEP && demand != POSTS_FILL) {
        PyErr_SetString(PyExc_ValueError, "The demand must be pf.POSTS_IDLE, pf.POSTS_KEEP or pf.POSTS_FILL.");
        return NULL;
    }
    G_Posts_SetDemand(uid, demand);
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_locked_posts(PyObject *self, PyObject *args)
{
    PyObject *obj;
    int locked;
    uint32_t uid;
    if(!PyArg_ParseTuple(args, "Oi", &obj, &locked)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a building and an integer.");
        return NULL;
    }
    if(!workshop_arg(obj, &uid))
        return NULL;
    G_Posts_SetLocked(uid, locked);
    Py_RETURN_NONE;
}

static PyObject *PyPf_workshop_posts(PyObject *self, PyObject *args)
{
    PyObject *obj;
    if(!PyArg_ParseTuple(args, "O", &obj) || !S_Entity_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a pf.Entity object.");
        return NULL;
    }
    uint32_t uid;
    S_Entity_UIDForObj(obj, &uid);
    int posts, locked, inside, coming;
    if(!G_Posts_Get(uid, &posts, &locked, &inside, &coming))
        Py_RETURN_NONE;
    return Py_BuildValue("(iiii)", posts, locked, inside, coming);
}

static PyObject *PyPf_post_workshop(PyObject *self, PyObject *args)
{
    PyObject *obj;
    if(!PyArg_ParseTuple(args, "O", &obj) || !S_Entity_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a pf.Entity object.");
        return NULL;
    }
    uint32_t uid;
    S_Entity_UIDForObj(obj, &uid);
    PyObject *ret = S_Entity_ObjForUID(G_Posts_WorkshopOf(uid));
    if(!ret)
        Py_RETURN_NONE;
    Py_INCREF(ret);
    return ret;
}

static PyObject *PyPf_man_post(PyObject *self, PyObject *args)
{
    PyObject *obj, *unit_obj = Py_None;
    uint32_t uid, unit;
    if(!PyArg_ParseTuple(args, "O|O", &obj, &unit_obj)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a building and optionally a pf.Entity object.");
        return NULL;
    }
    if(!workshop_arg(obj, &uid))
        return NULL;
    if(unit_obj == Py_None) {
        unit = G_Automation_PostWorker(uid);
    }else if(S_Entity_Check(unit_obj)) {
        S_Entity_UIDForObj(unit_obj, &unit);
    }else{
        PyErr_SetString(PyExc_TypeError, "The second argument must be a pf.Entity object or None.");
        return NULL;
    }
    if(unit == NULL_UID || !G_Posts_Man(uid, unit))
        Py_RETURN_NONE;
    PyObject *ret = S_Entity_ObjForUID(unit);
    if(!ret)
        Py_RETURN_NONE;
    Py_INCREF(ret);
    return ret;
}

static PyObject *PyPf_evict_post(PyObject *self, PyObject *args)
{
    PyObject *obj;
    uint32_t uid;
    if(!PyArg_ParseTuple(args, "O", &obj)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a building.");
        return NULL;
    }
    if(!workshop_arg(obj, &uid))
        return NULL;
    PyObject *ret = S_Entity_ObjForUID(G_Posts_Evict(uid));
    if(!ret)
        Py_RETURN_NONE;
    Py_INCREF(ret);
    return ret;
}

static PyObject *PyPf_leave_post(PyObject *self, PyObject *args)
{
    PyObject *obj;
    if(!PyArg_ParseTuple(args, "O", &obj) || !S_Entity_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a pf.Entity object.");
        return NULL;
    }
    uint32_t uid;
    S_Entity_UIDForObj(obj, &uid);
    if(G_Posts_Leave(uid))
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

static PyObject *PyPf_workshop_eating(PyObject *self, PyObject *args)
{
    PyObject *obj;
    if(!PyArg_ParseTuple(args, "O", &obj) || !S_Entity_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a pf.Entity object.");
        return NULL;
    }
    uint32_t uid;
    S_Entity_UIDForObj(obj, &uid);
    return PyInt_FromLong(G_Posts_Eating(uid));
}

static PyObject *PyPf_end_duty(PyObject *self, PyObject *args)
{
    PyObject *obj;
    if(!PyArg_ParseTuple(args, "O", &obj) || !S_Entity_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a pf.Entity object.");
        return NULL;
    }
    uint32_t uid;
    S_Entity_UIDForObj(obj, &uid);
    G_Duty_End(uid, "script");
    Py_RETURN_NONE;
}

static PyObject *PyPf_set_duty_by(PyObject *self, PyObject *args)
{
    PyObject *obj;
    const char *name;
    if(!PyArg_ParseTuple(args, "Os", &obj, &name) || !S_Entity_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a pf.Entity object and a string.");
        return NULL;
    }
    enum duty_by by;
    if(!G_Duty_ParseBy(name, &by)) {
        PyErr_SetString(PyExc_ValueError, "Expecting one of 'player', 'city' or 'automation'.");
        return NULL;
    }
    uint32_t uid;
    S_Entity_UIDForObj(obj, &uid);
    G_Duty_SetBy(uid, by);
    Py_RETURN_NONE;
}

static PyObject *PyPf_provision_demand(PyObject *self, PyObject *args)
{
    int faction;
    if(!PyArg_ParseTuple(args, "i", &faction) || faction < 0 || faction >= MAX_FACTIONS) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a faction ID.");
        return NULL;
    }
    int districts[256];
    float rates[256];
    size_t n = G_Provision_Demand(faction, ARR_SIZE(districts), districts, rates);

    PyObject *ret = PyDict_New();
    if(!ret)
        return NULL;
    for(int i = 0; i < n; i++) {
        PyObject *key = PyInt_FromLong(districts[i]);
        PyObject *val = PyFloat_FromDouble(rates[i]);
        int status = (key && val) ? PyDict_SetItem(ret, key, val) : -1;
        Py_XDECREF(key);
        Py_XDECREF(val);
        if(status) {
            Py_DECREF(ret);
            return NULL;
        }
    }
    return ret;
}

static PyObject *PyPf_cluster_preview_join(PyObject *self, PyObject *args)
{
    const char *type;
    int faction;
    vec2_t pos;
    float radius;
    if(!PyArg_ParseTuple(args, "si(ff)f", &type, &faction, &pos.x, &pos.z, &radius)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a type string, a faction ID, an (X, Z) tuple and a radius.");
        return NULL;
    }
    return PyInt_FromLong(G_Cluster_PreviewJoin(type, faction, pos, radius));
}

static PyObject *PyPf_set_cluster_preview(PyObject *self, PyObject *args)
{
    PyObject *obj;
    int cluster_id;
    if(!PyArg_ParseTuple(args, "Oi", &obj, &cluster_id)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an entity or None, and a cluster ID.");
        return NULL;
    }
    uint32_t uid = NULL_UID;
    if(obj != Py_None && !S_Entity_UIDForObj(obj, &uid)) {
        PyErr_SetString(PyExc_TypeError, "The first argument must be an entity or None.");
        return NULL;
    }
    G_Cluster_SetPreview(uid, cluster_id);
    Py_RETURN_NONE;
}

static PyObject *PyPf_district_at(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"pos", "faction_id", "water", NULL};
    vec2_t pos;
    int faction;
    int water = 0;
    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "(ff)i|i", kwlist, &pos.x, &pos.z, &faction, &water)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an (X, Z) tuple and a faction ID.");
        return NULL;
    }
    G_District_Update();
    float cost;
    int id = G_District_At(faction, water, pos, &cost);
    return Py_BuildValue("(ifO)", id, cost, cost <= G_DISTRICT_TERRITORY_COST && id ? Py_True : Py_False);
}

static PyObject *PyPf_show_districts(PyObject *self, PyObject *args)
{
    PyObject *on;
    if(!PyArg_ParseTuple(args, "O", &on)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a boolean.");
        return NULL;
    }
    G_District_SetShow(PyObject_IsTrue(on));
    Py_RETURN_NONE;
}

static PyObject *PyPf_districts_shown(PyObject *self)
{
    if(G_District_GetShow()) {
        Py_RETURN_TRUE;
    }else{
        Py_RETURN_FALSE;
    }
}

static PyObject *PyPf_show_district(PyObject *self, PyObject *args)
{
    int district;
    if(!PyArg_ParseTuple(args, "i", &district)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer (district ID).");
        return NULL;
    }
    G_District_SetFocus(district);
    Py_RETURN_NONE;
}

static PyObject *PyPf_district_shown(PyObject *self)
{
    return PyInt_FromLong(G_District_GetFocus());
}

static PyObject *PyPf_district_gathered(PyObject *self, PyObject *args)
{
    int district;
    if(!PyArg_ParseTuple(args, "i", &district)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer (district ID).");
        return NULL;
    }

    const char *names[16];
    int totals[16];
    size_t ngoods = G_StorageSite_DistrictGathered(district, names, totals, ARR_SIZE(names));

    PyObject *ret = PyDict_New();
    if(!ret)
        return NULL;
    for(int i = 0; i < ngoods; i++) {
        PyObject *total = PyInt_FromLong(totals[i]);
        if(!total || PyDict_SetItemString(ret, names[i], total) < 0) {
            Py_XDECREF(total);
            Py_DECREF(ret);
            return NULL;
        }
        Py_DECREF(total);
    }
    return ret;
}

static PyObject *PyPf_set_district_preview(PyObject *self, PyObject *args)
{
    int faction;
    PyObject *posobj;
    if(!PyArg_ParseTuple(args, "iO", &faction, &posobj)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a faction ID and an (X, Z) tuple or None.");
        return NULL;
    }
    if(posobj == Py_None) {
        G_District_SetPreview(false, faction, (vec2_t){0});
        return PyInt_FromLong(0);
    }
    vec2_t pos;
    if(!PyArg_ParseTuple(posobj, "ff", &pos.x, &pos.z)) {
        PyErr_SetString(PyExc_TypeError, "The position must be an (X, Z) tuple.");
        return NULL;
    }
    return PyInt_FromLong(G_District_SetPreview(true, faction, pos));
}

static PyObject *PyPf_show_console(PyObject *self)
{
    S_Console_Show();
    Py_RETURN_NONE;
}

static PyObject *PyPf_get_version_string(PyObject *self)
{
    return PyString_FromString(g_version);
}

static PyObject *PyPf_spawn_sprite_static(PyObject *self, PyObject *args)
{
    struct sprite_sheet_desc desc;
    vec2_t ws_size;
    vec3_t ws_pos;
    float duration;

    if(!PyArg_ParseTuple(args, "(snnn)(ff)(fff)f", &desc.filename, &desc.nrows, &desc.ncols,
        &desc.nframes, &ws_size.x, &ws_size.y, &ws_pos.x, &ws_pos.y, &ws_pos.z, &duration)) {

        PyErr_SetString(PyExc_TypeError, "Invalid arguments to spawn_sprite_static.");
        return NULL;
    }

    Sprite_ShowStatic(desc, ws_size, duration, ws_pos);
    Py_RETURN_NONE;
}

static PyObject *PyPf_spawn_sprite_animated(PyObject *self, PyObject *args)
{
    struct sprite_sheet_desc desc;
    vec2_t ws_size;
    vec3_t ws_pos;
    int fps;
    int repeat_count;

    if(!PyArg_ParseTuple(args, "(snnn)(ff)(fff)ii", &desc.filename, &desc.nrows, &desc.ncols,
        &desc.nframes, &ws_size.x, &ws_size.y, &ws_pos.x, &ws_pos.y, &ws_pos.z, &fps, 
        &repeat_count)) {

        PyErr_SetString(PyExc_TypeError, "Invalid arguments to spawn_sprite_animated.");
        return NULL;
    }

    Sprite_PlayAnim(repeat_count, fps, ws_size, desc, ws_pos);
    Py_RETURN_NONE;
}

static void script_task_destructor(void *arg)
{
    PF_FREE(arg);
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
    S_PerfInfo_PyRegister(module);
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

    if(!S_Console_Init())
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
    S_Console_Shutdown();
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
    if(!s_sys_path_add_dir(path))
        goto done;

    PyObject *main_module = PyImport_AddModule("__main__"); /* borrowed */
    if(!main_module)
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
    struct script_arg *arg = PF_MALLOC(sizeof(struct script_arg));
    if(!arg)
        return;

    arg->path = path;
    arg->argc = argc;
    arg->argv = (const char**)argv;

    uint32_t tid = Sched_Create(31, script_task, arg, "script_task",
        result, TASK_MAIN_THREAD_PINNED | TASK_BIG_STACK);
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

/* With the perf log on, a handler that held the frame up says so, by name */
static void log_slow_handler(PyObject *callable, uint64_t ticks)
{
    double us = ticks * 1e6 / SDL_GetPerformanceFrequency();
    if(us < SLOW_HANDLER_US)
        return;
    struct sval setting;
    if(Settings_Get("pf.debug.log_perf_csv", &setting) != SS_OKAY || !setting.as_bool)
        return;
    PyObject *err_type, *err_val, *err_tb;
    PyErr_Fetch(&err_type, &err_val, &err_tb);
    PyObject *module = PyObject_GetAttrString(callable, "__module__");
    PyObject *name = PyObject_GetAttrString(callable, "__name__");
    PyErr_Clear();
    fprintf(stdout, "[slow-handler] %lu,%.0f,%s.%s\n", g_frame_idx, us,
        (module && PyString_Check(module)) ? PyString_AS_STRING(module) : "?",
        (name && PyString_Check(name)) ? PyString_AS_STRING(name) : "?");
    Py_XDECREF(module);
    Py_XDECREF(name);
    PyErr_Restore(err_type, err_val, err_tb);
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
    clear_stale_error("handler", callable);
    uint64_t t0 = SDL_GetPerformanceCounter();
    ret = PyObject_CallObject(callable, args);
    log_slow_handler(callable, SDL_GetPerformanceCounter() - t0);
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
    case EVENT_BUILDING_MARKED:
    case EVENT_BUILDING_CONSTRUCTED:
    case EVENT_ORDER_ISSUED:
    case EVENT_PLAYER_ORDER_ISSUED:
    case EVENT_HUNT_HIT:
    case EVENT_ORDER_HOLD_BEGIN:
    case EVENT_ORDER_HOLD_END:
    case EVENT_POST_MANNED:
    case EVENT_DUTY_CHANGED:
    case EVENT_UNIT_BECAME_IDLE:
    case EVENT_UNIT_BECAME_ACTIVE:
    case EVENT_UNIT_IDLE_PENDING:
    case EVENT_REQUEST_BLOCKED:
    case EVENT_PROVISION_CHANGED:
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
    case EVENT_RESOURCES_SPILLED: {
        struct ss_spill_event *event = arg;
        return Py_BuildValue("(ff)isi", event->x, event->z, event->faction_id, event->name, event->amount);
    }
    case EVENT_CLUSTER_CHANGED:
        return PyInt_FromLong((intptr_t)arg);
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
        return Py_BuildValue("OIOII", ent, hit->proj_uid, parent, hit->cookie, hit->dmg_type);
    }
    case EVENT_ENTERED_REGION:
    case EVENT_EXITED_REGION: {
        return PyString_FromString(arg);
    }
    case EVENT_BUILDING_PLACED: {
        PyObject *ent = S_Entity_ObjForUID(((struct entity_block_desc*)arg)->uid);
        if(!ent)
            Py_RETURN_NONE;
        Py_INCREF(ent);
        return ent;
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
    struct script_handler *handlers = PF_MALLOC(max_handlers * sizeof(struct script_handler));
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
    /* Shown here, or the error is left set for whatever script runs next */
    if(!ret && PyErr_Occurred()) {
        S_ShowLastError();
    }
    return ret;
}

bool S_LoadState(SDL_RWops *stream)
{
    PyInterpreterState *interp = PyThreadState_Get()->interp;
    assert(interp);
    bool ret = false;

    PyObject *state = S_UnpickleObjgraph(stream);
    if(!state) {
        S_ShowLastError();
        return false;
    }

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

    /* The module comes back as it was saved: the bindings and constants the
     * engine has gained since are set in it afresh, so that newer scripts
     * and probes find them in an older save */
    PyObject *pfmod = PyDict_GetItemString(interp->modules, "pf");
    if(pfmod) {
        Py_InitModule("pf", pf_module_methods);
        S_Constants_Expose(pfmod);
    }

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
    /* A second error over one still shown must not lose the state to go back to */
    bool showing = s_err_ctx.occurred;
    Py_CLEAR(s_err_ctx.type);
    Py_CLEAR(s_err_ctx.value);
    Py_CLEAR(s_err_ctx.traceback);

    s_err_ctx.occurred = PyErr_Occurred();
    PyErr_Fetch(&s_err_ctx.type, &s_err_ctx.value, &s_err_ctx.traceback);
    PyErr_NormalizeException(&s_err_ctx.type, &s_err_ctx.value, &s_err_ctx.traceback);

    if(s_err_ctx.occurred) {
        if(!showing) {
            s_err_ctx.prev_state = G_GetSimState();
        }
        G_SetSimState(G_PAUSED_FULL);

        struct sval setting;
        if(Settings_Get("pf.debug.log_python", &setting) == SS_OKAY && setting.as_bool) {
            S_Error_Print(&s_err_ctx);
        }
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

int S_FormationPriority(uint32_t uid)
{
    PyObject *ent = S_Entity_ObjForUID(uid);
    if(!ent)
        return 0;

    PyTypeObject *type = ent->ob_type;
    PyObject *attr = PyObject_GetAttrString((PyObject*)type, "formation_priority");
    if(!attr) {
        PyErr_Clear();
        return 0;
    }

    if(!PyInt_Check(attr)) {
        Py_DECREF(attr);
        return 0;
    }

    int ret = PyInt_AS_LONG(attr);
    Py_DECREF(attr);
    return ret;
}

