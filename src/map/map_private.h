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

#ifndef MAP_PRIVATE_H
#define MAP_PRIVATE_H

#include "pfchunk.h"
#include "../pf_math.h"

struct map{
    /* ------------------------------------------------------------------------
     * Map dimensions in numbers of chunks.
     * ------------------------------------------------------------------------
     */
    size_t width, height;
    /* ------------------------------------------------------------------------
     * World-space location of the top left corner of the map.
     * ------------------------------------------------------------------------
     */
    vec3_t pos;
    /* ------------------------------------------------------------------------
     * The map chunks stored in row-major order. In total, there must be 
     * (width * height) number of chunks.
     * ------------------------------------------------------------------------
     */
    struct pfchunk chunks[];
};

struct chunkpos{
    int r, c;
};

void M_ModelMatrixForChunk(const struct map *map, struct chunkpos p, mat4x4_t *out);

#endif
