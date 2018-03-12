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

#include "map_private.h"
#include "public/tile.h"
#include "public/map.h"
#include "../event/public/event.h"
#include "../pf_math.h"
#include "../camera.h"
#include "../config.h"
#include "../collision.h"

#include "../render/public/render.h"

#include <SDL.h>
#include <assert.h>
#include <string.h>


#define MAX_CANDIDATE_TILES 256
#define EPSILON             (1.0f / 1000.0)

struct box{
    float x, z; 
    float width, height;
};

struct ray{
    vec3_t origin;
    vec3_t dir;
};

struct line_seg_2d{
    float ax, az; 
    float bx, bz;
};

struct rc_ctx{
    struct map       *map;
    struct camera    *cam;
    bool              active_tile;
    struct tile_desc  hovered;
};


/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct rc_ctx s_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool tile_for_point_2d(const struct map *map, float px, float pz, struct tile_desc *out)
{
    size_t width  = map->width * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    size_t height = map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;
    struct box map_box = (struct box){map->pos.x, map->pos.z, width, height};

    /* Recall X increases to the left in our engine */
    if(px > map_box.x || px < map_box.x - map_box.width)
        return false;

    if(pz < map_box.z || px > map_box.z + map_box.height)
        return false;

    int chunk_r, chunk_c;
    chunk_r = fabs(map_box.z - pz) / (TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE);
    chunk_c = fabs(map_box.x - px) / (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE);

    assert(chunk_r >= 0 && chunk_r < map->height);
    assert(chunk_c >= 0 && chunk_r < map->width);

    float chunk_base_x, chunk_base_z;
    chunk_base_x = map_box.x - (chunk_c * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE);
    chunk_base_z = map_box.z + (chunk_r * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE);

    int tile_r, tile_c;
    tile_r = fabs(chunk_base_z - pz) / Z_COORDS_PER_TILE;
    tile_c = fabs(chunk_base_x - px) / X_COORDS_PER_TILE;

    assert(tile_c >= 0 && tile_c < TILES_PER_CHUNK_WIDTH);
    assert(tile_r >= 0 && tile_r < TILES_PER_CHUNK_HEIGHT);

    out->chunk_r = chunk_r;
    out->chunk_c = chunk_c;
    out->tile_r = tile_r;
    out->tile_c = tile_c;
    return true;
}

static int mod(int a, int b)
{
    int r = a % b;
    return r < 0 ? r + b : r;
}

static bool box_point_inside(float px, float pz, struct box bounds)
{
    return (px <= bounds.x && px >= bounds.x - bounds.width)
        && (pz >= bounds.z && pz <= bounds.z + bounds.height);
}

static bool line_line_intersection(struct line_seg_2d l1, 
                                   struct line_seg_2d l2, 
                                   float *out_x, float *out_z)
{
    float s1_x, s1_z, s2_x, s2_z;
    s1_x = l1.bx - l1.ax;     s1_z = l1.bz - l1.az;
    s2_x = l2.bx - l2.ax;     s2_z = l2.bz - l2.az;
    
    float s, t;
    s = (-s1_z * (l1.ax - l2.ax) + s1_x * (l1.az - l2.az)) / (-s2_x * s1_z + s1_x * s2_z);
    t = ( s2_x * (l1.az - l2.az) - s2_z * (l1.ax - l2.ax)) / (-s2_x * s1_z + s1_x * s2_z);
    
    if (s >= 0 && s <= 1 && t >= 0 && t <= 1) {
        /* Intersection detected */
        if (out_x)
            *out_x = l1.ax + (t * s1_x);
        if (out_z)
            *out_z = l1.az + (t * s1_z);
        return true;
    }
    
    return false; /* No intersection */
}

static float line_len(float ax, float az, float bx, float bz)
{
    return sqrt( pow(bx - ax, 2) + pow(bz - az, 2) );
}

static bool tiles_equal(const struct tile_desc *a, const struct tile_desc *b)
{
    return (a->chunk_r == b->chunk_r)
        && (a->chunk_c == b->chunk_c)
        && (a->tile_r  == b->tile_r)
        && (a->tile_c  == b->tile_c);
}

static struct box bounds_for_tile(const struct map *map, struct tile_desc desc)
{
    size_t width  = map->width * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    size_t height = map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;
    struct box map_box = (struct box){map->pos.x, map->pos.z, width, height};

    return (struct box){
        map_box.x - desc.chunk_c * (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE)
                  - desc.tile_c  * X_COORDS_PER_TILE,
        map_box.z + desc.chunk_r * (TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE)
                  + desc.tile_r * Z_COORDS_PER_TILE,
        X_COORDS_PER_TILE,
        Z_COORDS_PER_TILE
    };
}

struct aabb aabb_for_tile(struct tile_desc desc, const struct map *map)
{
    const struct pfchunk *chunk = &map->chunks[desc.chunk_r * map->width + desc.chunk_c];
    const struct tile *tile = &chunk->tiles[desc.tile_r * TILES_PER_CHUNK_WIDTH + desc.tile_c];

    struct box tile_bounds = bounds_for_tile(map, desc);

    return (struct aabb) {
        .x_min = tile_bounds.x - tile_bounds.width,
        .x_max = tile_bounds.x,
        .y_min = 0.0f,
        .y_max = (tile->base_height + (tile->type == TILETYPE_FLAT ? 0.0f : tile->ramp_height)) * Y_COORDS_PER_TILE,
        .z_min = tile_bounds.z,
        .z_max = tile_bounds.z + tile_bounds.height,
    };
}

static bool relative_tile_desc(const struct map *map, struct tile_desc *inout, int tile_dc, int tile_dr)
{
    assert(abs(tile_dc) <= TILES_PER_CHUNK_WIDTH);
    assert(abs(tile_dr) <= TILES_PER_CHUNK_HEIGHT);

    struct tile_desc ret = (struct tile_desc) {
        inout->chunk_r + ((inout->tile_r + tile_dr < 0)                       ? -1 :
                          (inout->tile_r + tile_dr >= TILES_PER_CHUNK_HEIGHT) ?  1 :
                                                                                 0),

        inout->chunk_c + ((inout->tile_c + tile_dc < 0)                       ? -1 :
                          (inout->tile_c + tile_dc >= TILES_PER_CHUNK_WIDTH)  ?  1 :
                                                                                 0),
        mod(inout->tile_r + tile_dr, TILES_PER_CHUNK_HEIGHT),
        mod(inout->tile_c + tile_dc, TILES_PER_CHUNK_WIDTH)
    };

    if( !(ret.chunk_r >= 0 && ret.chunk_r < map->height)
     || !(ret.chunk_c >= 0 && ret.chunk_c < map->width)) {
     
        return false;
    }

    *inout = ret;
    return true;
}

static int line_box_intersection(struct line_seg_2d line, 
                                 struct box bounds, 
                                 float out_x[2], float out_z[2])
{
    int ret = 0;

    struct line_seg_2d top = (struct line_seg_2d){
        bounds.x, 
        bounds.z, 
        bounds.x - bounds.width,
        bounds.z
    };

    struct line_seg_2d bot = (struct line_seg_2d){
        bounds.x, 
        bounds.z + bounds.height, 
        bounds.x - bounds.width,
        bounds.z + bounds.height
    };

    struct line_seg_2d left = (struct line_seg_2d){
        bounds.x, 
        bounds.z, 
        bounds.x,
        bounds.z + bounds.height
    };

    struct line_seg_2d right = (struct line_seg_2d){
        bounds.x - bounds.width, 
        bounds.z, 
        bounds.x - bounds.width,
        bounds.z + bounds.height
    };

    if(line_line_intersection(line, top, out_x + ret, out_z + ret))
        ret++;

    if(line_line_intersection(line, bot, out_x + ret, out_z + ret))
        ret++;

    if(line_line_intersection(line, left, out_x + ret, out_z + ret))
        ret++;

    if(line_line_intersection(line, right, out_x + ret, out_z + ret))
        ret++;

    assert(ret >= 0 && ret <= 2);
    return ret;
}

/* Uses a variant of the algorithm outlined here:
 * http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.42.3443&rep=rep1&type=pdf 
 * ('A Fast Voxel Traversal Algorithm for Ray Tracing' by John Amanatides, Andrew Woo)
 */
int candidate_tiles_sorted(const struct map *map, vec3_t ray_origin, vec3_t ray_dir, struct tile_desc out[])
{
    int ret = 0;

    /* Project the ray on the Y=0 plane between the camera position and where the 
     * ray intersects the Y=0 plane. */
    float t = fabs(ray_origin.y / ray_dir.y);
    struct line_seg_2d y_eq_0_seg = {
        ray_origin.x, 
        ray_origin.z,
        ray_origin.x + t * ray_dir.x, 
        ray_origin.z + t * ray_dir.z,
    };
    
    /* Initialization: find the coordinate of the ray origin within the map.
     * In the case of the ray originating inside the map, this is simple - take the ray origin.
     * In case the ray originates outside but intersects the map, we take the intersection 
     * point as the start. Lastly, if ray doesn't even intersect the map, no work needs to be 
     * done - return an empty list. 
     */
    size_t width  = map->width * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    size_t height = map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;
    struct box map_box = (struct box){map->pos.x, map->pos.z, width, height};

    int num_intersect;
    float intersect_x[2], intersect_z[2];
    float start_x, start_z;
    struct tile_desc curr_tile_desc;

    if(box_point_inside(y_eq_0_seg.ax, y_eq_0_seg.az, map_box)) {

        start_x = y_eq_0_seg.ax; 
        start_z = y_eq_0_seg.az; 

    }else if(num_intersect = line_box_intersection(y_eq_0_seg, map_box, intersect_x, intersect_z)) {

        /* Nudge the intersection point by EPSILON in the direction of the ray to make sure 
         * the intersection point is within the map bounds. */

        /* One intersection means that the end of the ray is inside the map */
        if(num_intersect == 1) {

            start_x = intersect_x[0] + EPSILON * ray_dir.x;
            start_z = intersect_z[0] + EPSILON * ray_dir.z;
        
        }
        /* If the first of the 2 intersection points is closer to the ray origin */
        else if( line_len(intersect_x[0], intersect_z[0], y_eq_0_seg.ax, y_eq_0_seg.az)
               < line_len(intersect_x[1], intersect_z[1], y_eq_0_seg.ax, y_eq_0_seg.az) ) {
         
            start_x = intersect_x[0] + EPSILON * ray_dir.x;
            start_z = intersect_z[0] + EPSILON * ray_dir.z;

         }else{
         
            start_x = intersect_x[1] + EPSILON * ray_dir.x;
            start_z = intersect_z[1] + EPSILON * ray_dir.z;
         }

    }else {
        return 0;
    }

    bool r = tile_for_point_2d(map, start_x, start_z, &curr_tile_desc);
    assert(r);

    assert(curr_tile_desc.chunk_r >= 0 && curr_tile_desc.chunk_r < map->height);
    assert(curr_tile_desc.chunk_c >= 0 && curr_tile_desc.chunk_c < map->width);

    float t_max_x, t_max_z;

    const int step_c = ray_dir.x <= 0.0f ? 1 : -1;
    const int step_r = ray_dir.z >= 0.0f ? 1 : -1;

    const float t_delta_x = fabs(X_COORDS_PER_TILE / ray_dir.x);
    const float t_delta_z = fabs(Z_COORDS_PER_TILE / ray_dir.z);

    struct box tile_bounds = bounds_for_tile(map, curr_tile_desc);

    t_max_x = (step_c > 0) ? fabs(start_x - (tile_bounds.x - tile_bounds.width)) / fabs(ray_dir.x) 
                           : fabs(start_x - tile_bounds.x) / fabs(ray_dir.x);

    t_max_z = (step_r > 0) ? fabs(start_z - (tile_bounds.z + tile_bounds.height)) / fabs(ray_dir.z)
                           : fabs(start_z - tile_bounds.z) / fabs(ray_dir.z);

    assert(t_max_x > 0.0f && t_max_z > 0.0f);

    bool ray_ends_inside = box_point_inside(y_eq_0_seg.bx, y_eq_0_seg.bz, map_box);
    struct tile_desc final_tile_desc;

    if(ray_ends_inside) {
        int r = tile_for_point_2d(map, y_eq_0_seg.bx, y_eq_0_seg.bz, &final_tile_desc);
        assert(r);
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

        if(ray_ends_inside && tiles_equal(&curr_tile_desc, &final_tile_desc)){
            break;
        }

        if(!relative_tile_desc(map, &curr_tile_desc, dc, dr)) {
            break;
        }

    }while(ret < 1024);
    assert(ret < 1024);

    return ret;
}

static vec3_t rc_unproject_mouse_coords(void)
{
    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    vec3_t ndc = (vec3_t){-1.0f + 2.0*((float)mouse_x/CONFIG_RES_X),
                           1.0f - 2.0*((float)mouse_y/CONFIG_RES_Y),
                          -1.0f};
    vec4_t clip = (vec4_t){ndc.x, ndc.y, ndc.z, 1.0f};

    mat4x4_t view_proj_inverse; 
    mat4x4_t view, proj, tmp;

    Camera_MakeViewMat(s_ctx.cam, &view);
    Camera_MakeProjMat(s_ctx.cam, &proj);
    PFM_Mat4x4_Mult4x4(&proj, &view, &tmp);
    PFM_Mat4x4_Inverse(&tmp, &view_proj_inverse); 

    vec4_t ret_homo;
    PFM_Mat4x4_Mult4x1(&view_proj_inverse, &clip, &ret_homo);
    return (vec3_t){ret_homo.x/ret_homo.w, ret_homo.y/ret_homo.w, ret_homo.z/ret_homo.w};
}

static void on_mousemove(void *user, void *event)
{
    vec3_t ray_origin = rc_unproject_mouse_coords();
    vec3_t ray_dir;

    vec3_t cam_pos = Camera_GetPos(s_ctx.cam);
    PFM_Vec3_Sub(&ray_origin, &cam_pos, &ray_dir);
    PFM_Vec3_Normal(&ray_dir, &ray_dir);

    const struct tile_desc initial_hovered = s_ctx.hovered;
    const bool initial_active = s_ctx.active_tile;

    s_ctx.active_tile = false;

    struct tile_desc cts[MAX_CANDIDATE_TILES];
    int len = candidate_tiles_sorted(s_ctx.map, ray_origin, ray_dir, cts);

    for(int i = 0; i < len; i++) {
    
        struct aabb tile_aabb = aabb_for_tile(cts[i], s_ctx.map);
        float t;

        /* The first level check is to see if the ray intersects the AABB */
        if(C_RayIntersectsAABB(ray_origin, ray_dir, tile_aabb, &t)) {

            /* If the ray hits the AABB, perform the second level check:
             * Check if it intersects the exact triangle mesh of the tile. */
            vec3_t tile_mesh[36];
            mat4x4_t model;

            const struct pfchunk *chunk = 
                &s_ctx.map->chunks[cts[i].chunk_r * s_ctx.map->width + cts[i].chunk_c];
            M_ModelMatrixForChunk(s_ctx.map, (struct chunkpos){cts[i].chunk_r, cts[i].chunk_c}, &model);

            int num_verts = R_GL_TriMeshForTile(&cts[i], chunk->render_private, &model, 
                TILES_PER_CHUNK_WIDTH, tile_mesh);
            assert(num_verts == sizeof(tile_mesh) / sizeof(vec3_t));

            if(C_RayIntersectsTriMesh(ray_origin, ray_dir, tile_mesh, num_verts)) {

                s_ctx.hovered = cts[i]; 
                s_ctx.active_tile = true;
                break;
            }
        }
    }

    if(s_ctx.active_tile != initial_active || memcmp(&s_ctx.hovered, &initial_hovered, sizeof(struct tile_desc)) ) {

        if(s_ctx.active_tile)
            E_Global_Notify(EVENT_SELECTED_TILE_CHANGED, &s_ctx.hovered, ES_ENGINE);
        else
            E_Global_Notify(EVENT_SELECTED_TILE_CHANGED, NULL, ES_ENGINE);
    }
}

static void on_render(void *user, void *event)
{
    if(!s_ctx.active_tile)
        return;

    const struct tile_desc *td = &s_ctx.hovered;
    const struct pfchunk *chunk = &s_ctx.map->chunks[td->chunk_r * s_ctx.map->width + td->chunk_c];
    mat4x4_t model;

    M_ModelMatrixForChunk(s_ctx.map, (struct chunkpos){td->chunk_r, td->chunk_c}, &model);
    R_GL_DrawTileSelected(td, chunk->render_private, &model, TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT); 
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

int M_Raycast_Install(struct map *map, struct camera *cam)
{
    s_ctx.map = map; 
    s_ctx.cam = cam;

    E_Global_Register(SDL_MOUSEMOTION, on_mousemove, NULL);
    E_Global_Register(EVENT_RENDER, on_render, NULL);

    return 0;
}

