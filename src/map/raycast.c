/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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

#include "map_private.h"
#include "public/tile.h"
#include "public/map.h"
#include "../event.h"
#include "../pf_math.h"
#include "../camera.h"
#include "../config.h"
#include "../main.h"

#include "../phys/public/collision.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../game/public/game.h"

#include <SDL.h>
#include <assert.h>
#include <string.h>


#define MAX_CANDIDATE_TILES 1024
#define EPSILON             (1.0f/1024)

struct ray{
    vec3_t origin;
    vec3_t dir;
};

struct rc_ctx{
    struct map       *map;
    struct camera    *cam;
    bool              tile_active;
    size_t            highlight_size;
    /* Valid bit gets cleared at the start of each frame and set when the intersection point 
     * is computed. This way the computation only needs to be done once per frame. */
    bool              valid;
    struct tile_desc  intersec_tile;
    vec3_t            intersec_pos;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct rc_ctx s_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

struct aabb aabb_for_tile(struct tile_desc desc, const struct map *map)
{
    assert(map);
    assert(desc.chunk_r >= 0 && desc.chunk_r < map->height);
    assert(desc.chunk_c >= 0 && desc.chunk_c < map->width);
    assert(desc.tile_r >= 0  && desc.tile_r < TILES_PER_CHUNK_HEIGHT);
    assert(desc.tile_c >= 0  && desc.tile_c < TILES_PER_CHUNK_HEIGHT);

    const struct pfchunk *chunk = &map->chunks[desc.chunk_r * map->width + desc.chunk_c];
    const struct tile *tile = &chunk->tiles[desc.tile_r * TILES_PER_CHUNK_WIDTH + desc.tile_c];

    struct map_resolution res = {
        map->width, map->height,
        TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT
    };
    struct box tile_bounds = M_Tile_Bounds(res, map->pos, desc);

    return (struct aabb) {
        .x_min = tile_bounds.x - tile_bounds.width,
        .x_max = tile_bounds.x,
        .y_min = -TILE_DEPTH * Y_COORDS_PER_TILE,
        .y_max = (tile->base_height + (tile->type == TILETYPE_FLAT ? 0 : tile->ramp_height)) * Y_COORDS_PER_TILE,
        .z_min = tile_bounds.z,
        .z_max = tile_bounds.z + tile_bounds.height,
    };
}

static vec3_t rc_unproject_mouse_coords(void)
{
    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    int width, height;
    Engine_WinDrawableSize(&width, &height);

    vec3_t ndc = (vec3_t){-1.0f + 2.0*((float)mouse_x/width),
                           1.0f - 2.0*((float)mouse_y/height),
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

static bool rc_find_intersection(vec3_t ray_origin, vec3_t ray_dir,
                                 struct tile_desc *out_intersec, vec3_t *out_pos)
{
    struct map_resolution res = {
        s_ctx.map->width, s_ctx.map->height,
        TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT
    };

    /* Project the ray on the Y=(-TILE_DEPTH*Y_COORDS_PER_TILE) plane between the ray origin and where the 
     * ray intersects the Y=(-TILE_DEPTH*Y_COORDS_PER_TILE) plane. */
    float t = fabs((ray_origin.y + TILE_DEPTH*Y_COORDS_PER_TILE) / ray_dir.y);
    struct line_seg_2d y_eq_0_seg = {
        ray_origin.x, 
        ray_origin.z,
        ray_origin.x + t * ray_dir.x, 
        ray_origin.z + t * ray_dir.z,
    };

    struct tile_desc cts[MAX_CANDIDATE_TILES];
    int len = M_Tile_LineSupercoverTilesSorted(res, s_ctx.map->pos, y_eq_0_seg, cts, MAX_CANDIDATE_TILES);

    for(int i = 0; i < len; i++) {
    
        struct aabb tile_aabb = aabb_for_tile(cts[i], s_ctx.map);
        float t;

        /* The first level check is to see if the ray intersects the AABB */
        if(C_RayIntersectsAABB(ray_origin, ray_dir, tile_aabb, &t)) {

            /* If the ray hits the AABB, perform the second level check:
             * Check if it intersects the exact triangle mesh of the tile. */
            vec3_t tile_mesh[VERTS_PER_TILE];
            mat4x4_t model;

            M_ModelMatrixForChunk(s_ctx.map, (struct chunkpos){cts[i].chunk_r, cts[i].chunk_c}, &model);
            int num_verts = R_TileGetTriMesh(s_ctx.map, &cts[i], &model, tile_mesh);

            if(C_RayIntersectsTriMesh(ray_origin, ray_dir, tile_mesh, num_verts, &t)) {

                PFM_Vec3_Scale(&ray_dir, t, &ray_dir);
                PFM_Vec3_Add(&ray_origin, &ray_dir, out_pos);

                *out_intersec = cts[i]; 
                return true;
            }
        }
    }
    return false;
}

static void rc_compute(void)
{
    vec3_t ray_origin = rc_unproject_mouse_coords();
    vec3_t cam_pos = Camera_GetPos(s_ctx.cam);

    vec3_t ray_dir;
    PFM_Vec3_Sub(&ray_origin, &cam_pos, &ray_dir);
    if(PFM_Vec3_Len(&ray_dir) > EPSILON) {
        PFM_Vec3_Normal(&ray_dir, &ray_dir);
    }
    s_ctx.tile_active = rc_find_intersection(ray_origin, ray_dir, &s_ctx.intersec_tile, &s_ctx.intersec_pos);
    s_ctx.valid = true;
}

static void on_mousemove(void *user, void *event)
{
    static struct tile_desc s_initial_intersec_tile;
    static bool s_initial_active;

    if(!s_ctx.valid) {
        rc_compute();
    }

    if(s_ctx.tile_active != s_initial_active || memcmp(&s_ctx.intersec_tile, &s_initial_intersec_tile, sizeof(struct tile_desc)) ) {

        if(s_ctx.tile_active)
            E_Global_Notify(EVENT_SELECTED_TILE_CHANGED, &s_ctx.intersec_tile, ES_ENGINE);
        else
            E_Global_Notify(EVENT_SELECTED_TILE_CHANGED, NULL, ES_ENGINE);
    }

    s_initial_intersec_tile = s_ctx.intersec_tile;
    s_initial_active = s_ctx.tile_active;
}

static void on_render(void *user, void *event)
{
    if(!s_ctx.tile_active)
        return;

    if(s_ctx.highlight_size == 0)
        return;

    int num_tiles = s_ctx.highlight_size * 2 - 1;

    for(int r = -(num_tiles / 2); r < (num_tiles / 2) + 1; r++) {
    for(int c = -(num_tiles / 2); c < (num_tiles / 2) + 1; c++) {

        struct map_resolution res = {
            s_ctx.map->width, s_ctx.map->height,
            TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT
        };

        struct tile_desc curr = s_ctx.intersec_tile;
        if(M_Tile_RelativeDesc(res, &curr, r, c)) {
        
            const struct pfchunk *chunk = &s_ctx.map->chunks[curr.chunk_r * s_ctx.map->width + curr.chunk_c];

            mat4x4_t model;
            M_ModelMatrixForChunk(s_ctx.map, (struct chunkpos){curr.chunk_r, curr.chunk_c}, &model);

            const int tpcw = TILES_PER_CHUNK_WIDTH;
            const int tpch = TILES_PER_CHUNK_HEIGHT;

            R_PushCmd((struct rcmd){
                .func = R_GL_TileDrawSelected,
                .nargs = 5,
                .args = {
                    R_PushArg(&curr, sizeof(curr)),
                    chunk->render_private,
                    R_PushArg(&model, sizeof(model)),
                    R_PushArg(&tpcw, sizeof(tpcw)),
                    R_PushArg(&tpch, sizeof(tpch)),
                },
            });
        }
    }}
}

static void on_update_start(void *user, void *event)
{
    s_ctx.valid = false;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

int M_Raycast_Install(struct map *map, struct camera *cam)
{
    s_ctx.map = map; 
    s_ctx.cam = cam;

    E_Global_Register(SDL_MOUSEMOTION, on_mousemove, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render, NULL, G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    E_Global_Register(EVENT_UPDATE_START, on_update_start, NULL, G_RUNNING);

    return 0;
}

void M_Raycast_Uninstall(void)
{
    E_Global_Unregister(SDL_MOUSEMOTION, on_mousemove);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render);
    E_Global_Unregister(EVENT_UPDATE_START, on_update_start);

    s_ctx.map = NULL;
    s_ctx.cam = NULL;
    s_ctx.tile_active = false;
    s_ctx.valid = false;
}

void M_Raycast_SetHighlightSize(size_t size)
{
    s_ctx.highlight_size = size;
}

size_t M_Raycast_GetHighlightSize(void)
{
    return s_ctx.highlight_size;
}

bool M_Raycast_MouseIntersecCoord(vec3_t *out)
{
    if(!s_ctx.valid) {
        rc_compute();
    }

    if(!s_ctx.tile_active) 
        return false;

    *out = s_ctx.intersec_pos;
    return true;
}

bool M_Raycast_CameraIntersecCoord(const struct camera *cam, vec3_t *out)
{
    vec3_t ray_origin = Camera_GetPos(cam);
    vec3_t ray_dir = Camera_GetDir(cam);
    return rc_find_intersection(ray_origin, ray_dir, &(struct tile_desc){0}, out);
}

