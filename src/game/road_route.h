/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
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


#ifndef ROAD_ROUTE_H
#define ROAD_ROUTE_H

#include "../pf_math.h"

#include <stdbool.h>
#include <stdint.h>

struct SDL_RWops;

bool G_RoadRoute_Init(void);
void G_RoadRoute_Shutdown(void);
void G_RoadRoute_RemoveEntity(uint32_t uid);

/* Sends a road-locked or road-preferring unit along the roads toward the 
 * point. Returns false when the unit should be ordered there directly. */
bool G_RoadRoute_MoveTo(uint32_t uid, vec2_t xz, bool attack);
/* Orders the unit to surround the target, along the roads when it prefers 
 * them and they lead there within its limit */
void G_RoadRoute_SurroundEntity(uint32_t uid, uint32_t target);
void G_RoadRoute_Cancel(uint32_t uid);
bool G_RoadRoute_Routing(uint32_t uid);

/* Kept to the roads: ordered only along them */
void  G_RoadRoute_SetLocked(uint32_t uid, bool locked);
bool  G_RoadRoute_GetLocked(uint32_t uid);
/* Sent along the roads when the way along them is at most 'limit' times the
 * straight way; 0 turns it off */
void  G_RoadRoute_SetPreference(uint32_t uid, float limit);
float G_RoadRoute_GetPreference(uint32_t uid);

bool G_RoadRoute_SaveState(struct SDL_RWops *stream);
bool G_RoadRoute_LoadState(struct SDL_RWops *stream);

#endif
