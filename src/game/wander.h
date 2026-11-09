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

#ifndef WANDER_H
#define WANDER_H

#include "../pf_math.h"

#include <stdbool.h>
#include <stdint.h>

struct map;
struct SDL_RWops;

bool G_Wander_Init(const struct map *map);
void G_Wander_Shutdown(void);

/* Roams the entity about 'anchor', within 'radius': a few short walks with a
 * pause between them, then a long rest, and so on */
bool G_Wander_Start(uint32_t uid, vec2_t anchor, float radius);
void G_Wander_Stop(uint32_t uid);
void G_Wander_RemoveEntity(uint32_t uid);
bool G_Wander_Active(uint32_t uid);

/* The wanderer runs from 'threat' for 'seconds' at 'speed', then settles and
 * makes its way back */
bool G_Wander_Flee(uint32_t uid, vec2_t threat, float seconds, float speed);
bool G_Wander_Fleeing(uint32_t uid);

bool G_Wander_SaveState(struct SDL_RWops *stream);
bool G_Wander_LoadState(struct SDL_RWops *stream);

#endif
