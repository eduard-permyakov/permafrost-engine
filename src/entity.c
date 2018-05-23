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

#include "entity.h" 
#include "anim/public/anim.h"

#include <assert.h>

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void Entity_ModelMatrix(const struct entity *ent, mat4x4_t *out)
{
    mat4x4_t trans, scale, rot, tmp;

    PFM_Mat4x4_MakeTrans(ent->pos.x, ent->pos.y, ent->pos.z, &trans);
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
    assert(ent->flags & ENTITY_FLAG_COLLISION);

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

    vec3_t axis0, axis1, axis2;   
    PFM_Vec3_Sub(&out->corners[4], &out->corners[0], &axis0);
    PFM_Vec3_Sub(&out->corners[2], &out->corners[0], &axis1);
    PFM_Vec3_Sub(&out->corners[1], &out->corners[0], &axis2);

    PFM_Vec3_Normal(&axis0, &out->axes[0]);
    PFM_Vec3_Normal(&axis1, &out->axes[1]);
    PFM_Vec3_Normal(&axis2, &out->axes[2]);
}

