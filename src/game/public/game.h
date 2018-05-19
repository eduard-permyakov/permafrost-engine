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

#include "../../map/public/map.h"

#include <stdbool.h>
#include <SDL.h>

struct entity;
struct map;
struct tile_desc;
struct tile;

enum cam_mode{
    CAM_MODE_FPS,
    CAM_MODE_RTS
};


bool G_Init(void);
bool G_NewGameWithMap(const char *dir, const char *pfmap);
bool G_NewGameWithMapString(const char *mapstr);
void G_Shutdown(void);

void G_Render(void);
void G_SetMapRenderMode(enum chunk_render_mode mode);
void G_SetMinimapPos(float x, float y);
bool G_MouseOverMinimap(void);
bool G_MapHeightAtPoint(vec2_t xz, float *out_height);

bool G_AddEntity(struct entity *ent);
bool G_RemoveEntity(struct entity *ent);

bool G_ActivateCamera(int idx, enum cam_mode mode);
void G_MoveActiveCamera(vec2_t xz_ground_pos);

bool G_UpdateMinimapChunk(int chunk_r, int chunk_c);
bool G_UpdateChunkMats(int chunk_c, int chunk_r, const char *mats_string);
bool G_UpdateTile(const struct tile_desc *desc, const struct tile *tile);

#endif

