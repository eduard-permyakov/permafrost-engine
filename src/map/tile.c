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

#include "public/tile.h"
#include "public/map.h"
#include "../pf_math.h"
#include "../collision.h"

#include <assert.h>
#include <string.h>


#define CHUNK_WIDTH  (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE)
#define CHUNK_HEIGHT (TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE)
#define EPSILON      (1.0f / 1024.0)

#define MIN(a, b)          ((a) < (b) ? (a) : (b))
#define MAX(a, b)          ((a) > (b) ? (a) : (b))
#define CLAMP(a, min, max) (MIN(MAX((a), (min)), (max)))

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static int mod(int a, int b)
{
    int r = a % b;
    return r < 0 ? r + b : r;
}

static float line_len(float ax, float az, float bx, float bz)
{
    return sqrt( pow(bx - ax, 2) + pow(bz - az, 2) );
}

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
        vec3_t *first_tri = NULL, *second_tri = NULL;

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
        (void)result;
        assert(result);

        vec3_t intersec_point;
        PFM_Vec3_Scale(&ray_dir, t, &ray_dir);
        PFM_Vec3_Add(&ray_origin, &ray_dir, &intersec_point);

        return intersec_point.y;
    }
}

struct box M_Tile_Bounds(struct map_resolution res, vec3_t map_pos, struct tile_desc desc)
{
    const int TILE_X_DIM = CHUNK_WIDTH / res.tile_w;
    const int TILE_Z_DIM = CHUNK_HEIGHT / res.tile_h;

    size_t width  = res.chunk_w * CHUNK_WIDTH;
    size_t height = res.chunk_h * CHUNK_HEIGHT;

    struct box map_box = (struct box){map_pos.x, map_pos.z, width, height};

    return (struct box){
        map_box.x - desc.chunk_c * (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE)
                  - desc.tile_c  * TILE_X_DIM,
        map_box.z + desc.chunk_r * (TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE)
                  + desc.tile_r * TILE_Z_DIM,
        TILE_X_DIM,
        TILE_Z_DIM 
    };
}

bool M_Tile_RelativeDesc(struct map_resolution res, struct tile_desc *inout, 
                         int tile_dc, int tile_dr)
{
    assert(abs(tile_dc) <= res.tile_w);
    assert(abs(tile_dr) <= res.tile_h);

    struct tile_desc ret = (struct tile_desc) {
        inout->chunk_r + ((inout->tile_r + tile_dr < 0)           ? -1 :
                          (inout->tile_r + tile_dr >= res.tile_h) ?  1 :
                                                                     0),

        inout->chunk_c + ((inout->tile_c + tile_dc < 0)            ? -1 :
                          (inout->tile_c + tile_dc >= res.tile_w)  ?  1 :
                                                                      0),
        mod(inout->tile_r + tile_dr, res.tile_h),
        mod(inout->tile_c + tile_dc, res.tile_w)
    };

    if( !(ret.chunk_r >= 0 && ret.chunk_r < res.chunk_h)
     || !(ret.chunk_c >= 0 && ret.chunk_c < res.chunk_w)) {
     
        return false;
    }

    *inout = ret;
    return true;
}

/* Uses a variant of the algorithm outlined here:
 * http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.42.3443&rep=rep1&type=pdf 
 * ('A Fast Voxel Traversal Algorithm for Ray Tracing' by John Amanatides, Andrew Woo)
 */
int M_Tile_LineSupercoverTilesSorted(struct map_resolution res, vec3_t map_pos, 
                                     struct line_seg_2d line, struct tile_desc out[])
{
    int ret = 0;

    const int TILE_X_DIM = CHUNK_WIDTH / res.tile_w;
    const int TILE_Z_DIM = CHUNK_HEIGHT / res.tile_h;

    /* Initialization: find the coordinate of the line segment origin within the map.
     * In the case of the line segmennt originating inside the map, this is simple - take the 
     * first point. In case the ray originates outside but intersects the map, we take the 
     * intersection point as the start. Lastly, if ray doesn't even intersect the map, no work 
     * needs to be done - return an empty list. 
     */
    size_t width  = res.chunk_w * CHUNK_WIDTH;
    size_t height = res.chunk_h * CHUNK_HEIGHT;
    struct box map_box = (struct box){map_pos.x, map_pos.z, width, height};

    vec2_t line_dir = (vec2_t){line.bx - line.ax, line.bz - line.az};
    PFM_Vec2_Normal(&line_dir, &line_dir);

    int num_intersect;
    vec2_t intersect_xz[2];
    float start_x, start_z;
    struct tile_desc curr_tile_desc;

    if(C_BoxPointIntersection(line.ax, line.az, map_box)) {

        start_x = line.ax; 
        start_z = line.az; 

    }else if((num_intersect = C_LineBoxIntersection(line, map_box, intersect_xz))) {

        /* Nudge the intersection point by EPSILON in the direction of the ray to make sure 
         * the intersection point is within the map bounds. */

        /* One intersection means that the end of the ray is inside the map */
        if(num_intersect == 1) {

            start_x = intersect_xz[0].x + EPSILON * line_dir.x;
            start_z = intersect_xz[0].z + EPSILON * line_dir.z;
        
        }
        /* If the first of the 2 intersection points is closer to the ray origin */
        else if( line_len(intersect_xz[0].x, intersect_xz[0].z, line.ax, line.az)
               < line_len(intersect_xz[1].x, intersect_xz[1].z, line.ax, line.az) ) {
         
            start_x = intersect_xz[0].x + EPSILON * line_dir.x;
            start_z = intersect_xz[0].z + EPSILON * line_dir.z;

         }else{
         
            start_x = intersect_xz[1].x + EPSILON * line_dir.x;
            start_z = intersect_xz[1].z + EPSILON * line_dir.z;
         }

    }else {
        return 0;
    }

    bool result = M_Tile_DescForPoint2D(res, map_pos, (vec2_t){start_x, start_z}, &curr_tile_desc);
    (void)result;
    assert(result);

    assert(curr_tile_desc.chunk_r >= 0 && curr_tile_desc.chunk_r < res.chunk_h);
    assert(curr_tile_desc.chunk_c >= 0 && curr_tile_desc.chunk_c < res.chunk_w);

    float t_max_x, t_max_z;

    const int step_c = line_dir.x <= 0.0f ? 1 : -1;
    const int step_r = line_dir.z >= 0.0f ? 1 : -1;

    const float t_delta_x = fabs(TILE_X_DIM / line_dir.x);
    const float t_delta_z = fabs(TILE_Z_DIM / line_dir.z);

    struct box bounds = M_Tile_Bounds(res, map_pos, curr_tile_desc);

    t_max_x = (step_c > 0) ? fabs(start_x - (bounds.x - bounds.width)) / fabs(line_dir.x) 
                           : fabs(start_x - bounds.x) / fabs(line_dir.x);

    t_max_z = (step_r > 0) ? fabs(start_z - (bounds.z + bounds.height)) / fabs(line_dir.z)
                           : fabs(start_z - bounds.z) / fabs(line_dir.z);

    bool line_ends_inside = C_BoxPointIntersection(line.bx, line.bz, map_box);
    struct tile_desc final_tile_desc;

    if(line_ends_inside) {
        int result = M_Tile_DescForPoint2D(res, map_pos, (vec2_t){line.bx, line.bz}, &final_tile_desc);
        (void)result;
        assert(result);
    }

    do{
        out[ret++] = curr_tile_desc;

        int dc = 0, dr = 0;
        if(t_max_x < t_max_z) {
            t_max_x = t_max_x + t_delta_x; 
            dc = step_c;
        }else{
            t_max_z = t_max_z + t_delta_z; 
            dr = step_r;
        }

        if(line_ends_inside && 0 == memcmp(&curr_tile_desc, &final_tile_desc, sizeof(struct tile_desc))){
            break;
        }

        if(!M_Tile_RelativeDesc(res, &curr_tile_desc, dc, dr)) {
            break;
        }

    }while(ret < 1024);
    assert(ret < 1024);

    return ret;
}

bool M_Tile_DescForPoint2D(struct map_resolution res, vec3_t map_pos, 
                           vec2_t point, struct tile_desc *out)
{
    const int TILE_X_DIM = CHUNK_WIDTH / res.tile_w;
    const int TILE_Z_DIM = CHUNK_HEIGHT / res.tile_h;

    size_t width  = res.chunk_w * CHUNK_WIDTH;
    size_t height = res.chunk_h * CHUNK_HEIGHT;
    struct box map_box = (struct box){map_pos.x, map_pos.z, width, height};

    /* Recall X increases to the left in our engine */
    if(point.x > map_box.x || point.x < map_box.x - map_box.width)
        return false;

    if(point.z < map_box.z || point.z > map_box.z + map_box.height)
        return false;

    int chunk_r, chunk_c;
    chunk_r = fabs(map_box.z - point.z) / CHUNK_HEIGHT;
    chunk_c = fabs(map_box.x - point.x) / CHUNK_WIDTH;

    /* Need to account for rounding imprecision when we're at the
     * razor's edge of the map. */
    chunk_r = CLAMP(chunk_r, 0, res.chunk_h-1);  
    chunk_c = CLAMP(chunk_c, 0, res.chunk_w-1);

    float chunk_base_x, chunk_base_z;
    chunk_base_x = map_box.x - (chunk_c * CHUNK_WIDTH);
    chunk_base_z = map_box.z + (chunk_r * CHUNK_HEIGHT);

    int tile_r, tile_c;
    tile_r = fabs(chunk_base_z - point.z) / TILE_Z_DIM;
    tile_c = fabs(chunk_base_x - point.x) / TILE_X_DIM;

    assert(tile_c >= 0 && tile_c < res.tile_w);
    assert(tile_r >= 0 && tile_r < res.tile_h);

    out->chunk_r = chunk_r;
    out->chunk_c = chunk_c;
    out->tile_r = tile_r;
    out->tile_c = tile_c;
    return true;
}

