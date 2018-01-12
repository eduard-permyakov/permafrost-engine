/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017 Eduard Permyakov 
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

#include "camera.h"
#include "gl_uniforms.h"
#include "render/public/render.h"
#include "config.h"

#include <SDL2/SDL.h>

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
};

/*****************************************************************************/
/* GLOBAL VARIABLES                                                          */
/*****************************************************************************/

const unsigned g_sizeof_camera = sizeof(struct camera);

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
}

void Camera_SetFrontAndUp(struct camera *cam, vec3_t front, vec3_t up)
{
    float dot = PFM_Vec3_Dot(&front, &up);
    assert(dot < 0.001);

    cam->front = front; 
    cam->up = up;

    cam->yaw = RAD_TO_DEG(atan2( -front.z, -front.x ));
    cam->pitch = RAD_TO_DEG(atan2( -up.z, up.y ));
    printf("yaw: %f\n", cam->yaw);
    printf("pitch: %f\n", cam->pitch);
}

void Camera_SetSpeed(struct camera *cam, float speed)
{
    cam->speed = speed;
}

void Camera_SetSens(struct camera *cam, float sensitivity)
{
    cam->sensitivity = sensitivity;
}

void Camera_MoveLeftTick(struct camera *cam)
{
    uint32_t tdelta;
    vec3_t   vdelta, right;
    
    if(!cam->prev_frame_ts)
        cam->prev_frame_ts = SDL_GetTicks();

    uint32_t curr = SDL_GetTicks();
    tdelta = curr - cam->prev_frame_ts;

    PFM_Vec3_Cross(&cam->front, &cam->up, &right);
    PFM_Vec3_Normal(&right, &right);

    PFM_Vec3_Scale(&right, tdelta * cam->speed, &vdelta);
    PFM_Vec3_Add(&cam->pos, &vdelta, &cam->pos);
}

void Camera_MoveRightTick(struct camera *cam)
{
    uint32_t tdelta;
    vec3_t   vdelta, right;
    
    if(!cam->prev_frame_ts)
        cam->prev_frame_ts = SDL_GetTicks();

    uint32_t curr = SDL_GetTicks();
    tdelta = curr - cam->prev_frame_ts;

    PFM_Vec3_Cross(&cam->front, &cam->up, &right);
    PFM_Vec3_Normal(&right, &right);

    PFM_Vec3_Scale(&right, tdelta * cam->speed, &vdelta);
    PFM_Vec3_Sub(&cam->pos, &vdelta, &cam->pos);
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

void Camera_TickFinish(struct camera *cam)
{
    mat4x4_t view, proj;

    /* Set the view matrix for the vertex shader */
    vec3_t target;
    PFM_Vec3_Add(&cam->pos, &cam->front, &target);
    PFM_Mat4x4_MakeLookAt(&cam->pos, &target, &cam->up, &view);

    R_GL_SetViewMatAndPos(&view, &cam->pos);
    
    /* Set the projection matrix for the vertex shader */
    GLint viewport[4]; 
    glGetIntegerv(GL_VIEWPORT, viewport);
    PFM_Mat4x4_MakePerspective(DEG_TO_RAD(45.0f), ((GLfloat)viewport[2])/viewport[3], 0.1f, CONFIG_DRAWDIST, &proj);

    R_GL_SetProj(&proj);

    /* Update our last timestamp */
    cam->prev_frame_ts = SDL_GetTicks();
}

