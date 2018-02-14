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
#include <SDL.h>

struct entity;
struct map;

enum cam_mode{
    CAM_MODE_FPS,
    CAM_MODE_RTS
};


bool G_Init(void);
bool G_NewGameWithMap(const char *dir, const char *pfmap);
void G_Shutdown(void);

void G_Render(void);
void G_Update(void);

bool G_AddEntity(struct entity *ent);
bool G_RemoveEntity(struct entity *ent);

bool G_ActivateCamera(int idx, enum cam_mode mode);

#endif

