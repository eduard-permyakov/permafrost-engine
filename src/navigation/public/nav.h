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

struct tile;
struct map;

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
void *N_BuildForMapData(size_t w, size_t h, 
                        size_t chunk_w, size_t chunk_h,
                        struct tile **chunk_tiles);

/* ------------------------------------------------------------------------
 * Clean up resources allocated by 'N_BuildForMapData'.
 * ------------------------------------------------------------------------
 */
void  N_FreePrivate(void *nav_private);

/* ------------------------------------------------------------------------
 * Draw a translucent overlay over the map chunk, showing the pathable and 
 * non-pathable regions. 'chunk_x_dim' and 'chunk_z_dim' are the chunk
 * dimensions in OpenGL coordinates.
 * ------------------------------------------------------------------------
 */
void  N_RenderPathableChunk(void *nav_private, mat4x4_t *chunk_model,
                            const struct map *map,
                            int chunk_r, int chunk_c);

#endif

