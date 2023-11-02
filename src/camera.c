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

#include "camera.h"
#include "config.h"
#include "main.h"
#include "render/public/render.h"
#include "render/public/render_ctrl.h"
#include "phys/public/collision.h"

#include <SDL.h>

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

struct camera {
    float    speed;
    float    sensitivity;

    vec3_t   pos;
    vec3_t   front;
    vec3_t   up;

    float    pitch;
    float    yaw;

    uint32_t prev_frame_ts;

    /* When 'bounded' is true, the camera position must 
     * always be within the 'bounds' box */
    bool            bounded;
    struct bound_box bounds;
};

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define EPSILON (1.0f/1024)

/*****************************************************************************/
/* GLOBAL VARIABLES                                                          */
/*****************************************************************************/

const unsigned g_sizeof_camera = sizeof(struct camera);

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool camera_pos_in_bounds(const struct camera *cam)
{
    /* X is increasing to the left in our coordinate system */
    return (cam->pos.x <= cam->bounds.x && cam->pos.x >= cam->bounds.x - cam->bounds.w)
        && (cam->pos.z >= cam->bounds.z && cam->pos.z <= cam->bounds.z + cam->bounds.h);
}

static void camera_move_within_bounds(struct camera *cam)
{
    /* X is increasing to the left in our coordinate system */
    cam->pos.x = MIN(cam->pos.x, cam->bounds.x);
    cam->pos.x = MAX(cam->pos.x, cam->bounds.x - cam->bounds.w);

    cam->pos.z = MAX(cam->pos.z, cam->bounds.z);
    cam->pos.z = MIN(cam->pos.z, cam->bounds.z + cam->bounds.h);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

struct camera *Camera_New(void)
{
    struct camera *ret = calloc(1, sizeof(struct camera));
    if(!ret)
        return NULL;

    return ret;
}

void Camera_Free(struct camera *cam)
{
    free(cam);
}

void Camera_SetPos(struct camera *cam, vec3_t pos)
{
    cam->pos = pos; 
    if(cam->bounded) {
        camera_move_within_bounds(cam);
    }
    assert(!cam->bounded || camera_pos_in_bounds(cam));
}

void Camera_SetDir(struct camera *cam, vec3_t dir)
{
    PFM_Vec3_Normal(&dir, &dir);
    cam->front = dir;

    /* Find a vector that is orthogonal to 'front' in the XZ plane */
    vec3_t xz = (vec3_t){cam->front.z, 0.0f, -cam->front.x};
    PFM_Vec3_Cross(&cam->front, &xz, &cam->up);
    PFM_Vec3_Normal(&cam->up, &cam->up);

    cam->pitch = -RAD_TO_DEG(asin(-dir.y));
    cam->yaw = -RAD_TO_DEG(atan2(dir.x, dir.z));
}

vec3_t Camera_GetDir(const struct camera *cam)
{
    return cam->front;
}

void Camera_SetPitchAndYaw(struct camera *cam, float pitch, float yaw)
{
    cam->pitch = pitch;
    cam->yaw = yaw;

    vec3_t front;
    front.x = cos(DEG_TO_RAD(cam->yaw)) * cos(DEG_TO_RAD(cam->pitch));
    front.y = sin(DEG_TO_RAD(cam->pitch));
    front.z = sin(DEG_TO_RAD(cam->yaw)) * cos(DEG_TO_RAD(cam->pitch)) * -1;
    PFM_Vec3_Normal(&front, &cam->front);

    /* Find a vector that is orthogonal to 'front' in the XZ plane */
    vec3_t xz = (vec3_t){cam->front.z, 0.0f, -cam->front.x};
    PFM_Vec3_Cross(&cam->front, &xz, &cam->up);
    PFM_Vec3_Normal(&cam->up, &cam->up);
}

void Camera_SetSpeed(struct camera *cam, float speed)
{
    cam->speed = speed;
}

void Camera_SetSens(struct camera *cam, float sensitivity)
{
    cam->sensitivity = sensitivity;
}

float Camera_GetSpeed(const struct camera *cam)
{
    return cam->speed;
}

float Camera_GetSens(const struct camera *cam)
{
    return cam->sensitivity;
}

float Camera_GetYaw(const struct camera *cam)
{
    return cam->yaw;
}

float Camera_GetPitch(const struct camera *cam)
{
    return cam->pitch;
}

float Camera_GetHeight(const struct camera *cam)
{
    return cam->pos.y;
}

vec3_t Camera_GetPos(const struct camera *cam)
{
    return cam->pos;
}

void Camera_MoveLeftTick(struct camera *cam)
{
    uint32_t tdelta;
    vec3_t   vdelta, left;
    
    if(!cam->prev_frame_ts)
        cam->prev_frame_ts = SDL_GetTicks();

    uint32_t curr = SDL_GetTicks();
    tdelta = curr - cam->prev_frame_ts;

    PFM_Vec3_Cross(&cam->front, &cam->up, &left);
    PFM_Vec3_Normal(&left, &left);

    PFM_Vec3_Scale(&left, tdelta * cam->speed, &vdelta);
    PFM_Vec3_Add(&cam->pos, &vdelta, &cam->pos);

    if(cam->bounded) {
        camera_move_within_bounds(cam);
    }
    assert(!cam->bounded || camera_pos_in_bounds(cam));
}

void Camera_MoveRightTick(struct camera *cam)
{
    uint32_t tdelta;
    vec3_t   vdelta, left;
    
    if(!cam->prev_frame_ts)
        cam->prev_frame_ts = SDL_GetTicks();

    uint32_t curr = SDL_GetTicks();
    tdelta = curr - cam->prev_frame_ts;

    PFM_Vec3_Cross(&cam->front, &cam->up, &left);
    PFM_Vec3_Normal(&left, &left);

    PFM_Vec3_Scale(&left, tdelta * cam->speed, &vdelta);
    PFM_Vec3_Sub(&cam->pos, &vdelta, &cam->pos);

    if(cam->bounded) {
        camera_move_within_bounds(cam);
    }
    assert(!cam->bounded || camera_pos_in_bounds(cam));
}

void Camera_MoveFrontTick(struct camera *cam)
{
    uint32_t tdelta;
    vec3_t   vdelta;
    
    if(!cam->prev_frame_ts)
        cam->prev_frame_ts = SDL_GetTicks();

    uint32_t curr = SDL_GetTicks();
    tdelta = curr - cam->prev_frame_ts;

    PFM_Vec3_Scale(&cam->front, tdelta * cam->speed, &vdelta);
    PFM_Vec3_Add(&cam->pos, &vdelta, &cam->pos);

    if(cam->bounded) {
        camera_move_within_bounds(cam);
    }
    assert(!cam->bounded || camera_pos_in_bounds(cam));
}

void Camera_MoveBackTick(struct camera *cam)
{
    uint32_t tdelta;
    vec3_t   vdelta;
    
    if(!cam->prev_frame_ts)
        cam->prev_frame_ts = SDL_GetTicks();

    uint32_t curr = SDL_GetTicks();
    tdelta = curr - cam->prev_frame_ts;

    PFM_Vec3_Scale(&cam->front, tdelta * cam->speed, &vdelta);
    PFM_Vec3_Sub(&cam->pos, &vdelta, &cam->pos);

    if(cam->bounded) {
        camera_move_within_bounds(cam);
    }
    assert(!cam->bounded || camera_pos_in_bounds(cam));
}

void Camera_MoveDirectionTick(struct camera *cam, vec3_t dir)
{
    uint32_t tdelta;
    vec3_t   vdelta;

    if(!cam->prev_frame_ts)
        cam->prev_frame_ts = SDL_GetTicks();

    float mag = sqrt(pow(dir.x,2) + pow(dir.y,2) + pow(dir.z,2));
    if(mag < EPSILON)
        return;

    PFM_Vec3_Normal(&dir, &dir);

    uint32_t curr = SDL_GetTicks();
    tdelta = curr - cam->prev_frame_ts;

    PFM_Vec3_Scale(&dir, tdelta * cam->speed, &vdelta);
    PFM_Vec3_Add(&cam->pos, &vdelta, &cam->pos);

    if(cam->bounded) {
        camera_move_within_bounds(cam);
    }
    assert(!cam->bounded || camera_pos_in_bounds(cam));
}

void Camera_ChangeDirection(struct camera *cam, int dx, int dy)
{
    float sdx = dx * cam->sensitivity; 
    float sdy = dy * cam->sensitivity;

    cam->yaw   += sdx;
    cam->pitch -= sdy;

    cam->pitch = cam->pitch <  89.0f ? cam->pitch :  89.0f;
    cam->pitch = cam->pitch > -89.0f ? cam->pitch : -89.0f;

    vec3_t front;         
    front.x = cos(DEG_TO_RAD(cam->yaw)) * cos(DEG_TO_RAD(cam->pitch));
    front.y = sin(DEG_TO_RAD(cam->pitch));
    front.z = sin(DEG_TO_RAD(cam->yaw)) * cos(DEG_TO_RAD(cam->pitch)) * -1;
    PFM_Vec3_Normal(&front, &cam->front);

    /* Find a vector that is orthogonal to 'front' in the XZ plane */
    vec3_t xz = (vec3_t){cam->front.z, 0.0f, -cam->front.x};
    PFM_Vec3_Cross(&cam->front, &xz, &cam->up);
    PFM_Vec3_Normal(&cam->up, &cam->up);
}

void Camera_TickFinishPerspective(struct camera *cam)
{
    mat4x4_t view, proj;

    /* Set the view matrix for the vertex shader */
    vec3_t target;
    PFM_Vec3_Add(&cam->pos, &cam->front, &target);
    PFM_Mat4x4_MakeLookAt(&cam->pos, &target, &cam->up, &view);

    R_PushCmd((struct rcmd){
        .func = R_GL_SetViewMatAndPos,
        .nargs = 2,
        .args = {
            R_PushArg(&view, sizeof(view)),
            R_PushArg(&cam->pos, sizeof(cam->pos)),
        },
    });
    
    /* Set the projection matrix for the vertex shader */
    int w, h;
    Engine_WinDrawableSize(&w, &h);
    PFM_Mat4x4_MakePerspective(DEG_TO_RAD(45.0f), ((GLfloat)w)/h, CAM_Z_NEAR_DIST, CONFIG_DRAWDIST, &proj);

    R_PushCmd((struct rcmd){
        .func = R_GL_SetProj,
        .nargs = 1,
        .args = { R_PushArg(&proj, sizeof(proj)) },
    });

    /* Update our last timestamp */
    cam->prev_frame_ts = SDL_GetTicks();
}

void Camera_TickFinishOrthographic(struct camera *cam, vec2_t bot_left, vec2_t top_right)
{
    mat4x4_t view, proj;

    /* Set the view matrix for the vertex shader */
    vec3_t target;
    PFM_Vec3_Add(&cam->pos, &cam->front, &target);
    PFM_Mat4x4_MakeLookAt(&cam->pos, &target, &cam->up, &view);

    R_PushCmd((struct rcmd){
        .func = R_GL_SetViewMatAndPos,
        .nargs = 2,
        .args = {
            R_PushArg(&view, sizeof(view)),
            R_PushArg(&cam->pos, sizeof(cam->pos)),
        },
    });
    
    /* Set the projection matrix for the vertex shader */
    PFM_Mat4x4_MakeOrthographic(bot_left.raw[0], top_right.raw[0], bot_left.raw[1], top_right.raw[1], CAM_Z_NEAR_DIST, CONFIG_DRAWDIST, &proj);
    R_PushCmd((struct rcmd){
        .func = R_GL_SetProj,
        .nargs = 1,
        .args = { R_PushArg(&proj, sizeof(proj)) },
    });

    /* Update our last timestamp */
    cam->prev_frame_ts = SDL_GetTicks();
}

void Camera_RestrictPosWithBox(struct camera *cam, struct bound_box box)
{
    cam->bounded = true;
    cam->bounds = box;

    assert(!cam->bounded || camera_pos_in_bounds(cam));
}

void Camera_UnrestrictPos(struct camera *cam)
{
    cam->bounded = false;

    assert(!cam->bounded || camera_pos_in_bounds(cam));
}

bool Camera_PosIsRestricted(const struct camera *cam)
{
    return cam->bounded;
}

void Camera_MakeViewMat(const struct camera *cam, mat4x4_t *out)
{
    vec3_t target;
    PFM_Vec3_Add((vec3_t*)&cam->pos, (vec3_t*)&cam->front, &target);
    PFM_Mat4x4_MakeLookAt((vec3_t*)&cam->pos, &target, (vec3_t*)&cam->up, out);
}

void Camera_MakeProjMat(const struct camera *cam, mat4x4_t *out)
{
    int w, h;
    Engine_WinDrawableSize(&w, &h);
    PFM_Mat4x4_MakePerspective(CAM_FOV_RAD, ((GLfloat)w)/h, 0.1f, CONFIG_DRAWDIST, out);
}

void Camera_MakeFrustum(const struct camera *cam, struct frustum *out)
{
    int w, h;
    Engine_WinDrawableSize(&w, &h);
    const float aspect_ratio = ((float)w)/h;

    C_MakeFrustum(cam->pos, cam->up, cam->front, aspect_ratio, CAM_FOV_RAD, 
        CAM_Z_NEAR_DIST, CONFIG_DRAWDIST, out);
}

