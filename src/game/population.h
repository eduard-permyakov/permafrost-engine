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

#ifndef POPULATION_H
#define POPULATION_H

#include "public/game.h"

#include <stdbool.h>
#include <stdint.h>

struct SDL_RWops;

bool G_Population_Init(void);
void G_Population_Shutdown(void);
void G_Population_ClearState(void);

/* Entity lifecycle hooks. Called from G_AddEntity / G_RemoveEntity / G_Zombiefy.
 * Idempotent: calling Remove* on an entity that was never added is a no-op. 
 */
void G_Population_AddContributor(uint32_t uid);
void G_Population_RemoveContributor(uint32_t uid);
void G_Population_AddLimitContributor(uint32_t uid);
void G_Population_RemoveLimitContributor(uint32_t uid);

/* Called from G_SetFactionID for any entity whose faction has changed. 
 */
void G_Population_UpdateFaction(uint32_t uid, int oldfac, int newfac);

/* Per-entity limit amount setter/getter. The amount only contributes to the
 * faction-wide limit once the building has been completed (the activation is
 * tracked internally via EVENT_BUILDING_CONSTRUCTED). 
 */
void G_Population_SetEntityLimit(uint32_t uid, int amount);
int  G_Population_GetEntityLimit(uint32_t uid);

/* Faction-level queries. */
int  G_Population_Get(int faction_id);
int  G_Population_GetLimit(int faction_id);
int  G_Population_GetAllied(int faction_id);
int  G_Population_GetEnemy(int faction_id);
int  G_Population_GetPlayer(void);
int  G_Population_GetPlayerLimit(void);

bool G_Population_SaveState(struct SDL_RWops *stream);
bool G_Population_LoadState(struct SDL_RWops *stream);

#endif
