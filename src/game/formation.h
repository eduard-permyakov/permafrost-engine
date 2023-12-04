/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2023 Eduard Permyakov 
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

#ifndef FORMATION_H
#define FORMATION_H

#include "public/game.h"
#include "../pf_math.h"
#include "../navigation/public/nav.h"

#define NULL_FID (~((uint32_t)0))

typedef uint32_t formation_id_t;

struct map;

bool           G_Formation_Init(const struct map *map);
void           G_Formation_Shutdown(void);

void           G_Formation_Create(vec2_t target, const vec_entity_t *ents, enum formation_type type);
formation_id_t G_Formation_GetForEnt(uint32_t uid);
void           G_Formation_RemoveUnit(uint32_t uid);
void           G_Formation_RemoveEntity(uint32_t uid);

bool           G_Formation_InRangeOfCell(uint32_t uid);
bool           G_Formation_CanUseArrivalField(uint32_t uid);
vec2_t         G_Formation_DesiredArrivalVelocity(uint32_t uid);
vec2_t         G_Formation_ApproximateDesiredArrivalVelocity(uint32_t uid);
bool           G_Formation_ArrivedAtCell(uint32_t uid);
bool           G_Formation_AssignedToCell(uint32_t uid);
vec2_t         G_Formation_CellPosition(uint32_t uid);
quat_t         G_Formation_TargetOrientation(uint32_t uid);
void           G_Formation_UpdateFieldIfNeeded(uint32_t uid);
float          G_Formation_Speed(uint32_t uid);
enum formation_type G_Formation_Type(formation_id_t fid);

vec2_t         G_Formation_CohesionForce(uint32_t uid);
vec2_t         G_Formation_AlignmentForce(uint32_t uid);
vec2_t         G_Formation_DragForce(uint32_t uid);

#endif

