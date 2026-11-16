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

#ifndef COMMAND_H
#define COMMAND_H

#include "public/game.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct SDL_RWops;

bool G_Command_Init(void);
void G_Command_Shutdown(void);
void G_Command_RemoveEntity(uint32_t uid);

/* Put an order ahead of the unit's others, to be taken up when its work ends */
bool G_Command_PushFront(uint32_t uid, struct command cmd);
/* Take up the next of the unit's orders that can still be carried out. Returns
 * false once there are none left. */
bool G_Command_Next(uint32_t uid);

bool G_Command_SaveState(struct SDL_RWops *stream);
bool G_Command_LoadState(struct SDL_RWops *stream);

#endif
