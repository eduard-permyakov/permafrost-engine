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

#include "entity.h" 
#include "game/public/game.h"
#include "anim/public/anim.h"

#include <assert.h>

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void Entity_ModelMatrix(const struct entity *ent, mat4x4_t *out)
{
    mat4x4_t trans, scale, rot, tmp;
    vec3_t pos = G_Pos_Get(ent->uid);

    PFM_Mat4x4_MakeTrans(pos.x, pos.y, pos.z, &trans);
    PFM_Mat4x4_MakeScale(ent->scale.x, ent->scale.y, ent->scale.z, &scale);
    PFM_Mat4x4_RotFromQuat(&ent->rotation, &rot);

    PFM_Mat4x4_Mult4x4(&scale, &rot, &tmp);
    PFM_Mat4x4_Mult4x4(&trans, &tmp, out);
}

uint32_t Entity_NewUID(void)
{
    static uint32_t uid = 0;
    return uid++;
}

void Entity_CurrentOBB(const struct entity *ent, struct obb *out)
{
    const struct aabb *aabb;
    if(ent->flags & ENTITY_FLAG_ANIMATED)
        aabb = A_GetCurrPoseAABB(ent);
    else
        aabb = &ent->identity_aabb;

    vec4_t identity_verts_homo[8] = {
        {aabb->x_min, aabb->y_min, aabb->z_min, 1.0f},
        {aabb->x_min, aabb->y_min, aabb->z_max, 1.0f},
        {aabb->x_min, aabb->y_max, aabb->z_min, 1.0f},
        {aabb->x_min, aabb->y_max, aabb->z_max, 1.0f},
        {aabb->x_max, aabb->y_min, aabb->z_min, 1.0f},
        {aabb->x_max, aabb->y_min, aabb->z_max, 1.0f},
        {aabb->x_max, aabb->y_max, aabb->z_min, 1.0f},
        {aabb->x_max, aabb->y_max, aabb->z_max, 1.0f},
    };

    vec4_t identity_center_homo = (vec4_t){
        (aabb->x_min + aabb->x_max) / 2.0f,
        (aabb->y_min + aabb->y_max) / 2.0f,
        (aabb->z_min + aabb->z_max) / 2.0f,
        1.0f
    };

    mat4x4_t model;
    Entity_ModelMatrix(ent, &model);

    vec4_t obb_verts_homo[8];
    for(int i = 0; i < 8; i++) {
        PFM_Mat4x4_Mult4x1(&model, identity_verts_homo + i, obb_verts_homo + i);
        out->corners[i] = (vec3_t){
            obb_verts_homo[i].x / obb_verts_homo[i].w,
            obb_verts_homo[i].y / obb_verts_homo[i].w,
            obb_verts_homo[i].z / obb_verts_homo[i].w,
        };
    }

    vec4_t obb_center_homo;
    PFM_Mat4x4_Mult4x1(&model, &identity_center_homo, &obb_center_homo);
    out->center = (vec3_t){
        obb_center_homo.x / obb_center_homo.w,
        obb_center_homo.y / obb_center_homo.w,
        obb_center_homo.z / obb_center_homo.w,
    };
    out->half_lengths[0] = (aabb->x_max - aabb->x_min) / 2.0f * ent->scale.x;
    out->half_lengths[1] = (aabb->y_max - aabb->y_min) / 2.0f * ent->scale.y;
    out->half_lengths[2] = (aabb->z_max - aabb->z_min) / 2.0f * ent->scale.z;

    vec3_t axis0, axis1, axis2;   
    PFM_Vec3_Sub(&out->corners[4], &out->corners[0], &axis0);
    PFM_Vec3_Sub(&out->corners[2], &out->corners[0], &axis1);
    PFM_Vec3_Sub(&out->corners[1], &out->corners[0], &axis2);

    PFM_Vec3_Normal(&axis0, &out->axes[0]);
    PFM_Vec3_Normal(&axis1, &out->axes[1]);
    PFM_Vec3_Normal(&axis2, &out->axes[2]);
}

vec3_t Entity_TopCenterPointWS(const struct entity *ent)
{
    const struct aabb *aabb = &ent->identity_aabb;
    vec4_t top_center_homo = (vec4_t) {
        (aabb->x_min + aabb->x_max) / 2.0f,
        aabb->y_max,
        (aabb->z_min + aabb->z_max) / 2.0f,
        1.0f
    };

    mat4x4_t model; 
    vec4_t out_ws_homo;

    Entity_ModelMatrix(ent, &model);
    PFM_Mat4x4_Mult4x1(&model, &top_center_homo, &out_ws_homo);

    return (vec3_t) {
        out_ws_homo.x / out_ws_homo.w,
        out_ws_homo.y / out_ws_homo.w,
        out_ws_homo.z / out_ws_homo.w,
    };
}

