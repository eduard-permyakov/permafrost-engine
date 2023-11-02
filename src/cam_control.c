/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2023 Eduard Permyakov 
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

#include "cam_control.h"
#include "config.h"
#include "event.h"
#include "main.h"
#include "cursor.h"
#include "game/public/game.h"

#include <stdbool.h>
#include <assert.h>
#include <string.h>


#define KEYUP_TICKS_TIMEOUT (1)

/* Certain *ahem* OS/Windows System combos send KEYUP events
 * even when holding down a key. So holding down a key looks 
 * like UP,DOWN,UP,DOWN,UP,DOWN... Use the following simple 
 * state machine to filter out the KEYUP events where we get
 * a KEYDOWN for the same key in the next frame.
 */
enum keystate{
    KEY_PRESSED,
    KEY_RELEASED_NO_TIMEOUT,
    KEY_RELEASED,
};

struct cam_fps_ctx{
    enum keystate front_state;
    uint32_t      front_pressed_tick;
    uint32_t      front_released_tick;

    enum keystate back_state;
    uint32_t      back_pressed_tick;
    uint32_t      back_released_tick;

    enum keystate left_state;
    uint32_t      left_pressed_tick;
    uint32_t      left_released_tick;

    enum keystate right_state;
    uint32_t      right_pressed_tick;
    uint32_t      right_released_tick;
};

struct cam_rts_ctx{
    bool          scroll_up;
    bool          scroll_down;
    bool          scroll_left;
    bool          scroll_right;
    bool          pan_disabled;

    enum keystate front_state;
    uint32_t      front_pressed_tick;
    uint32_t      front_released_tick;

    enum keystate back_state;
    uint32_t      back_pressed_tick;
    uint32_t      back_released_tick;

    enum keystate left_state;
    uint32_t      left_pressed_tick;
    uint32_t      left_released_tick;

    enum keystate right_state;
    uint32_t      right_pressed_tick;
    uint32_t      right_released_tick;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

struct{
    struct camera *active;
    union{
        struct cam_fps_ctx fps; 
        struct cam_rts_ctx rts;
    }active_ctx;
    handler_t installed_on_keydown;
    handler_t installed_on_keyup;
    handler_t installed_on_mousemove;
    handler_t installed_on_mousedown;
    handler_t installed_on_mouseup;
    handler_t installed_on_update_end;
}s_cam_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void fps_cam_on_keydown(void *unused, void *event_arg)
{
    struct cam_fps_ctx *ctx = &s_cam_ctx.active_ctx.fps;
    SDL_Event *e = (SDL_Event*)event_arg;
    uint32_t curr_tick = g_frame_idx;

    if(S_UI_TextEditHasFocus())
        return;

    switch(e->key.keysym.scancode) {
    case SDL_SCANCODE_W: 
        ctx->front_pressed_tick = curr_tick;
        ctx->front_state = KEY_PRESSED;
        break; 
    case SDL_SCANCODE_A: 
        ctx->left_pressed_tick = curr_tick;
        ctx->left_state = KEY_PRESSED;
        break;
    case SDL_SCANCODE_S: 
        ctx->back_pressed_tick = curr_tick;
        ctx->back_state = KEY_PRESSED;
        break;
    case SDL_SCANCODE_D: 
        ctx->right_pressed_tick = curr_tick;
        ctx->right_state = KEY_PRESSED;
        break;
	default: break;
    }
}

static void fps_cam_on_keyup(void *unused, void *event_arg)
{
    struct cam_fps_ctx *ctx = &s_cam_ctx.active_ctx.fps;
    SDL_Event *e = (SDL_Event*)event_arg;
    uint32_t curr_tick = g_frame_idx;

    if(S_UI_TextEditHasFocus())
        return;

    switch(e->key.keysym.scancode) {
    case SDL_SCANCODE_W: 
        ctx->front_released_tick = curr_tick;
        ctx->front_state = KEY_RELEASED_NO_TIMEOUT;
        break; 
    case SDL_SCANCODE_A: 
        ctx->left_released_tick = curr_tick;
        ctx->left_state = KEY_RELEASED_NO_TIMEOUT;
        break;
    case SDL_SCANCODE_S: 
        ctx->back_released_tick = curr_tick;
        ctx->back_state = KEY_RELEASED_NO_TIMEOUT;
        break;
    case SDL_SCANCODE_D: 
        ctx->right_released_tick = curr_tick;
        ctx->right_state = KEY_RELEASED_NO_TIMEOUT;
        break;
	default: break;
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
    uint32_t curr_tick = g_frame_idx;

    if((ctx->front_state == KEY_RELEASED_NO_TIMEOUT)
    && (curr_tick - ctx->front_released_tick > KEYUP_TICKS_TIMEOUT))
        ctx->front_state = KEY_RELEASED;

    if(ctx->left_state == KEY_RELEASED_NO_TIMEOUT
    && curr_tick - ctx->left_released_tick > KEYUP_TICKS_TIMEOUT)
        ctx->left_state = KEY_RELEASED;

    if(ctx->back_state == KEY_RELEASED_NO_TIMEOUT
    && curr_tick - ctx->back_released_tick > KEYUP_TICKS_TIMEOUT)
        ctx->back_state = KEY_RELEASED;

    if(ctx->right_state == KEY_RELEASED_NO_TIMEOUT
    && curr_tick - ctx->right_released_tick > KEYUP_TICKS_TIMEOUT)
        ctx->right_state = KEY_RELEASED;

    vec3_t front, back, up, left, right;
    front = Camera_GetDir(cam);
    PFM_Vec3_Scale(&front, -1.0f, &back);

    /* Find a vector that is orthogonal to 'front' in the XZ plane */
    vec3_t xz = (vec3_t){front.z, 0.0f, -front.x};
    PFM_Vec3_Cross(&front, &xz, &up);
    PFM_Vec3_Normal(&up, &up);

    PFM_Vec3_Cross(&front, &up, &left);
    PFM_Vec3_Normal(&left, &left);
    PFM_Vec3_Scale(&left, -1.0f, &right);

    vec3_t dir = (vec3_t){0.0f, 0.0f, 0.0f};

    if(ctx->front_state != KEY_RELEASED) PFM_Vec3_Add(&dir, &front, &dir);
    if(ctx->left_state != KEY_RELEASED)  PFM_Vec3_Add(&dir, &left, &dir);
    if(ctx->back_state != KEY_RELEASED)  PFM_Vec3_Add(&dir, &back, &dir);
    if(ctx->right_state != KEY_RELEASED) PFM_Vec3_Add(&dir, &right, &dir);
    
    Camera_MoveDirectionTick(cam, dir);
    Camera_TickFinishPerspective(cam);
}

static void rts_cam_on_mousemove(void *unused, void *event_arg)
{
    struct cam_rts_ctx *ctx = &s_cam_ctx.active_ctx.rts;
    SDL_MouseMotionEvent *e = (SDL_MouseMotionEvent*)event_arg;

    int width, height;
    Engine_WinDrawableSize(&width, &height);
    
    ctx->scroll_up    = (e->y == 0);
    ctx->scroll_down  = (e->y == height - 1);
    ctx->scroll_left  = (e->x == 0);
    ctx->scroll_right = (e->x == width - 1);
}

static void rts_cam_on_mousedown(void *unused, void *event_arg)
{
    struct cam_rts_ctx *ctx = &s_cam_ctx.active_ctx.rts;
    SDL_Event *e = (SDL_Event*)event_arg;

    if(ctx->scroll_up || ctx->scroll_down || ctx->scroll_left || ctx->scroll_right)
        return;

    if(e->button.button == SDL_BUTTON_LEFT)
        ctx->pan_disabled = true;
}

static void rts_cam_on_mouseup(void *unused, void *event_arg)
{
    struct cam_rts_ctx *ctx = &s_cam_ctx.active_ctx.rts;
    SDL_Event *e = (SDL_Event*)event_arg;

    if(e->button.button == SDL_BUTTON_LEFT)
        ctx->pan_disabled = false;
}

static void rts_cam_on_keydown(void *unused, void *event_arg)
{
    struct cam_rts_ctx *ctx = &s_cam_ctx.active_ctx.rts;
    SDL_Event *e = (SDL_Event*)event_arg;
    uint32_t curr_tick = g_frame_idx;

    if(S_UI_TextEditHasFocus())
        return;

    switch(e->key.keysym.scancode) {
    case SDL_SCANCODE_UP: 
        ctx->front_pressed_tick = curr_tick;
        ctx->front_state = KEY_PRESSED;
        break; 
    case SDL_SCANCODE_LEFT: 
        ctx->left_pressed_tick = curr_tick;
        ctx->left_state = KEY_PRESSED;
        break;
    case SDL_SCANCODE_DOWN: 
        ctx->back_pressed_tick = curr_tick;
        ctx->back_state = KEY_PRESSED;
        break;
    case SDL_SCANCODE_RIGHT: 
        ctx->right_pressed_tick = curr_tick;
        ctx->right_state = KEY_PRESSED;
        break;
	default: break;
    }
}

static void rts_cam_on_keyup(void *unused, void *event_arg)
{
    struct cam_rts_ctx *ctx = &s_cam_ctx.active_ctx.rts;
    SDL_Event *e = (SDL_Event*)event_arg;
    uint32_t curr_tick = g_frame_idx;

    if(S_UI_TextEditHasFocus())
        return;

    switch(e->key.keysym.scancode) {
    case SDL_SCANCODE_UP: 
        ctx->front_released_tick = curr_tick;
        ctx->front_state = KEY_RELEASED_NO_TIMEOUT;
        break; 
    case SDL_SCANCODE_LEFT: 
        ctx->left_released_tick = curr_tick;
        ctx->left_state = KEY_RELEASED_NO_TIMEOUT;
        break;
    case SDL_SCANCODE_DOWN: 
        ctx->back_released_tick = curr_tick;
        ctx->back_state = KEY_RELEASED_NO_TIMEOUT;
        break;
    case SDL_SCANCODE_RIGHT: 
        ctx->right_released_tick = curr_tick;
        ctx->right_state = KEY_RELEASED_NO_TIMEOUT;
        break;
	default: break;
    }
}

static void rts_cam_on_update_end(void *unused1, void *unused2)
{
    struct cam_rts_ctx *ctx = &s_cam_ctx.active_ctx.rts;
    struct camera *cam = s_cam_ctx.active;
    uint32_t curr_tick = g_frame_idx;

    if((ctx->front_state == KEY_RELEASED_NO_TIMEOUT)
    && (curr_tick - ctx->front_released_tick > KEYUP_TICKS_TIMEOUT))
        ctx->front_state = KEY_RELEASED;

    if(ctx->left_state == KEY_RELEASED_NO_TIMEOUT
    && curr_tick - ctx->left_released_tick > KEYUP_TICKS_TIMEOUT)
        ctx->left_state = KEY_RELEASED;

    if(ctx->back_state == KEY_RELEASED_NO_TIMEOUT
    && curr_tick - ctx->back_released_tick > KEYUP_TICKS_TIMEOUT)
        ctx->back_state = KEY_RELEASED;

    if(ctx->right_state == KEY_RELEASED_NO_TIMEOUT
    && curr_tick - ctx->right_released_tick > KEYUP_TICKS_TIMEOUT)
        ctx->right_state = KEY_RELEASED;

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

    assert(!(ctx->scroll_left && ctx->scroll_right));
    assert(!(ctx->scroll_up && ctx->scroll_down));

    vec3_t dir = (vec3_t){0.0f, 0.0f, 0.0f};

    if(!ctx->pan_disabled) {

        if(ctx->scroll_left || ctx->left_state != KEY_RELEASED) 
            PFM_Vec3_Add(&dir, &left, &dir);
        if(ctx->scroll_right || ctx->right_state != KEY_RELEASED) 
            PFM_Vec3_Add(&dir, &right, &dir);
        if(ctx->scroll_up || ctx->front_state != KEY_RELEASED) 
            PFM_Vec3_Add(&dir, &up, &dir);
        if(ctx->scroll_down || ctx->back_state != KEY_RELEASED) 
            PFM_Vec3_Add(&dir, &down, &dir);
    }

    Camera_MoveDirectionTick(cam, dir);
    Camera_TickFinishPerspective(cam);
}

static void free_cam_on_update_end(void *unused1, void *unused2)
{
    struct camera *cam = s_cam_ctx.active;
    Camera_TickFinishPerspective(cam);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void CamControl_FPS_Install(struct camera *cam)
{
    CamControl_UninstallActive();

    E_Global_Register(SDL_KEYDOWN,      fps_cam_on_keydown,    NULL, G_RUNNING | G_PAUSED_UI_RUNNING);
    E_Global_Register(SDL_KEYUP,        fps_cam_on_keyup,      NULL, G_RUNNING | G_PAUSED_UI_RUNNING);
    E_Global_Register(SDL_MOUSEMOTION,  fps_cam_on_mousemove,  NULL, G_RUNNING | G_PAUSED_UI_RUNNING);
    E_Global_Register(EVENT_UPDATE_END, fps_cam_on_update_end, NULL, 
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);

    s_cam_ctx.installed_on_keydown    = fps_cam_on_keydown;
    s_cam_ctx.installed_on_keyup      = fps_cam_on_keyup;
    s_cam_ctx.installed_on_mousemove  = fps_cam_on_mousemove;
    s_cam_ctx.installed_on_update_end = fps_cam_on_update_end;
    s_cam_ctx.active = cam;

    s_cam_ctx.active_ctx.fps.front_state = KEY_RELEASED;
    s_cam_ctx.active_ctx.fps.left_state = KEY_RELEASED;
    s_cam_ctx.active_ctx.fps.back_state = KEY_RELEASED;
    s_cam_ctx.active_ctx.fps.right_state = KEY_RELEASED;

    SDL_SetRelativeMouseMode(true);
}

void CamControl_RTS_Install(struct camera *cam)
{
    CamControl_UninstallActive();

    E_Global_Register(SDL_KEYDOWN,         rts_cam_on_keydown,    NULL, G_RUNNING);
    E_Global_Register(SDL_KEYUP,           rts_cam_on_keyup,      NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEMOTION,     rts_cam_on_mousemove,  NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, rts_cam_on_mousedown,  NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONUP,   rts_cam_on_mouseup,    NULL, G_RUNNING);
    E_Global_Register(EVENT_UPDATE_END,    rts_cam_on_update_end, NULL, 
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);

    s_cam_ctx.installed_on_keydown    = rts_cam_on_keydown;
    s_cam_ctx.installed_on_keyup      = rts_cam_on_keyup;
    s_cam_ctx.installed_on_mousemove  = rts_cam_on_mousemove;
    s_cam_ctx.installed_on_mousedown  = rts_cam_on_mousedown;
    s_cam_ctx.installed_on_mouseup    = rts_cam_on_mouseup;
    s_cam_ctx.installed_on_update_end = rts_cam_on_update_end;
    s_cam_ctx.active = cam;

    s_cam_ctx.active_ctx.rts.front_state = KEY_RELEASED;
    s_cam_ctx.active_ctx.rts.left_state = KEY_RELEASED;
    s_cam_ctx.active_ctx.rts.back_state = KEY_RELEASED;
    s_cam_ctx.active_ctx.rts.right_state = KEY_RELEASED;

    Cursor_SetRTSMode(true);
}

void CamControl_Free_Install(struct camera *cam)
{
    CamControl_UninstallActive();

    E_Global_Register(EVENT_UPDATE_END, free_cam_on_update_end, NULL, 
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);

    s_cam_ctx.installed_on_update_end = free_cam_on_update_end;
    s_cam_ctx.active = cam;
}

void CamControl_UninstallActive(void)
{
    if(s_cam_ctx.installed_on_keydown)    E_Global_Unregister(SDL_KEYDOWN,         s_cam_ctx.installed_on_keydown);
    if(s_cam_ctx.installed_on_keyup)      E_Global_Unregister(SDL_KEYUP,           s_cam_ctx.installed_on_keyup);
    if(s_cam_ctx.installed_on_mousemove)  E_Global_Unregister(SDL_MOUSEMOTION,     s_cam_ctx.installed_on_mousemove);
    if(s_cam_ctx.installed_on_mousedown)  E_Global_Unregister(SDL_MOUSEBUTTONDOWN, s_cam_ctx.installed_on_mousedown);
    if(s_cam_ctx.installed_on_mouseup)    E_Global_Unregister(SDL_MOUSEBUTTONUP,   s_cam_ctx.installed_on_mouseup);
    if(s_cam_ctx.installed_on_update_end) E_Global_Unregister(EVENT_UPDATE_END,    s_cam_ctx.installed_on_update_end);

    memset(&s_cam_ctx, 0, sizeof(s_cam_ctx));

    Cursor_SetRTSMode(false);
    SDL_SetRelativeMouseMode(false);
}

