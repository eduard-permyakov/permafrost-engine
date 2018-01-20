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

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void Entity_ModelMatrix(const struct entity *ent, mat4x4_t *out)
{
    mat4x4_t trans, scale;

    PFM_Mat4x4_MakeTrans(ent->pos.x, ent->pos.y, ent->pos.z, &trans);
    PFM_Mat4x4_MakeScale(ent->scale.x, ent->scale.y, ent->scale.z, &scale);
    PFM_Mat4x4_Mult4x4(&trans, &scale, out);
}

