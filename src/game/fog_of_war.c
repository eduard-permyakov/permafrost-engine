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
#include "../lib/public/pqueue.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"

#include <stdint.h>


#define MAX(a, b)   ((a) > (b) ? (a) : (b))

enum vision_state{
    VISION_UNEXPLORED = 0,
    VISION_IN_FOG,
    VISION_VISIBLE,
};

PQUEUE_TYPE(td, struct tile_desc)
PQUEUE_IMPL(static, td, struct tile_desc)

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
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static int td_index(struct tile_desc td)
{
    struct map_resolution res;
    M_GetResolution(s_map, &res);

    size_t tiles_per_chunk = res.tile_w * res.tile_h;
    return td.chunk_r * (res.chunk_w * tiles_per_chunk) + td.chunk_c * tiles_per_chunk
        + (td.tile_r * res.tile_w + td.tile_c);
}

static void update_tile(int faction_id, struct tile_desc td, int delta)
{
    uint8_t old = s_vision_refcnts[faction_id][td_index(td)];
    uint8_t new = old + delta;

    if(new) {
        s_vision_state[faction_id][td_index(td)] = VISION_VISIBLE;
    }else{
        s_vision_state[faction_id][td_index(td)] = VISION_IN_FOG;
    }

    s_vision_refcnts[faction_id][td_index(td)] = new;
}

static size_t neighbours(struct tile_desc curr, struct tile_desc *out)
{
    size_t ret = 0;

    struct map_resolution res;
    M_GetResolution(s_map, &res);

    for(int dr = -1; dr <= 1; dr++) {
    for(int dc = -1; dc <= 1; dc++) {

        if(dr == 0 && dc == 0)
            continue;

        struct tile_desc cand = curr;
        bool exists = M_Tile_RelativeDesc(res, &curr, dc, dr);
        if(!exists)
            continue;
        out[ret++] = cand;
    }}

    return ret;
}

static vec2_t tile_center_pos(struct tile_desc td)
{
    struct map_resolution res;
    M_GetResolution(s_map, &res);

    struct box box = M_Tile_Bounds(res, M_GetCenterPos(s_map), td);
    return (vec2_t){
        box.x - box.width / 2.0f,
        box.z + box.height / 2.0f
    };
}

static bool td_los_blocked(struct tile_desc td, int ref_height)
{
    struct tile *tile;
    M_TileForDesc(s_map, td, &tile);
    return (M_Tile_BaseHeight(tile) - ref_height > 1);
}

static bool td_is_los_corner(struct tile_desc td, int ref_height)
{
    if(!td_los_blocked(td, ref_height))
        return false;

    struct map_resolution res;
    M_GetResolution(s_map, &res);

    struct tile_desc left = td, right = td;
    if(M_Tile_RelativeDesc(res, &left,  -1, 0)
    && M_Tile_RelativeDesc(res, &right, +1, 0)) {

        if(td_los_blocked(left, ref_height) ^ td_los_blocked(right, ref_height))
            return true;
    }

    struct tile_desc top = td, bot = td;
    if(M_Tile_RelativeDesc(res, &top, 0, -1)
    && M_Tile_RelativeDesc(res, &bot, 0, +1)) {

        if(td_los_blocked(top, ref_height) ^ td_los_blocked(bot, ref_height))
            return true;
    }

    return false;
}

static void wf_create_blocked_line(int xrad, int zrad, bool *wf[xrad * 2 + 1], 
                                   struct tile_desc origin, int delta_r, int delta_c)
{
    struct map_resolution res;
    M_GetResolution(s_map, &res);

    struct tile_desc corner;
    M_Tile_RelativeDesc(res, &corner, delta_c, delta_r);

    vec2_t origin_center = tile_center_pos(origin);
    vec2_t corner_center = tile_center_pos(corner);

    vec2_t slope;
    PFM_Vec2_Sub(&origin_center, &corner_center, &slope);
    PFM_Vec2_Normal(&slope, &slope);

    /* Now use Bresenham's line drawing algorithm to follow a line 
     * of the computed slope starting at the 'corner' until we hit the 
     * edge of the field. 
     * Multiply by 1_000 to convert slope to integer deltas, but keep 
     * 3 digits of precision after the decimal.*/
    int dx =  abs(slope.raw[0] * 1000);
    int dy = -abs(slope.raw[1] * 1000);
    int sx = slope.raw[0] > 0.0f ? 1 : -1;
    int sy = slope.raw[1] < 0.0f ? 1 : -1;
    int err = dx + dy, e2;

    int curr_dr = delta_r, curr_dc = delta_c;
    do {

        wf[curr_dr][curr_dc] = true;

        e2 = 2 * err;
        if(e2 >= dy) {
            err += dy;
            curr_dc  += sx;
        }
        if(e2 <= dx) {
            err += dx;
            curr_dr += sy;
        }

    }while(curr_dr >= -zrad && curr_dr <= zrad && curr_dc >= xrad && curr_dc <= -xrad);
}

void td_delta(struct tile_desc a, struct tile_desc b, int *out_dr, int *out_dc)
{
    struct map_resolution res;
    M_GetResolution(s_map, &res);

    int ar = a.chunk_r * res.chunk_h + a.tile_r;
    int ac = a.chunk_c * res.chunk_w + a.tile_c;

    int br = b.chunk_r * res.chunk_h + b.tile_r;
    int bc = b.chunk_c * res.chunk_w + b.tile_c;

    *out_dr = br - ar;
    *out_dc = bc - ac;
}

static void fog_update_visible(int faction_id, vec2_t xz_pos, float radius, int delta)
{
    struct map_resolution res;
    M_GetResolution(s_map, &res);

    struct tile_desc origin;
    M_Tile_DescForPoint2D(res, M_GetCenterPos(s_map), xz_pos, &origin);

    struct tile *tile;
    M_TileForDesc(s_map, origin, &tile);
    int origin_height = M_Tile_BaseHeight(tile);

    const int tile_x_radius = ceil(radius / X_COORDS_PER_TILE);
    const int tile_z_radius = ceil(radius / Z_COORDS_PER_TILE);

    /* Declare a byte for every tile within a box having a half-length of 'radius' 
     * that surrounds the position. When the position is near the map edge, some
     * elements may be unused.  wf_blocked[tile_x_radius][tile_z_radius] gives the 
     * byte corresponding to the origin-most tile. */
    bool wf_blocked[2 * tile_x_radius  + 1][2 * tile_z_radius + 1];
    memset(wf_blocked, 0, sizeof(wf_blocked));

    bool visited[2 * tile_x_radius  + 1][2 * tile_z_radius + 1];
    memset(visited, 0, sizeof(visited));

    pq_td_t frontier;
    pq_td_init(&frontier);

    pq_td_push(&frontier, 0.0f, origin);
    visited[tile_x_radius][tile_z_radius] = true;
    update_tile(faction_id, origin, delta);

    while(pq_size(&frontier) > 0) {

        struct tile_desc curr;
        pq_td_pop(&frontier, &curr);

        struct tile_desc neighbs[8];
        size_t num_neighbs = neighbours(curr, neighbs);

        for(int i = 0; i < num_neighbs; i++) {

            int dr, dc;
            td_delta(origin, neighbs[i], &dr, &dc);

            if(visited[tile_x_radius + dr][tile_z_radius + dc])
                continue;
            visited[tile_x_radius + dr][tile_z_radius + dc] = true;

            if(wf_blocked[tile_x_radius + dr][tile_z_radius + dc])
                continue;

            vec2_t origin_pos = tile_center_pos(origin);
            vec2_t neighb_pos = tile_center_pos(neighbs[i]);

            vec2_t origin_delta;
            PFM_Vec2_Sub(&neighb_pos, &origin_pos, &origin_delta);

            if(PFM_Vec2_Len(&origin_delta) > radius)
                continue;

            if(td_is_los_corner(neighbs[i], origin_height))
                wf_create_blocked_line(tile_x_radius, tile_z_radius, (bool**)wf_blocked, origin, dr, dc);

            if(td_los_blocked(neighbs[i], origin_height))
                continue;

            update_tile(faction_id, neighbs[i], delta);
            pq_td_push(&frontier, PFM_Vec2_Len(&origin_delta), neighbs[i]);
        }
    }

    pq_td_destroy(&frontier);
}

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
    fog_update_visible(faction_id, xz_pos, radius, +1);
}

void G_Fog_RemoveVision(vec2_t xz_pos, int faction_id, float radius)
{
    fog_update_visible(faction_id, xz_pos, radius, -1);
}

