/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2020 Eduard Permyakov 
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

#ifndef MOVEMENT_H
#define MOVEMENT_H

#include "../pf_math.h"
#include <stdbool.h>

#define MOVE_TICK_RES (20)

struct map;
struct entity;
struct SDL_RWops;

bool G_Move_Init(const struct map *map);
void G_Move_Shutdown(void);

void G_Move_AddEntity(const struct entity *ent);
void G_Move_RemoveEntity(const struct entity *ent);
void G_Move_Stop(const struct entity *ent);

bool G_Move_GetDest(const struct entity *ent, vec2_t *out_xz);
bool G_Move_Still(const struct entity *ent);

void G_Move_SetSeekEnemies(const struct entity *ent);
void G_Move_SetSurroundEntity(const struct entity *ent, const struct entity *target);
void G_Move_UpdatePos(const struct entity *ent, vec2_t pos);

bool G_Move_SaveState(struct SDL_RWops *stream);
bool G_Move_LoadState(struct SDL_RWops *stream);

#endif

