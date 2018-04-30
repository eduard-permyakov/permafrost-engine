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

#include "public/tile.h"

#include <assert.h>

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

int M_Tile_NWHeight(const struct tile *tile)
{
    bool top_nw_raised =  (tile->type == TILETYPE_RAMP_SN)
                       || (tile->type == TILETYPE_RAMP_EW)
                       || (tile->type == TILETYPE_CORNER_CONVEX_SW)
                       || (tile->type == TILETYPE_CORNER_CONVEX_SE)
                       || (tile->type == TILETYPE_CORNER_CONCAVE_SE)
                       || (tile->type == TILETYPE_CORNER_CONVEX_NE);

    if(top_nw_raised)
        return tile->base_height + tile->ramp_height;
    else
        return tile->base_height;
}

int M_Tile_NEHeight(const struct tile *tile)
{
    bool top_ne_raised =  (tile->type == TILETYPE_RAMP_SN)
                       || (tile->type == TILETYPE_RAMP_WE)
                       || (tile->type == TILETYPE_CORNER_CONVEX_SW)
                       || (tile->type == TILETYPE_CORNER_CONCAVE_SW)
                       || (tile->type == TILETYPE_CORNER_CONVEX_SE)
                       || (tile->type == TILETYPE_CORNER_CONVEX_NW);

    if(top_ne_raised)
        return tile->base_height + tile->ramp_height;
    else
        return tile->base_height;
}

int M_Tile_SWHeight(const struct tile *tile)
{
    bool top_sw_raised =  (tile->type == TILETYPE_RAMP_NS)
                       || (tile->type == TILETYPE_RAMP_EW)
                       || (tile->type == TILETYPE_CORNER_CONVEX_SE)
                       || (tile->type == TILETYPE_CORNER_CONVEX_NW)
                       || (tile->type == TILETYPE_CORNER_CONCAVE_NE)
                       || (tile->type == TILETYPE_CORNER_CONVEX_NE);

    if(top_sw_raised)
        return tile->base_height + tile->ramp_height;
    else
        return tile->base_height;
}

int M_Tile_SEHeight(const struct tile *tile)
{
    bool top_se_raised =  (tile->type == TILETYPE_RAMP_NS)
                       || (tile->type == TILETYPE_RAMP_WE)
                       || (tile->type == TILETYPE_CORNER_CONVEX_SW)
                       || (tile->type == TILETYPE_CORNER_CONVEX_NE)
                       || (tile->type == TILETYPE_CORNER_CONCAVE_NW)
                       || (tile->type == TILETYPE_CORNER_CONVEX_NW);

    if(top_se_raised)
        return tile->base_height + tile->ramp_height;
    else
        return tile->base_height;
}

bool M_Tile_FrontFaceVisible(const struct tile *tiles, int r, int c)
{
    assert(r >= 0 && r < TILES_PER_CHUNK_HEIGHT);
    assert(c >= 0 && c < TILES_PER_CHUNK_WIDTH);    

    if(r == TILES_PER_CHUNK_HEIGHT-1)
        return true;

    const struct tile *curr  = &tiles[r     * TILES_PER_CHUNK_HEIGHT + c]; 
    const struct tile *front = &tiles[(r+1) * TILES_PER_CHUNK_HEIGHT + c]; 

    return (M_Tile_SEHeight(curr) > M_Tile_NEHeight(front))
        || (M_Tile_SWHeight(curr) > M_Tile_NWHeight(front));
}

bool M_Tile_BackFaceVisible (const struct tile *tiles, int r, int c)
{
    assert(r >= 0 && r < TILES_PER_CHUNK_HEIGHT);    
    assert(c >= 0 && c < TILES_PER_CHUNK_WIDTH);    

    if(r == 0)
        return true;

    const struct tile *curr = &tiles[r     * TILES_PER_CHUNK_HEIGHT + c]; 
    const struct tile *back = &tiles[(r-1) * TILES_PER_CHUNK_HEIGHT + c]; 

    return (M_Tile_NEHeight(curr) > M_Tile_SEHeight(back))
        || (M_Tile_NWHeight(curr) > M_Tile_SWHeight(back));
}

bool M_Tile_LeftFaceVisible (const struct tile *tiles, int r, int c)
{
    assert(r >= 0 && r < TILES_PER_CHUNK_HEIGHT);    
    assert(c >= 0 && c < TILES_PER_CHUNK_WIDTH);    

    if(c == 0)
        return true;

    const struct tile *curr = &tiles[r * TILES_PER_CHUNK_HEIGHT + c    ]; 
    const struct tile *left = &tiles[r * TILES_PER_CHUNK_HEIGHT + (c-1)]; 

    return (M_Tile_NWHeight(curr) > M_Tile_NEHeight(left))
        || (M_Tile_SWHeight(curr) > M_Tile_SEHeight(left));
}

bool M_Tile_RightFaceVisible(const struct tile *tiles, int r, int c)
{
    assert(r >= 0 && r < TILES_PER_CHUNK_HEIGHT);    
    assert(c >= 0 && c < TILES_PER_CHUNK_WIDTH);    

    if(c == TILES_PER_CHUNK_WIDTH-1)
        return true;

    const struct tile *curr  = &tiles[r * TILES_PER_CHUNK_HEIGHT + c    ]; 
    const struct tile *right = &tiles[r * TILES_PER_CHUNK_HEIGHT + (c+1)]; 

    return (M_Tile_NEHeight(curr) > M_Tile_NWHeight(right))
        || (M_Tile_SEHeight(curr) > M_Tile_SWHeight(right));
}

