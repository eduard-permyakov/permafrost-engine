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

#ifndef NAV_H
#define NAV_H

#include "../../pf_math.h"
#include <stddef.h>
#include <stdbool.h>

struct tile;
struct map;
struct obb;
struct entity;

typedef uint32_t dest_id_t;

struct fc_stats{
    unsigned los_used;
    unsigned los_max;
    float    los_hit_rate;
    unsigned los_invalidated;
    unsigned flow_used;
    unsigned flow_max;
    float    flow_hit_rate;
    unsigned flow_invalidated;
    unsigned ffid_used;
    unsigned ffid_max;
    float    ffid_hit_rate;
    unsigned grid_path_used;
    unsigned grid_path_max;
    float    grid_path_hit_rate;
};

#define DEST_ID_INVALID (~((uint32_t)0))

/*###########################################################################*/
/* NAV GENERAL                                                               */
/*###########################################################################*/

/* ------------------------------------------------------------------------
 * Intialize the navigation subsystem.
 * ------------------------------------------------------------------------
 */
bool      N_Init(void);

/* ------------------------------------------------------------------------
 * Once per frame updates of the private navigation data.
 * ------------------------------------------------------------------------
 */
void      N_Update(void *nav_private);

/* ------------------------------------------------------------------------
 * Clean up resources acquired by 'N_Init'
 * ------------------------------------------------------------------------
 */
void      N_Shutdown(void);

/* ------------------------------------------------------------------------
 * Return a new navigation context for a map, containing pathability
 * information. 'w' and 'h' are the number of chunk columns/rows per map.
 * 'chunk_tiles' holds pointers to tile arrays for every chunk, in 
 * row-major order.
 * ------------------------------------------------------------------------
 */
void     *N_BuildForMapData(size_t w, size_t h, 
                            size_t chunk_w, size_t chunk_h,
                            const struct tile **chunk_tiles);

/* ------------------------------------------------------------------------
 * Clean up resources allocated by 'N_BuildForMapData'.
 * ------------------------------------------------------------------------
 */
void      N_FreePrivate(void *nav_private);

/* ------------------------------------------------------------------------
 * Draw a translucent overlay over the map chunk, showing the pathable and 
 * non-pathable regions. 'chunk_x_dim' and 'chunk_z_dim' are the chunk
 * dimensions in OpenGL coordinates.
 * ------------------------------------------------------------------------
 */
void      N_RenderPathableChunk(void *nav_private, mat4x4_t *chunk_model,
                                const struct map *map,
                                int chunk_r, int chunk_c);

/* ------------------------------------------------------------------------
 * Renders a flow field that will steer entities to the destination id for
 * the specified chunk. If the desired flow field is not in the cache, no 
 * action will take place.
 * ------------------------------------------------------------------------
 */
void      N_RenderPathFlowField(void *nav_private, const struct map *map, 
                                mat4x4_t *chunk_model, int chunk_r, int chunk_c, 
                                dest_id_t id);

/* ------------------------------------------------------------------------
 * Debug rendering of the 'Line of Sight' field from a particular
 * destination tile. Tiles that are directly visible from this tile without
 * an obstruction blocking the way are highlighted in a different color.
 * ------------------------------------------------------------------------
 */
void      N_RenderLOSField(void *nav_private, const struct map *map, mat4x4_t *chunk_model, 
                           int chunk_r, int chunk_c, dest_id_t id);

/* ------------------------------------------------------------------------
 * Debug rendering of the fields that guide enemies in combat to their 
 * targets. 
 * ------------------------------------------------------------------------
 */
void      N_RenderEnemySeekField(void *nav_private, const struct map *map, 
                                 mat4x4_t *chunk_model, int chunk_r, int chunk_c, 
                                 int faction_id);

/* ------------------------------------------------------------------------
 * Debug rendering to show which navigation tiles are currently occupied
 * by 'stationary' entities. Occupied tiles will be red, others will be green.
 * ------------------------------------------------------------------------
 */
void      N_RenderNavigationBlockers(void *nav_private, const struct map *map, 
                                     mat4x4_t *chunk_model, int chunk_r, int chunk_c);

/* ------------------------------------------------------------------------
 * Debug rendering to show the portals between chunks. 'Active' portals
 * are green and 'blocked' portals are red.
 * ------------------------------------------------------------------------
 */
void      N_RenderNavigationPortals(void *nav_private, const struct map *map, 
                                    mat4x4_t *chunk_model, int chunk_r, int chunk_c);

/* ------------------------------------------------------------------------
 * Make an impassable region in the cost field, completely covering the 
 * specified OBB.
 * ------------------------------------------------------------------------
 */
void      N_CutoutStaticObject(void *nav_private, vec3_t map_pos, const struct obb *obb);

/* ------------------------------------------------------------------------
 * Update portals and the links between them after there have been 
 * changes to the cost field, as new obstructions could have closed off 
 * paths or removed obstructions could have opened up new ones.
 * ------------------------------------------------------------------------
 */
void      N_UpdatePortals(void *nav_private);

/* ------------------------------------------------------------------------
 * Update the islands (sets of tiles which are reachable from one another)
 * information after there have been changes to the cost field.
 * ------------------------------------------------------------------------
 */
void      N_UpdateIslandsField(void *nav_private);

/* ------------------------------------------------------------------------
 * Generate the required flowfield and LOS sectors for moving towards the 
 * specified destination.
 * Returns true, if pathing is possible. In that case, 'out_dest_id' will
 * be set to a handle that can be used to query relevant fields.
 * ------------------------------------------------------------------------
 */
bool      N_RequestPath(void *nav_private, vec2_t xz_src, vec2_t xz_dest, 
                        vec3_t map_pos, dest_id_t *out_dest_id);

/* ------------------------------------------------------------------------
 * Returns the desired velocity for an entity at 'curr_pos' for it to flow
 * towards a particular destination.
 * ------------------------------------------------------------------------
 */
vec2_t    N_DesiredPointSeekVelocity(dest_id_t id, vec2_t curr_pos, vec2_t xz_dest, 
                                     void *nav_private, vec3_t map_pos);

/* ------------------------------------------------------------------------
 * Returns the desired velocity for an entity at 'curr_pos' for it to flow
 * towards the closest enemy units within the same chunk.
 * ------------------------------------------------------------------------
 */
vec2_t    N_DesiredEnemySeekVelocity(vec2_t curr_pos, void *nav_private, 
                                     vec3_t map_pos, int faction_id);

/* ------------------------------------------------------------------------
 * Returns true if the particular destination is in direct line of sight 
 * of the specified position.
 * ------------------------------------------------------------------------
 */
bool      N_HasDestLOS(dest_id_t id, vec2_t curr_pos, void *nav_private, vec3_t map_pos);

/* ------------------------------------------------------------------------
 * Returns true if the specified XZ position is pathable.
 * ------------------------------------------------------------------------
 */
bool      N_PositionPathable(vec2_t xz_pos, void *nav_private, vec3_t map_pos);

/* ------------------------------------------------------------------------
 * Returns the X and Z dimentions (in OpenGL coordinates) of a single 
 * navigation tile.
 * ------------------------------------------------------------------------
 */
vec2_t    N_TileDims(void);

/* ------------------------------------------------------------------------
 * Returns the worldspace XZ position of the closest reachable point on the
 * map to the specified destination. If the destination is reachable, it is
 * returned.
 * ------------------------------------------------------------------------
 */
vec2_t    N_ClosestReachableDest(void *nav_private, vec3_t map_pos, vec2_t xz_src, 
                                 vec2_t xz_dst);

/* ------------------------------------------------------------------------
 * Changes the blocker reference count for the navigation tile under the
 * cursor position. This may cause flow field eviction from caches.
 * ------------------------------------------------------------------------
 */
void      N_BlockersIncref(vec2_t xz_pos, float range, vec3_t map_pos, void *nav_private);
void      N_BlockersDecref(vec2_t xz_pos, float range, vec3_t map_pos, void *nav_private);

/* ------------------------------------------------------------------------
 * Returns true if the entity position (xz_pos) is within a 'tolerance' 
 * range of the closest non-blocked tile that is reachable from the
 * destination position (xz_dest).
 * ------------------------------------------------------------------------
 */
bool      N_IsMaximallyClose(void *nav_private, vec3_t map_pos, 
                             vec2_t xz_pos, vec2_t xz_dest, float tolerance);

/*###########################################################################*/
/* NAV FIELD CACHE                                                           */
/*###########################################################################*/

/* ------------------------------------------------------------------------
 * Reset the field cache performance counters.
 * ------------------------------------------------------------------------
 */
void      N_FC_ClearStats(void);

/* ------------------------------------------------------------------------
 * Get up-to-date performance counters for the field/path caches.
 * ------------------------------------------------------------------------
 */
void      N_FC_GetStats(struct fc_stats *out_stats);

/* ------------------------------------------------------------------------
 * Reset the contents of all the caches.
 * ------------------------------------------------------------------------
 */
void      N_FC_ClearAll(void);

/* ------------------------------------------------------------------------
 * Evict all enemy seeking flow from the flow field cache.
 * ------------------------------------------------------------------------
 */
void      N_FC_ClearAllEnemySeekFields(void);

#endif

