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

/*###########################################################################*/
/* NAV GENERAL                                                               */
/*###########################################################################*/

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
 * Generate the required flowfield and LOS sectors for moving towards the 
 * specified destination.
 * Returns true, if pathing is possible. In that case, 'out_dest_id' will
 * be set to a handle that can be used to query relevant fields.
 * ------------------------------------------------------------------------
 */
bool      N_RequestPath(void *nav_private, struct entity *ent, vec2_t xz_dest, 
                        vec3_t map_pos, dest_id_t *out_dest_id);

/* ------------------------------------------------------------------------
 * Returns the desired velocity for an entity at 'curr_pos' for it to flow
 * towards a particular destination.
 * ------------------------------------------------------------------------
 */
vec2_t    N_DesiredVelocity(dest_id_t id, vec2_t curr_pos);

#endif

