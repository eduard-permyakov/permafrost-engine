/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2023 Eduard Permyakov 
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

#ifndef HARVESTER_H
#define HARVESTER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define DEFAULT_GATHER_SPEED (0)
#define DEFAULT_MAX_CARRY    (0)

struct map;
struct SDL_RWops;

bool G_Harvester_Init(const struct map *map);
void G_Harvester_Shutdown(void);
bool G_Harvester_AddEntity(uint32_t uid);
void G_Harvester_RemoveEntity(uint32_t uid);
void G_Harvester_Stop(uint32_t uid);
bool G_Harvester_SupplyBuilding(uint32_t uid, uint32_t building_uid);
bool G_Harvester_InTargetMode(void);
int  G_Harvester_CurrContextualAction(void);
bool G_Harvester_GetContextualCursor(char *out, size_t maxout);
void G_Harvester_ClearQueuedCmd(uint32_t uid);
bool G_Harvester_Idle(uint32_t uid);

bool G_Harvester_SaveState(struct SDL_RWops *stream);
bool G_Harvester_LoadState(struct SDL_RWops *stream);

#endif

