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

#define TILES_PER_CHUNK_HEIGHT 32
#define TILES_PER_CHUNK_WIDTH  32

enum tiletype{
    /* TILETYPE_FLAT:
     *                     +----------+
     *                    /          /|
     *                -  +----------+ +
     * base_height -> |  |          |/
     *                -  +----------+
     */
    TILETYPE_FLAT              = 0x0,
    /* By convention, the second letter (ex. 'N' in 'SN') is the raised end */
    TILETYPE_RAMP_SN           = 0x1,
    TILETYPE_RAMP_NS           = 0x2,
    TILETYPE_RAMP_EW           = 0x3,
    TILETYPE_RAMP_WE           = 0x4,
    /* For corners, the direction in the name is that of the central lowered corner */
    TILETYPE_CORNER_CONCAVE_SW = 0x5,
    TILETYPE_CORNER_CONVEX_SW  = 0x6,
    TILETYPE_CORNER_CONCAVE_SE = 0x7,
    TILETYPE_CORNER_CONVEX_SE  = 0x8,
    TILETYPE_CORNER_CONCAVE_NW = 0x9,
    TILETYPE_CORNER_CONVEX_NW  = 0xa,
    TILETYPE_CORNER_CONCAVE_NE = 0xb,
    TILETYPE_CORNER_CONVEX_NE  = 0xc,
};

enum blend_mode{
    BLEND_MODE_NOBLEND = 0,
    BLEND_MODE_BLUR,
};

struct tile{
    bool          pathable;    
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


#define TILETYPE_IS_RAMP(t) \
    (  ((t) == TILETYPE_RAMP_SN ) \
    || ((t) == TILETYPE_RAMP_NS ) \
    || ((t) == TILETYPE_RAMP_EW ) \
    || ((t) == TILETYPE_RAMP_WE))


#define TILETYPE_IS_CORNER_CONCAVE(t) \
    (  ((t) == TILETYPE_CORNER_CONCAVE_SW ) \
    || ((t) == TILETYPE_CORNER_CONCAVE_SE ) \
    || ((t) == TILETYPE_CORNER_CONCAVE_NW ) \
    || ((t) == TILETYPE_CORNER_CONCAVE_NE))

#define TILETYPE_IS_CORNER_CONVEX(t) \
    (  ((t) == TILETYPE_CORNER_CONVEX_SW ) \
    || ((t) == TILETYPE_CORNER_CONVEX_SE ) \
    || ((t) == TILETYPE_CORNER_CONVEX_NW ) \
    || ((t) == TILETYPE_CORNER_CONVEX_NE))


int  M_Tile_NWHeight(const struct tile *tile);
int  M_Tile_NEHeight(const struct tile *tile);
int  M_Tile_SWHeight(const struct tile *tile);
int  M_Tile_SEHeight(const struct tile *tile);

bool M_Tile_FrontFaceVisible(const struct tile *tiles, int r, int c);
bool M_Tile_BackFaceVisible (const struct tile *tiles, int r, int c);
bool M_Tile_LeftFaceVisible (const struct tile *tiles, int r, int c);
bool M_Tile_RightFaceVisible(const struct tile *tiles, int r, int c);

#endif
