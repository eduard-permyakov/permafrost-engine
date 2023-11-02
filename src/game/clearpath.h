/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2023 Eduard Permyakov 
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

#ifndef CLEARPATH_H
#define CLEARPATH_H

#include "../pf_math.h"
#include "../lib/public/vec.h"

#include <stdint.h>


#define CLEARPATH_NEIGHBOUR_RADIUS (10.0f)
/* This is added to the entity's radius so that it will take wider turns 
 * and leave this as a buffer between it and the obstacle.
 */
#define CLEARPATH_BUFFER_RADIUS    (0.0f)

struct map;

struct cp_ent{
    vec2_t xz_pos;
    vec2_t xz_vel; /* specified per pathfinding tick */
    float  radius;
};

VEC_TYPE(cp_ent, struct cp_ent)
VEC_IMPL(static inline, cp_ent, struct cp_ent)

void G_ClearPath_Init(const struct map *map);
void G_ClearPath_Shutdown(void);
bool G_ClearPath_ShouldSaveDebug(uint32_t ent_uid);

vec2_t G_ClearPath_NewVelocity(struct cp_ent ent,
                               uint32_t ent_uid,
                               vec2_t ent_des_v,
                               vec_cp_ent_t dyn_neighbs,
                               vec_cp_ent_t stat_neighbs,
                               bool save_debug);

#endif

