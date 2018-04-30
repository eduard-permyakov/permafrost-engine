/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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

#include "public/map.h"
#include "../render/public/render.h"
#include "map_private.h"
#include "pfchunk.h"
#include "../camera.h"

#include <unistd.h>
#include <string.h>
#include <assert.h>

#include <SDL.h>

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void M_ModelMatrixForChunk(const struct map *map, struct chunkpos p, mat4x4_t *out)
{
    ssize_t x_offset = -(p.c * TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE);
    ssize_t z_offset =  (p.r * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE);
    vec3_t chunk_pos = (vec3_t) {map->pos.x + x_offset, map->pos.y, map->pos.z + z_offset};
   
    PFM_Mat4x4_MakeTrans(chunk_pos.x, chunk_pos.y, chunk_pos.z, out);
}

void M_RenderEntireMap(const struct map *map)
{
    for(int r = 0; r < map->height; r++) {
        for(int c = 0; c < map->width; c++) {
        
            mat4x4_t chunk_model;
            const struct pfchunk *chunk = &map->chunks[r * map->width + c];
            void *render_private = 
                (chunk->mode == CHUNK_RENDER_MODE_REALTIME_BLEND) ? chunk->render_private_tiles
                                                                  : chunk->render_private_prebaked;

            M_ModelMatrixForChunk(map, (struct chunkpos) {r, c}, &chunk_model);
            R_GL_Draw(render_private, &chunk_model);
        }
    }
}

void M_CenterAtOrigin(struct map *map)
{
    size_t width  = map->width * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    size_t height = map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    map->pos = (vec3_t) {(width / 2.0f), 0.0f, -(height / 2.0f)};
}

void M_RestrictRTSCamToMap(const struct map *map, struct camera *cam)
{
    struct bound_box bounds;

    /* 'Camera_RestrictPosWithBox' restricts the position of the camera to an XZ box.
     * However, if we just let this box be the map position and dimensions, the corners will 
     * not appear equal due to the camera tilt. For example, with yaw = 135 and pitch = -70, 
     * less of the top center corner will be visible than the bottom center corner due to 
     * the camera being tilted up. What we actually want is the camera ray position at ground 
     * level to be bounded within the map box. To achieve this, we offset the camera 
     * position bounding box by the XZ components of the difference between the camera position 
     * and where the camera ray intersects the ground. 
     *
     * This assumes the camera pitch, yaw, and height will not change.
     */
    
    float offset_mag = cos(DEG_TO_RAD(Camera_GetPitch(cam))) * Camera_GetHeight(cam);

    bounds.x = map->pos.x - cos(DEG_TO_RAD(Camera_GetYaw(cam))) * offset_mag;
    bounds.z = map->pos.z + sin(DEG_TO_RAD(Camera_GetYaw(cam))) * offset_mag;
    bounds.w = map->width  * TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE;
    bounds.h = map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    Camera_RestrictPosWithBox(cam, bounds);
}

void M_SetChunkRenderMode(struct map *map, int chunk_r, int chunk_c, enum chunk_render_mode mode)
{
    assert(chunk_r >= 0 && chunk_r < map->height);
    assert(chunk_c >= 0 && chunk_r < map->width);

    struct pfchunk *chunk = &map->chunks[chunk_r * map->width + chunk_c];
    chunk->mode = mode;

    if(mode == CHUNK_RENDER_MODE_PREBAKED) {
        
        ssize_t x_offset = -(chunk_c * TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE);
        ssize_t z_offset =  (chunk_r * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE);
        vec3_t chunk_pos = (vec3_t) {map->pos.x + x_offset, map->pos.y, map->pos.z + z_offset};

        mat4x4_t chunk_model;
        PFM_Mat4x4_MakeTrans(chunk_pos.x, chunk_pos.y, chunk_pos.z, &chunk_model);

        vec3_t chunk_center = (vec3_t) {
            chunk_pos.x - (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE)/2.0f,
            chunk_pos.y,
            chunk_pos.z + (TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE)/2.0f
        };

        chunk->render_private_prebaked = R_GL_BakeChunk(chunk->render_private_tiles, chunk_center, &chunk_model,
            TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT, chunk->tiles, chunk_r, chunk_c);
    }
}

void M_SetMapRenderMode(struct map *map, enum chunk_render_mode mode)
{
    assert(map);

    for(int r = 0; r < map->height; r++) {
        for(int c = 0; c < map->width; c++) {
           M_SetChunkRenderMode(map, r, c, mode); 
        }
    }
}

