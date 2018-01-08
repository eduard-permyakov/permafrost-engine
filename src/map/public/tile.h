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

#ifndef TILE_H
#define TILE_H

#include <stdbool.h>

#define X_COORDS_PER_TILE 8 
#define Y_COORDS_PER_TILE 4 
#define Z_COORDS_PER_TILE 8 

enum tiletype{
    /* TILETYPE_FLAT:
     *                     +----------+
     *                    /          /|
     *                -  +----------+ +
     * base_height -> |  |          |/
     *                -  +----------+
     */
    TILETYPE_FLAT              = 0,
    /* By convention, the second letter (ex. 'N' in 'SN') is the raised end */
    TILETYPE_RAMP_SN           = 1,
    TILETYPE_RAMP_NS           = 2,
    TILETYPE_RAMP_EW           = 3,
    TILETYPE_RAMP_WE           = 4,
    /* For corners, the direction in the name is that of the central lowered corner */
    TILETYPE_CORNER_CONCAVE_SW = 5,
    TILETYPE_CORNER_CONVEX_SW  = 6,
    TILETYPE_CORNER_CONCAVE_SE = 7,
    TILETYPE_CORNER_CONVEX_SE  = 8,
};

struct tile{
    /* ------------------------------------------------------------------------
     * 'pathable' is only valid when subtile_resolution is false. This means 
     * that all subtiles for this tile have the same pathability held in this 
     * tile's 'pathable' property. Otherwise, the subtiles have different 
     * pathability, which we must read from the subtile grid. 
     *
     * TODO: subtile grid
     * ------------------------------------------------------------------------
     */
    bool          pathable;    
    bool          subtile_resolution;
    enum tiletype type;
    int           base_height;
    /* ------------------------------------------------------------------------
     * Only valid when 'type' is a ramp or corner tile.
     * ------------------------------------------------------------------------
     */
    int           ramp_height;
    /* ------------------------------------------------------------------------
     * Render-specific tile attributes. Only used for populating private render
     * data.
     * ------------------------------------------------------------------------
     */
    int           top_mat_idx;
    int           sides_mat_idx;
};

#endif
