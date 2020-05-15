/* 
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2020 Eduard Permyakov 
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

#include "public/nav.h"
#include "nav_private.h"
#include "a_star.h"
#include "field.h"
#include "fieldcache.h"
#include "../map/public/tile.h"
#include "../game/public/game.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../pf_math.h"
#include "../collision.h"
#include "../entity.h"
#include "../event.h"
#include "../main.h"
#include "../perf.h"
#include "../lib/public/queue.h"

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <float.h>


#define IDX(r, width, c)   ((r) * (width) + (c))
#define CURSOR_OFF(cursor, base) ((ptrdiff_t)((cursor) - (base)))
#define ARR_SIZE(a)              (sizeof(a)/sizeof(a[0]))

#define MIN(a, b)                ((a) < (b) ? (a) : (b))
#define MAX(a, b)                ((a) > (b) ? (a) : (b))
#define CLAMP(a, min, max)       (MIN(MAX((a), (min)), (max)))

#define EPSILON                  (1.0f / 1024)

#define FOREACH_PORTAL(_priv, _local, ...)                                                      \
    do{                                                                                         \
        for(int chunk_r = 0; chunk_r < (_priv)->height; chunk_r++) {                            \
        for(int chunk_c = 0; chunk_c < (_priv)->width;  chunk_c++) {                            \
                                                                                                \
            struct nav_chunk *curr_chunk = &(_priv)->chunks[IDX(chunk_r, (_priv)->width, chunk_c)]; \
            for((_local) = curr_chunk->portals;                                                 \
                (_local) < curr_chunk->portals + curr_chunk->num_portals; (_local)++) {         \
                                                                                                \
                __VA_ARGS__                                                                     \
            }                                                                                   \
        }}                                                                                      \
    }while(0)

QUEUE_TYPE(td, struct tile_desc)
QUEUE_IMPL(static, td, struct tile_desc)

struct cost_coord{
    float        cost;
    struct coord coord;
};

QUEUE_TYPE(cc, struct cost_coord)
QUEUE_IMPL(static, cc, struct cost_coord)

enum edge_type{
    EDGE_BOT   = (1 << 0),
    EDGE_LEFT  = (1 << 1),
    EDGE_RIGHT = (1 << 2),
    EDGE_TOP   = (1 << 3),
};

KHASH_SET_INIT_INT(coord)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(coord) *s_dirty_chunks;
static bool            s_local_islands_dirty = false;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool n_tile_pathable(const struct tile *tile)
{
    if(!tile->pathable)
        return false;

    if(tile->type != TILETYPE_FLAT && tile->ramp_height > 1)
        return false;

    return true;
}

static void n_set_cost_for_tile(struct nav_chunk *chunk, 
                                size_t chunk_w, size_t chunk_h,
                                size_t tile_r,  size_t tile_c,
                                const struct tile *tile)
{
    assert(FIELD_RES_R / chunk_h == 2);
    assert(FIELD_RES_C / chunk_w == 2);

    const int (*tile_path_map)[2] = {0};
    const int clear[2][2] = {
        {0,0}, 
        {0,0},
    };
    const int bl[2][2] = {
        {0,0}, 
        {1,0},
    };
    const int br[2][2] = {
        {0,0}, 
        {0,1},
    };
    const int tl[2][2] = {
        {1,0}, 
        {0,0},
    };
    const int tr[2][2] = {
        {0,1}, 
        {0,0},
    };

    switch(tile->type) {
    case TILETYPE_FLAT:
    case TILETYPE_RAMP_SN:
    case TILETYPE_RAMP_NS:
    case TILETYPE_RAMP_EW:
    case TILETYPE_RAMP_WE:
        tile_path_map = clear;
        break;
    case TILETYPE_CORNER_CONCAVE_SW:
    case TILETYPE_CORNER_CONVEX_NE:
        tile_path_map = bl;
        break;
    case TILETYPE_CORNER_CONCAVE_SE:
    case TILETYPE_CORNER_CONVEX_NW:
        tile_path_map = br;
        break;
    case TILETYPE_CORNER_CONCAVE_NW:
    case TILETYPE_CORNER_CONVEX_SE:
        tile_path_map = tl;
        break;
    case TILETYPE_CORNER_CONCAVE_NE:
    case TILETYPE_CORNER_CONVEX_SW:
        tile_path_map = tr;
        break;
    default: assert(0);
    }

    size_t r_base = tile_r * 2;
    size_t c_base = tile_c * 2;

    for(int r = 0; r < 2; r++) {
    for(int c = 0; c < 2; c++) {

        chunk->cost_base[r_base + r][c_base + c] = n_tile_pathable(tile) ? 1 
                                                 : tile_path_map[r][c]   ? 1
                                                 : COST_IMPASSABLE;
    }}
}

static void n_clear_cost_for_tile(struct nav_chunk *chunk, 
                                  size_t chunk_w, size_t chunk_h,
                                  size_t tile_r,  size_t tile_c)
{
    assert(FIELD_RES_R / chunk_h == 2);
    assert(FIELD_RES_C / chunk_w == 2);

    size_t r_base = tile_r * 2;
    size_t c_base = tile_c * 2;

    for(int r = 0; r < 2; r++) {
    for(int c = 0; c < 2; c++) {

        chunk->cost_base[r_base + r][c_base + c] = 0;
    }}
}

static void n_set_cost_edge(struct nav_chunk *chunk,
                            size_t chunk_w, size_t chunk_h,
                            size_t tile_r,  size_t tile_c,
                            enum edge_type edge)
{
    assert(FIELD_RES_R / chunk_h == 2);
    assert(FIELD_RES_C / chunk_w == 2);

    const int (*tile_path_map)[2] = (int[2][2]){
        {0,0}, 
        {0,0},
    };
    const int bot[2][2] = {
        {0,0}, 
        {1,1},
    };
    const int top[2][2] = {
        {1,1}, 
        {0,0},
    };
    const int left[2][2] = {
        {1,0}, 
        {1,0},
    };
    const int right[2][2] = {
        {0,1}, 
        {0,1},
    };

    switch(edge){
    case EDGE_BOT:
        tile_path_map = bot;
        break;
    case EDGE_TOP:
        tile_path_map = top;
        break;
    case EDGE_LEFT:
        tile_path_map = left;
        break;
    case EDGE_RIGHT:
        tile_path_map = right;
        break;
    default: 
        assert(0);
    }

    size_t r_base = tile_r * 2;
    size_t c_base = tile_c * 2;

    for(int r = 0; r < 2; r++) {
    for(int c = 0; c < 2; c++) {

        if(!tile_path_map[r][c])
            chunk->cost_base[r_base + r][c_base + c] = COST_IMPASSABLE;
    }}
}

static bool n_cliff_edge(const struct tile *a, const struct tile *b)
{
    if(!a || !b)
        return false;

    if(a->type != TILETYPE_FLAT || b->type != TILETYPE_FLAT)
        return false;

    return (a->base_height != b->base_height);
}

static void n_make_cliff_edges(struct nav_private *priv, const struct tile **tiles,
                               size_t chunk_w, size_t chunk_h)
{
    for(int r = 0; r < priv->height; r++) {
    for(int c = 0; c < priv->width; c++) {
            
        struct nav_chunk *curr_chunk = &priv->chunks[IDX(r, priv->width, c)];

        const struct tile *bot_tiles = (r < priv->height-1)  ? tiles[IDX(r+1, priv->width, c)] : NULL;
        const struct tile *top_tiles = (r > 0)               ? tiles[IDX(r-1, priv->width, c)] : NULL;
        const struct tile *right_tiles = (c < priv->width-1) ? tiles[IDX(r, priv->width, c+1)] : NULL;
        const struct tile *left_tiles = (c > 0)              ? tiles[IDX(r, priv->width, c-1)] : NULL;

        for(int chr = 0; chr < chunk_h; chr++) {
        for(int chc = 0; chc < chunk_w; chc++) {

            const struct tile *curr_tile = &tiles[IDX(r, priv->width, c)][IDX(chr, chunk_w, chc)];
            const struct tile *bot_tile   = (chr < chunk_h-1) ? curr_tile + chunk_w 
                                          : bot_tiles         ? &bot_tiles[IDX(0, chunk_w, chc)]
                                          : NULL;
            const struct tile *top_tile   = (chr > 0)         ? curr_tile - chunk_w
                                          : top_tiles         ? &top_tiles[IDX(chunk_h-1, chunk_w, chc)]
                                          : NULL;
            const struct tile *left_tile  = (chc > 0)         ? curr_tile - 1 
                                          : left_tiles        ? &left_tiles[IDX(chr, chunk_w, chunk_w-1)]
                                          : NULL;
            const struct tile *right_tile = (chc < chunk_w-1) ? curr_tile + 1 
                                          : right_tiles       ? &right_tiles[IDX(chr, chunk_w, 0)]
                                          : NULL;

            if(n_cliff_edge(curr_tile, bot_tile))
                n_set_cost_edge(curr_chunk, chunk_w, chunk_h, chr, chc, EDGE_BOT);

            if(n_cliff_edge(curr_tile, top_tile))
                n_set_cost_edge(curr_chunk, chunk_w, chunk_h, chr, chc, EDGE_TOP);

            if(n_cliff_edge(curr_tile, left_tile))
                n_set_cost_edge(curr_chunk, chunk_w, chunk_h, chr, chc, EDGE_LEFT);

            if(n_cliff_edge(curr_tile, right_tile))
                n_set_cost_edge(curr_chunk, chunk_w, chunk_h, chr, chc, EDGE_RIGHT);
        }}
    }}
}

static void n_link_chunks(struct nav_chunk *a, enum edge_type a_type, struct coord a_coord,
                          struct nav_chunk *b, enum edge_type b_type, struct coord b_coord)
{
    assert(((a_type | b_type) == (EDGE_BOT | EDGE_TOP)) || ((a_type | b_type) == (EDGE_LEFT | EDGE_RIGHT)));
    size_t stride = (a_type & (EDGE_BOT | EDGE_TOP)) ? 1 : FIELD_RES_C;
    size_t line_len = (a_type & (EDGE_BOT | EDGE_TOP)) ? FIELD_RES_C : FIELD_RES_R;

    uint8_t *a_cursor = &a->cost_base[a_type == EDGE_BOT   ? FIELD_RES_R-1 : 0]
                                     [a_type == EDGE_RIGHT ? FIELD_RES_C-1 : 0];
    uint8_t *b_cursor = &b->cost_base[b_type == EDGE_BOT   ? FIELD_RES_R-1 : 0]
                                     [b_type == EDGE_RIGHT ? FIELD_RES_C-1 : 0];

    int a_fixed_idx = a_type == EDGE_TOP   ? 0
                    : a_type == EDGE_BOT   ? FIELD_RES_R-1
                    : a_type == EDGE_LEFT  ? 0
                    : a_type == EDGE_RIGHT ? FIELD_RES_C-1
                    : (assert(0), 0);
    int b_fixed_idx = b_type == EDGE_TOP   ? 0
                    : b_type == EDGE_BOT   ? FIELD_RES_R-1
                    : b_type == EDGE_LEFT  ? 0
                    : b_type == EDGE_RIGHT ? FIELD_RES_C-1
                    : (assert(0), 0);

    bool in_portal = false;
    for(int i = 0; i < line_len; i++) {

        assert(CURSOR_OFF(a_cursor, &a->cost_base[0][0]) >= 0 
            && CURSOR_OFF(a_cursor, &a->cost_base[0][0]) < (FIELD_RES_R*FIELD_RES_C));
        assert(CURSOR_OFF(b_cursor, &b->cost_base[0][0]) >= 0 
            && CURSOR_OFF(a_cursor, &b->cost_base[0][0]) < (FIELD_RES_R*FIELD_RES_C));

        bool can_cross = *a_cursor != COST_IMPASSABLE && *b_cursor != COST_IMPASSABLE;
        /* First tile of portal */
        if(can_cross && !in_portal) {

            in_portal = true;
            a->portals[a->num_portals] = (struct portal) {
                .component_id   = 0,
                .chunk          = a_coord,
                .endpoints[0]   = (a_type & (EDGE_TOP | EDGE_BOT))    ? (struct coord){a_fixed_idx, i}
                                : (a_type & (EDGE_LEFT | EDGE_RIGHT)) ? (struct coord){i, a_fixed_idx}
                                : (assert(0), (struct coord){0}),
                .num_neighbours = 0,
                .connected      = &b->portals[b->num_portals]
            };
            b->portals[b->num_portals] = (struct portal) {
                .component_id   = 0,
                .chunk          = b_coord,
                .endpoints[0]   = (b_type & (EDGE_TOP | EDGE_BOT))    ? (struct coord){b_fixed_idx, i}
                                : (b_type & (EDGE_LEFT | EDGE_RIGHT)) ? (struct coord){i, b_fixed_idx}
                                : (assert(0), (struct coord){0}),
                .num_neighbours = 0,
                .connected      = &a->portals[a->num_portals]
            };

        /* Last tile of portal */
        }else if(in_portal && (!can_cross || i == line_len - 1)) {

            int idx = !can_cross ? i-1 : i;
            in_portal = false;
            a->portals[a->num_portals].endpoints[1] 
                = (a_type & (EDGE_TOP | EDGE_BOT))    ? (struct coord){a_fixed_idx, idx}
                : (a_type & (EDGE_LEFT | EDGE_RIGHT)) ? (struct coord){idx, a_fixed_idx}
                : (assert(0), (struct coord){0});
            b->portals[b->num_portals].endpoints[1] 
                = (b_type & (EDGE_TOP | EDGE_BOT))    ? (struct coord){b_fixed_idx, idx}
                : (b_type & (EDGE_LEFT | EDGE_RIGHT)) ? (struct coord){idx, b_fixed_idx}
                : (assert(0), (struct coord){0});

            a->num_portals++;
            b->num_portals++;

            assert(a->num_portals <= MAX_PORTALS_PER_CHUNK);
            assert(b->num_portals <= MAX_PORTALS_PER_CHUNK);
        }

        a_cursor += stride;
        b_cursor += stride;
    }
}

static void n_create_portals(struct nav_private *priv)
{
    size_t n_links = 0;

    for(int r = 0; r < priv->height; r++) {
        for(int c = 0; c < priv->width; c++) {
            
            struct nav_chunk *curr = &priv->chunks[IDX(r, priv->width, c)];
            struct nav_chunk *bot = (r < priv->height-1) ? &priv->chunks[IDX(r+1, priv->width, c)] : NULL;
            struct nav_chunk *right = (c < priv->width-1) ? &priv->chunks[IDX(r, priv->width, c+1)] : NULL;

            if(bot)   n_link_chunks(curr, EDGE_BOT, (struct coord){r, c}, bot, EDGE_TOP, (struct coord){r+1, c});
            if(right) n_link_chunks(curr, EDGE_RIGHT, (struct coord){r, c}, right, EDGE_LEFT, (struct coord){r, c+1});
    
            n_links += (!!bot + !!right);
        }
    }

    assert(n_links == (priv->height)*(priv->width-1) + (priv->width)*(priv->height-1));
}

static void n_link_chunk_portals(struct nav_chunk *chunk, struct coord chunk_coord)
{
    vec_coord_t path;
    vec_coord_init(&path);

    for(int i = 0; i < chunk->num_portals; i++) {

        struct portal *port = &chunk->portals[i];
        for(int j = 0; j < chunk->num_portals; j++) {

            if(i == j)
                continue;

            struct portal *link_candidate = &chunk->portals[j];
            struct coord a = (struct coord){
                (port->endpoints[0].r + port->endpoints[1].r) / 2,
                (port->endpoints[0].c + port->endpoints[1].c) / 2,
            };
            struct coord b = (struct coord){
                (link_candidate->endpoints[0].r + link_candidate->endpoints[1].r) / 2,
                (link_candidate->endpoints[0].c + link_candidate->endpoints[1].c) / 2,
            };

            float cost;
            bool has_path = AStar_GridPath(a, b, chunk_coord, chunk->cost_base, &path, &cost);
            if(has_path) {
                port->edges[port->num_neighbours] = (struct edge){EDGE_STATE_ACTIVE, link_candidate, cost};
                port->num_neighbours++;    
            }
        }
    }

    vec_coord_destroy(&path);
}

static void n_visit_portal(struct portal *port, int comp_id)
{
    if(port->component_id != 0)
        return;

    port->component_id = comp_id;
    n_visit_portal(port->connected, comp_id); 

    for(int i = 0; i < port->num_neighbours; i++) {

        struct portal *curr = port->edges[i].neighbour;
        if(port->edges[i].es == EDGE_STATE_BLOCKED)
            continue;
        n_visit_portal(curr, comp_id);
    }
}

static void n_update_components(struct nav_private *priv)
{
    struct portal *port;
    FOREACH_PORTAL(priv, port,{
        port->component_id = 0;
    });

    int comp_id = 1;
    FOREACH_PORTAL(priv, port, {
        n_visit_portal(port, comp_id++);
    });
}

/* Two portals are considered reachable from one another if they
 * have at least one local island ID in common between their 
 * non-blocked tiles. */
static bool n_local_ports_connected(struct portal *a, struct portal *b, 
                                    const struct nav_chunk *chunk)
{
    for(int r1 = a->endpoints[0].r; r1 <= a->endpoints[1].r; r1++) {
    for(int c1 = a->endpoints[0].c; c1 <= a->endpoints[1].c; c1++) {

        if(chunk->blockers[r1][c1] > 0)
            continue;

        for(int r2 = b->endpoints[0].r; r2 <= b->endpoints[1].r; r2++) {
        for(int c2 = b->endpoints[0].c; c2 <= b->endpoints[1].c; c2++) {

            if(chunk->blockers[r2][c2] > 0)
                continue;

            if(chunk->local_islands[r1][c1] == chunk->local_islands[r2][c2])
                return true;
        }}
    }}
    return false;
}

static int n_update_edge_states(struct nav_chunk *chunk)
{
    int ret = 0;
    for(int i = 0; i < chunk->num_portals; i++) {

        struct portal *port = &chunk->portals[i];

        for(int j = 0; j < port->num_neighbours; j++) {

            struct portal *neighb = port->edges[j].neighbour;
            bool conn = n_local_ports_connected(port, neighb, chunk);

            enum edge_state new_es = conn ? EDGE_STATE_ACTIVE : EDGE_STATE_BLOCKED;
            enum edge_state old_es = port->edges[j].es;

            if(new_es != old_es) {
                port->edges[j].es = new_es;
                ret++;
            }
        }
    }

    return ret;
}

static void n_render_grid_path(struct nav_chunk *chunk, mat4x4_t *chunk_model,
                               const struct map *map, vec_coord_t *path, vec3_t color)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    vec2_t corners_buff[4 * vec_size(path)];
    vec3_t colors_buff[vec_size(path)];

    vec2_t *corners_base = corners_buff;
    vec3_t *colors_base = colors_buff; 

    for(int i = 0, r = vec_AT(path, i).r, c = vec_AT(path, i).c; 
        i < vec_size(path); 
        i++, r = vec_AT(path, i).r, c = vec_AT(path, i).c) {

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP( (((float)r) / FIELD_RES_R) * chunk_z_dim, -chunk_z_dim, chunk_z_dim);

        *corners_base++ = (vec2_t){square_x, square_z};
        *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

        *colors_base++ = color;
    }

    assert(colors_base == colors_buff + ARR_SIZE(colors_buff));
    assert(corners_base == corners_buff + ARR_SIZE(corners_buff));

    size_t count = vec_size(path);
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawMapOverlayQuads,
        .nargs = 5,
        .args = {
            R_PushArg(corners_buff, sizeof(corners_buff)),
            R_PushArg(colors_buff, sizeof(colors_buff)),
            R_PushArg(&count, sizeof(count)),
            R_PushArg(chunk_model, sizeof(*chunk_model)),
            (void*)G_GetPrevTickMap(),
        },
    });
}

static void n_render_portals(const struct nav_chunk *chunk, mat4x4_t *chunk_model,
                             const struct map *map, vec3_t color)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    vec2_t corners_buff[4 * (2 * FIELD_RES_C + 2 * FIELD_RES_R)];
    vec3_t colors_buff[2 * FIELD_RES_C + 2 * FIELD_RES_R];
    size_t num_tiles = 0;

    vec2_t *corners_base = corners_buff;
    vec3_t *colors_base = colors_buff; 

    for(int i = 0; i < chunk->num_portals; i++) {
        
        const struct portal *port = &chunk->portals[i];
        int r_start = MIN(port->endpoints[0].r, port->endpoints[1].r);
        int r_end = MAX(port->endpoints[0].r, port->endpoints[1].r);

        for(int r = r_start; r <= r_end; r++) {

            int c_start = MIN(port->endpoints[0].c, port->endpoints[1].c);
            int c_end = MAX(port->endpoints[0].c, port->endpoints[1].c);
            for(int c = c_start; c <= c_end; c++) {

                float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
                float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
                float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, -chunk_x_dim, chunk_x_dim);
                float square_z = CLAMP( (((float)r) / FIELD_RES_R) * chunk_z_dim, -chunk_z_dim, chunk_z_dim);

                *corners_base++ = (vec2_t){square_x, square_z};
                *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
                *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
                *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

                *colors_base++ = color;
                num_tiles++;
            }
        }
    }

    R_PushCmd((struct rcmd){
        .func = R_GL_DrawMapOverlayQuads,
        .nargs = 5,
        .args = {
            R_PushArg(corners_buff, sizeof(corners_buff)),
            R_PushArg(colors_buff, sizeof(colors_buff)),
            R_PushArg(&num_tiles, sizeof(num_tiles)),
            R_PushArg(chunk_model, sizeof(*chunk_model)),
            (void*)G_GetPrevTickMap(),
        },
    });
}

static dest_id_t n_dest_id(struct tile_desc dst_desc)
{
    return (((uint32_t)dst_desc.chunk_r & 0xff) << 24)
         | (((uint32_t)dst_desc.chunk_c & 0xff) << 16)
         | (((uint32_t)dst_desc.tile_r  & 0xff) <<  8)
         | (((uint32_t)dst_desc.tile_c  & 0xff) <<  0);
}

static void n_visit_island(struct nav_private *priv, uint16_t id, struct tile_desc start)
{
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct nav_chunk *chunk = &priv->chunks[IDX(start.chunk_r, priv->width, start.chunk_c)];
    chunk->islands[start.tile_r][start.tile_c] = id;

    queue_td_t frontier;
    queue_td_init(&frontier, 1024);
    queue_td_push(&frontier, &start);

    while(queue_size(frontier) > 0) {
    
        struct tile_desc curr;
        queue_td_pop(&frontier, &curr);

        struct coord deltas[] = {
            { 0, -1},
            { 0, +1},
            {-1,  0},
            {+1,  0},
        };

        for(int i = 0; i < ARR_SIZE(deltas); i++) {
        
            struct tile_desc neighb = curr;
            if(!M_Tile_RelativeDesc(res, &neighb, deltas[i].c, deltas[i].r))
                continue;

            assert(memcmp(&curr, &neighb, sizeof(struct tile_desc)) != 0);
            chunk = &priv->chunks[IDX(neighb.chunk_r, priv->width, neighb.chunk_c)];

            if(chunk->islands[neighb.tile_r][neighb.tile_c] == ISLAND_NONE
            && chunk->cost_base[neighb.tile_r][neighb.tile_c] != COST_IMPASSABLE) {
            
                chunk->islands[neighb.tile_r][neighb.tile_c] = id;
                queue_td_push(&frontier, &neighb);
            }
        }
    }

    queue_td_destroy(&frontier);
}

static void n_visit_island_local(struct nav_chunk *chunk, uint16_t id, struct coord start)
{
    struct map_resolution res = {
        1, 1, FIELD_RES_C, FIELD_RES_R
    };
    struct tile_desc start_td = {
        0, 0, start.r, start.c
    };

    queue_td_t frontier;
    queue_td_init(&frontier, 1024);

    chunk->local_islands[start.r][start.c] = id;
    queue_td_push(&frontier, &start_td);

    while(queue_size(frontier) > 0) {
    
        struct tile_desc curr;
        queue_td_pop(&frontier, &curr);

        struct coord deltas[] = {
            { 0, -1},
            { 0, +1},
            {-1,  0},
            {+1,  0},
        };

        for(int i = 0; i < ARR_SIZE(deltas); i++) {
        
            struct tile_desc neighb = curr;
            if(!M_Tile_RelativeDesc(res, &neighb, deltas[i].c, deltas[i].r))
                continue;

            if(chunk->cost_base[neighb.tile_r][neighb.tile_c] == COST_IMPASSABLE)
                continue;

            if(chunk->blockers[neighb.tile_r][neighb.tile_c] > 0)
                continue;

            if(chunk->local_islands[neighb.tile_r][neighb.tile_c] != ISLAND_NONE)
                continue;

            chunk->local_islands[neighb.tile_r][neighb.tile_c] = id;
            queue_td_push(&frontier, &neighb);
        }
    }

    queue_td_destroy(&frontier);
}

static bool enemy_ent(const struct entity *ent, void *arg)
{
    int faction_id = (uintptr_t)arg;
    enum diplomacy_state ds;

    if(!(ent->flags & ENTITY_FLAG_COMBATABLE))
        return false;
    if(ent->faction_id == faction_id)
        return false;

    bool result = G_GetDiplomacyState(ent->faction_id, faction_id, &ds);
    assert(result);
    return (ds == DIPLOMACY_STATE_WAR);
}

static void n_update_local_islands(struct nav_chunk *chunk)
{
    int local_iid = 0;
    memset(chunk->local_islands, 0xff, sizeof(chunk->local_islands));

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        if(chunk->local_islands[r][c] != ISLAND_NONE)
            continue;
        if(chunk->cost_base[r][c] == COST_IMPASSABLE)
            continue;
        if(chunk->blockers[r][c] > 0)
            continue;
        n_visit_island_local(chunk, ++local_iid, (struct coord){r, c});
    }}
}

static void n_update_dirty_local_islands(void *nav_private)
{
    struct nav_private *priv = nav_private;
    if(!s_local_islands_dirty)
        return;

    for(int i = kh_begin(s_dirty_chunks); i != kh_end(s_dirty_chunks); i++) {

        if(!kh_exist(s_dirty_chunks, i))
            continue;

        uint32_t key = kh_key(s_dirty_chunks, i);
        struct coord curr = (struct coord){ key >> 16, key & 0xffff };

        struct nav_chunk *chunk = &priv->chunks[IDX(curr.r, priv->width, curr.c)];
        n_update_local_islands(chunk);
    }
    s_local_islands_dirty = false;
}

static void n_update_blockers(struct nav_private *priv, vec2_t xz_pos, float range, 
                              vec3_t map_pos, int ref_delta)
{
    struct tile_desc tds[256];
    int ntds = N_TilesUnderCircle(priv, xz_pos, range, map_pos, tds, ARR_SIZE(tds));

    for(int i = 0; i < ntds; i++) {
    
        struct tile_desc curr = tds[i];
        struct nav_chunk *chunk = &priv->chunks[IDX(curr.chunk_r, priv->width, curr.chunk_c)];

        assert(ref_delta < 0 ? chunk->blockers[curr.tile_r][curr.tile_c] >= -ref_delta : true);
        assert(ref_delta > 0 ? chunk->blockers[curr.tile_r][curr.tile_c] < 256 - ref_delta : true);

        int prev_val = chunk->blockers[curr.tile_r][curr.tile_c];
        chunk->blockers[curr.tile_r][curr.tile_c] += ref_delta;

        int val = chunk->blockers[curr.tile_r][curr.tile_c] += ref_delta;
        if(!!val != !!prev_val) { /* The tile changed states between occupied/non-occupied */
            int ret;
            uint64_t key = ((curr.chunk_r & 0xffff) << 16) | (curr.chunk_c & 0xffff);
            kh_put(coord, s_dirty_chunks, key, &ret);
            assert(ret != -1);

            s_local_islands_dirty = true;
        }
    }
}

static int manhattan_dist(struct tile_desc a, struct tile_desc b)
{
    int dr = abs(
        (a.chunk_r * TILES_PER_CHUNK_HEIGHT + a.tile_r) 
      - (b.chunk_r * TILES_PER_CHUNK_HEIGHT + b.tile_r)
    );
    int dc = abs(
        (a.chunk_c * TILES_PER_CHUNK_HEIGHT + a.tile_c) 
      - (b.chunk_c * TILES_PER_CHUNK_HEIGHT + b.tile_c)
    );
    return dr + dc;
}

int n_closest_island_tiles(const struct nav_private *priv, struct tile_desc target, 
                           uint16_t global_iid, bool ignore_blockers,
                           struct tile_desc *out, int maxout)
{
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    queue_td_t frontier;
    queue_td_init(&frontier, 1024);
    queue_td_push(&frontier, &target);

    bool visited[res.chunk_h][res.chunk_w][res.tile_h][res.tile_w];
    memset(visited, 0, sizeof(visited));
    visited[target.chunk_r][target.chunk_c][target.tile_r][target.tile_c] = true;

    int ret = 0;
    int first_mh_dist = -1;

    while(queue_size(frontier) > 0) {
    
        struct tile_desc curr;
        queue_td_pop(&frontier, &curr);

        struct coord deltas[] = {
            { 0, -1},
            { 0, +1},
            {-1,  0},
            {+1,  0},
        };

        for(int i = 0; i < ARR_SIZE(deltas); i++) {
        
            struct tile_desc neighb = curr;
            if(!M_Tile_RelativeDesc(res, &neighb, deltas[i].c, deltas[i].r))
                continue;

            assert(memcmp(&curr, &neighb, sizeof(struct tile_desc)) != 0);
            const struct nav_chunk *chunk = &priv->chunks[IDX(neighb.chunk_r, priv->width, neighb.chunk_c)];

            if(visited[neighb.chunk_r][neighb.chunk_c][neighb.tile_r][neighb.tile_c])
                continue;

            bool skip = (chunk->islands[neighb.tile_r][neighb.tile_c] != global_iid);
            if(!ignore_blockers
            && chunk->blockers[neighb.tile_r][neighb.tile_c] > 0)
                skip = true;

            int mh_dist = manhattan_dist(target, neighb);
            if(first_mh_dist > 0 && mh_dist > first_mh_dist)
                goto done; /* The mh distance is strictly increasing as we go outwards */

            if(!skip) {
            
                assert(ret < maxout);
                out[ret++] = neighb;

                if(first_mh_dist == -1)
                    first_mh_dist = mh_dist;
                if(ret == maxout)
                    goto done;
            }

            visited[neighb.chunk_r][neighb.chunk_c][neighb.tile_r][neighb.tile_c] = true;
            queue_td_push(&frontier, &neighb);
        }
    }

done:
    queue_td_destroy(&frontier);
    return ret; 
}

static void n_build_portal_travel_index(struct nav_chunk *chunk)
{
    queue_cc_t frontier;
    queue_cc_init(&frontier, 1024);

    for(int pi = 0; pi < chunk->num_portals; pi++) {

        bool visited[FIELD_RES_R][FIELD_RES_C] = {0};
        assert(queue_size(frontier) == 0);

        for(int r = 0; r < FIELD_RES_R; r++) {
        for(int c = 0; c < FIELD_RES_C; c++) {

            chunk->portal_travel_costs[pi][r][c] = FLT_MAX;
        }}

        const struct portal *port = &chunk->portals[pi];
        for(int r = port->endpoints[0].r; r <= port->endpoints[1].r; r++) {
        for(int c = port->endpoints[0].c; c <= port->endpoints[1].c; c++) {

            struct cost_coord cc = (struct cost_coord){0.0f, (struct coord){r,c}};
            queue_cc_push(&frontier, &cc);
            visited[r][c] = true;
        }}

        while(queue_size(frontier) > 0) {

            struct cost_coord curr;
            queue_cc_pop(&frontier, &curr);

            chunk->portal_travel_costs[pi][curr.coord.r][curr.coord.c] = curr.cost;

            struct coord neighbours[8];
            float costs[8];
            int num_neighbours = N_GridNeighbours(chunk->cost_base, curr.coord, neighbours, costs);

            for(int i = 0; i < num_neighbours; i++) {

                if(visited[neighbours[i].r][neighbours[i].c])
                    continue;

                struct cost_coord cc = (struct cost_coord){curr.cost + costs[i], neighbours[i]};
                queue_cc_push(&frontier, &cc);
                visited[neighbours[i].r][neighbours[i].c] = true;
            }
        }
    }

    queue_cc_destroy(&frontier);
}

static const struct portal *n_closest_reachable_portal(const struct nav_chunk *chunk, struct coord start)
{
    const struct portal *ret = NULL;
    float min_cost = FLT_MAX;

    for(int i = 0; i < chunk->num_portals; i++) {

        const struct portal *curr = &chunk->portals[i];
        float cost = chunk->portal_travel_costs[i][start.r][start.c];

        if(cost < min_cost) {
            ret = curr;
            min_cost = cost;
        }
    }
    return ret;
}

static void chunk_bounds(vec3_t map_pos, struct coord chunk, 
                         vec2_t *out_xz_min, vec2_t *out_xz_max)
{
    out_xz_min->x = map_pos.x - (chunk.c + 1) * X_COORDS_PER_TILE * TILES_PER_CHUNK_WIDTH;
    out_xz_max->x = map_pos.x - (chunk.c + 0) * X_COORDS_PER_TILE * TILES_PER_CHUNK_WIDTH;

    out_xz_min->z = map_pos.z + (chunk.r + 0) * Z_COORDS_PER_TILE * TILES_PER_CHUNK_HEIGHT;
    out_xz_max->z = map_pos.z + (chunk.r + 1) * Z_COORDS_PER_TILE * TILES_PER_CHUNK_HEIGHT;
}

/* Every set bit in the returned value represents the index of a portal 
 * in the chunk that we can path to to find enemies on the other side. 
 */
static uint64_t n_enemy_seek_portalmask(const struct nav_private *priv, vec3_t map_pos, 
                                        struct coord chunk, int faction_id)
{
    bool top = false, bot = false, left = false, right = false;

    struct entity *ents[1];
    vec2_t xz_min, xz_max;
    struct coord curr;

    if(chunk.r > 0) {

        curr = (struct coord){chunk.r - 1, chunk.c};
        chunk_bounds(map_pos, curr, &xz_min, &xz_max);

        top = (G_Pos_EntsInRectWithPred(xz_min, xz_max, ents, ARR_SIZE(ents), 
            enemy_ent, (void*)(uintptr_t)faction_id) > 0);
    }

    if(chunk.r < FIELD_RES_R-1) {

        curr = (struct coord){chunk.r + 1, chunk.c};
        chunk_bounds(map_pos, curr, &xz_min, &xz_max);

        bot = (G_Pos_EntsInRectWithPred(xz_min, xz_max, ents, ARR_SIZE(ents), 
            enemy_ent, (void*)(uintptr_t)faction_id) > 0);
    }

    if(chunk.c > 0) {

        curr = (struct coord){chunk.r, chunk.c - 1};
        chunk_bounds(map_pos, curr, &xz_min, &xz_max);

        left = (G_Pos_EntsInRectWithPred(xz_min, xz_max, ents, ARR_SIZE(ents), 
            enemy_ent, (void*)(uintptr_t)faction_id) > 0);
    }

    if(chunk.c < FIELD_RES_C-1) {

        curr = (struct coord){chunk.r, chunk.c + 1};
        chunk_bounds(map_pos, curr, &xz_min, &xz_max);

        right = (G_Pos_EntsInRectWithPred(xz_min, xz_max, ents, ARR_SIZE(ents), 
            enemy_ent, (void*)(uintptr_t)faction_id) > 0);
    }

    uint64_t ret = 0;
    const struct nav_chunk *nchunk = &priv->chunks[IDX(chunk.r, priv->width, chunk.c)];

    for(int i = 0; i < nchunk->num_portals; i++) {

        const struct portal *curr = &nchunk->portals[i];

        if(top && curr->endpoints[0].r == 0 
               && curr->endpoints[1].r == 0)
            ret |= (((uint64_t)1) << i);

        if(bot && curr->endpoints[0].r == FIELD_RES_R-1 
               && curr->endpoints[1].r == FIELD_RES_R-1)
            ret |= (((uint64_t)1) << i);

        if(left && curr->endpoints[0].c == 0 
                && curr->endpoints[1].c == 0)
            ret |= (((uint64_t)1) << i);

        if(right && curr->endpoints[0].c == FIELD_RES_C-1 
                 && curr->endpoints[1].c == FIELD_RES_C-1)
            ret |= (((uint64_t)1) << i);
    }

    return ret;
}

/* Returns true if, in the abscence of any blockers, the tiles would be on the same local island */
static bool n_normally_reachable(const struct nav_chunk *chunk, struct coord a, struct coord b)
{
    for(int i = 0; i < chunk->num_portals; i++) {
    
        bool areach = (chunk->portal_travel_costs[i][a.r][a.c] != FLT_MAX);
        bool breach = (chunk->portal_travel_costs[i][b.r][b.c] != FLT_MAX);
        if(areach != breach)
            return false;
    }
    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool N_Init(void)
{
    if(!N_FC_Init())
        return false;

    if((s_dirty_chunks = kh_init(coord)) == NULL)
        return false;

    return true;
}

void N_Update(void *nav_private)
{
    PERF_ENTER();

    struct nav_private *priv = nav_private;
    bool components_dirty = false;

    for(int i = kh_begin(s_dirty_chunks); i != kh_end(s_dirty_chunks); i++) {

        if(!kh_exist(s_dirty_chunks, i))
            continue;

        uint32_t key = kh_key(s_dirty_chunks, i);
        struct coord curr = (struct coord){ key >> 16, key & 0xffff };
        N_FC_InvalidateAllAtChunk(curr);

        struct nav_chunk *chunk = &priv->chunks[IDX(curr.r, priv->width, curr.c)];
        int nflipped = n_update_edge_states(chunk);

        if(nflipped) {
            components_dirty = true;
            N_FC_InvalidateAllThroughChunk(curr);
        }
    }

    n_update_dirty_local_islands(priv);
    if(components_dirty)
        n_update_components(priv);

    kh_clear(coord, s_dirty_chunks);
    PERF_RETURN_VOID();
}

void N_Shutdown(void)
{
    kh_destroy(coord, s_dirty_chunks);
    N_FC_Shutdown();
}

void *N_BuildForMapData(size_t w, size_t h, size_t chunk_w, size_t chunk_h,
                        const struct tile **chunk_tiles, bool update)
{
    struct nav_private *ret;
    size_t alloc_size = sizeof(struct nav_private) + (w * h * sizeof(struct nav_chunk));

    ret = malloc(alloc_size);
    if(!ret)
        goto fail_alloc;

    ret->width = w;
    ret->height = h;

    assert(FIELD_RES_R >= chunk_h && FIELD_RES_R % chunk_h == 0);
    assert(FIELD_RES_C >= chunk_w && FIELD_RES_C % chunk_w == 0);

    /* First build the base cost field based on terrain */
    for(int chunk_r = 0; chunk_r < ret->height; chunk_r++){
    for(int chunk_c = 0; chunk_c < ret->width;  chunk_c++){

        struct nav_chunk *curr_chunk = &ret->chunks[IDX(chunk_r, ret->width, chunk_c)];
        const struct tile *curr_tiles = chunk_tiles[IDX(chunk_r, ret->width, chunk_c)];
        curr_chunk->num_portals = 0;

        for(int tile_r = 0; tile_r < chunk_h; tile_r++) {
        for(int tile_c = 0; tile_c < chunk_w; tile_c++) {

            if(update) {
                const struct tile *curr_tile = &curr_tiles[tile_r * chunk_w + tile_c];
                n_set_cost_for_tile(curr_chunk, chunk_w, chunk_h, tile_r, tile_c, curr_tile);
            }else{
                n_clear_cost_for_tile(curr_chunk, chunk_w, chunk_h, tile_r, tile_c);
            }
        }}
        memset(curr_chunk->blockers, 0, sizeof(curr_chunk->blockers));
    }}

    n_make_cliff_edges(ret, chunk_tiles, chunk_w, chunk_h);
    N_UpdatePortals(ret);
    N_UpdateIslandsField(ret);
    return ret;

fail_alloc:
    return NULL;
}

void N_FreePrivate(void *nav_private)
{
    assert(nav_private);
    free(nav_private);
}

void N_RenderPathableChunk(void *nav_private, mat4x4_t *chunk_model,
                           const struct map *map,
                           int chunk_r, int chunk_c)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const struct nav_private *priv = nav_private;
    assert(chunk_r < priv->height);
    assert(chunk_c < priv->width);

    vec2_t corners_buff[4 * FIELD_RES_R * FIELD_RES_C];
    vec3_t colors_buff[FIELD_RES_R * FIELD_RES_C];

    const struct nav_chunk *chunk = &priv->chunks[IDX(chunk_r, priv->width, chunk_c)];
    n_render_portals(chunk, chunk_model, map, (vec3_t){1.0f, 1.0f, 0.0f});

    vec2_t *corners_base = corners_buff;
    vec3_t *colors_base = colors_buff; 

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP( (((float)r) / FIELD_RES_R) * chunk_z_dim, -chunk_z_dim, chunk_z_dim);

        *corners_base++ = (vec2_t){square_x, square_z};
        *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

        *colors_base++ = chunk->cost_base[r][c] == COST_IMPASSABLE ? (vec3_t){1.0f, 0.0f, 0.0f}
                                                                   : (vec3_t){0.0f, 1.0f, 0.0f};
    }}

    assert(colors_base == colors_buff + ARR_SIZE(colors_buff));
    assert(corners_base == corners_buff + ARR_SIZE(corners_buff));

    size_t count = FIELD_RES_R * FIELD_RES_C;
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawMapOverlayQuads,
        .nargs = 5,
        .args = {
            R_PushArg(corners_buff, sizeof(corners_buff)),
            R_PushArg(colors_buff, sizeof(colors_buff)),
            R_PushArg(&count, sizeof(count)),
            R_PushArg(chunk_model, sizeof(*chunk_model)),
            (void*)G_GetPrevTickMap(),
        },
    });
}

void N_RenderPathFlowField(void *nav_private, const struct map *map, 
                           mat4x4_t *chunk_model, int chunk_r, int chunk_c, 
                           dest_id_t id)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const struct nav_private *priv = nav_private;
    assert(chunk_r < priv->height);
    assert(chunk_c < priv->width);

    vec2_t positions_buff[FIELD_RES_R * FIELD_RES_C];
    vec2_t dirs_buff[FIELD_RES_R * FIELD_RES_C];

    ff_id_t field_id;
    if(!N_FC_GetDestFFMapping(id, (struct coord){chunk_r, chunk_c}, &field_id))
        return;
    const struct flow_field *ff = N_FC_FlowFieldAt(field_id);
    if(!ff)
        return;

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP( (((float)r) / FIELD_RES_R) * chunk_z_dim, -chunk_z_dim, chunk_z_dim);

        positions_buff[r * FIELD_RES_C + c] = (vec2_t){
            square_x - square_x_len / 2.0f,
            square_z + square_z_len / 2.0f
        };
        dirs_buff[r * FIELD_RES_C + c] = g_flow_dir_lookup[ff->field[r][c].dir_idx];
    }}

    size_t count = FIELD_RES_R * FIELD_RES_C;
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawFlowField,
        .nargs = 5,
        .args = {
            R_PushArg(positions_buff, sizeof(positions_buff)),
            R_PushArg(dirs_buff, sizeof(dirs_buff)),
            R_PushArg(&count, sizeof(count)),
            R_PushArg(chunk_model, sizeof(*chunk_model)),
            (void*)G_GetPrevTickMap(),
        },
    });
}

void N_RenderLOSField(void *nav_private, const struct map *map, mat4x4_t *chunk_model, 
                      int chunk_r, int chunk_c, dest_id_t id)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const struct nav_private *priv = nav_private;
    assert(chunk_r < priv->height);
    assert(chunk_c < priv->width);

    vec2_t corners_buff[4 * FIELD_RES_R * FIELD_RES_C];
    vec3_t colors_buff[FIELD_RES_R * FIELD_RES_C];

    if(!N_FC_ContainsLOSField(id, (struct coord){chunk_r, chunk_c}))
        return;

    const struct LOS_field *lf = N_FC_LOSFieldAt(id, (struct coord){chunk_r, chunk_c});
    if(!lf)
        return;

    vec2_t *corners_base = corners_buff;
    vec3_t *colors_base = colors_buff; 

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP( (((float)r) / FIELD_RES_R) * chunk_z_dim, -chunk_z_dim, chunk_z_dim);

        *corners_base++ = (vec2_t){square_x, square_z};
        *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

        *colors_base++ = lf->field[r][c].visible ? (vec3_t){1.0f, 1.0f, 0.0f}
                                                 : (vec3_t){0.0f, 0.0f, 0.0f};
    }}

    assert(colors_base == colors_buff + ARR_SIZE(colors_buff));
    assert(corners_base == corners_buff + ARR_SIZE(corners_buff));

    size_t count = FIELD_RES_R * FIELD_RES_C;
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawMapOverlayQuads,
        .nargs = 5,
        .args = {
            R_PushArg(corners_buff, sizeof(corners_buff)),
            R_PushArg(colors_buff, sizeof(colors_buff)),
            R_PushArg(&count, sizeof(count)),
            R_PushArg(chunk_model, sizeof(*chunk_model)),
            (void*)G_GetPrevTickMap(),
        },
    });
}

void N_RenderEnemySeekField(void *nav_private, const struct map *map, 
                            mat4x4_t *chunk_model, int chunk_r, int chunk_c, 
                            int faction_id)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const struct nav_private *priv = nav_private;
    assert(chunk_r < priv->height);
    assert(chunk_c < priv->width);

    vec2_t positions_buff[FIELD_RES_R * FIELD_RES_C];
    vec2_t dirs_buff[FIELD_RES_R * FIELD_RES_C];

    vec2_t corners_buff[4 * FIELD_RES_R * FIELD_RES_C];
    vec3_t colors_buff[FIELD_RES_R * FIELD_RES_C];

    vec2_t *corners_base = corners_buff;
    vec3_t *colors_base = colors_buff; 

    struct field_target target = (struct field_target){
        .type = TARGET_ENEMIES,
        .enemies.faction_id = faction_id,
        /* rest of the fields are unused */
    };
    ff_id_t ffid = N_FlowField_ID((struct coord){chunk_r, chunk_c}, target);
    if(!N_FC_ContainsFlowField(ffid))
        return;

    const struct flow_field *ff = N_FC_FlowFieldAt(ffid);
    if(!ff)
        return;

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP( (((float)r) / FIELD_RES_R) * chunk_z_dim, -chunk_z_dim, chunk_z_dim);

        positions_buff[r * FIELD_RES_C + c] = (vec2_t){
            square_x - square_x_len / 2.0f,
            square_z + square_z_len / 2.0f
        };
        dirs_buff[r * FIELD_RES_C + c] = g_flow_dir_lookup[ff->field[r][c].dir_idx];

        *corners_base++ = (vec2_t){square_x, square_z};
        *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

        *colors_base++ = ff->field[r][c].dir_idx == FD_NONE ? (vec3_t){1.0f, 0.0f, 0.0f}
                                                            : (vec3_t){0.0f, 1.0f, 0.0f};
    }}

    assert(colors_base == colors_buff + ARR_SIZE(colors_buff));
    assert(corners_base == corners_buff + ARR_SIZE(corners_buff));

    size_t count = FIELD_RES_R * FIELD_RES_C;
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawFlowField,
        .nargs = 5,
        .args = {
            R_PushArg(positions_buff, sizeof(positions_buff)),
            R_PushArg(dirs_buff, sizeof(dirs_buff)),
            R_PushArg(&count, sizeof(count)),
            R_PushArg(chunk_model, sizeof(*chunk_model)),
            (void*)G_GetPrevTickMap(),
        },
    });
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawMapOverlayQuads,
        .nargs = 5,
        .args = {
            R_PushArg(corners_buff, sizeof(corners_buff)),
            R_PushArg(colors_buff, sizeof(colors_buff)),
            R_PushArg(&count, sizeof(count)),
            R_PushArg(chunk_model, sizeof(*chunk_model)),
            (void*)G_GetPrevTickMap(),
        },
    });
}

void N_RenderNavigationBlockers(void *nav_private, const struct map *map, 
                                mat4x4_t *chunk_model, int chunk_r, int chunk_c)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const struct nav_private *priv = nav_private;
    assert(chunk_r < priv->height);
    assert(chunk_c < priv->width);

    vec2_t corners_buff[4 * FIELD_RES_R * FIELD_RES_C];
    vec3_t colors_buff[FIELD_RES_R * FIELD_RES_C];

    const struct nav_chunk *chunk = &priv->chunks[IDX(chunk_r, priv->width, chunk_c)];
    vec2_t *corners_base = corners_buff;
    vec3_t *colors_base = colors_buff; 

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP( (((float)r) / FIELD_RES_R) * chunk_z_dim, -chunk_z_dim, chunk_z_dim);

        *corners_base++ = (vec2_t){square_x, square_z};
        *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

        *colors_base++ = chunk->blockers[r][c] ? (vec3_t){1.0f, 0.0f, 0.0f}
                                               : (vec3_t){0.0f, 1.0f, 0.0f};
    }}

    assert(colors_base == colors_buff + ARR_SIZE(colors_buff));
    assert(corners_base == corners_buff + ARR_SIZE(corners_buff));

    size_t count = FIELD_RES_R * FIELD_RES_C;
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawMapOverlayQuads,
        .nargs = 5,
        .args = {
            R_PushArg(corners_buff, sizeof(corners_buff)),
            R_PushArg(colors_buff, sizeof(colors_buff)),
            R_PushArg(&count, sizeof(count)),
            R_PushArg(chunk_model, sizeof(*chunk_model)),
            (void*)G_GetPrevTickMap(),
        },
    });
}

void N_RenderNavigationPortals(void *nav_private, const struct map *map, 
                               mat4x4_t *chunk_model, int chunk_r, int chunk_c)
{
    struct nav_private *priv = nav_private;
    struct nav_chunk *chunk = &priv->chunks[IDX(chunk_r, priv->width, chunk_c)];
    n_render_portals(chunk, chunk_model, map, (vec3_t){0.0f, 0.0f, 1.0f});

    vec_coord_t path;
    vec_coord_init(&path);

    for(int i = 0; i < chunk->num_portals; i++) {

        struct portal *port = &chunk->portals[i];

        for(int j = 0; j < port->num_neighbours; j++) {

            struct portal *neighb = port->edges[j].neighbour;
            struct coord a = (struct coord){
                (port->endpoints[0].r + port->endpoints[1].r) / 2,
                (port->endpoints[0].c + port->endpoints[1].c) / 2,
            };
            struct coord b = (struct coord){
                (neighb->endpoints[0].r + neighb->endpoints[1].r) / 2,
                (neighb->endpoints[0].c + neighb->endpoints[1].c) / 2,
            };

            vec3_t link_color = port->edges[j].es == EDGE_STATE_ACTIVE  ? (vec3_t){0.0f, 1.0f, 0.0f} 
                              : port->edges[j].es == EDGE_STATE_BLOCKED ? (vec3_t){1.0f, 0.0f, 0.0f}
                              : (assert(0), (vec3_t){0});

            float cost;
            bool has_path = AStar_GridPath(a, b, (struct coord){chunk_r, chunk_c}, chunk->cost_base, &path, &cost);
            assert(has_path);
            n_render_grid_path(chunk, chunk_model, map, &path, link_color);
        }
    }

    vec_coord_destroy(&path);
}

void N_CutoutStaticObject(void *nav_private, vec3_t map_pos, const struct obb *obb)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tds[4096];
    size_t ntiles = M_Tile_AllUnderObj(map_pos, res, obb, tds, ARR_SIZE(tds));

    for(int i = 0; i < ntiles; i++) {

        priv->chunks[IDX(tds[i].chunk_r, priv->width, tds[i].chunk_c)]
            .cost_base[tds[i].tile_r][tds[i].tile_c] = COST_IMPASSABLE;
    }
}

void N_UpdatePortals(void *nav_private)
{
    struct nav_private *priv = nav_private;

    for(int chunk_r = 0; chunk_r < priv->height; chunk_r++){
    for(int chunk_c = 0; chunk_c < priv->width; chunk_c++){
            
        struct nav_chunk *curr_chunk = &priv->chunks[IDX(chunk_r, priv->width, chunk_c)];
        curr_chunk->num_portals = 0;
    }}
    
    n_create_portals(priv);

    for(int chunk_r = 0; chunk_r < priv->height; chunk_r++){
    for(int chunk_c = 0; chunk_c < priv->width; chunk_c++){
            
        struct nav_chunk *curr_chunk = &priv->chunks[IDX(chunk_r, priv->width, chunk_c)];
        n_link_chunk_portals(curr_chunk, (struct coord){chunk_r, chunk_c});
        n_build_portal_travel_index(curr_chunk);
    }}
}

void N_UpdateIslandsField(void *nav_private)
{
    /* We assign a unique ID to each set of tiles that are mutually connected
     * (i.e. are on the same 'island'). The tile's 'island ID' can then be 
     * queried from the 'islands' field using the coordinate. 
     * To build the field, we treat every tile in the cost field as a node in
     * a graph, with cardinally adjacent pathable tiles being the 'neighbors'. 
     * Then we solve an instance of the 'coonected components' problem. 
     */

    struct nav_private *priv = nav_private;
    uint16_t island_id = 0;

    for(int chunk_r = 0; chunk_r < priv->height; chunk_r++) {
    for(int chunk_c = 0; chunk_c < priv->width;  chunk_c++) {

        /* Initialize every node as 'unvisited' */
        struct nav_chunk *curr_chunk = &priv->chunks[IDX(chunk_r, priv->width, chunk_c)];
        memset(curr_chunk->islands, 0xff, sizeof(curr_chunk->islands));
    }}

    for(int chunk_r = 0; chunk_r < priv->height; chunk_r++) {
    for(int chunk_c = 0; chunk_c < priv->width;  chunk_c++) {

        struct nav_chunk *curr_chunk = &priv->chunks[IDX(chunk_r, priv->width, chunk_c)];

        for(int tile_r = 0; tile_r < FIELD_RES_R; tile_r++) {
        for(int tile_c = 0; tile_c < FIELD_RES_C; tile_c++) {

            if(curr_chunk->islands[tile_r][tile_c] != ISLAND_NONE)
                continue;

            if(curr_chunk->cost_base[tile_r][tile_c] == COST_IMPASSABLE)
                continue;

            struct tile_desc td = {chunk_r, chunk_c, tile_r, tile_c};
            n_visit_island(priv, island_id, td);
            island_id++;
        }}
    }}
}

dest_id_t N_DestIDForPos(void *nav_private, vec3_t map_pos, vec2_t xz_pos)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc td;
    bool result = M_Tile_DescForPoint2D(res, map_pos, xz_pos, &td);
    assert(result);

    return n_dest_id(td);
}

bool N_RequestPath(void *nav_private, vec2_t xz_src, vec2_t xz_dest, 
                   vec3_t map_pos, dest_id_t *out_dest_id)
{
    PERF_ENTER();

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    n_update_dirty_local_islands(nav_private);

    /* Convert source and destination positions to tile coordinates */
    bool result;
    (void)result;

    struct tile_desc src_desc, dst_desc;
    result = M_Tile_DescForPoint2D(res, map_pos, xz_src, &src_desc);
    assert(result);
    result = M_Tile_DescForPoint2D(res, map_pos, xz_dest, &dst_desc);
    assert(result);

    dest_id_t ret = n_dest_id(dst_desc);

    /* Handle the case where no path exists between the source and destination 
     * (i.e. they are on different 'islands'). 
     */
    const struct nav_chunk *src_chunk = &priv->chunks[src_desc.chunk_r * priv->width + src_desc.chunk_c];
    const struct nav_chunk *dst_chunk = &priv->chunks[dst_desc.chunk_r * priv->width + dst_desc.chunk_c];
    uint16_t src_iid = src_chunk->islands[src_desc.tile_r][src_desc.tile_c];
    uint16_t dst_iid = dst_chunk->islands[dst_desc.tile_r][dst_desc.tile_c];

    if(src_iid != dst_iid)
        PERF_RETURN(false); 

    /* Even if a mapping exists, the actual flow field may have been evicted from
     * the cache, due to space constraints or invalidation. */
    ff_id_t id;
    if(!N_FC_GetDestFFMapping(ret, (struct coord){dst_desc.chunk_r, dst_desc.chunk_c}, &id)
    || !N_FC_ContainsFlowField(id)) {

        struct field_target target = (struct field_target){
            .type = TARGET_TILE,
            .tile = (struct coord){dst_desc.tile_r, dst_desc.tile_c}
        };

        struct flow_field ff;
        id = N_FlowField_ID((struct coord){dst_desc.chunk_r, dst_desc.chunk_c}, target);

        if(!N_FC_ContainsFlowField(id)) {
        
            struct coord chunk = (struct coord){dst_desc.chunk_r, dst_desc.chunk_c};
            N_FlowFieldInit(chunk, priv, &ff);
            N_FlowFieldUpdate(chunk, priv, target, &ff);
            N_FC_PutFlowField(id, &ff);
        }

        N_FC_PutDestFFMapping(ret, (struct coord){dst_desc.chunk_r, dst_desc.chunk_c}, id);
    }

    /* Create the LOS field for the destination chunk, if necessary */
    if(!N_FC_ContainsLOSField(ret, (struct coord){dst_desc.chunk_r, dst_desc.chunk_c})) {

        struct LOS_field lf;
        N_LOSFieldCreate(ret, (struct coord){dst_desc.chunk_r, dst_desc.chunk_c}, dst_desc, priv, map_pos, &lf, NULL);
        N_FC_PutLOSField(ret, (struct coord){dst_desc.chunk_r, dst_desc.chunk_c}, &lf);
    }

    /* Source and destination positions are in the same chunk, and a path exists
     * between them. In this case, we only need a single flow field. .
     */
    if(src_desc.chunk_r == dst_desc.chunk_r && src_desc.chunk_c == dst_desc.chunk_c
    && src_chunk->local_islands[src_desc.tile_r][src_desc.tile_c] == src_chunk->local_islands[dst_desc.tile_r][dst_desc.tile_c]) {

        *out_dest_id = ret;
        PERF_RETURN(true);
    }

    /* If the source and destination are on the same chunk and, in the absence of blockers,
     * would be reachable from one another, that means that the destination is blocked in
     * by blockers. In this case, get as close as possible. 
     */
    if((src_desc.chunk_r == dst_desc.chunk_r && src_desc.chunk_c == dst_desc.chunk_c)
    && n_normally_reachable(src_chunk, 
        (struct coord){src_desc.tile_r, src_desc.tile_c},
        (struct coord){dst_desc.tile_r, dst_desc.tile_c})) {
        
        *out_dest_id = ret;
        PERF_RETURN(true);
    }

    const struct portal *dst_port = n_closest_reachable_portal(dst_chunk, 
        (struct coord){dst_desc.tile_r, dst_desc.tile_c});

    if(!dst_port)
        PERF_RETURN(false); 

    float cost;
    vec_portal_t path;
    vec_portal_init(&path);

    bool path_exists = AStar_PortalGraphPath(src_desc, dst_port, priv, &path, &cost);
    if(!path_exists) {
        vec_portal_destroy(&path);
        PERF_RETURN(false); 
    }

    struct coord prev_los_coord = (struct coord){dst_desc.chunk_r, dst_desc.chunk_c};

    /* Traverse the portal path _backwards_ and generate the required fields, if they are not already 
     * cached. Add the results to the fieldcache. */
    for(int i = vec_size(&path)-1; i > 0; i--) {

        const struct portal *curr_node = vec_AT(&path, i - 1);
        const struct portal *next_hop = vec_AT(&path, i);

        /* If the very first hop takes us into another chunk, that means that the 'nearest portal'
         * to the source borders the 'next' chunk already. In this case, we must remember to
         * still generate a flow field for the current chunk steering to this portal. */
        if(i == 1 && (next_hop->chunk.r != src_desc.chunk_r || next_hop->chunk.c != src_desc.chunk_c))
            next_hop = vec_AT(&path, 0);

        if(curr_node->connected == next_hop)
            continue;

        /* Since we are moving from 'closest portal' to 'closest portal', it 
         * may be possible that the very last hop takes us from another portal in the 
         * destination chunk to the destination portal. This is not needed and will
         * overwrite the destination flow field made earlier. */
        if(curr_node->chunk.r == dst_desc.chunk_r 
        && curr_node->chunk.c == dst_desc.chunk_c
        && next_hop == dst_port)
            continue;

        struct coord chunk_coord = curr_node->chunk;
        struct field_target target = (struct field_target){
            .type = TARGET_PORTAL,
            .port = next_hop
        };

        ff_id_t new_id = N_FlowField_ID(chunk_coord, target);
        ff_id_t exist_id;
        struct flow_field ff;

        if(N_FC_GetDestFFMapping(ret, chunk_coord, &exist_id)
        && N_FC_ContainsFlowField(exist_id)) {

            /* The exact flow field we need has already been made */
            if(new_id == exist_id)
                goto ff_exists;

            /* This is the edge case when a path to a particular target takes us through
             * the same chunk more than once. This can happen if a chunk is divided into
             * 'islands' by unpathable barriers. */
            const struct flow_field *exist_ff  = N_FC_FlowFieldAt(exist_id);
            memcpy(&ff, exist_ff, sizeof(struct flow_field));

            N_FlowFieldUpdate(chunk_coord, priv, target, &ff);
            /* We set the updated flow field for the new (least recently used) key. Since in 
             * this case more than one flowfield ID maps to the same field but we only keep 
             * one of the IDs, it may be possible that the same flowfield will be redundantly 
             * updated at a later time. However, this is largely inconsequential. */
            N_FC_PutDestFFMapping(ret, chunk_coord, new_id);
            N_FC_PutFlowField(new_id, &ff);

            goto ff_exists;
        }

        N_FC_PutDestFFMapping(ret, chunk_coord, new_id);
        if(!N_FC_ContainsFlowField(new_id)) {
        
            N_FlowFieldInit(chunk_coord, priv, &ff);
            N_FlowFieldUpdate(chunk_coord, priv, target, &ff);
            N_FC_PutFlowField(new_id, &ff);
        }

    ff_exists:
        assert(N_FC_ContainsFlowField(new_id));
        /* Reference field in the cache */
        (void)N_FC_FlowFieldAt(new_id);

        if(!N_FC_ContainsLOSField(ret, chunk_coord)) {

            assert((abs(prev_los_coord.r - chunk_coord.r) + abs(prev_los_coord.c - chunk_coord.c)) == 1);
            assert(N_FC_ContainsLOSField(ret, prev_los_coord));

            const struct LOS_field *prev_los = N_FC_LOSFieldAt(ret, prev_los_coord);
            assert(prev_los);
            assert(prev_los->chunk.r == prev_los_coord.r && prev_los->chunk.c == prev_los_coord.c);

            struct LOS_field lf;
            N_LOSFieldCreate(ret, chunk_coord, dst_desc, priv, map_pos, &lf, prev_los);
            N_FC_PutLOSField(ret, chunk_coord, &lf);
        }

        prev_los_coord = chunk_coord;
    }
    vec_portal_destroy(&path);

    *out_dest_id = ret; 
    PERF_RETURN(true);
}

vec2_t N_DesiredPointSeekVelocity(dest_id_t id, vec2_t curr_pos, vec2_t xz_dest, 
                                  void *nav_private, vec3_t map_pos)
{
    unsigned dir_idx;
    struct nav_private *priv = nav_private;

    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tile;
    bool result = M_Tile_DescForPoint2D(res, map_pos, curr_pos, &tile);
    assert(result);

    ff_id_t ffid;
    if(!N_FC_GetDestFFMapping(id, (struct coord){tile.chunk_r, tile.chunk_c}, &ffid)) {

        dest_id_t ret;
        bool result = N_RequestPath(nav_private, curr_pos, xz_dest, map_pos, &ret);
        if(!result)
            return (vec2_t){0.0f};
        assert(ret == id);
        N_FC_GetDestFFMapping(id, (struct coord){tile.chunk_r, tile.chunk_c}, &ffid);
    }

    const struct flow_field *ff = N_FC_FlowFieldAt(ffid);
    if(!ff || ff->field[tile.tile_r][tile.tile_c].dir_idx == FD_NONE) {

        dest_id_t ret;
        bool result = N_RequestPath(nav_private, curr_pos, xz_dest, map_pos, &ret);
        if(!result)
            return (vec2_t){0.0f};
        assert(ret == id);
        N_FC_GetDestFFMapping(id, (struct coord){tile.chunk_r, tile.chunk_c}, &ffid);
    }

    ff = N_FC_FlowFieldAt(ffid);
    assert(ff);

    /*   1. The original path took us through another global 'island' in
     *      this chunk which is separated from the current tile's island 
     *      by an impassable barrier. In this case, the path query above
     *      would have updated the flow field with a valid direction for
     *      the current tile.
     */
    if(ff->field[tile.tile_r][tile.tile_c].dir_idx != FD_NONE)
        goto ff_found;

    const struct nav_chunk *chunk = &priv->chunks[IDX(tile.chunk_r, priv->width, tile.chunk_c)];
    uint16_t local_iid = chunk->local_islands[tile.tile_r][tile.tile_c];

    /*   2. The entity has somehow ended up on an impassable tile. One example
     *      where this can happen is if an adjacent entity 'stops' and occupies 
     *      the tile directly under its' neighbour. The neighbour entity can 
     *      then unexpectedly find itself positioned on a 'blocked' tile. 
     */
    if(local_iid == ISLAND_NONE) {

        struct flow_field exist_ff = *ff;
        N_FlowFieldUpdateToNearestPathable(chunk, (struct coord){tile.tile_r, tile.tile_c}, &exist_ff);
        N_FC_PutFlowField(ffid, &exist_ff);
        ff = N_FC_FlowFieldAt(ffid);
        goto ff_found;
    }

    /*   3. If the direction is still 'FD_NONE' after querying the path with
     *      the current position as the source, this could mean that the
     *      current position is 'orphaned' from its' goal by blockers (i.e.
     *      the frontier was prevented from advancing from the destination
     *      due to blockers).
     */
    struct flow_field exist_ff = *ff;
    N_FlowFieldUpdateIslandToNearest(local_iid, priv, &exist_ff);
    N_FC_PutFlowField(ffid, &exist_ff);

    /*   4. If the direction is still FD_NONE, that means that the
     *      entity is at its' destination of maximally close to it.
     *      We have nothing left to do but pass the 'None' direction
     *      to the caller.
     */
    ff = N_FC_FlowFieldAt(ffid);

ff_found:
    assert(ff);
    dir_idx = ff->field[tile.tile_r][tile.tile_c].dir_idx;
    return g_flow_dir_lookup[dir_idx];
}

vec2_t N_DesiredEnemySeekVelocity(vec2_t curr_pos, void *nav_private, vec3_t map_pos, int faction_id)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc curr_tile;
    bool result = M_Tile_DescForPoint2D(res, map_pos, curr_pos, &curr_tile);
    assert(result);

    struct coord chunk = (struct coord){curr_tile.chunk_r, curr_tile.chunk_c};

    struct field_target target = (struct field_target){
        .type = TARGET_ENEMIES,
        .enemies.faction_id = faction_id,
        .enemies.map_pos = map_pos,
        .enemies.chunk = chunk
    };

    ff_id_t ffid = N_FlowField_ID(chunk, target);
    struct flow_field ff;

    if(!N_FC_ContainsFlowField(ffid)) {

        vec2_t chunk_center = (vec2_t){
            map_pos.x - (chunk.c + 0.5f) * X_COORDS_PER_TILE * TILES_PER_CHUNK_WIDTH,
            map_pos.z + (chunk.r + 0.5f) * Z_COORDS_PER_TILE * TILES_PER_CHUNK_HEIGHT,
        };
        struct entity *nearest_enemy = G_Pos_NearestWithPred(chunk_center, enemy_ent, 
                                                             (void*)(uintptr_t)target.enemies.faction_id);
        if(!nearest_enemy)
            return (vec2_t){0.0f, 0.0f};

        struct tile_desc target_tile;
        bool result = M_Tile_DescForPoint2D(res, map_pos, G_Pos_GetXZ(nearest_enemy->uid), &target_tile);
        assert(result);
        bool done = false;

        /* If there are enemies on this chunk, guide towards them. 
         */
        if(target_tile.chunk_r == curr_tile.chunk_r
        && target_tile.chunk_c == curr_tile.chunk_c) {
        
            N_FlowFieldInit(chunk, priv, &ff);
            N_FlowFieldUpdate(chunk, priv, target, &ff);
            N_FC_PutFlowField(ffid, &ff);
            done = true;
        }

        /* Else, if there are enemies on the cardinally adjacent chunks, guide
         * towards all portals that will lead to them. 
         */
        uint64_t pm = n_enemy_seek_portalmask(priv, map_pos, chunk, faction_id);
        if(!done && pm) {

            struct field_target pm_target = (struct field_target){
                .type = TARGET_PORTALMASK,
                .portalmask = pm,
            };

            N_FlowFieldInit(chunk, priv, &ff);
            N_FlowFieldUpdate(chunk, priv, pm_target, &ff);
            N_FC_PutFlowField(ffid, &ff);
            done = true;
        }

        /* Lastly, resort towards pathing towards the closest entity 
         */
        if(!done) {

            assert(target_tile.chunk_r != curr_tile.chunk_r || target_tile.chunk_c != curr_tile.chunk_c);

            /* The closest enemy is in another chunk. In this case, compute the
             * field that will take us to the next chunk in the path and store
             * that with the computed ID as the key. The rest of the fields will
             * be computed on-the-fly, if necessary.
             */
            struct nav_chunk *target_chunk = &priv->chunks[IDX(target_tile.chunk_r, priv->width, target_tile.chunk_c)];
            const struct portal *dst_port = n_closest_reachable_portal(target_chunk, 
                (struct coord){target_tile.tile_r, target_tile.tile_c});

            if(!dst_port)
                return (vec2_t){0.0f, 0.0f};

            float cost;
            vec_portal_t path;
            vec_portal_init(&path);

            bool path_exists = AStar_PortalGraphPath(curr_tile, dst_port, priv, &path, &cost);
            if(!path_exists) {
                vec_portal_destroy(&path);
                return (vec2_t){0.0f, 0.0f}; 
            }

            struct field_target portal_target = (struct field_target){
                .type = TARGET_PORTAL,
                .port = vec_AT(&path, 0)
            };

            N_FlowFieldInit(chunk, priv, &ff);
            N_FlowFieldUpdate(chunk, priv, portal_target, &ff);
            N_FC_PutFlowField(ffid, &ff);

            vec_portal_destroy(&path);
        }

        assert(N_FC_ContainsFlowField(ffid));
    }

    const struct flow_field *pff = N_FC_FlowFieldAt(ffid);
    assert(pff);

    int dir_idx = pff->field[curr_tile.tile_r][curr_tile.tile_c].dir_idx;
    if(dir_idx == FD_NONE) {

        const struct nav_chunk *nchunk = &priv->chunks[IDX(curr_tile.chunk_r, priv->width, curr_tile.chunk_c)];
        uint16_t local_iid = nchunk->local_islands[curr_tile.tile_r][curr_tile.tile_c];

        struct flow_field exist_ff = *pff;
        N_FlowFieldUpdateIslandToNearest(local_iid, priv, &exist_ff);
        N_FC_PutFlowField(ffid, &exist_ff);

        dir_idx = exist_ff.field[curr_tile.tile_r][curr_tile.tile_c].dir_idx;
    }

    return g_flow_dir_lookup[dir_idx];
}

bool N_HasDestLOS(dest_id_t id, vec2_t curr_pos, void *nav_private, vec3_t map_pos)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tile;
    bool result = M_Tile_DescForPoint2D(res, map_pos, curr_pos, &tile);
    assert(result);

    if(!N_FC_ContainsLOSField(id, (struct coord){tile.chunk_r, tile.chunk_c}))
        return false;

    const struct LOS_field *lf = N_FC_LOSFieldAt(id, (struct coord){tile.chunk_r, tile.chunk_c});
    assert(lf);
    return lf->field[tile.tile_r][tile.tile_c].visible;
}

bool N_PositionPathable(vec2_t xz_pos, void *nav_private, vec3_t map_pos)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tile;
    bool result = M_Tile_DescForPoint2D(res, map_pos, xz_pos, &tile);
    assert(result);

    const struct nav_chunk *chunk = &priv->chunks[IDX(tile.chunk_r, priv->width, tile.chunk_c)];
    return chunk->cost_base[tile.tile_r][tile.tile_c] != COST_IMPASSABLE;
}

bool N_PositionBlocked(vec2_t xz_pos, void *nav_private, vec3_t map_pos)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tile;
    bool result = M_Tile_DescForPoint2D(res, map_pos, xz_pos, &tile);
    assert(result);

    const struct nav_chunk *chunk = &priv->chunks[IDX(tile.chunk_r, priv->width, tile.chunk_c)];
    return chunk->blockers[tile.tile_r][tile.tile_c] > 0;
}

vec2_t N_ClosestReachableDest(void *nav_private, vec3_t map_pos, vec2_t xz_src, vec2_t xz_dst)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    /* Convert source and destination positions to tile coordinates */
    bool result;
    (void)result;

    struct tile_desc src_desc, dst_desc;
    result = M_Tile_DescForPoint2D(res, map_pos, xz_src, &src_desc);
    assert(result);
    result = M_Tile_DescForPoint2D(res, map_pos, xz_dst, &dst_desc);
    assert(result);

    const struct nav_chunk *src_chunk = &priv->chunks[src_desc.chunk_r * priv->width + src_desc.chunk_c];
    const struct nav_chunk *dst_chunk = &priv->chunks[dst_desc.chunk_r * priv->width + dst_desc.chunk_c];
    uint16_t src_iid = src_chunk->islands[src_desc.tile_r][src_desc.tile_c];
    uint16_t dst_iid = dst_chunk->islands[dst_desc.tile_r][dst_desc.tile_c];

    if(src_iid == dst_iid)
        return xz_dst;

    /* Get the worldspace coordinates of the tile's center */
    vec2_t tile_dims = N_TileDims(); 
    struct tile_desc closest_td;
    int ntd = n_closest_island_tiles(priv, dst_desc, src_iid, true, &closest_td, 1);
    if(!ntd)
        return xz_src;
     
    vec2_t ret = {
        map_pos.x - (closest_td.chunk_c * FIELD_RES_C + closest_td.tile_c + 0.5f) * tile_dims.x,
        map_pos.z + (closest_td.chunk_r * FIELD_RES_R + closest_td.tile_r + 0.5f) * tile_dims.z,
    };
    return ret;
}

vec2_t N_TileDims(void)
{
    float x_ratio = (float)TILES_PER_CHUNK_WIDTH / FIELD_RES_C;
    float z_ratio = (float)TILES_PER_CHUNK_HEIGHT / FIELD_RES_R;
    return (vec2_t) {
        x_ratio * X_COORDS_PER_TILE,
        z_ratio * Z_COORDS_PER_TILE
    };
}

void N_BlockersIncref(vec2_t xz_pos, float range, vec3_t map_pos, void *nav_private)
{
    n_update_blockers(nav_private, xz_pos, range, map_pos, +1);
}

void N_BlockersDecref(vec2_t xz_pos, float range, vec3_t map_pos, void *nav_private)
{
    n_update_blockers(nav_private, xz_pos, range, map_pos, -1);
}

bool N_IsMaximallyClose(void *nav_private, vec3_t map_pos, 
                        vec2_t xz_pos, vec2_t xz_dest, float tolerance)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc dest_td;
    bool result = M_Tile_DescForPoint2D(res, map_pos, xz_dest, &dest_td);
    assert(result);

    struct tile_desc tds[FIELD_RES_R*2 + FIELD_RES_C*2];
    struct nav_chunk *chunk = &priv->chunks[IDX(dest_td.chunk_r, priv->width, dest_td.chunk_c)];

    uint16_t giid = chunk->islands[dest_td.tile_r][dest_td.tile_c];
    int ntds = n_closest_island_tiles(priv, dest_td, giid, false, tds, ARR_SIZE(tds));

    for(int i = 0; i < ntds; i++) {

        struct tile_desc *td = &tds[i];
        vec2_t tile_dims = N_TileDims();
        vec2_t tile_center = (vec2_t){
            map_pos.x - (td->chunk_c * FIELD_RES_C + td->tile_c) * tile_dims.x,
            map_pos.z + (td->chunk_r * FIELD_RES_R + td->tile_r) * tile_dims.z,
        };
        vec2_t delta;
        PFM_Vec2_Sub(&tile_center, &xz_pos, &delta);

        if(PFM_Vec2_Len(&delta) <= tolerance)
            return true;
    }

    return false;
}

bool N_PortalReachableFromTile(const struct portal *port, struct coord tile, const struct nav_chunk *chunk)
{
    for(int r = port->endpoints[0].r; r <= port->endpoints[1].r; r++) {
    for(int c = port->endpoints[0].c; c <= port->endpoints[1].c; c++) {

        if(chunk->local_islands[r][c] == ISLAND_NONE)
            continue;

        /* Do a sweep of the 3x3 area around the target tile,
         * to see if any adjacent tile can be reached. This
         * helps fix those cases where the tile directly under
         * an entity is also retained by a different nearby
         * entity and, thus, is considered 'unreachable'. 
         */
        for(int r1 = tile.r - 1; r1 <= tile.r + 1; r1++) {
        for(int c1 = tile.c - 1; c1 <= tile.c + 1; c1++) {

            if(r1 < 0 || r1 >= FIELD_RES_R)
                continue;
            if(c1 < 0 || c1 >= FIELD_RES_C)
                continue;
            if(chunk->local_islands[r][c] == chunk->local_islands[r1][c1])
                return true;
        }}
    }}
    return false;
}

int N_GridNeighbours(const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C], struct coord coord, 
                     struct coord out_neighbours[static 8], float out_costs[static 8])
{
    int ret = 0;

    for(int r = -1; r <= 1; r++) {
    for(int c = -1; c <= 1; c++) {

        int abs_r = coord.r + r;
        int abs_c = coord.c + c;

        if(abs_r < 0 || abs_r >= FIELD_RES_R)
            continue;
        if(abs_c < 0 || abs_c >= FIELD_RES_C)
            continue;
        if(r == 0 && c == 0)
            continue;
        if(cost_field[abs_r][abs_c] == COST_IMPASSABLE)
            continue;

        bool diag = (r == c) || (r == -c);
        if(diag && cost_field[abs_r][coord.c] == COST_IMPASSABLE 
                && cost_field[coord.r][abs_c] == COST_IMPASSABLE)
            continue;
        float cost_mult = diag ? sqrt(2) : 1.0f;

        out_neighbours[ret] = (struct coord){abs_r, abs_c};
        out_costs[ret] = cost_field[abs_r][abs_c] * cost_mult;
        ret++;
    }}
    assert(ret < 9);
    return ret;
}

int N_TilesUnderCircle(const struct nav_private *priv, vec2_t xz_center, float radius, 
                       vec3_t map_pos, struct tile_desc *out, int maxout)
{
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tile;
    bool result = M_Tile_DescForPoint2D(res, map_pos, xz_center, &tile);
    assert(result);

    int tile_len = X_COORDS_PER_TILE / (FIELD_RES_C / TILES_PER_CHUNK_WIDTH);
    int ntiles = ceil(radius / tile_len);

    int ret = 0;

    for(int dr = -ntiles; dr <= ntiles; dr++) {
    for(int dc = -ntiles; dc <= ntiles; dc++) {

        struct tile_desc curr = tile;
        if(!M_Tile_RelativeDesc(res, &curr, dc, dr))
            continue;

        struct box bounds = M_Tile_Bounds(res, map_pos, curr);
        vec2_t coords[] = {
            (vec2_t){bounds.x,                bounds.z                },
            (vec2_t){bounds.x - bounds.width, bounds.z                },
            (vec2_t){bounds.x,                bounds.z + bounds.height},
            (vec2_t){bounds.x - bounds.width, bounds.z + bounds.height},
            (vec2_t){bounds.x - bounds.width/2.0f, bounds.z + bounds.height/2.0f}
        };

        bool inside = false;
        for(int i = 0; i < ARR_SIZE(coords); i++) {

            if(C_PointInsideCircle2D(coords[i], xz_center, radius)) {
                inside = true;
                break;
            }
        }

        if(!inside)
            continue;

        out[ret++] = curr;
        if(ret == maxout) 
            break;
    }}

    return ret;
}

