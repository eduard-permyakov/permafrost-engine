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
#include "../perf.h"
#include "../lib/public/pf_string.h"
#include "../phys/public/collision.h"

#include <assert.h>
#include <string.h>
#include <limits.h>


#define CHUNK_WIDTH  (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE)
#define CHUNK_HEIGHT (TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE)
#define EPSILON      (1.0f / 1024.0)

#define ARR_SIZE(a)        (sizeof(a)/sizeof(a[0]))
#define MIN(a, b)          ((a) < (b) ? (a) : (b))
#define MAX(a, b)          ((a) > (b) ? (a) : (b))
#define CLAMP(a, min, max) (MIN(MAX((a), (min)), (max)))

#define MAX_TILES_PER_LINE (128)

struct row_desc{
    int chunk_r;
    int tile_r;
};

struct col_desc{
    int chunk_c;
    int tile_c;
};

#define HIGHER(tile_desc, row_desc) \
       ((tile_desc).chunk_r < (row_desc).chunk_r) \
    || ((tile_desc).chunk_r == (row_desc).chunk_r && (tile_desc).tile_r < (row_desc).tile_r)

#define LOWER(tile_desc, row_desc) \
       ((tile_desc).chunk_r > (row_desc).chunk_r) \
    || ((tile_desc).chunk_r == (row_desc).chunk_r && (tile_desc).tile_r > (row_desc).tile_r)

#define MORE_LEFT(tile_desc, col_desc) \
       ((tile_desc).chunk_c < (col_desc).chunk_c) \
    || ((tile_desc).chunk_c == (col_desc).chunk_c && (tile_desc).tile_c < (col_desc).tile_c)

#define MORE_RIGHT(tile_desc, col_desc) \
       ((tile_desc).chunk_c > (col_desc).chunk_c) \
    || ((tile_desc).chunk_c == (col_desc).chunk_c && (tile_desc).tile_c > (col_desc).tile_c)

#define MIN_ROW(a, b)          (HIGHER((a), (b)) ? (a) : (b))
#define MIN_ROW_4(a, b, c, d)  (MIN_ROW(MIN_ROW((a), (b)), MIN_ROW((c), (d))))

#define MAX_ROW(a, b)          (LOWER((a), (b)) ? (a) : (b))
#define MAX_ROW_4(a, b, c, d)  (MAX_ROW(MAX_ROW((a), (b)), MAX_ROW((c), (d))))

#define MIN_COL(a, b)          (MORE_LEFT((a), (b)) ? (a) : (b))
#define MIN_COL_4(a, b, c, d)  (MIN_COL(MIN_COL((a), (b)), MIN_COL((c), (d))))

#define MAX_COL(a, b)          (MORE_RIGHT((a), (b)) ? (a) : (b))
#define MAX_COL_4(a, b, c, d)  (MAX_COL(MAX_COL((a), (b)), MAX_COL((c), (d))))

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

int M_Tile_BaseHeight(const struct tile *tile)
{
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
        vec3_t first_tri[3] = {0}, second_tri[3] = {0};

        switch(tile->type){
        case TILETYPE_CORNER_CONVEX_NE:
        case TILETYPE_CORNER_CONCAVE_NE:
        case TILETYPE_CORNER_CONVEX_SW:
        case TILETYPE_CORNER_CONCAVE_SW: 
            {
                first_tri[0] = corners[1];
                first_tri[1] = corners[3];
                first_tri[2] = corners[0];

                second_tri[0] = corners[2];
                second_tri[1] = corners[0];
                second_tri[2] = corners[3];
                break;
            }
        case TILETYPE_CORNER_CONVEX_NW:
        case TILETYPE_CORNER_CONCAVE_NW:
        case TILETYPE_CORNER_CONVEX_SE:
        case TILETYPE_CORNER_CONCAVE_SE:
            {
                first_tri[0] = corners[0];
                first_tri[1] = corners[1];
                first_tri[2] = corners[2];

                second_tri[0] = corners[3];
                second_tri[1] = corners[2];
                second_tri[2] = corners[1];
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

struct box M_Tile_ChunkBounds(struct map_resolution res, vec3_t map_pos, int chunk_r, int chunk_c)
{
    size_t width  = res.chunk_w * CHUNK_WIDTH;
    size_t height = res.chunk_h * CHUNK_HEIGHT;

    struct box map_box = (struct box){map_pos.x, map_pos.z, width, height};

    return (struct box){
        map_box.x - chunk_c * CHUNK_WIDTH,
        map_box.z + chunk_r * CHUNK_HEIGHT,
        CHUNK_WIDTH,
        CHUNK_HEIGHT
    };
}

bool M_Tile_RelativeDesc(struct map_resolution res, struct tile_desc *inout, 
                         int tile_dc, int tile_dr)
{
    int abs_r = inout->chunk_r * res.tile_h + inout->tile_r + tile_dr;
    int abs_c = inout->chunk_c * res.tile_w + inout->tile_c + tile_dc;

    int max_r = res.chunk_h * res.tile_h;
    int max_c = res.chunk_w * res.tile_w;

    if(abs_r < 0 || abs_r >= max_r)
        return false;
    if(abs_c < 0 || abs_c >= max_c)
        return false;

    *inout = (struct tile_desc) {
        abs_r / res.tile_h,
        abs_c / res.tile_w,
        abs_r % res.tile_h,
        abs_c % res.tile_w,
    };
    return true;
}

void M_Tile_Distance(struct map_resolution res, struct tile_desc *a, struct tile_desc *b, 
                     int *out_r, int *out_c)
{
    int a_abs_r = a->chunk_r * res.tile_h + a->tile_r;
    int a_abs_c = a->chunk_c * res.tile_w + a->tile_c;
    int b_abs_r = b->chunk_r * res.tile_h + b->tile_r;
    int b_abs_c = b->chunk_c * res.tile_w + b->tile_c;

    *out_r = b_abs_r - a_abs_r;
    *out_c = b_abs_c - a_abs_c;
}

/* Uses a variant of the algorithm outlined here:
 * http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.42.3443&rep=rep1&type=pdf 
 * ('A Fast Voxel Traversal Algorithm for Ray Tracing' by John Amanatides, Andrew Woo)
 */
int M_Tile_LineSupercoverTilesSorted(struct map_resolution res, vec3_t map_pos, 
                                     struct line_seg_2d line, struct tile_desc out[], size_t maxout)
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
    if(!result)
        return 0;

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

    }while(ret < maxout);

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

    tile_r = CLAMP(tile_r, 0, res.tile_h-1);
    tile_c = CLAMP(tile_c, 0, res.tile_w-1);

    assert(tile_c >= 0 && tile_c < res.tile_w);
    assert(tile_r >= 0 && tile_r < res.tile_h);

    out->chunk_r = chunk_r;
    out->chunk_c = chunk_c;
    out->tile_r = tile_r;
    out->tile_c = tile_c;
    return true;
}

size_t M_Tile_AllUnderObj(vec3_t map_pos, struct map_resolution res, const struct obb *obb,
                          struct tile_desc *out, size_t maxout)
{
    size_t ret = 0;

    vec3_t bot_corners[4] = {obb->corners[0], obb->corners[1], obb->corners[5], obb->corners[4]};
    vec2_t bot_corners_2d[4] = {
        (vec2_t){bot_corners[0].x, bot_corners[0].z},
        (vec2_t){bot_corners[1].x, bot_corners[1].z},
        (vec2_t){bot_corners[2].x, bot_corners[2].z},
        (vec2_t){bot_corners[3].x, bot_corners[3].z},
    };
    struct line_seg_2d xz_line_segs[4] = {
        (struct line_seg_2d){bot_corners[0].x, bot_corners[0].z, bot_corners[1].x, bot_corners[1].z},
        (struct line_seg_2d){bot_corners[1].x, bot_corners[1].z, bot_corners[2].x, bot_corners[2].z},
        (struct line_seg_2d){bot_corners[2].x, bot_corners[2].z, bot_corners[3].x, bot_corners[3].z},
        (struct line_seg_2d){bot_corners[3].x, bot_corners[3].z, bot_corners[0].x, bot_corners[0].z},
    };

    struct row_desc min_rows[4], max_rows[4];
    struct col_desc min_cols[4], max_cols[4];

    /* For each line segment of the bottom face of the OBB, find the supercover (set of all 
     * tiles which intersect the line segment). Keep track of the top-most, bottom-most,
     * left-most and right-most tiles of the outline.
     */
    for(int i = 0; i < ARR_SIZE(xz_line_segs); i++) {

        min_rows[i] = (struct row_desc){res.chunk_h-1, res.tile_h-1};
        max_rows[i] = (struct row_desc){0,0};
        min_cols[i] = (struct col_desc){res.chunk_w-1, res.chunk_w-1};
        max_cols[i] = (struct col_desc){0,0};

        size_t num_tiles = M_Tile_LineSupercoverTilesSorted(res, map_pos, xz_line_segs[i], out + ret, maxout - ret);
        const struct tile_desc *descs = out + ret;
        ret += num_tiles;

        if(ret == maxout)
            goto done;

        for(int j = 0; j < num_tiles; j++) {

            struct row_desc rd = (struct row_desc){descs[j].chunk_r, descs[j].tile_r};
            struct col_desc cd = (struct col_desc){descs[j].chunk_c, descs[j].tile_c};

            min_rows[i] = MIN_ROW(min_rows[i], rd);
            max_rows[i] = MAX_ROW(max_rows[i], rd);
            min_cols[i] = MIN_COL(min_cols[i], cd);
            max_cols[i] = MAX_COL(max_cols[i], cd);
        }
    }

    struct row_desc min_row = MIN_ROW_4(min_rows[0], min_rows[1], min_rows[2], min_rows[3]);
    struct row_desc max_row = MAX_ROW_4(max_rows[0], max_rows[1], max_rows[2], max_rows[3]);

    struct col_desc min_col = MIN_COL_4(min_cols[0], min_cols[1], min_cols[2], min_cols[3]);
    struct col_desc max_col = MAX_COL_4(max_cols[0], max_cols[1], max_cols[2], max_cols[3]);

    /* Now iterate over the square region of tiles defined by the extrema of the outline 
     * and check whether the tiles of this box fall within the OBB and should have their
     * cost updated. 
     */
    for(int r = min_row.chunk_r * res.tile_h + min_row.tile_r; 
            r < max_row.chunk_r * res.tile_h + max_row.tile_r; r++) {
        for(int c = min_col.chunk_c * res.tile_w + min_col.tile_c; 
                c < max_col.chunk_c * res.tile_w + max_col.tile_c; c++) {

            struct tile_desc desc = {
                .chunk_r = r / res.tile_h,
                .chunk_c = c / res.tile_w,
                .tile_r  = r % res.tile_h,
                .tile_c  = c % res.tile_w,
            };
            struct box bounds = M_Tile_Bounds(res, map_pos, desc);
            vec2_t center = (vec2_t){
                bounds.x - bounds.width/2.0f,
                bounds.z + bounds.height/2.0f
            };

            if(C_PointInsideRect2D(center, bot_corners_2d[0], bot_corners_2d[1], 
                bot_corners_2d[2], bot_corners_2d[3])) {

                out[ret++] = desc;
                if(ret == maxout)
                    goto done;
            }
        }
    }

done:
    return ret;
}

size_t M_Tile_AllUnderCircle(struct map_resolution res, vec2_t xz_center, float radius,
                             vec3_t map_pos, struct tile_desc *out, size_t maxout)
{
    struct tile_desc tile;
    bool result = M_Tile_DescForPoint2D(res, map_pos, xz_center, &tile);
    assert(result);

    const int TILE_X_DIM = CHUNK_WIDTH / res.tile_w;
    const int TILE_Z_DIM = CHUNK_HEIGHT / res.tile_h;
    int tile_len = MAX(TILE_X_DIM, TILE_Z_DIM);
    int ntiles = ceil(radius / tile_len);

    size_t ret = 0;

    for(int dr = -ntiles; dr <= ntiles; dr++) {
    for(int dc = -ntiles; dc <= ntiles; dc++) {

        struct tile_desc curr = tile;
        if(!M_Tile_RelativeDesc(res, &curr, dc, dr))
            continue;

        struct box bounds = M_Tile_Bounds(res, map_pos, curr);
        if(!C_CircleRectIntersection(xz_center, radius, bounds))
            continue;

        out[ret++] = curr;
        if(ret == maxout) 
            break;
    }}
    return ret;
}

size_t M_Tile_Countour(size_t ntds, const struct tile_desc tds[static ntds],
                       struct map_resolution res, struct tile_desc *out, size_t maxout)
{
    if(ntds == 0)
        return 0;

    int minr = INT_MAX;
    int minc = INT_MAX;
    int maxr = INT_MIN;
    int maxc = INT_MIN;

    for(int i = 0; i < ntds; i++) {

        const struct tile_desc *curr = &tds[i];
        int absr = curr->chunk_r * res.tile_h + curr->tile_r;
        int absc = curr->chunk_c * res.tile_w + curr->tile_c;

        minr = MIN(minr, absr);
        minc = MIN(minc, absc);
        maxr = MAX(maxr, absr);
        maxc = MAX(maxc, absc);
    }

    int dr = maxr - minr + 1;
    int dc = maxc - minc + 1;

    uint8_t marked[dr + 2][dc + 2];
    memset(marked, 0, sizeof(marked));

    for(int i = 0; i < ntds; i++) {

        const struct tile_desc *curr = &tds[i];
        int absr = curr->chunk_r * res.tile_h + curr->tile_r;
        int absc = curr->chunk_c * res.tile_w + curr->tile_c;

        int relr = absr - minr + 1;
        int relc = absc - minc + 1;
        marked[relr][relc] = 1;
    }

    size_t ret = 0;
    for(int r = minr - 1; r <= maxr + 1; r++) {
    for(int c = minc - 1; c <= maxc + 1; c++) {

        int relr = r - minr + 1;
        int relc = c - minc + 1;
        bool contour = false;

        if(marked[relr][relc])
            continue;

        if(ret == maxout)
            return ret;

        /* If any of the neighbours are 'marked', this tile is part of the contour  */
        if((relr > (0)    && marked[relr - 1][relc])
        || (relr < (dr-1) && marked[relr + 1][relc])
        || (relc > (0)    && marked[relr][relc - 1])
        || (relc < (dc-1) && marked[relr][relc + 1])) {
            contour = true;
        }

        if((relr > 0      && relc > 0      && marked[relr - 1][relc - 1])
        || (relr > 0      && relc < (dc-1) && marked[relr - 1][relc + 1])
        || (relr < (dr-1) && relc > 0      && marked[relr + 1][relc - 1])
        || (relr < (dr-1) && relc < (dc-1) && marked[relr + 1][relc + 1])) {
            contour = true;
        }

        if(contour) {
            out[ret++] = (struct tile_desc){
                .chunk_r = r / res.tile_h,
                .chunk_c = c / res.tile_w,
                .tile_r = r % res.tile_h,
                .tile_c = c % res.tile_w,
            };
        }
    }}

    return ret;
}

