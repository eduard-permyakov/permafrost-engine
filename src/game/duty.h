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

#ifndef DUTY_H
#define DUTY_H

#include "public/game.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct SDL_RWops;
struct map;

bool G_Duty_Init(const struct map *map);
void G_Duty_Shutdown(void);
/* Ends the entity's own duty and every duty that has it for a target */
void G_Duty_RemoveEntity(uint32_t uid);

/* Takes the duty on; the same kind of work on the same good keeps who gave it
 * and when, only its target moves. 'by' is overridden by a tag set beforehand. */
void G_Duty_Set(uint32_t uid, enum duty_kind kind, uint32_t target, const char *good, enum duty_by by);
enum duty_kind G_Duty_Kind(uint32_t uid);
bool G_Duty_ByPlayer(uint32_t uid);
bool G_Duty_Waiting(uint32_t uid);
void G_Duty_SetWaiting(uint32_t uid, bool waiting);
/* Takes the duty up again after an interruption; true when the unit was sent on */
bool G_Duty_Resume(uint32_t uid);
/* A supplier with nothing to carry stands by its target for its next request */
bool G_Duty_StandBy(uint32_t uid);
/* The builders of a finished site are done with it, and so are its suppliers
 * if it stores nothing */
void G_Duty_OnCompleted(uint32_t site);
/* How many hold a duty of the kind on the target; of every good for a NULL name */
int  G_Duty_CountAt(uint32_t target, enum duty_kind kind, const char *good);

bool G_Duty_SaveState(struct SDL_RWops *stream);
bool G_Duty_LoadState(struct SDL_RWops *stream);

#endif
