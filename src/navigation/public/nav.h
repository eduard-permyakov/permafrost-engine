/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2020 Eduard Permyakov 
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
struct map_resolution;
struct camera;

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

/* Pathfinding happens on a per-layer basis. Each layer has 
 * its' own view of the navigation state. For example, passages
 * that are blocked for 3x3 units may not be blocked for 1x1 
 * units, and so forth. Thus, different 'categories' of units
 * may take different paths to the same destination.
 */
enum nav_layer{
    NAV_LAYER_GROUND_1X1,
    NAV_LAYER_GROUND_3X3,
    NAV_LAYER_GROUND_5X5,
    NAV_LAYER_GROUND_7X7,
    NAV_LAYER_MAX,
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
 * Reset all internal navigation subsystem state.
 * ------------------------------------------------------------------------
 */
void      N_ClearState(void);

/* ------------------------------------------------------------------------
 * Return a new navigation context for a map, containing pathability
 * information. 'w' and 'h' are the number of chunk columns/rows per map.
 * 'chunk_tiles' holds pointers to tile arrays for every chunk, in 
 * row-major order.
 * ------------------------------------------------------------------------
 */
void     *N_BuildForMapData(size_t w, size_t h, size_t chunk_w, size_t chunk_h,
                            const struct tile **chunk_tiles, bool update);

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
                                const struct map *map, int chunk_r, int chunk_c,
                                enum nav_layer layer);

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
                                 enum nav_layer layer, int faction_id);

/* ------------------------------------------------------------------------
 * Debug rendering of the fields that guide enemies towards an entity.
 * ------------------------------------------------------------------------
 */
void      N_RenderSurroundField(void *nav_private, const struct map *map, 
                                mat4x4_t *chunk_model, int chunk_r, int chunk_c, 
                                enum nav_layer layer, const struct entity *ent);

/* ------------------------------------------------------------------------
 * Debug rendering to show which navigation tiles are currently occupied
 * by 'stationary' entities. Occupied tiles will be red, others will be green.
 * ------------------------------------------------------------------------
 */
void      N_RenderNavigationBlockers(void *nav_private, const struct map *map, 
                                     mat4x4_t *chunk_model, int chunk_r, int chunk_c,
                                     enum nav_layer layer);

/* ------------------------------------------------------------------------
 * Render the tiles under the OBB on which buildings can be placed. Buildable
 * tiles are those that are pathable, explored by the player, and not currently
 * blocked.
 * ------------------------------------------------------------------------
 */
void      N_RenderBuildableTiles(void *nav_private, const struct map *map, 
                                 mat4x4_t *chunk_model, int chunk_r, int chunk_c,
                                 const struct obb *obb, enum nav_layer layer);

/* ------------------------------------------------------------------------
 * Debug rendering to show the portals between chunks. 'Active' portals
 * are green and 'blocked' portals are red.
 * ------------------------------------------------------------------------
 */
void      N_RenderNavigationPortals(void *nav_private, const struct map *map, 
                                    mat4x4_t *chunk_model, int chunk_r, int chunk_c,
                                    enum nav_layer layer);

/* ------------------------------------------------------------------------
 * Debug rendering to show the global island ID over every navigation tile.
 * ------------------------------------------------------------------------
 */
void      N_RenderIslandIDs(void *nav_private, const struct map *map, 
                            const struct camera *cam, mat4x4_t *chunk_model, 
                            int chunk_r, int chunk_c, enum nav_layer layer);

/* ------------------------------------------------------------------------
 * Debug rendering to show the local island ID over every navigation tile.
 * ------------------------------------------------------------------------
 */
void      N_RenderLocalIslandIDs(void *nav_private, const struct map *map, 
                                 const struct camera *cam, mat4x4_t *chunk_model, 
                                 int chunk_r, int chunk_c, enum nav_layer layer);

/* ------------------------------------------------------------------------
 * Make an impassable region in the cost field, completely covering the 
 * specified OBB.
 * ------------------------------------------------------------------------
 */
void      N_CutoutStaticObject(void *nav_private, vec3_t map_pos, 
                               const struct obb *obb);

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
 * Returns a unique ID that is used to associated all flow fields guiding 
 * to this (at tile granularity) position.
 * ------------------------------------------------------------------------
 */
dest_id_t N_DestIDForPos(void *nav_private, vec3_t map_pos, vec2_t xz_pos, 
                         enum nav_layer layer);

/* ------------------------------------------------------------------------
 * Like N_DestIDForPos, but the path for this ID will not consider combatable
 * entities that are enemies of specified faction as blockers. (It is expected 
 * that entities following these paths will attack any nearby entities.
 * ------------------------------------------------------------------------
 */
dest_id_t N_DestIDForPosAttacking(void *nav_private, vec3_t map_pos, vec2_t xz_pos, 
                                  enum nav_layer layer, int faction_id);

/* ------------------------------------------------------------------------
 * Returns true if the specified path ID corresponds to an 'attacking' path.
 * ------------------------------------------------------------------------
 */
bool     N_DestIDIsAttacking(dest_id_t id);

/* ------------------------------------------------------------------------
 * Generate the required flowfield and LOS sectors for moving towards the 
 * specified destination.
 * Returns true, if pathing is possible. In that case, 'out_dest_id' will
 * be set to a handle that can be used to query relevant fields.
 * ------------------------------------------------------------------------
 */
bool      N_RequestPath(void *nav_private, vec2_t xz_src, 
                        vec2_t xz_dest, vec3_t map_pos, 
                        enum nav_layer layer, dest_id_t *out_dest_id);

/* ------------------------------------------------------------------------
 * Like N_RequestPath, but the path for this ID will not consider combatable
 * entities that are enemies of specified faction as blockers. (It is expected 
 * that entities following these paths will attack any nearby entities.
 * ------------------------------------------------------------------------
 */
bool      N_RequestPathAttacking(void *nav_private, vec2_t xz_src, 
                                 vec2_t xz_dest, int faction_id,
                                 vec3_t map_pos, enum nav_layer layer, 
                                 dest_id_t *out_dest_id);

/* ------------------------------------------------------------------------
 * Returns the desired velocity for an entity at 'curr_pos' for it to flow
 * towards a particular destination.
 * ------------------------------------------------------------------------
 */
vec2_t    N_DesiredPointSeekVelocity(dest_id_t id, vec2_t curr_pos, vec2_t xz_dest, 
                                     void *nav_private, vec3_t map_pos);

/* ------------------------------------------------------------------------
 * Returns the desired velocity for an entity at 'curr_pos' for it to flow
 * towards the closest enemy units.
 * ------------------------------------------------------------------------
 */
vec2_t    N_DesiredEnemySeekVelocity(vec2_t curr_pos, void *nav_private, 
                                     enum nav_layer layer, vec3_t map_pos, 
                                     int faction_id);

/* ------------------------------------------------------------------------
 * Returns the desired velocity for an entity at 'curr_pos' for it to flow
 * towards the the specified entity. The flow field will be NULL if this is
 * outside a chunk-sized box centered at the enemy position. This is for
 * fine-tuned movement when we are already close to the target.
 * ------------------------------------------------------------------------
 */
vec2_t N_DesiredSurroundVelocity(vec2_t curr_pos, void *nav_private, enum nav_layer layer, 
                                 vec3_t map_pos, const struct entity *ent, int faction_id);

/* ------------------------------------------------------------------------
 * Returns true if the particular entity is in direct line of sight of the 
 * specified position.
 * ------------------------------------------------------------------------
 */
bool      N_HasEntityLOS(vec2_t curr_pos, const struct entity *ent, 
                         void *nav_private, enum nav_layer layer, 
                         vec3_t map_pos);

/* ------------------------------------------------------------------------
 * Returns true if the particular destination is in direct line of sight 
 * of the specified position.
 * ------------------------------------------------------------------------
 */
bool      N_HasDestLOS(dest_id_t id, vec2_t curr_pos, void *nav_private, 
                       vec3_t map_pos);

/* ------------------------------------------------------------------------
 * Returns true if the specified XZ position is pathable.
 * ------------------------------------------------------------------------
 */
bool      N_PositionPathable(vec2_t xz_pos, enum nav_layer layer, 
                             void *nav_private, vec3_t map_pos);

/* ------------------------------------------------------------------------
 * Returns true if the specified XZ position is currently blocked.
 * ------------------------------------------------------------------------
 */
bool      N_PositionBlocked(vec2_t xz_pos, enum nav_layer layer, 
                            void *nav_private, vec3_t map_pos);

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
vec2_t    N_ClosestReachableDest(void *nav_private, enum nav_layer layer, 
                                 vec3_t map_pos, vec2_t xz_src, vec2_t xz_dst);

/* ------------------------------------------------------------------------
 * If true is returned, 'out' is set to the worldspace XZ position of the 
 * closest reachable point on the map that is adjacent to the specified entity.
 * ------------------------------------------------------------------------
 */
bool N_ClosestReachableAdjacentPosStatic(void *nav_private, enum nav_layer layer, 
                                         vec3_t map_pos, vec2_t xz_src, 
                                         const struct obb *target, vec2_t *out);

bool N_ClosestReachableAdjacentPosDynamic(void *nav_private, enum nav_layer layer, 
                                          vec3_t map_pos, vec2_t xz_src, vec2_t xz_pos, 
                                          float radius, vec2_t *out);

/* ------------------------------------------------------------------------
 * If true is returned, 'out' is set to the worldspace XZ position of the 
 * closest pathable point on the map to the specified position.
 * ------------------------------------------------------------------------
 */
bool N_ClosestPathable(void *nav_private, enum nav_layer layer, 
                       vec3_t map_pos, vec2_t xz_src, vec2_t *out);

/* ------------------------------------------------------------------------
 * Returns 'true' if the two locations are currenntly reachable from one another.
 * ------------------------------------------------------------------------
 */
bool N_LocationsReachable(void *nav_private, enum nav_layer layer, 
                          vec3_t map_pos, vec2_t a, vec2_t b);

/* ------------------------------------------------------------------------
 * Changes the blocker reference count for the navigation tile under the
 * cursor position. This may cause flow field eviction from caches.
 * The faction_id is for keeping track of which factions are currently
 * 'occupying' which tiles.
 * ------------------------------------------------------------------------
 */
void      N_BlockersIncref(vec2_t xz_pos, float range, int faction_id, 
                           vec3_t map_pos, void *nav_private);
void      N_BlockersDecref(vec2_t xz_pos, float range, int faction_id, 
                           vec3_t map_pos, void *nav_private);

void      N_BlockersIncrefOBB(void *nav_private, int faction_id, 
                              vec3_t map_pos, const struct obb *obb);
void      N_BlockersDecrefOBB(void *nav_private, int faction_id, 
                              vec3_t map_pos, const struct obb *obb);

/* ------------------------------------------------------------------------
 * Returns true if the entity position (xz_pos) is within a 'tolerance' 
 * range of the closest non-blocked tile that is reachable from the
 * destination position (xz_dest).
 * ------------------------------------------------------------------------
 */
bool      N_IsMaximallyClose(void *nav_private, enum nav_layer layer, 
                             vec3_t map_pos, vec2_t xz_pos, 
                             vec2_t xz_dest, float tolerance);

/* ------------------------------------------------------------------------
 * Returns true if the tile under the position or any tile having a Manhattan
 * distance of 1 is not currently pathable.
 * ------------------------------------------------------------------------
 */
bool      N_IsAdjacentToImpassable(void *nav_private, enum nav_layer layer, 
                                   vec3_t map_pos, vec2_t xz_pos);

/* ------------------------------------------------------------------------
 * Returns true if the tiles under the entity selection cirlce overlap or 
 * share an edge with any of the tiles under the OBB.
 * ------------------------------------------------------------------------
 */
bool      N_ObjAdjacentToStatic(void *nav_private, vec3_t map_pos, 
                                const struct entity *ent, const struct obb *stat);

/* ------------------------------------------------------------------------
 * Returns true if the tiles under the entity selection cirlce overlap or 
 * share an edge with any of the tiles under the other entity's selection circle.
 * ------------------------------------------------------------------------
 */
bool      N_ObjAdjacentToDynamic(void *nav_private, vec3_t map_pos, 
                                 const struct entity *ent, vec2_t xz_pos, 
                                 float radius);

/* ------------------------------------------------------------------------
 * Get the resolution (chunks, tiles) of the navigation data.
 * ------------------------------------------------------------------------
 */
void      N_GetResolution(void *nav_private, struct map_resolution *out);

/* ------------------------------------------------------------------------
 * Returns true if the building with the specified OBB can be successfully
 * placed, meaning all tiles under it are pathable, explored by the player, 
 * and not currently blocked.
 * ------------------------------------------------------------------------
 */
bool      N_ObjectBuildable(void *nav_private, enum nav_layer layer, 
                            vec3_t map_pos, const struct obb *obb);

/* ------------------------------------------------------------------------
 * Will return the XZ map position that the entity at the source location
 * should travel to in order to get in range of the target position using
 * the shortest path.
 * ------------------------------------------------------------------------
 */
vec2_t N_ClosestReachableInRange(void *nav_private, vec3_t map_pos, 
                                 vec2_t xz_src, vec2_t xz_target, 
                                 float range, enum nav_layer layer);

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

#endif

