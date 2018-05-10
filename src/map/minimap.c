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
#include "map_private.h"
#include "../render/public/render.h"
#include "../collision.h"

#include <assert.h>

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool M_InitMinimap(struct map *map, vec2_t center_pos)
{
    assert(map);
    map->minimap_center_pos = center_pos;

    void *chunk_rprivates[map->width * map->height];
    mat4x4_t chunk_model_mats[map->width * map->height];

    for(int r = 0; r < map->height; r++) {
        for(int c = 0; c < map->width; c++) {
            
            const struct pfchunk *curr = &map->chunks[r * map->height + c];
            chunk_rprivates[r * map->height + c] = curr->render_private_tiles;
            M_ModelMatrixForChunk(map, (struct chunkpos){r, c}, chunk_model_mats + (r * map->height + c));
        }
    }
    
    vec2_t map_size = (vec2_t) {
        map->width * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE, 
        map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE
    };
    vec3_t map_center = (vec3_t){ map->pos.x - map_size.raw[0]/2.0f, map->pos.y, map->pos.z + map_size.raw[1]/2.0f };

    return R_GL_MinimapBake(chunk_rprivates, chunk_model_mats, 
        map->width, map->height, map_center, map_size);
}

void M_SetMinimapPos(struct map *map, vec2_t center_pos)
{
    assert(map);
    map->minimap_center_pos = center_pos;
}

void M_RenderMinimap(const struct map *map, const struct camera *cam)
{
    assert(map);
    R_GL_MinimapRender(map, cam, map->minimap_center_pos);
}

bool M_MouseOverMinimap(const struct map *map)
{
    /* a, b, c, d are given in screen coordinates ((0,0) in top left corner) */
    vec2_t a, b, c, d;
    float minimap_axis_height = MINIMAP_SIZE/cos(M_PI/4.0f);

    /*      b
     *      + 
     *    /   \
     * a +     + c
     *    \   /
     *      +
     *      d
     */

    a = (vec2_t) { map->minimap_center_pos.x - minimap_axis_height/2.0f, map->minimap_center_pos.y };
    b = (vec2_t) { map->minimap_center_pos.x, map->minimap_center_pos.y - minimap_axis_height/2.0f };
    c = (vec2_t) { map->minimap_center_pos.x + minimap_axis_height/2.0f, map->minimap_center_pos.y };
    d = (vec2_t) { map->minimap_center_pos.x, map->minimap_center_pos.y + minimap_axis_height/2.0f };

    int x, y;
    SDL_GetMouseState(&x, &y);
    vec2_t mouse_pos = (vec2_t){x, y};
    return C_PointInsideScreenRect(mouse_pos, a, b, c ,d);
}

