/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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

#include "cam_control.h"
#include "config.h"

#include <stdbool.h>
#include <assert.h>


struct cam_fps_ctx{
    bool move_front;
    bool move_back;
    bool move_left;
    bool move_right;
};

struct cam_rts_ctx{
    bool move_up;
    bool move_down;
    bool move_left;
    bool move_right;
};

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

struct cam_fps_ctx *CamControl_FPS_CtxNew(void)
{
    struct cam_fps_ctx *ret = calloc(1, sizeof(struct cam_fps_ctx)); 
    if(!ret)
        return NULL;

    return ret;
}

void CamControl_FPS_CtxFree(struct cam_fps_ctx *ctx)
{
    free(ctx);
}

void CamControl_FPS_HandleEvent(struct cam_fps_ctx *ctx, struct camera *cam, SDL_Event e)
{
    switch(e.type) {

    case SDL_KEYDOWN:
        switch(e.key.keysym.scancode) {
        case SDL_SCANCODE_W: ctx->move_front = true; break;
        case SDL_SCANCODE_A: ctx->move_left = true; break;
        case SDL_SCANCODE_S: ctx->move_back = true; break;
        case SDL_SCANCODE_D: ctx->move_right = true; break;
        
        }
    
        break;
    
    case SDL_KEYUP:
        switch(e.key.keysym.scancode) {
        case SDL_SCANCODE_W: ctx->move_front = false; break;
        case SDL_SCANCODE_A: ctx->move_left = false; break;
        case SDL_SCANCODE_S: ctx->move_back = false; break;
        case SDL_SCANCODE_D: ctx->move_right = false; break;
        }

        break;

    case SDL_MOUSEMOTION: 
        Camera_ChangeDirection(cam, e.motion.xrel, e.motion.yrel);
        break;
    };
}

void CamControl_FPS_TickFinish(struct cam_fps_ctx *ctx, struct camera *cam)
{
    if(ctx->move_front) Camera_MoveFrontTick(cam);
    if(ctx->move_left)  Camera_MoveLeftTick (cam);
    if(ctx->move_back)  Camera_MoveBackTick (cam);
    if(ctx->move_right) Camera_MoveRightTick(cam);

    Camera_TickFinish(cam);
}

void CamControl_FPS_SetMouseMode(void)
{
    SDL_SetRelativeMouseMode(true);
}


struct cam_rts_ctx *CamControl_RTS_CtxNew(void)
{
    struct cam_rts_ctx *ret = calloc(1, sizeof(struct cam_rts_ctx)); 
    if(!ret)
        return NULL;

    return ret;
}

void CamControl_RTS_CtxFree(struct cam_rts_ctx *ctx)
{
    free(ctx);
}

void CamControl_RTS_HandleEvent(struct cam_rts_ctx *ctx, struct camera *cam, SDL_Event e)
{
    switch(e.type) {

        case SDL_MOUSEMOTION: {
        
            int mouse_x, mouse_y; 
            SDL_GetMouseState(&mouse_x, &mouse_y);

            ctx->move_up    = (mouse_y == 0);
            ctx->move_down  = (mouse_y == CONFIG_RES_Y - 1);
            ctx->move_left  = (mouse_x == 0);
            ctx->move_right = (mouse_x == CONFIG_RES_X - 1);
        }
        break;
    }
}

void CamControl_RTS_TickFinish(struct cam_rts_ctx *ctx, struct camera *cam)
{
    float yaw = Camera_GetYaw(cam);

    /*
     * Our yaw represent the following rotations:
     *          90*
     *           ^
     *  sin +ve  | sin +ve
     *  cos -ve  | cos +ve
     *           |
     * 180* <----+----> 0*
     *           |
     *  sin -ve  | sin -ve
     *  cos -ve  | cos +ve
     *           v
     *          270*
     *
     * Our coodinate system is the following:
     *         -Z
     *          ^
     *          |
     *   +X <---+---> -X
     *          |
     *          v
     *          +Z
     *
     * We want the behavior in which the camera is always scrolled up, down, left, right
     * depending on which corner/edge of the screen the mouse is touching. However, which
     * direction is 'up' or 'left' depends completely on where the camera is facing. For example,
     * 'up' becomes 'down' when the camera pitch is changed from 90* to 270*.
     */
    
    vec3_t up    = (vec3_t){  1.0f * cos(DEG_TO_RAD(yaw)), 0.0f, -1.0f * sin(DEG_TO_RAD(yaw)) };
    vec3_t left  = (vec3_t){  1.0f * sin(DEG_TO_RAD(yaw)), 0.0f,  1.0f * cos(DEG_TO_RAD(yaw)) };
    vec3_t down  = (vec3_t){-up.x, up.y, -up.z};
    vec3_t right = (vec3_t){-left.x, left.y, -left.z};

    assert(!(ctx->move_left && ctx->move_right));
    assert(!(ctx->move_up && ctx->move_down));

    vec3_t dir = (vec3_t){0.0f, 0.0f, 0.0f};

    if(ctx->move_left)  PFM_Vec3_Add(&dir, &left, &dir);
    if(ctx->move_right) PFM_Vec3_Add(&dir, &right, &dir);
    if(ctx->move_up)    PFM_Vec3_Add(&dir, &up, &dir);
    if(ctx->move_down)  PFM_Vec3_Add(&dir, &down, &dir);

    Camera_MoveDirectionTick(cam, dir);
    Camera_TickFinish(cam);
}

void CamControl_RTS_SetMouseMode(void)
{
    SDL_SetRelativeMouseMode(false);
}

