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
#include "../pf_math.h"

#include <stdbool.h>

struct pfchunk{
    /* ------------------------------------------------------------------------
     * Initialized and used by the rendering subsystem. Holds the mesh data 
     * and everything the rendering subsystem needs to render this PFChunk.
     * ------------------------------------------------------------------------
     */
    void           *render_private;
    /* ------------------------------------------------------------------------
     * Pointers to adjacent chunks. NULL if there are none.
     * ------------------------------------------------------------------------
     */
    struct pfchunk *north, *south, *east, *west;
    /* ------------------------------------------------------------------------
     * Worldspace position of the top left corner. 
     * ------------------------------------------------------------------------
     */
    vec3_t          position;
    /* ------------------------------------------------------------------------
     * The 'has_path' bools are precomputed when the pfchunk is initialized or 
     * updated.
     *
     * They are used later to quickly query if we can reach onother chunk 
     * adjacent to this one. If not, there is no point looking at the tile or 
     * subtile resolution.
     *
     * Note that having a North->South path implies that there is also a 
     * South->North path and so on.
     * ------------------------------------------------------------------------
     */
    bool            has_ns_path;
    bool            has_ew_path;
    /* ------------------------------------------------------------------------
     * Each tiles' attributes, stored in row-major order.
     * ------------------------------------------------------------------------
     */
    struct tile     tiles[TILES_PER_CHUNK_HEIGHT * TILES_PER_CHUNK_WIDTH];
};

#endif
