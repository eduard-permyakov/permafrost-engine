/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2023 Eduard Permyakov 
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

#ifndef GAMESTATE_H
#define GAMESTATE_H

#include "public/game.h"
#include "faction.h"
#include "selection.h"
#include "../lib/public/vec.h"
#include "../lib/public/stalloc.h"
#include "../render/public/render_ctrl.h"

#include <stdint.h>

KHASH_DECLARE(id, khint32_t, int)
KHASH_DECLARE(range, khint32_t, float)


struct gamestate{
    enum simstate           ss;
    /*-------------------------------------------------------------------------
     * The requested simulation state to change to at the end of the tick.
     *-------------------------------------------------------------------------
     */
    enum simstate           requested_ss;
    /*-------------------------------------------------------------------------
     * The SDL tick during which we last changed simulation states.
     *-------------------------------------------------------------------------
     */
    uint32_t                ss_change_tick;
    /*-------------------------------------------------------------------------
     * Currently loaded map. May be NULL.
     *-------------------------------------------------------------------------
     */
    struct map             *map;
    /*-------------------------------------------------------------------------
     * Light position in worldspace coordinates.
     *-------------------------------------------------------------------------
     */
    vec3_t                  light_pos;
    /*-------------------------------------------------------------------------
     * Cache of currently set lighting parameters.
     *-------------------------------------------------------------------------
     */
    vec3_t                  ambient_light_color;
    vec3_t                  emit_light_color;
    /*-------------------------------------------------------------------------
     * Cache of skybox state.
     *-------------------------------------------------------------------------
     */
    char                    skybox_directory[256];
    char                    skybox_extension[16];
    /*-------------------------------------------------------------------------
     * Boolean to disable rendering of healthbars. Overrides the user-modifiable
     * setting.
     *-------------------------------------------------------------------------
     */
    bool                    hide_healthbars;
    /*-------------------------------------------------------------------------
     * Boolean to force rendering every single entity on the minimap.
     *-------------------------------------------------------------------------
     */
    bool                    minimap_render_all;
    /*-------------------------------------------------------------------------
     * Boolean to toggle showing of icons over entities.
     *-------------------------------------------------------------------------
     */
    bool                    show_unit_icons;
    /*-------------------------------------------------------------------------
     * The camera from which the scene is currently being rendered.
     *-------------------------------------------------------------------------
     */
    struct camera          *active_cam;
    /*-------------------------------------------------------------------------
     * The camera mode determines which camera controller is installed.
     *-------------------------------------------------------------------------
     */
    enum cam_mode           active_cam_mode;
    /*-------------------------------------------------------------------------
     * The set of all game entities currently taking part in the game simulation.
     *-------------------------------------------------------------------------
     */
    khash_t(entity)        *active;
    /*-------------------------------------------------------------------------
     * Table mapping an entity to its' faction ID for all active entities.
     * Synchronized with 'active' table at function call boundaries.
     *-------------------------------------------------------------------------
     */
    khash_t(id)           *ent_faction_map;
    /*-------------------------------------------------------------------------
     * Table mapping an entity to its' vision range for active entities.
     * Synchronized with 'active' table at function call boundaries.
     *-------------------------------------------------------------------------
     */
    khash_t(range)         *ent_visrange_map;
    /*-------------------------------------------------------------------------
     * Table mapping an entity to its' selection radius for active entities.
     * Synchronized with 'active' table at function call boundaries.
     *-------------------------------------------------------------------------
     */
    khash_t(range)         *selection_radiuses;
    /*-------------------------------------------------------------------------
     * Up-to-date set of all non-static entities. (Subset of 'active' set). 
     * Used for collision avoidance force computations.
     *-------------------------------------------------------------------------
     */
    khash_t(entity)        *dynamic;
    /*-------------------------------------------------------------------------
     * A bidirectional mapping of entity IDs to GPU IDs for every dynamic entity.
     * The GPU IDs are in the range of [1:kh_size(dynamic)] and thus are 
     * better suited to be used as indices. An ID of 0 represents a NULL ID.
     *-------------------------------------------------------------------------
     */
    khash_t(id)            *ent_gpu_id_map;
    khash_t(id)            *gpu_id_ent_map;
    /*-------------------------------------------------------------------------
     * A table mapping the entity's ID to its' flags.
     *-------------------------------------------------------------------------
     */
    khash_t(id)            *ent_flag_map;
    /*-------------------------------------------------------------------------
     * The set of entities potentially visible by the active camera. Updated
     * every frame.
     *-------------------------------------------------------------------------
     */
    vec_entity_t            visible;
    /*-------------------------------------------------------------------------
     * The set of entities that should be rendered from the light's point of 
     * view (for creating the shadow depth map).
     *-------------------------------------------------------------------------
     */
    vec_entity_t            light_visible;
    /*-------------------------------------------------------------------------
     * Cache of current-frame OBBs for visible entities.
     *-------------------------------------------------------------------------
     */
    vec_obb_t               visible_obbs;
    /*-------------------------------------------------------------------------
     * The state of the factions in the current game. 'factions_allocd' has a 
     * set bit for every faction index that's 'allocated'. Clear bits are 'free'.
     *-------------------------------------------------------------------------
     */
    uint16_t                factions_allocd;
    struct faction          factions[MAX_FACTIONS];
    /*-------------------------------------------------------------------------
     * Holds the relationships between every 2 factions. Note that diplomatic
     * relations are always symmetric (i.e always 'mutually' at war or peace).
     *-------------------------------------------------------------------------
     */
    enum diplomacy_state    diplomacy_table[MAX_FACTIONS][MAX_FACTIONS];
    /*-------------------------------------------------------------------------
     * The index indo the 'ws' field, where the rendering commands are stored.
     * The previous frame workspace is owned by the render thread. The render
     * and simulatino workspaces are swapped at the end of every frame.
     *-------------------------------------------------------------------------
     */
    int                     curr_ws_idx;
    struct render_workspace ws[2];
    /*-------------------------------------------------------------------------
     * A readonly snapshot (copy) of the map from the previous simulation tick. 
     * This is used by the render thread for making certain queries like size,
     * height at a point, etc.
     *-------------------------------------------------------------------------
     */
    const struct map       *prev_tick_map;
    /*-------------------------------------------------------------------------
     * Entities currently scheduled for removal. They will be removed from the
     * game simulation at the end of the tick.
     *-------------------------------------------------------------------------
     */
    vec_entity_t            removed;
    /*-------------------------------------------------------------------------
     * Stack allocator used for per-frame render descriptors.
     *-------------------------------------------------------------------------
     */
    struct memstack         render_data_stack;
};

#endif

