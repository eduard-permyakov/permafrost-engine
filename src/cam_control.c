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
#include "event/public/event.h"

#include <stdbool.h>
#include <assert.h>
#include <string.h>


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
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

struct{
    struct camera *active;
    union{
        struct cam_fps_ctx fps;; 
        struct cam_rts_ctx rts;
    }active_ctx;
    handler_t installed_on_keydown;
    handler_t installed_on_keyup;
    handler_t installed_on_mousemove;
    handler_t installed_on_update_end;
}s_cam_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void fps_cam_on_keydown(void *unused, void *event_arg)
{
    struct cam_fps_ctx *ctx = &s_cam_ctx.active_ctx.fps;
    SDL_Event *e = (SDL_Event*)event_arg;

    switch(e->key.keysym.scancode) {
    case SDL_SCANCODE_W: ctx->move_front = true; break;
    case SDL_SCANCODE_A: ctx->move_left = true; break;
    case SDL_SCANCODE_S: ctx->move_back = true; break;
    case SDL_SCANCODE_D: ctx->move_right = true; break;
    }
}

static void fps_cam_on_keyup(void *unused, void *event_arg)
{
    struct cam_fps_ctx *ctx = &s_cam_ctx.active_ctx.fps;
    SDL_Event *e = (SDL_Event*)event_arg;

    switch(e->key.keysym.scancode) {
    case SDL_SCANCODE_W: ctx->move_front = false; break;
    case SDL_SCANCODE_A: ctx->move_left = false; break;
    case SDL_SCANCODE_S: ctx->move_back = false; break;
    case SDL_SCANCODE_D: ctx->move_right = false; break;
    }
}

static void fps_cam_on_mousemove(void *unused, void *event_arg)
{
    struct camera *cam = s_cam_ctx.active;
    SDL_Event *e = (SDL_Event*)event_arg;

    Camera_ChangeDirection(cam, e->motion.xrel, e->motion.yrel);
}

static void fps_cam_on_update_end(void *unused1, void *unused2)
{
    struct cam_fps_ctx *ctx = &s_cam_ctx.active_ctx.fps;
    struct camera *cam = s_cam_ctx.active;

    if(ctx->move_front) Camera_MoveFrontTick(cam);
    if(ctx->move_left)  Camera_MoveLeftTick (cam);
    if(ctx->move_back)  Camera_MoveBackTick (cam);
    if(ctx->move_right) Camera_MoveRightTick(cam);

    Camera_TickFinish(cam);
}

static void rts_cam_on_mousemove(void *unused, void *event_arg)
{
    struct cam_rts_ctx *ctx = &s_cam_ctx.active_ctx.rts;
    SDL_Event *e = (SDL_Event*)event_arg;

    int mouse_x, mouse_y; 
    SDL_GetMouseState(&mouse_x, &mouse_y);
    
    ctx->move_up    = (mouse_y == 0);
    ctx->move_down  = (mouse_y == CONFIG_RES_Y - 1);
    ctx->move_left  = (mouse_x == 0);
    ctx->move_right = (mouse_x == CONFIG_RES_X - 1);
}

static void rts_cam_on_update_end(void *unused1, void *unused2)
{
    struct cam_rts_ctx *ctx = &s_cam_ctx.active_ctx.rts;
    struct camera *cam = s_cam_ctx.active;

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

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void CamControl_FPS_Install(struct camera *cam)
{
    CamControl_UninstallActive();

    E_Global_Register(SDL_KEYDOWN,      fps_cam_on_keydown,    NULL);
    E_Global_Register(SDL_KEYUP,        fps_cam_on_keyup,      NULL);
    E_Global_Register(SDL_MOUSEMOTION,  fps_cam_on_mousemove,  NULL);
    E_Global_Register(EVENT_UPDATE_END, fps_cam_on_update_end, NULL);

    s_cam_ctx.installed_on_keydown    = fps_cam_on_keydown;
    s_cam_ctx.installed_on_keyup      = fps_cam_on_keyup;
    s_cam_ctx.installed_on_mousemove  = fps_cam_on_mousemove;
    s_cam_ctx.installed_on_update_end = fps_cam_on_update_end;
    s_cam_ctx.active = cam;

    SDL_SetRelativeMouseMode(true);
}

void CamControl_RTS_Install(struct camera *cam)
{
    CamControl_UninstallActive();

    E_Global_Register(SDL_MOUSEMOTION,  rts_cam_on_mousemove,  NULL);
    E_Global_Register(EVENT_UPDATE_END, rts_cam_on_update_end, NULL);

    s_cam_ctx.installed_on_mousemove  = rts_cam_on_mousemove;
    s_cam_ctx.installed_on_update_end = rts_cam_on_update_end;
    s_cam_ctx.active = cam;

    SDL_SetRelativeMouseMode(false);
}

void CamControl_UninstallActive(void)
{
    if(s_cam_ctx.installed_on_keydown)    E_Global_Unregister(SDL_KEYDOWN,      s_cam_ctx.installed_on_keydown);
    if(s_cam_ctx.installed_on_keyup)      E_Global_Unregister(SDL_KEYUP,        s_cam_ctx.installed_on_keyup);
    if(s_cam_ctx.installed_on_mousemove)  E_Global_Unregister(SDL_MOUSEMOTION,  s_cam_ctx.installed_on_mousemove);
    if(s_cam_ctx.installed_on_update_end) E_Global_Unregister(EVENT_UPDATE_END, s_cam_ctx.installed_on_update_end);

    memset(&s_cam_ctx, 0, sizeof(s_cam_ctx));

    SDL_SetRelativeMouseMode(false);
}

