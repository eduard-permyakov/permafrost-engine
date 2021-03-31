/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#ifndef STORAGE_SITE_H
#define STORAGE_SITE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define DEFAULT_CAPACITY (0)

struct entity;
struct SDL_RWops;

bool G_StorageSite_Init(void);
void G_StorageSite_Shutdown(void);
bool G_StorageSite_AddEntity(const struct entity *ent);
void G_StorageSite_RemoveEntity(const struct entity *ent);
bool G_StorageSite_IsSaturated(uint32_t uid);
void G_StorageSite_UpdateFaction(uint32_t uid, int oldfac, int newfac);
bool G_StorageSite_Desires(uint32_t uid, const char *rname);

bool G_StorageSite_SaveState(struct SDL_RWops *stream);
bool G_StorageSite_LoadState(struct SDL_RWops *stream);

void G_StorageSite_SetUseAlt(const struct entity *ent, bool use);
bool G_StorageSite_GetUseAlt(uint32_t uid);
void G_StorageSite_ClearAlt(const struct entity *ent);
void G_StorageSite_ClearCurr(const struct entity *ent);

bool G_StorageSite_SetAltCapacity(const struct entity *ent, const char *rname, int max);
int  G_StorageSite_GetAltCapacity(uint32_t uid, const char *rname);
bool G_StorageSite_SetAltDesired(uint32_t uid, const char *rname, int des);
int  G_StorageSite_GetAltDesired(uint32_t uid, const char *rname);

#endif

