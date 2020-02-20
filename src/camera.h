/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2020 Eduard Permyakov 
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

#ifndef CAMERA_H
#define CAMERA_H

#include "pf_math.h"
#include <stdbool.h>

struct camera;
struct frustum;
extern const unsigned g_sizeof_camera;

struct bound_box{
    GLfloat x, z;
    GLfloat w, h;
};

#define DECL_CAMERA_STACK(_name)        \
    char _name[g_sizeof_camera]         \


#define CAM_Z_NEAR_DIST     (5.0f)
#define CAM_FOV_RAD         (M_PI/4.0f)

struct camera *Camera_New (void);
void           Camera_Free(struct camera *cam);

void           Camera_SetPos       (struct camera *cam, vec3_t pos);
void           Camera_SetDir       (struct camera *cam, vec3_t dir);
void           Camera_SetPitchAndYaw(struct camera *cam, float pitch, float yaw);
void           Camera_SetSpeed     (struct camera *cam, float speed);
void           Camera_SetSens      (struct camera *cam, float sensitivity);
float          Camera_GetYaw       (const struct camera *cam);
float          Camera_GetPitch     (const struct camera *cam);
float          Camera_GetHeight    (const struct camera *cam);
vec3_t         Camera_GetDir       (const struct camera *cam);
vec3_t         Camera_GetPos       (const struct camera *cam);

void           Camera_MakeViewMat  (const struct camera *cam, mat4x4_t *out);
void           Camera_MakeProjMat  (const struct camera *cam, mat4x4_t *out);

void           Camera_RestrictPosWithBox(struct camera *cam, struct bound_box box);
void           Camera_UnrestrictPos     (struct camera *cam);
bool           Camera_PosIsRestricted   (const struct camera *cam);

/* These should be called once per tick, at most. The amount moved depends 
 * on the camera speed. 
 */
void           Camera_MoveLeftTick     (struct camera *cam);
void           Camera_MoveRightTick    (struct camera *cam);
void           Camera_MoveFrontTick    (struct camera *cam);
void           Camera_MoveBackTick     (struct camera *cam);
void           Camera_MoveDirectionTick(struct camera *cam, vec3_t dir);
void           Camera_ChangeDirection  (struct camera *cam, int dx, int dy);

/* Should be called once per frame, after all movements have been set, but 
 * prior to rendering.
 */
void           Camera_TickFinishPerspective(struct camera *cam);
void           Camera_TickFinishOrthographic(struct camera *cam, vec2_t bot_left, vec2_t top_right);

void           Camera_MakeFrustum(const struct camera *cam, struct frustum *out);

#endif
