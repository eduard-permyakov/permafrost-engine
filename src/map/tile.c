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
#include "public/map.h"
#include "../pf_math.h"
#include "../collision.h"

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

float M_Tile_HeightAtPos(const struct tile *tile, float frac_width, float frac_height)
{
    if(tile->type == TILETYPE_FLAT) {
        return tile->base_height * Y_COORDS_PER_TILE;

    }else if(TILETYPE_IS_RAMP(tile->type)) {
        return PFM_BilinearInterp(
            M_Tile_NWHeight(tile) * Y_COORDS_PER_TILE, M_Tile_SWHeight(tile) * Y_COORDS_PER_TILE,
            M_Tile_NEHeight(tile) * Y_COORDS_PER_TILE, M_Tile_SEHeight(tile) * Y_COORDS_PER_TILE,
            0.0f, 1.0f, 0.0f, 1.0f,
            frac_width, frac_height);

    }else /*corner tiles */ {

        /* For corner tiles, we break up the top face into two triangles, 
         * figure out which triangle the point is in, and determine the map 
         * height by finding the intersection point of a downward facing ray
         * and the plane of the triangle. */

        vec3_t corners[] = {
            (vec3_t){0.0f, M_Tile_NWHeight(tile) * Y_COORDS_PER_TILE, 0.0f},
            (vec3_t){1.0f, M_Tile_NEHeight(tile) * Y_COORDS_PER_TILE, 0.0f},
            (vec3_t){0.0f, M_Tile_SWHeight(tile) * Y_COORDS_PER_TILE, 1.0f},
            (vec3_t){1.0f, M_Tile_SEHeight(tile) * Y_COORDS_PER_TILE, 1.0f}
        };

        /* Triangles are defined in screen coordinates */
        vec3_t *first_tri, *second_tri;

        switch(tile->type){
        case TILETYPE_CORNER_CONVEX_NE:
        case TILETYPE_CORNER_CONCAVE_NE:
        case TILETYPE_CORNER_CONVEX_SW:
        case TILETYPE_CORNER_CONCAVE_SW: 
            {
                first_tri  = (vec3_t[3]){corners[1], corners[3], corners[0]};
                second_tri = (vec3_t[3]){corners[2], corners[0], corners[3]};
                break;
            }
        case TILETYPE_CORNER_CONVEX_NW:
        case TILETYPE_CORNER_CONCAVE_NW:
        case TILETYPE_CORNER_CONVEX_SE:
        case TILETYPE_CORNER_CONCAVE_SE:
            {
                first_tri  = (vec3_t[3]){corners[0], corners[1], corners[2]};
                second_tri = (vec3_t[3]){corners[3], corners[2], corners[1]};
                break;
            }
        default: assert(0);
        }

        vec3_t tri_normal;
        vec3_t tri_point;
        vec3_t edge1, edge2;

        if(C_PointInsideTriangle2D(
            (vec2_t){frac_width, frac_height}, 
            (vec2_t){first_tri[0].x, first_tri[0].z}, 
            (vec2_t){first_tri[1].x, first_tri[1].z}, 
            (vec2_t){first_tri[2].x, first_tri[2].z})) {

            PFM_Vec3_Sub(&first_tri[1], &first_tri[0], &edge1);
            PFM_Vec3_Sub(&first_tri[2], &first_tri[0], &edge2);
            tri_point = first_tri[0];

        }else{

            PFM_Vec3_Sub(&second_tri[1], &second_tri[0], &edge1);
            PFM_Vec3_Sub(&second_tri[2], &second_tri[0], &edge2);
            tri_point = second_tri[0];
        }

        PFM_Vec3_Cross(&edge2, &edge1, &tri_normal);
        PFM_Vec3_Normal(&tri_normal, &tri_normal);
        assert(tri_normal.y > 0.0f);

        struct plane tri_plane = (struct plane){
            .point = tri_point,
            .normal = tri_normal
        };

        vec3_t ray_origin = (vec3_t){frac_width, (MAX_HEIGHT_LEVEL * Y_COORDS_PER_TILE) + 10, frac_height};
        vec3_t ray_dir = (vec3_t){0.0f, -1.0f, 0.0f};

        float t;
        bool result = C_RayIntersectsPlane(ray_origin, ray_dir, tri_plane, &t);
        assert(result);

        vec3_t intersec_point;
        PFM_Vec3_Scale(&ray_dir, t, &ray_dir);
        PFM_Vec3_Add(&ray_origin, &ray_dir, &intersec_point);

        return intersec_point.y;
    }
}

