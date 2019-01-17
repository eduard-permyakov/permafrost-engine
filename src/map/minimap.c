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

#include "public/map.h"
#include "map_private.h"
#include "../render/public/render.h"
#include "../collision.h"
#include "../game/public/game.h"
#include "../event.h"

#include <SDL.h>

#include <assert.h>


#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static bool s_mouse_down_in_minimap = false;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static vec2_t m_minimap_mouse_coords_to_world(const struct map *map, vec2_t screen_coords)
{
    /* a, b, c, d are given in screen coordinates ((0,0) in top left corner) */
    vec2_t a, b, c, d;
    float minimap_axis_height = MINIMAP_SIZE/cos(M_PI/4.0f);

    /*            b
     *            + 
     *          /   \
     * (0,0) a +     + c (1,1)
     *          \   /
     *            +
     *            d
     */

    a = (vec2_t) { map->minimap_center_pos.x - minimap_axis_height/2.0f, map->minimap_center_pos.y };
    b = (vec2_t) { map->minimap_center_pos.x, map->minimap_center_pos.y - minimap_axis_height/2.0f };
    c = (vec2_t) { map->minimap_center_pos.x + minimap_axis_height/2.0f, map->minimap_center_pos.y };
    d = (vec2_t) { map->minimap_center_pos.x, map->minimap_center_pos.y + minimap_axis_height/2.0f };

    /* Now project the mouse coordinates (relative to A) on the AB line segment to get the X dimension fraction
     * and project onto the AD line segment to get the Z dimension fraction. */
    vec2_t ap, ab, ad;
    PFM_Vec2_Sub(&screen_coords, &a, &ap);
    PFM_Vec2_Sub(&b, &a, &ab);
    PFM_Vec2_Sub(&d, &a, &ad);

    float x_frac = PFM_Vec2_Dot(&ap, &ab) / PFM_Vec2_Dot(&ab, &ab);
    float z_frac = PFM_Vec2_Dot(&ap, &ad) / PFM_Vec2_Dot(&ad, &ad);
    
    /* Clamp to the range of [0.0, 1.0] to account for any imprecision */
    x_frac = MAX(x_frac, 0.0f);
    x_frac = MIN(x_frac, 1.0f);

    z_frac = MAX(z_frac, 0.0f);
    z_frac = MIN(z_frac, 1.0f);

    float map_ws_width  = map->width  * TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE;
    float map_ws_height = map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    return (vec2_t) {
        map->pos.x - x_frac * map_ws_width,
        map->pos.z + z_frac * map_ws_height
    };
}

static void on_mouseclick(void *user, void *event)
{
    const struct map *map = (const struct map*)user; 
    SDL_Event *mouse_event = (SDL_Event*)event;
    assert(mouse_event->type == SDL_MOUSEBUTTONDOWN);

    if(!M_MouseOverMinimap(map)){
        s_mouse_down_in_minimap = false;
        return;
    }else{
        s_mouse_down_in_minimap = true; 
    }

    if(mouse_event->button.button != SDL_BUTTON_LEFT)
        return;

    vec2_t ws_coords = m_minimap_mouse_coords_to_world(map, (vec2_t){mouse_event->button.x, mouse_event->button.y});
    G_MoveActiveCamera(ws_coords);
}

static void on_mousemove(void *user, void *event)
{
    const struct map *map = (const struct map*)user; 
    SDL_Event *mouse_event = (SDL_Event*)event;
    assert(mouse_event->type == SDL_MOUSEMOTION);

    if(!M_MouseOverMinimap(map))
        return;

    if(!s_mouse_down_in_minimap)
        return;

    if(!(mouse_event->motion.state & SDL_BUTTON_LMASK))
        return;

    vec2_t ws_coords = m_minimap_mouse_coords_to_world(map, (vec2_t){mouse_event->motion.x, mouse_event->motion.y});
    G_MoveActiveCamera(ws_coords);
}

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
            
            const struct pfchunk *curr = &map->chunks[r * map->width + c];
            chunk_rprivates[r * map->width + c] = curr->render_private;
            M_ModelMatrixForChunk(map, (struct chunkpos){r, c}, chunk_model_mats + (r * map->width + c));
        }
    }
    
    vec2_t map_size = (vec2_t) {
        map->width * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE, 
        map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE
    };
    vec3_t map_center = (vec3_t){ map->pos.x - map_size.raw[0]/2.0f, map->pos.y, map->pos.z + map_size.raw[1]/2.0f };

    bool ret = R_GL_MinimapBake(chunk_rprivates, chunk_model_mats, 
        map->width, map->height, map_center, map_size);

    if(ret) {
        E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mouseclick, map);
        E_Global_Register(SDL_MOUSEMOTION,     on_mousemove,  map);
    }
    return ret;
}

bool M_UpdateMinimapChunk(const struct map *map, int chunk_r, int chunk_c)
{
    vec2_t map_size = (vec2_t) {
        map->width * TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE, 
        map->height * TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE
    };
    vec3_t map_center = (vec3_t){ map->pos.x - map_size.raw[0]/2.0f, map->pos.y, map->pos.z + map_size.raw[1]/2.0f };
    mat4x4_t model;
    M_ModelMatrixForChunk(map, (struct chunkpos){chunk_r, chunk_c}, &model);

    return R_GL_MinimapUpdateChunk(map, map->chunks[chunk_r * map->width + chunk_c].render_private, &model,
        map_center, map_size);
}

void M_FreeMinimap(struct map *map)
{
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mouseclick);
    E_Global_Unregister(SDL_MOUSEMOTION,     on_mousemove);

    R_GL_MinimapFree();
    s_mouse_down_in_minimap = false;
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
    return C_PointInsideRect2D(mouse_pos, a, b, c ,d);
}

