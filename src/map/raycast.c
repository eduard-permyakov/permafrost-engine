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
#include "../pf_math.h"
#include "../camera.h"
#include "../config.h"

//temp
#include "../render/public/render.h"

#include <SDL.h>

struct rc_ctx{
    struct map    *map;
    struct camera *cam;
    struct tile   *hovered;

    vec3_t         ray_origin;
    vec3_t         ray_dir;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct rc_ctx s_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

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

