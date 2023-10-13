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

#ifndef COMBAT_H
#define COMBAT_H

#include "public/game.h"
#include <stdbool.h>
#include <stdint.h>

struct entity;
struct SDL_RWops;
struct map;


bool G_Combat_Init(const struct map *map);
void G_Combat_Shutdown(void);

void G_Combat_AddEntity(uint32_t uid, enum combat_stance initial);
void G_Combat_RemoveEntity(uint32_t uid);
void G_Combat_StopAttack(uint32_t uid);
void G_Combat_ClearSavedMoveCmd(uint32_t uid);
int  G_Combat_CurrContextualAction(void);

void G_Combat_AddRef(int faction_id, vec2_t pos);
void G_Combat_RemoveRef(int faction_id, vec2_t pos);
void G_Combat_AddTimeDelta(uint32_t delta);

bool G_Combat_SaveState(struct SDL_RWops *stream);
bool G_Combat_LoadState(struct SDL_RWops *stream);

#endif

