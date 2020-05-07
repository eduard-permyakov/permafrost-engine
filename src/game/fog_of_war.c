/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#include "fog_of_war.h"
#include "public/game.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"

#include <stdint.h>


enum vision_state{
    VISION_UNEXPLORED = 0,
    VISION_IN_FOG,
    VISION_VISIBLE,
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map *s_map;
/* Holds a tile for every tile of the map. The chunks are stored in row-major
 * order. Within a chunk, the tiles are in row-major order. */
static uint8_t          *s_vision_state[MAX_FACTIONS];
/* How many units of a faction currently 'see' every tile. */
static uint8_t          *s_vision_refcnts[MAX_FACTIONS];

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Fog_Init(const struct map *map)
{
    struct map_resolution res;
    M_GetResolution(map, &res);
    const size_t ntiles = res.chunk_w * res.chunk_h * res.tile_w * res.tile_h;

    for(int i = 0; i < MAX_FACTIONS; i++) {
        s_vision_state[i] = calloc(sizeof(s_vision_state[0]), ntiles);
        if(!s_vision_state[i])
            goto fail;
        s_vision_refcnts[i] = calloc(sizeof(s_vision_refcnts[0]), ntiles);
        if(!s_vision_refcnts[i])
            goto fail;
    }

    s_map = map;
    return true;

fail:
    for(int i = 0; i < MAX_FACTIONS; i++) {
        free(s_vision_state[i]);
        free(s_vision_refcnts[i]);
    }
    return false;
}

void G_Fog_Shutdown(void)
{
    for(int i = 0; i < MAX_FACTIONS; i++) {
        free(s_vision_state[i]);
        free(s_vision_refcnts[i]);
    }
    memset(s_vision_state, 0, sizeof(s_vision_state));
    memset(s_vision_refcnts, 0, sizeof(s_vision_refcnts));
    s_map = NULL;
}

void G_Fog_AddVision(vec2_t xz_pos, int faction_id, float radius)
{

}

void G_Fog_RemoveVision(vec2_t xz_pos, int faction_id, float radius)
{

}

