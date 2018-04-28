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

#ifndef PFCHUNK_H
#define PFCHUNK_H

#include "public/tile.h"
#include "public/map.h"
#include "../pf_math.h"

#include <stdbool.h>

struct pfchunk{

    enum chunk_render_mode mode;
    /* ------------------------------------------------------------------------
     * Initialized and used by the rendering subsystem. Holds the mesh data 
     * and everything the rendering subsystem needs to render this PFChunk.
     * 
     * There are two rendering contexts. The one that is used to render the 
     * chunk depends on the 'mode' attribute.
     * ------------------------------------------------------------------------
     */
    void           *render_private_tiles;
    void           *render_private_prebaked;
    /* ------------------------------------------------------------------------
     * Worldspace position of the top left corner. 
     * ------------------------------------------------------------------------
     */
    vec3_t          position;
    /* ------------------------------------------------------------------------
     * Each tiles' attributes, stored in row-major order.
     * ------------------------------------------------------------------------
     */
    struct tile     tiles[TILES_PER_CHUNK_HEIGHT * TILES_PER_CHUNK_WIDTH];
};

#endif
