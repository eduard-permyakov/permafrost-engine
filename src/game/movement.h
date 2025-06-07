/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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

#include "public/game.h"
#include "../pf_math.h"
#include <stdbool.h>
#include <stdint.h>

#define MOVE_TICK_RES (20)
enum movement_hz{
    MOVE_HZ_20,
    MOVE_HZ_10,
    MOVE_HZ_5,
    MOVE_HZ_1
};

struct map;
struct SDL_RWops;

bool G_Move_Init(const struct map *map);
void G_Move_Shutdown(void);
bool G_Move_HasWork(void);
void G_Move_FlushWork(void);

void G_Move_SetTickHz(enum movement_hz hz);
int  G_Move_GetTickHz(void);

void G_Move_AddEntity(uint32_t uid, vec3_t pos, float sel_radius, int faction_id);
void G_Move_RemoveEntity(uint32_t uid);

bool G_Move_GetDest(uint32_t uid, vec2_t *out_xz, bool *out_attack);
bool G_Move_GetSurrounding(uint32_t uid, uint32_t *out_uid);

void G_Move_Stop(uint32_t uid);
void G_Move_SetSeekEnemies(uint32_t uid);
void G_Move_SetSurroundEntity(uint32_t uid, uint32_t target);
void G_Move_SetChangeDirection(uint32_t uid, quat_t target);
void G_Move_SetEnterRange(uint32_t uid, uint32_t target, float range);

void G_Move_UpdatePos(uint32_t uid, vec2_t pos);
void G_Move_UpdateFactionID(uint32_t uid, int oldfac, int newfac);
bool G_Move_InTargetMode(void);

void G_Move_Unblock(uint32_t uid);
void G_Move_BlockAt(uint32_t uid, vec3_t pos);

bool G_Move_SaveState(struct SDL_RWops *stream);
bool G_Move_LoadState(struct SDL_RWops *stream);
void G_Move_Upload(void);

#endif

