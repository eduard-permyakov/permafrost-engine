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

#ifndef FOG_OF_WAR_H
#define FOG_OF_WAR_H

#include "../pf_math.h"
#include <stdbool.h>
#include <stdint.h>

struct map;
struct obb;
struct SDL_RWops;


bool G_Fog_Init(const struct map *map);
void G_Fog_Shutdown(void);

void G_Fog_AddVision(vec2_t xz_pos, int faction_id, float radius);
void G_Fog_RemoveVision(vec2_t xz_pos, int faction_id, float radius);
void G_Fog_UpdateVisionRange(vec2_t xz_pos, int faction_id, float oldr, float newr);

bool G_Fog_CircleExplored(uint16_t fac_mask, vec2_t xz_pos, float radius);
bool G_Fog_RectExplored(uint16_t fac_mask, vec2_t xz_pos, float halfx, float halfz);
bool G_Fog_NearVisibleWater(uint16_t fac_mask, vec2_t xz_pos, float radius);

void G_Fog_ExploreCircle(vec2_t xz_pos, int faction_id, float radius);
void G_Fog_ExploreRectangle(vec2_t xz_pos, int faction_id, float halfx, float halfz);

void G_Fog_UpdateVisionState(void);
void G_Fog_ClearExploredCache(void);

bool G_Fog_SaveState(struct SDL_RWops *stream);
bool G_Fog_LoadState(struct SDL_RWops *stream);

bool G_Fog_Enabled(void);

uint32_t *G_Fog_CopyState(void);

#endif

