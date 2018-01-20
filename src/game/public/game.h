/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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
 */

#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <SDL2/SDL.h>

struct entity;
struct map;

bool G_Init(void);
bool G_NewGameWithMap(const char *dir, const char *pfmap, const char *pfmat);
void G_Shutdown(void);

void G_Render(void);
void G_Update(void);
/* TODO: Eventually, have a centralized place of event handling and have different subsystems
 * register handlers for particular events. */
void G_HandleEvent(SDL_Event *e);

bool G_AddEntity(struct entity *ent);
bool G_RemoveEntity(struct entity *ent);

#endif
