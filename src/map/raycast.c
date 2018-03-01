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
#include "../event/public/event.h"
#include "../lib/public/klist.h"
#include "../pf_math.h"
#include "../camera.h"
#include "../config.h"

//temp
#include "../render/public/render.h"

#include <SDL.h>
#include <assert.h>

struct rc_ctx{
    struct map    *map;
    struct camera *cam;
    struct tile   *hovered;

    vec3_t         ray_origin;
    vec3_t         ray_dir;
};


#define _dummy_free(x) /* No need to free data on list removal - only store pointers */
KLIST_INIT(tile, struct tile*, _dummy_free);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct rc_ctx s_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

const struct tile *tile_for_point_2d(const struct map *map, float px, float pz)
{
    size_t width  = map->width * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    size_t height = map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;
    struct box{
        float x, z; 
        float width, height;
    }map_box = (struct box){map->pos.x, map->pos.z, width, height};

    /* Recall X increases to the left in our engine */
    if(px > map_box.x || px < map_box.x - map_box.width)
        return NULL;

    if(pz < map_box.z || px > map_box.z + map_box.height)
        return NULL;

    int chunk_r, chunk_c;
    chunk_r = (pz - map_box.z) / (TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE);
    chunk_c = (map_box.x - px) / (TILES_PER_CHUNK_HEIGHT * X_COORDS_PER_TILE);

    assert(chunk_r >= 0 && chunk_r < map->height);
    assert(chunk_c >= 0 && chunk_r < map->width);

    float chunk_base_x, chunk_base_z;
    chunk_base_x = map_box.x - (chunk_c * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE);
    chunk_base_z = map_box.z + (chunk_r * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE);

    int tile_r, tile_c;
    tile_r = (pz - chunk_base_z) / Z_COORDS_PER_TILE;
    tile_c = (chunk_base_x - px) / X_COORDS_PER_TILE;

    assert(tile_c >= 0 && tile_c < TILES_PER_CHUNK_WIDTH);
    assert(tile_r >= 0 && tile_r < TILES_PER_CHUNK_HEIGHT);

    printf("tile under cursor: (%d, %d) -> (%d, %d)\n", chunk_r, chunk_c, tile_r, tile_c);
    return &map->chunks[(chunk_r * map->width) + chunk_c].tiles[(tile_r * TILES_PER_CHUNK_WIDTH) + tile_c];
}

/* Uses the algorithm outlined here:
 * http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.42.3443&rep=rep1&type=pdf 
 */
static klist_t(tile) *candindate_tiles_sorted(vec3_t ray_origin, vec3_t ray_dir)
{
    klist_t(tile) *ret = kl_init(tile);
    if(!ret)
        return NULL;

    /* Project the ray on the Y=0 plane between the camera position and where the 
     * ray intersects the Y=0 plane. */
    float t = ray_origin.y / ray_dir.y;
    struct line_seg_2d{
        float ax, az; 
        float bx, bz;
    }y_eq_0_seg = {
        ray_origin.x, ray_origin.z,
        ray_origin.x - t * ray_dir.x, ray_origin.z - t * ray_dir.z,
    };

    const struct tile *tile = tile_for_point_2d(s_ctx.map, y_eq_0_seg.bx, y_eq_0_seg.bz);

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
    s_ctx.ray_origin = rc_unproject_mouse_coords();

    vec3_t cam_pos = Camera_GetPos(s_ctx.cam);
    PFM_Vec3_Sub(&s_ctx.ray_origin, &cam_pos, &s_ctx.ray_dir);

    klist_t(tile) *cand_tiles = candindate_tiles_sorted(s_ctx.ray_origin, s_ctx.ray_dir);
    kl_destroy(tile, cand_tiles);
}

static void on_render(void *user, void *event)
{
    mat4x4_t model;
    PFM_Mat4x4_Identity(&model);

    R_GL_DrawRay(Camera_GetPos(s_ctx.cam), s_ctx.ray_dir, &model);
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

