/* 
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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
#include "../lib/public/mem.h"
#include "../lib/public/queue.h"
#include "../lib/public/pf_string.h"
#include "../phys/public/collision.h"
#include "../pf_math.h"
#include "../entity.h"
#include "../event.h"
#include "../main.h"
#include "../perf.h"
#include "../sched.h"
#include "../ui.h"
#include "../camera.h"

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
#define MAX_FIELD_TASKS          (256)

#define FOREACH_PORTAL(_priv, _layer, _local, ...)                                              \
    do{                                                                                         \
        for(int chunk_r = 0; chunk_r < (_priv)->height; chunk_r++) {                            \
        for(int chunk_c = 0; chunk_c < (_priv)->width;  chunk_c++) {                            \
                                                                                                \
            struct nav_chunk *curr_chunk = &(_priv)->chunks[_layer]                             \
                                                           [IDX(chunk_r, (_priv)->width, chunk_c)]; \
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

struct field_work_in{
    struct nav_private *priv;
    struct coord        chunk;
    struct field_target target;
    int                 faction_id;
    enum nav_layer      layer;
    ff_id_t             id;
};

struct field_work_out{
    struct flow_field field;
};

VEC_TYPE(in, struct field_work_in)
VEC_IMPL(static inline, in, struct field_work_in)

VEC_TYPE(out, struct field_work_out)
VEC_IMPL(static inline, out, struct field_work_out)

struct field_work{
    struct memstack mem;
    vec_in_t        in;
    vec_out_t       out;
    size_t          nwork;
    size_t          ntasks;
    uint32_t        tids[MAX_FIELD_TASKS];
    struct future   futures[MAX_FIELD_TASKS];
};

KHASH_SET_INIT_INT(coord)
KHASH_SET_INIT_INT64(td)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(coord)   *s_dirty_chunks[NAV_LAYER_MAX];
static bool              s_local_islands_dirty[NAV_LAYER_MAX] = {0};
static struct field_work s_field_work;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void *vec_realloc(void *ptr, size_t size)
{
    if(!ptr)
        return stalloc(&s_field_work.mem, size);

    void *ret = stalloc(&s_field_work.mem, size);
    if(!ret)
        return NULL;

    assert(size % 2 == 0);
    memcpy(ret, ptr, size / 2);
    return ret;
}

static void vec_free(void *ptr)
{
    /* no-op */
}

static uint64_t td_key(const struct tile_desc *td)
{
    return (((uint64_t)td->chunk_r << 48)
          | ((uint64_t)td->chunk_c << 32)
          | ((uint64_t)td->tile_r  << 16)
          | ((uint64_t)td->tile_c  <<  0));
}

static bool n_tile_pathable(const struct tile *tile)
{
    if(!tile->pathable)
        return false;
    if(tile->type != TILETYPE_FLAT && tile->ramp_height > 1)
        return false;
    return true;
}

static bool n_tile_blocked(struct nav_private *priv, enum nav_layer layer, 
                           const struct tile_desc td)
{
    struct nav_chunk *chunk = &priv->chunks[layer]
                                           [IDX(td.chunk_r, priv->width, td.chunk_c)];
    if(chunk->cost_base[td.tile_r][td.tile_c] == COST_IMPASSABLE)
        return true;
    if(chunk->blockers[td.tile_r][td.tile_c] > 0)
        return true;
    return false;
}

static struct map_resolution n_res(void *nav_private)
{
    struct nav_private *priv = nav_private;
    return (struct map_resolution){
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };
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
                               enum nav_layer layer, size_t chunk_w, size_t chunk_h)
{
    for(int r = 0; r < priv->height; r++) {
    for(int c = 0; c < priv->width; c++) {
            
        struct nav_chunk *curr_chunk = &priv->chunks[layer]
                                                    [IDX(r, priv->width, c)];

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
                .endpoints[0]   = (a_type & (EDGE_TOP | EDGE_BOT))    
                                ? (struct coord){a_fixed_idx, i}
                                : (a_type & (EDGE_LEFT | EDGE_RIGHT)) 
                                ? (struct coord){i, a_fixed_idx}
                                : (assert(0), (struct coord){0}),
                .num_neighbours = 0,
                .connected      = &b->portals[b->num_portals]
            };
            b->portals[b->num_portals] = (struct portal) {
                .component_id   = 0,
                .chunk          = b_coord,
                .endpoints[0]   = (b_type & (EDGE_TOP | EDGE_BOT))    
                                ? (struct coord){b_fixed_idx, i}
                                : (b_type & (EDGE_LEFT | EDGE_RIGHT)) 
                                ? (struct coord){i, b_fixed_idx}
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

static void n_create_portals(struct nav_private *priv, enum nav_layer layer)
{
    size_t n_links = 0;

    for(int r = 0; r < priv->height; r++) {
    for(int c = 0; c < priv->width;  c++) {
        
        struct nav_chunk *curr = &priv->chunks[layer][IDX(r, priv->width, c)];
        struct nav_chunk *bot = (r < priv->height-1) 
                              ? &priv->chunks[layer][IDX(r+1, priv->width, c)] 
                              : NULL;
        struct nav_chunk *right = (c < priv->width-1) 
                                ? &priv->chunks[layer][IDX(r, priv->width, c+1)] 
                                : NULL;

        if(bot) {
            n_link_chunks(curr, EDGE_BOT, 
                (struct coord){r, c}, bot, EDGE_TOP, (struct coord){r+1, c});
        }
        if(right) {
            n_link_chunks(curr, EDGE_RIGHT, 
                (struct coord){r, c}, right, EDGE_LEFT, (struct coord){r, c+1});
        }

        n_links += (!!bot + !!right);
    }}

    assert(n_links == (priv->height)*(priv->width-1) + (priv->width)*(priv->height-1));
}

static void n_link_chunk_portals(struct nav_chunk *chunk, struct coord chunk_coord, 
                                 enum nav_layer layer)
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
            bool has_path = AStar_GridPath(a, b, chunk_coord, chunk->cost_base, 
                layer, &path, &cost);
            if(has_path) {
                port->edges[port->num_neighbours] = (struct edge){
                    EDGE_STATE_ACTIVE, link_candidate, cost
                };
                port->num_neighbours++;    
            }
            Sched_TryYield();
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

static void n_update_components(struct nav_private *priv, enum nav_layer layer)
{
    struct portal *port;
    FOREACH_PORTAL(priv, layer, port,{
        port->component_id = 0;
    });

    int comp_id = 1;
    FOREACH_PORTAL(priv, layer, port, {
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

    STALLOC(vec2_t, corners_buff, 4 * vec_size(path));
    STALLOC(vec3_t, colors_buff, vec_size(path));

    vec2_t *corners_base = corners_buff;
    vec3_t *colors_base = colors_buff; 

    for(int i = 0, r = vec_AT(path, i).r, c = vec_AT(path, i).c; 
        i < vec_size(path); 
        i++, r = vec_AT(path, i).r, c = vec_AT(path, i).c) {

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, 
                               -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP((((float)r) / FIELD_RES_R) * chunk_z_dim, 
                               -chunk_z_dim, chunk_z_dim);

        *corners_base++ = (vec2_t){square_x, square_z};
        *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

        *colors_base++ = color;
    }

    assert(colors_base == colors_buff + vec_size(path));
    assert(corners_base == corners_buff + 4 * vec_size(path));

    size_t count = vec_size(path);
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawMapOverlayQuads,
        .nargs = 5,
        .args = {
            R_PushArg(corners_buff, sizeof(vec2_t) * 4 * vec_size(path)),
            R_PushArg(colors_buff, sizeof(vec3_t) * vec_size(path)),
            R_PushArg(&count, sizeof(count)),
            R_PushArg(chunk_model, sizeof(*chunk_model)),
            (void*)G_GetPrevTickMap(),
        },
    });

    STFREE(corners_buff);
    STFREE(colors_buff);
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
                float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, 
                                       -chunk_x_dim, chunk_x_dim);
                float square_z = CLAMP((((float)r) / FIELD_RES_R) * chunk_z_dim, 
                                       -chunk_z_dim, chunk_z_dim);

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

static dest_id_t n_dest_id(struct tile_desc dst_desc, enum nav_layer layer, int faction_id)
{
    assert(dst_desc.chunk_r <= 0x3f);
    assert(dst_desc.chunk_c <= 0x3f);
    assert(dst_desc.tile_r <= 0x3f);
    assert(dst_desc.tile_c <= 0x3f);
    assert(layer <= 0xf);
    assert(faction_id <= 0xf);

    return (((uint32_t)dst_desc.chunk_r & 0x3f) << 26)
         | (((uint32_t)dst_desc.chunk_c & 0x3f) << 20)
         | (((uint32_t)dst_desc.tile_r  & 0x3f) << 14)
         | (((uint32_t)dst_desc.tile_c  & 0x3f) <<  8)
         | (((uint32_t)layer            & 0x0f) <<  4)
         | (((uint32_t)faction_id       & 0x0f) <<  0);
}

static void n_visit_island(struct nav_private *priv, uint16_t id, 
                           struct tile_desc start, enum nav_layer layer)
{
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct nav_chunk *chunk 
        = &priv->chunks[layer][IDX(start.chunk_r, priv->width, start.chunk_c)];
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
            chunk = &priv->chunks[layer][IDX(neighb.chunk_r, priv->width, neighb.chunk_c)];

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

static bool enemy_ent(uint32_t ent, void *arg)
{
    int faction_id = (uintptr_t)arg;
    enum diplomacy_state ds;

    if(!(G_FlagsGet(ent) & ENTITY_FLAG_COMBATABLE))
        return false;
    if(G_GetFactionID(ent) == faction_id)
        return false;

    bool result = G_GetDiplomacyState(G_GetFactionID(ent), faction_id, &ds);
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

static void n_update_dirty_local_islands(void *nav_private, enum nav_layer layer)
{
    struct nav_private *priv = nav_private;
    if(!s_local_islands_dirty[layer])
        return;

    khash_t(coord) *set = s_dirty_chunks[layer];
    for(int i = kh_begin(set); i != kh_end(set); i++) {

        if(!kh_exist(set, i))
            continue;

        uint32_t key = kh_key(set, i);
        struct coord curr = (struct coord){ key >> 16, key & 0xffff };

        struct nav_chunk *chunk = &priv->chunks[layer][IDX(curr.r, priv->width, curr.c)];
        n_update_local_islands(chunk);
    }
    s_local_islands_dirty[layer] = false;
}

static void n_update_blockers(struct nav_private *priv, enum nav_layer layer, int faction_id,
                              struct tile_desc *tds, size_t ntds, int ref_delta)
{
    for(int i = 0; i < ntds; i++) {
    
        volatile struct tile_desc curr = tds[i];
        struct nav_chunk *chunk = 
            &priv->chunks[layer][IDX(curr.chunk_r, priv->width, curr.chunk_c)];

        assert(ref_delta < 0 ? chunk->blockers[curr.tile_r][curr.tile_c] >= -ref_delta : true);
        assert(ref_delta > 0 ? chunk->blockers[curr.tile_r][curr.tile_c] < (16384 - ref_delta) 
                             : true);

        int prev_val = chunk->blockers[curr.tile_r][curr.tile_c];
        chunk->blockers[curr.tile_r][curr.tile_c] += ref_delta;
        chunk->factions[faction_id][curr.tile_r][curr.tile_c] += ref_delta;
        assert(chunk->blockers[curr.tile_r][curr.tile_c] < 16383);

        int val = chunk->blockers[curr.tile_r][curr.tile_c];
        if(!!val != !!prev_val) { /* The tile changed states between occupied/non-occupied */

            int ret;
            uint32_t key = ((((uint32_t)curr.chunk_r) & 0xffff) << 16) 
                          | (((uint32_t)curr.chunk_c) & 0xffff);

            khash_t(coord) *set = s_dirty_chunks[layer];
            kh_put(coord, set, key, &ret);
            assert(ret != -1);

            s_local_islands_dirty[layer] = true;
        }
    }
}

static void n_update_blockers_circle(struct nav_private *priv, vec2_t xz_pos, float range, 
                                     int faction_id, vec3_t map_pos, int ref_delta)
{
    struct tile_desc tds[1024];
    int ntds = M_Tile_AllUnderCircle(n_res(priv), xz_pos, range, map_pos, tds, ARR_SIZE(tds));
    n_update_blockers(priv, NAV_LAYER_GROUND_1X1, faction_id, tds, ntds, ref_delta);

    struct tile_desc outline3x3[1024];
    int noutline3x3 = M_Tile_Contour(ntds, tds, n_res(priv), outline3x3, ARR_SIZE(outline3x3));
    n_update_blockers(priv, NAV_LAYER_GROUND_3X3, faction_id, tds, ntds, ref_delta);
    n_update_blockers(priv, NAV_LAYER_GROUND_3X3, faction_id, outline3x3, noutline3x3, ref_delta);

    struct tile_desc outline5x5[1024];
    int noutline5x5 = M_Tile_Contour(noutline3x3, outline3x3, n_res(priv), outline5x5, 
        ARR_SIZE(outline5x5));
    n_update_blockers(priv, NAV_LAYER_GROUND_5X5, faction_id, tds, ntds, ref_delta);
    n_update_blockers(priv, NAV_LAYER_GROUND_5X5, faction_id, outline3x3, noutline3x3, ref_delta);
    n_update_blockers(priv, NAV_LAYER_GROUND_5X5, faction_id, outline5x5, noutline5x5, ref_delta);

    struct tile_desc outline7x7[1024];
    int noutline7x7 = M_Tile_Contour(noutline5x5, outline5x5, n_res(priv), outline7x7, 
        ARR_SIZE(outline7x7));
    n_update_blockers(priv, NAV_LAYER_GROUND_7X7, faction_id, tds, ntds, ref_delta);
    n_update_blockers(priv, NAV_LAYER_GROUND_7X7, faction_id, outline3x3, noutline3x3, ref_delta);
    n_update_blockers(priv, NAV_LAYER_GROUND_7X7, faction_id, outline5x5, noutline5x5, ref_delta);
    n_update_blockers(priv, NAV_LAYER_GROUND_7X7, faction_id, outline7x7, noutline7x7, ref_delta);
}

static void n_update_blockers_obb(struct nav_private *priv, const struct obb *obb, 
                                  int faction_id, vec3_t map_pos, int ref_delta)
{
    struct tile_desc tds[1024];
    int ntds = M_Tile_AllUnderObj(map_pos, n_res(priv), obb, tds, ARR_SIZE(tds));
    n_update_blockers(priv, NAV_LAYER_GROUND_1X1, faction_id, tds, ntds, ref_delta);

    struct tile_desc outline3x3[1024];
    int noutline3x3 = M_Tile_Contour(ntds, tds, n_res(priv), outline3x3, ARR_SIZE(outline3x3));
    n_update_blockers(priv, NAV_LAYER_GROUND_3X3, faction_id, tds, ntds, ref_delta);
    n_update_blockers(priv, NAV_LAYER_GROUND_3X3, faction_id, outline3x3, noutline3x3, ref_delta);

    struct tile_desc outline5x5[1024];
    int noutline5x5 = M_Tile_Contour(noutline3x3, outline3x3, n_res(priv), outline5x5, ARR_SIZE(outline5x5));
    n_update_blockers(priv, NAV_LAYER_GROUND_5X5, faction_id, tds, ntds, ref_delta);
    n_update_blockers(priv, NAV_LAYER_GROUND_5X5, faction_id, outline3x3, noutline3x3, ref_delta);
    n_update_blockers(priv, NAV_LAYER_GROUND_5X5, faction_id, outline5x5, noutline5x5, ref_delta);

    struct tile_desc outline7x7[1024];
    int noutline7x7 = M_Tile_Contour(noutline5x5, outline5x5, n_res(priv), outline7x7, ARR_SIZE(outline7x7));
    n_update_blockers(priv, NAV_LAYER_GROUND_7X7, faction_id, tds, ntds, ref_delta);
    n_update_blockers(priv, NAV_LAYER_GROUND_7X7, faction_id, outline3x3, noutline3x3, ref_delta);
    n_update_blockers(priv, NAV_LAYER_GROUND_7X7, faction_id, outline5x5, noutline5x5, ref_delta);
    n_update_blockers(priv, NAV_LAYER_GROUND_7X7, faction_id, outline7x7, noutline7x7, ref_delta);
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

static int n_closest_island_tiles(const struct nav_private *priv, 
                                  enum nav_layer layer, struct tile_desc target, 
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

    const size_t count = res.chunk_h * res.chunk_w * res.tile_h * res.tile_w;
    STALLOC(bool, visited, count);
    memset(visited, 0, count);

    const size_t stride_a = res.chunk_h * res.chunk_w * res.tile_h;
    const size_t stride_b = res.chunk_h * res.chunk_w;
    const size_t stride_c = res.chunk_h;

    visited[target.chunk_r * stride_a + target.chunk_c * stride_b 
          + target.tile_r  * stride_c + target.tile_c] = true;

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
            const struct nav_chunk *chunk = 
                &priv->chunks[layer][IDX(neighb.chunk_r, priv->width, neighb.chunk_c)];

            if(visited[neighb.chunk_r * stride_a + neighb.chunk_c * stride_b
                     + neighb.tile_r  * stride_c + neighb.tile_c])
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

            visited[neighb.chunk_r * stride_a + neighb.chunk_c * stride_b
                  + neighb.tile_r  * stride_c + neighb.tile_c] = true;
            queue_td_push(&frontier, &neighb);
        }
    }

done:
    STFREE(visited);
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

static const struct portal *n_closest_reachable_portal(const struct nav_chunk *chunk, 
                                                       struct coord start, bool unblocked)
{
    const struct portal *ret = NULL;
    float min_cost = FLT_MAX;

    for(int i = 0; i < chunk->num_portals; i++) {

        const struct portal *curr = &chunk->portals[i];
        float cost = chunk->portal_travel_costs[i][start.r][start.c];

        if(unblocked && !N_PortalReachableFromTile(curr, start, chunk))
            continue;

        if(cost < min_cost) {
            ret = curr;
            min_cost = cost;
        }
    }
    return ret;
}

static bool arr_contains(uint16_t *array, size_t size, int elem)
{
    for(int i = 0; i < size; i++) {
        if(array[i] == elem) 
            return true;
    }
    return false;
}

static const struct portal *n_closest_reachable_from_location(struct nav_private *priv, 
                                                              enum nav_layer layer,
                                                              const struct nav_chunk *chunk, 
                                                              struct tile_desc loc,
                                                              struct tile_desc *out_nearest)
{
    float shortest_dist = FLT_MAX;
    const struct portal *ret = NULL;
    struct tile_desc nearest = {0};

    vec_portal_t path;
    vec_portal_init(&path);

    for(int i = 0; i < chunk->num_portals; i++) {

        uint16_t liids[64];
        size_t nliids = 0;
        const struct portal *port = &chunk->portals[i];

        for(int r = port->endpoints[0].r; r <= port->endpoints[1].r; r++) {
        for(int c = port->endpoints[0].c; c <= port->endpoints[1].c; c++) {

            struct tile_desc curr = (struct tile_desc){port->chunk.r, port->chunk.c, r, c};
            uint16_t curr_liid = chunk->local_islands[r][c];
            float cost;

            if(!arr_contains(liids, nliids, curr_liid) && nliids < ARR_SIZE(liids)) {
                liids[nliids++] = curr_liid;
                if(AStar_PortalGraphPath(loc, curr, port, priv, layer, &path, &cost)
                && cost < shortest_dist) {

                    nearest = curr;
                    shortest_dist = cost;
                    ret = port;
                }
            }
        }}
    }

    vec_portal_destroy(&path);
    if(ret) {
        *out_nearest = nearest;
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
static uint64_t n_enemy_seek_portalmask(const struct nav_private *priv, enum nav_layer layer,
                                        vec3_t map_pos, struct coord chunk, int faction_id)
{
    bool top = false, bot = false, left = false, right = false;

    uint32_t ents[1];
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
    const struct nav_chunk *nchunk = &priv->chunks[layer]
                                                  [IDX(chunk.r, priv->width, chunk.c)];

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

/* Returns true if the tile is blocked off from all portals by blockers */
static bool n_blocked_off(const struct nav_chunk *chunk, struct coord tile)
{
    for(int i = 0; i < chunk->num_portals; i++) {
        const struct portal *curr = &chunk->portals[i];
        if(N_PortalReachableFromTile(curr, tile, chunk))
            return false;
    }
    return true;
}

static bool n_moving_entity(uint32_t ent, void *arg)
{
    return !G_Move_Still(ent);
}

static khash_t(td) *n_moving_entities_tileset(struct nav_private *priv, vec3_t map_pos, 
                                              const struct obb *area)
{
    assert(Sched_UsingBigStack());

    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    vec2_t center_xz = (vec2_t){area->center.x, area->center.z};
    float radius = MAX(area->half_lengths[0], area->half_lengths[2]);

    uint32_t ents[1024];
    size_t nents = G_Pos_EntsInCircleWithPred(center_xz, radius, ents, 
        ARR_SIZE(ents), n_moving_entity, NULL);

    khash_t(td) *ret = kh_init(td);
    if(!ret)
        return NULL;

    for(int i = 0; i < nents; i++) {

        vec2_t xz_pos = G_Pos_GetXZ(ents[i]);
        struct tile_desc tile;

        bool result = M_Tile_DescForPoint2D(res, map_pos, xz_pos, &tile);
        assert(result);

        int status;
        kh_put(td, ret, td_key(&tile), &status);
    }

    return ret;
}

bool n_closest_adjacent_pos(void *nav_private, enum nav_layer layer, vec3_t map_pos, vec2_t xz_src, 
                            size_t ntiles, const struct tile_desc tds[], vec2_t *out)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc src_desc;
    bool result = M_Tile_DescForPoint2D(res, map_pos, xz_src, &src_desc);
    assert(result);

    const struct nav_chunk *src_chunk 
        = &priv->chunks[layer][src_desc.chunk_r * priv->width + src_desc.chunk_c];
    const uint16_t src_iid = src_chunk->islands[src_desc.tile_r][src_desc.tile_c];

    vec2_t tile_dims = N_TileDims();
    float min_dist = INFINITY;
    vec2_t min_pos = {0};

    for(int i = 0; i < ntiles; i++) {

        struct tile_desc td;
        int ntd = n_closest_island_tiles(priv, layer, tds[i], src_iid, false, &td, 1);
        if(!ntd)
            continue;

        vec2_t tile_center = (vec2_t){
            map_pos.x - (td.chunk_c * FIELD_RES_C + td.tile_c) * tile_dims.x,
            map_pos.z + (td.chunk_r * FIELD_RES_R + td.tile_r) * tile_dims.z,
        };
        vec2_t delta;
        PFM_Vec2_Sub(&xz_src, &tile_center, &delta);

        if(PFM_Vec2_Len(&delta) < min_dist) {
            min_dist = PFM_Vec2_Len(&delta);
            min_pos = tile_center;
        }
    }

    if(min_dist == INFINITY)
        return false;

    *out = min_pos;
    return true;
}

static bool n_objects_adjacent(void *nav_private, vec3_t map_pos, vec2_t xz_pos, 
                               float radius, size_t ntiles, const struct tile_desc tds[])
{
    assert(Sched_UsingBigStack());

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tds_ent[2048];
    size_t ntiles_ent = M_Tile_AllUnderCircle(n_res(priv), xz_pos, 
        radius, map_pos, tds_ent, ARR_SIZE(tds_ent));

    for(int i = 0; i < ntiles; i++) {
    for(int j = 0; j < ntiles_ent; j++) {
    
        size_t arow = tds[i].chunk_r * FIELD_RES_R + tds[i].tile_r;
        size_t acol = tds[i].chunk_c * FIELD_RES_C + tds[i].tile_c;

        size_t brow = tds_ent[j].chunk_r * FIELD_RES_R + tds_ent[j].tile_r;
        size_t bcol = tds_ent[j].chunk_c * FIELD_RES_C + tds_ent[j].tile_c;

        if(abs(brow - arow) <= 1 && abs(bcol - acol) <= 1)
            return true;
    }}
    return false;
}

static void n_update_portals(struct nav_private *priv, enum nav_layer layer)
{
    for(int chunk_r = 0; chunk_r < priv->height; chunk_r++){
    for(int chunk_c = 0; chunk_c < priv->width; chunk_c++){
            
        struct nav_chunk *curr_chunk = &priv->chunks[layer][IDX(chunk_r, priv->width, chunk_c)];
        curr_chunk->num_portals = 0;
    }}
    
    n_create_portals(priv, layer);

    for(int chunk_r = 0; chunk_r < priv->height; chunk_r++){
    for(int chunk_c = 0; chunk_c < priv->width; chunk_c++){
            
        struct nav_chunk *curr_chunk = &priv->chunks[layer]
                                                    [IDX(chunk_r, priv->width, chunk_c)];
        n_link_chunk_portals(curr_chunk, (struct coord){chunk_r, chunk_c}, layer);
        n_build_portal_travel_index(curr_chunk);
    }}
}

static void n_update_island_field(struct nav_private *priv, enum nav_layer layer)
{
    /* We assign a unique ID to each set of tiles that are mutually connected
     * (i.e. are on the same 'island'). The tile's 'island ID' can then be 
     * queried from the 'islands' field using the coordinate. 
     * To build the field, we treat every tile in the cost field as a node in
     * a graph, with cardinally adjacent pathable tiles being the 'neighbors'. 
     * Then we solve an instance of the 'coonected components' problem. 
     */

    uint16_t island_id = 0;

    for(int chunk_r = 0; chunk_r < priv->height; chunk_r++) {
    for(int chunk_c = 0; chunk_c < priv->width;  chunk_c++) {

        /* Initialize every node as 'unvisited' */
        struct nav_chunk *curr_chunk = &priv->chunks[layer]
                                                    [IDX(chunk_r, priv->width, chunk_c)];
        memset(curr_chunk->islands, 0xff, sizeof(curr_chunk->islands));
    }}

    for(int chunk_r = 0; chunk_r < priv->height; chunk_r++) {
    for(int chunk_c = 0; chunk_c < priv->width;  chunk_c++) {

        struct nav_chunk *curr_chunk = &priv->chunks[layer]
                                                    [IDX(chunk_r, priv->width, chunk_c)];

        for(int tile_r = 0; tile_r < FIELD_RES_R; tile_r++) {
        for(int tile_c = 0; tile_c < FIELD_RES_C; tile_c++) {

            if(curr_chunk->islands[tile_r][tile_c] != ISLAND_NONE)
                continue;

            if(curr_chunk->cost_base[tile_r][tile_c] == COST_IMPASSABLE)
                continue;

            struct tile_desc td = {chunk_r, chunk_c, tile_r, tile_c};
            n_visit_island(priv, island_id, td, layer);
            island_id++;
        }}
    }}
}

static void n_render_overlay_text(const char *text, vec4_t map_pos, 
                                  mat4x4_t *model, mat4x4_t *view, mat4x4_t *proj)
{
    vec2_t vres = UI_GetTextVres();
    vec2_t adj_vres = UI_ArAdjustedVRes(vres);

    vec4_t ws_pos_homo;
    PFM_Mat4x4_Mult4x1(model, &map_pos, &ws_pos_homo);
    ws_pos_homo.x /= ws_pos_homo.w;
    ws_pos_homo.z /= ws_pos_homo.w;

    vec4_t clip, tmp;
    PFM_Mat4x4_Mult4x1(view, &ws_pos_homo, &tmp);
    PFM_Mat4x4_Mult4x1(proj, &tmp, &clip);
    vec3_t ndc = (vec3_t){ 
        clip.x / clip.w, 
        clip.y / clip.w, 
        clip.z / clip.w
    };

    float screen_x = (ndc.x + 1.0f) * adj_vres.x/2.0f;
    float screen_y = adj_vres.y - ((ndc.y + 1.0f) * adj_vres.y/2.0f);

    float len = strlen(text) * 8.0f;
    struct rect bounds = (struct rect){screen_x - len/2.0f, screen_y, len, 25};
    UI_DrawText(text, bounds, (struct rgba){255, 0, 0, 255});
}

static bool n_request_path(void *nav_private, vec2_t xz_src, vec2_t xz_dest, int faction_id,
                           vec3_t map_pos, enum nav_layer layer, dest_id_t *out_dest_id)
{
    PERF_ENTER();

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };
    bool result;
    (void)result;

    n_update_dirty_local_islands(nav_private, layer);

    /* Convert source and destination positions to tile coordinates */
    struct tile_desc src_desc, dst_desc;
    result = M_Tile_DescForPoint2D(res, map_pos, xz_src, &src_desc);
    assert(result);
    result = M_Tile_DescForPoint2D(res, map_pos, xz_dest, &dst_desc);
    assert(result);

    dest_id_t ret = n_dest_id(dst_desc, layer, faction_id);

    /* Handle the case where no path exists between the source and destination 
     * (i.e. they are on different 'islands'). 
     */
    const struct nav_chunk *src_chunk = 
        &priv->chunks[layer][src_desc.chunk_r * priv->width + src_desc.chunk_c];
    const struct nav_chunk *dst_chunk = 
        &priv->chunks[layer][dst_desc.chunk_r * priv->width + dst_desc.chunk_c];

    uint16_t src_iid = src_chunk->islands[src_desc.tile_r][src_desc.tile_c];
    uint16_t dst_iid = dst_chunk->islands[dst_desc.tile_r][dst_desc.tile_c];

    if(src_iid != dst_iid) {
        PERF_RETURN(false); 
    }

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
        id = N_FlowFieldID((struct coord){dst_desc.chunk_r, dst_desc.chunk_c}, target, layer);

        if(!N_FC_ContainsFlowField(id)) {
        
            struct coord chunk = (struct coord){dst_desc.chunk_r, dst_desc.chunk_c};
            N_FlowFieldInit(chunk, &ff);
            N_FlowFieldUpdate(chunk, priv, faction_id, layer, target, &ff);
            N_FC_PutFlowField(id, &ff);
        }

        N_FC_PutDestFFMapping(ret, (struct coord){dst_desc.chunk_r, dst_desc.chunk_c}, id);
    }

    /* Create the LOS field for the destination chunk, if necessary */
    if(!N_FC_ContainsLOSField(ret, (struct coord){dst_desc.chunk_r, dst_desc.chunk_c})) {

        struct LOS_field lf;
        N_LOSFieldCreate(ret, (struct coord){dst_desc.chunk_r, dst_desc.chunk_c}, 
            dst_desc, priv, map_pos, &lf, NULL);
        N_FC_PutLOSField(ret, (struct coord){dst_desc.chunk_r, dst_desc.chunk_c}, &lf);
    }

    /* Source and destination positions are in the same chunk, and a path exists
     * between them. In this case, we only need a single flow field. .
     */
    if(src_desc.chunk_r == dst_desc.chunk_r && src_desc.chunk_c == dst_desc.chunk_c
    && N_ClosestPathableLocalIsland(priv, src_chunk, src_desc) 
    == N_ClosestPathableLocalIsland(priv, dst_chunk, dst_desc)) {

        *out_dest_id = ret;
        PERF_RETURN(true);
    }

    /* If the source and destination are on the same chunk and, in the absence of blockers,
     * would be reachable from one another, that means that the destination is blocked in
     * by blockers. In this case, get as close as possible. 
     */
    if(src_desc.chunk_r == dst_desc.chunk_r && src_desc.chunk_c == dst_desc.chunk_c) {

        bool either_blocked = n_blocked_off(src_chunk, 
                                (struct coord){src_desc.tile_r, src_desc.tile_c})
                           || n_blocked_off(src_chunk, 
                                (struct coord){dst_desc.tile_r, dst_desc.tile_c});

        if(either_blocked && n_normally_reachable(src_chunk, 
            (struct coord){src_desc.tile_r, src_desc.tile_c},
            (struct coord){dst_desc.tile_r, dst_desc.tile_c})) {

            *out_dest_id = ret;
            PERF_RETURN(true);
        }
    }

    const struct portal *dst_port = n_closest_reachable_portal(dst_chunk, 
        (struct coord){dst_desc.tile_r, dst_desc.tile_c}, true);
    if(!dst_port) {
        dst_port = n_closest_reachable_portal(dst_chunk, 
            (struct coord){dst_desc.tile_r, dst_desc.tile_c}, false);
    }

    if(!dst_port) {
        PERF_RETURN(false); 
    }

    float cost;
    vec_portal_t path;
    vec_portal_init(&path);

    bool path_exists = AStar_PortalGraphPath(src_desc, dst_desc, dst_port, 
        priv, layer, &path, &cost);
    if(!path_exists) {

        /* if we didn't find a path to the 'closest portal' to the destination, that must mean 
         * that the portal is severed by blockers. Try to find a more suitable target. 
         */
        struct tile_desc orig_dst = dst_desc;
        dst_port = n_closest_reachable_from_location(priv, layer, dst_chunk, src_desc, &dst_desc);

        /* When we are already on the desination chunk but the closest portal to the target is 
         * on another island, it is not guaranteed that it will lead to to the target. Resort 
         * to getting maximally close on this chunk.
         */
        if(N_ClosestPathableLocalIsland(priv, dst_chunk, dst_desc) 
        != N_ClosestPathableLocalIsland(priv, dst_chunk, orig_dst)
        && (src_desc.chunk_r == dst_desc.chunk_r && src_desc.chunk_c == dst_desc.chunk_c)) {

            vec_portal_destroy(&path);
            *out_dest_id = ret;
            PERF_RETURN(true);
        }

        if(dst_port) {
            path_exists = AStar_PortalGraphPath(src_desc, dst_desc, dst_port, 
                priv, layer, &path, &cost);
        }
    }

    if(!path_exists) {
        vec_portal_destroy(&path);

        /* If the source and destination are on the same chunk, then there is nothing left for us 
         * to do but get as close as possible */
        if(src_desc.chunk_r == dst_desc.chunk_r && src_desc.chunk_c == dst_desc.chunk_c) {
            *out_dest_id = ret;
            PERF_RETURN(true);
        }else{
            PERF_RETURN(false); 
        }
    }

    struct coord prev_los_coord = (struct coord){dst_desc.chunk_r, dst_desc.chunk_c};

    /* Traverse the portal path _backwards_ and generate the required fields, 
     * If they are not already cached. Add the results to the fieldcache. */
    for(int i = vec_size(&path)-1; i > 0; i--) {

        int next_hop_idx = i;
        /* If the very first hop takes us into another chunk, that means that the 'nearest portal'
         * to the source borders the 'next' chunk already. In this case, we must remember to
         * still generate a flow field for the current chunk steering to this portal. 
         */
        if(i == 1 && (vec_AT(&path, i).portal->chunk.r != src_desc.chunk_r 
                   || vec_AT(&path, i).portal->chunk.c != src_desc.chunk_c)) {
            next_hop_idx = 0;
        }

        struct portal_hop curr_hop = vec_AT(&path, MAX(next_hop_idx - 1, 0));
        struct portal_hop next_hop = vec_AT(&path, next_hop_idx);

        if(curr_hop.portal->connected == next_hop.portal)
            continue;

        /* Since we are moving from 'closest portal' to 'closest portal', it 
         * may be possible that the very last hop takes us from another portal in the 
         * destination chunk to the destination portal. This is not needed and will
         * overwrite the destination flow field made earlier. 
         */
        if(curr_hop.portal->chunk.r == dst_desc.chunk_r 
        && curr_hop.portal->chunk.c == dst_desc.chunk_c
        && next_hop.portal == dst_port
        && next_hop.liid == N_ClosestPathableLocalIsland(priv, dst_chunk, dst_desc)) {
            continue;
        }

        struct coord chunk_coord = curr_hop.portal->chunk;
        struct field_target target = (struct field_target){
            .type = TARGET_PORTAL,
            .pd = (struct portal_desc){
                next_hop.portal, 
                next_hop.liid,
                next_hop.portal->connected, 
                (i < vec_size(&path)-1) ? vec_AT(&path, next_hop_idx + 1).liid 
                                        : N_ClosestPathableLocalIsland(priv, dst_chunk, dst_desc),
            }
        };

        ff_id_t new_id = N_FlowFieldID(chunk_coord, target, layer);
        ff_id_t exist_id;
        struct flow_field ff;

        if(N_FC_GetDestFFMapping(ret, chunk_coord, &exist_id)
        && N_FC_ContainsFlowField(exist_id)) {

            /* The exact flow field we need has already been made */
            if(new_id == exist_id)
                goto ff_exists;

            /* This is the edge case when a path to a particular target takes us through
             * the same chunk more than once. This can happen if a chunk is divided into
             * 'islands' by unpathable barriers. 
             */
            const struct flow_field *exist_ff  = N_FC_FlowFieldAt(exist_id);
            memcpy(&ff, exist_ff, sizeof(struct flow_field));

            N_FlowFieldUpdate(chunk_coord, priv, faction_id, layer, target, &ff);
            /* We set the updated flow field for the new (least recently used) key. Since in 
             * this case more than one flowfield ID maps to the same field but we only keep 
             * one of the IDs, it may be possible that the same flowfield will be redundantly 
             * updated at a later time. However, this is largely inconsequential. 
             */
            N_FC_PutDestFFMapping(ret, chunk_coord, new_id);
            N_FC_PutFlowField(new_id, &ff);

            goto ff_exists;
        }

        N_FC_PutDestFFMapping(ret, chunk_coord, new_id);
        if(!N_FC_ContainsFlowField(new_id)) {

            N_FlowFieldInit(chunk_coord, &ff);
            N_FlowFieldUpdate(chunk_coord, priv, faction_id, layer, target, &ff);
            N_FC_PutFlowField(new_id, &ff);
        }

    ff_exists:
        assert(N_FC_ContainsFlowField(new_id));
        /* Reference field in the cache */
        (void)N_FC_FlowFieldAt(new_id);

        if(!N_FC_ContainsLOSField(ret, chunk_coord)) {

            assert((abs(prev_los_coord.r - chunk_coord.r) 
                  + abs(prev_los_coord.c - chunk_coord.c)) == 1);
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

static struct result field_task(void *arg)
{
    size_t *index = arg;
    struct field_work_in *in = &vec_AT(&s_field_work.in, *index);
    struct field_work_out *out = &vec_AT(&s_field_work.out, *index);

    N_FlowFieldInit(in->chunk, &out->field);
    N_FlowFieldUpdate(in->chunk, in->priv, in->faction_id, in->layer, in->target, &out->field);

    return NULL_RESULT;
}

static void field_join_work(void)
{
    for(int i = 0; i < s_field_work.ntasks; i++) {
		while(!Sched_FutureIsReady(&s_field_work.futures[i])) {
			Sched_RunSync(s_field_work.tids[i]);
		}
	}
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool N_Init(void)
{
    if(!N_FC_Init())
        return false;

    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        if((s_dirty_chunks[i] = kh_init(coord)) == NULL)
            goto fail_alloc;
    }

    memset(&s_field_work, 0, sizeof(s_field_work));
    if(!stalloc_init(&s_field_work.mem))
        goto fail_alloc;

    return true;

fail_alloc:
    N_Shutdown();
    return false;
}

void N_Update(void *nav_private)
{
    PERF_ENTER();

    struct nav_private *priv = nav_private;
    N_FC_InvalidateDynamicSurroundFields();

    for(int layer = 0; layer < NAV_LAYER_MAX; layer++) {
    
        n_update_dirty_local_islands(priv, layer);

        khash_t(coord) *set = s_dirty_chunks[layer];
        bool components_dirty = false;

        for(int i = kh_begin(set); i != kh_end(set); i++) {

            if(!kh_exist(set, i))
                continue;

            uint32_t key = kh_key(set, i);
            struct coord curr = (struct coord){ key >> 16, key & 0xffff };

            N_FC_InvalidateAllAtChunk(curr, layer);
            N_FC_InvalidateNeighbourEnemySeekFields(priv->width, priv->height, curr, layer);

            struct nav_chunk *chunk = &priv->chunks[layer]
                                                   [IDX(curr.r, priv->width, curr.c)];
            int nflipped = n_update_edge_states(chunk);

            if(nflipped) {
                components_dirty = true;
                N_FC_InvalidateAllThroughChunk(curr, layer);
            }
        }

        if(components_dirty) {
            n_update_components(priv, layer);
        }

        kh_clear(coord, set);
    }

    PERF_RETURN_VOID();
}

void N_Shutdown(void)
{
    field_join_work();
    stalloc_destroy(&s_field_work.mem);
    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        kh_destroy(coord, s_dirty_chunks[i]);
    }
    N_FC_Shutdown();
}

void N_ClearState(void)
{
    field_join_work();
    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        s_local_islands_dirty[i] = false;
        kh_clear(coord, s_dirty_chunks[i]);
    }
    N_FC_ClearAll();
    N_FC_ClearStats();
}

void *N_BuildForMapData(size_t w, size_t h, size_t chunk_w, size_t chunk_h,
                        const struct tile **chunk_tiles, bool update)
{
    struct nav_private *ret;
    ret = malloc(sizeof(struct nav_private));
    if(!ret)
        goto fail_alloc;

    memset(ret->chunks, 0, sizeof(ret->chunks));
    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        ret->chunks[i] = malloc(w * h * sizeof(struct nav_chunk));
        if(!ret->chunks[i])
            goto fail_alloc_chunks;
    }

    ret->width = w;
    ret->height = h;

    assert(FIELD_RES_R >= chunk_h && FIELD_RES_R % chunk_h == 0);
    assert(FIELD_RES_C >= chunk_w && FIELD_RES_C % chunk_w == 0);

    for(int layer = 0; layer < NAV_LAYER_MAX; layer++) {
    
        /* First build the base cost field based on terrain */
        for(int chunk_r = 0; chunk_r < ret->height; chunk_r++){
        for(int chunk_c = 0; chunk_c < ret->width;  chunk_c++){

            struct nav_chunk *curr_chunk = &ret->chunks[layer][IDX(chunk_r, ret->width, chunk_c)];
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
            memset(curr_chunk->factions, 0, sizeof(curr_chunk->factions));
        }}

        n_make_cliff_edges(ret, chunk_tiles, layer, chunk_w, chunk_h);
        n_update_portals(ret, layer);
        n_update_island_field(ret, layer);
    }

    return ret;

fail_alloc_chunks:
    N_FreePrivate(ret);
fail_alloc:
    return NULL;
}

void N_FreePrivate(void *nav_private)
{
    assert(nav_private);
    struct nav_private *priv = nav_private;

    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        free(priv->chunks[i]);
    }
    free(nav_private);
}

void N_RenderPathableChunk(void *nav_private, mat4x4_t *chunk_model,
                           const struct map *map, int chunk_r, int chunk_c,
                           enum nav_layer layer)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const struct nav_private *priv = nav_private;
    assert(chunk_r < priv->height);
    assert(chunk_c < priv->width);

    vec2_t corners_buff[4 * FIELD_RES_R * FIELD_RES_C];
    vec3_t colors_buff[FIELD_RES_R * FIELD_RES_C];

    const struct nav_chunk *chunk = &priv->chunks[layer]
                                                 [IDX(chunk_r, priv->width, chunk_c)];

    n_render_portals(chunk, chunk_model, map, (vec3_t){1.0f, 1.0f, 0.0f});

    vec2_t *corners_base = corners_buff;
    vec3_t *colors_base = colors_buff; 

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, 
                               -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP((((float)r) / FIELD_RES_R) * chunk_z_dim, 
                               -chunk_z_dim, chunk_z_dim);

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
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, 
                               -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP((((float)r) / FIELD_RES_R) * chunk_z_dim, 
                               -chunk_z_dim, chunk_z_dim);

        positions_buff[r * FIELD_RES_C + c] = (vec2_t){
            square_x - square_x_len / 2.0f,
            square_z + square_z_len / 2.0f
        };
        dirs_buff[r * FIELD_RES_C + c] = N_FlowDir(ff->field[r][c].dir_idx);
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
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, 
                               -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP((((float)r) / FIELD_RES_R) * chunk_z_dim, 
                               -chunk_z_dim, chunk_z_dim);

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

void N_RenderEnemySeekField(void *nav_private, const struct map *map, mat4x4_t *chunk_model, 
                            int chunk_r, int chunk_c, enum nav_layer layer, int faction_id)
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
    ff_id_t ffid = N_FlowFieldID((struct coord){chunk_r, chunk_c}, target, layer);
    if(!N_FC_ContainsFlowField(ffid))
        return;

    const struct flow_field *ff = N_FC_FlowFieldAt(ffid);
    if(!ff)
        return;

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, 
                               -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP((((float)r) / FIELD_RES_R) * chunk_z_dim, 
                               -chunk_z_dim, chunk_z_dim);

        positions_buff[r * FIELD_RES_C + c] = (vec2_t){
            square_x - square_x_len / 2.0f,
            square_z + square_z_len / 2.0f
        };
        dirs_buff[r * FIELD_RES_C + c] = N_FlowDir(ff->field[r][c].dir_idx);

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

void N_RenderSurroundField(void *nav_private, const struct map *map, mat4x4_t *chunk_model, 
                           int chunk_r, int chunk_c, enum nav_layer layer, uint32_t ent)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const struct nav_private *priv = nav_private;
    assert(chunk_r < priv->height);
    assert(chunk_c < priv->width);

    vec2_t positions_buff[FIELD_RES_R * FIELD_RES_C];
    vec2_t dirs_buff[FIELD_RES_R * FIELD_RES_C];

    struct field_target target = (struct field_target){
        .type = TARGET_ENTITY,
        .ent.target = ent,
        /* rest of the fields are unused */
    };
    ff_id_t ffid = N_FlowFieldID((struct coord){chunk_r, chunk_c}, target, layer);
    const struct flow_field *ff = N_FC_FlowFieldAt(ffid);
    if(!ff)
        return;

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, 
                               -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP((((float)r) / FIELD_RES_R) * chunk_z_dim, 
                               -chunk_z_dim, chunk_z_dim);

        positions_buff[r * FIELD_RES_C + c] = (vec2_t){
            square_x - square_x_len / 2.0f,
            square_z + square_z_len / 2.0f
        };
        dirs_buff[r * FIELD_RES_C + c] = N_FlowDir(ff->field[r][c].dir_idx);
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

void N_RenderNavigationBlockers(void *nav_private, const struct map *map, 
                                mat4x4_t *chunk_model, int chunk_r, int chunk_c,
                                enum nav_layer layer)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const struct nav_private *priv = nav_private;
    assert(chunk_r < priv->height);
    assert(chunk_c < priv->width);

    vec2_t corners_buff[4 * FIELD_RES_R * FIELD_RES_C];
    vec3_t colors_buff[FIELD_RES_R * FIELD_RES_C];

    const struct nav_chunk *chunk = &priv->chunks[layer]
                                                 [IDX(chunk_r, priv->width, chunk_c)];

    vec2_t *corners_base = corners_buff;
    vec3_t *colors_base = colors_buff; 

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, -chunk_x_dim, 
                               chunk_x_dim);
        float square_z = CLAMP((((float)r) / FIELD_RES_R) * chunk_z_dim, -chunk_z_dim, 
                               chunk_z_dim);

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

void N_RenderBuildableTiles(void *nav_private, const struct map *map, 
                            mat4x4_t *chunk_model, int chunk_r, int chunk_c,
                            const struct obb *obb, enum nav_layer layer)
{
    assert(Sched_UsingBigStack());

    struct nav_private *priv = nav_private;
    const struct nav_chunk *chunk = &priv->chunks[layer]
                                                 [IDX(chunk_r, priv->width, chunk_c)];

    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };
    vec3_t map_pos = M_GetPos(map);

    struct tile_desc tds[2048];
    size_t ntiles = M_Tile_AllUnderObj(map_pos, res, obb, tds, ARR_SIZE(tds));

    khash_t(td) *tileset = n_moving_entities_tileset(priv, map_pos, obb);
    assert(tileset);

    vec2_t corners_buff[4 * FIELD_RES_R * FIELD_RES_C];
    vec3_t colors_buff[FIELD_RES_R * FIELD_RES_C];

    size_t count = 0;
    vec2_t *corners_base = corners_buff;
    vec3_t *colors_base = colors_buff; 

    for(int i = 0; i < ntiles; i++) {

        if(tds[i].chunk_r != chunk_r || tds[i].chunk_c != chunk_c)
            continue;

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)tds[i].tile_c) / FIELD_RES_C) * chunk_x_dim, 
                               -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP((((float)tds[i].tile_r) / FIELD_RES_R) * chunk_z_dim, 
                               -chunk_z_dim, chunk_z_dim);

        *corners_base++ = (vec2_t){square_x, square_z};
        *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

        vec4_t center_homo = (vec4_t) {
            square_x - square_x_len / 2.0,
            0.0,
            square_z + square_z_len / 2.0,
            1.0
        };
        vec4_t ws_center_homo;
        PFM_Mat4x4_Mult4x1(chunk_model, &center_homo, &ws_center_homo);
        ws_center_homo.x /= ws_center_homo.w;
        ws_center_homo.z /= ws_center_homo.w;

        if(chunk->blockers [tds[i].tile_r][tds[i].tile_c]
        || chunk->cost_base[tds[i].tile_r][tds[i].tile_c] == COST_IMPASSABLE
        || !G_Fog_PlayerExplored((vec2_t){ws_center_homo.x, ws_center_homo.z})
        || (kh_get(td, tileset, td_key(tds + i)) != kh_end(tileset))) {
            *colors_base++ = (vec3_t){1.0f, 0.0f, 0.0f};
        }else{
            *colors_base++ = (vec3_t){0.0f, 1.0f, 0.0f};
        }
        count++;
    }
    free(tileset);

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

void N_RenderIslandIDs(void *nav_private, const struct map *map, 
                       const struct camera *cam, mat4x4_t *chunk_model, 
                       int chunk_r, int chunk_c, enum nav_layer layer)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const struct nav_private *priv = nav_private;
    assert(chunk_r < priv->height);
    assert(chunk_c < priv->width);

    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view); 
    Camera_MakeProjMat(cam, &proj);

    const struct nav_chunk *chunk = &priv->chunks[layer]
                                                 [IDX(chunk_r, priv->width, chunk_c)];

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, 
                               -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP((((float)r) / FIELD_RES_R) * chunk_z_dim, 
                               -chunk_z_dim, chunk_z_dim);

        vec4_t center_homo = (vec4_t) {
            square_x - square_x_len / 2.0,
            0.0,
            square_z + square_z_len / 2.0,
            1.0
        };

        char text[8];
        pf_snprintf(text, sizeof(text), "%d", chunk->islands[r][c]);
        n_render_overlay_text(text, center_homo, chunk_model, &view, &proj);
    }}
}

void N_RenderLocalIslandIDs(void *nav_private, const struct map *map, 
                            const struct camera *cam, mat4x4_t *chunk_model, 
                            int chunk_r, int chunk_c, enum nav_layer layer)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const struct nav_private *priv = nav_private;
    assert(chunk_r < priv->height);
    assert(chunk_c < priv->width);

    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view); 
    Camera_MakeProjMat(cam, &proj);

    const struct nav_chunk *chunk = &priv->chunks[layer]
                                                 [IDX(chunk_r, priv->width, chunk_c)];

    for(int r = 0; r < FIELD_RES_R; r++) {
    for(int c = 0; c < FIELD_RES_C; c++) {

        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / FIELD_RES_C) * chunk_x_dim, 
                               -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP((((float)r) / FIELD_RES_R) * chunk_z_dim, 
                               -chunk_z_dim, chunk_z_dim);

        vec4_t center_homo = (vec4_t) {
            square_x - square_x_len / 2.0,
            0.0,
            square_z + square_z_len / 2.0,
            1.0
        };

        char text[8];
        pf_snprintf(text, sizeof(text), "%d", chunk->local_islands[r][c]);
        n_render_overlay_text(text, center_homo, chunk_model, &view, &proj);
    }}
}

void N_RenderNavigationPortals(void *nav_private, const struct map *map, 
                               mat4x4_t *chunk_model, int chunk_r, int chunk_c,
                               enum nav_layer layer)
{
    struct nav_private *priv = nav_private;
    struct nav_chunk *chunk = &priv->chunks[layer]
                                           [IDX(chunk_r, priv->width, chunk_c)];

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

            vec3_t link_color = port->edges[j].es == EDGE_STATE_ACTIVE  
                              ? (vec3_t){0.0f, 1.0f, 0.0f} 
                              : port->edges[j].es == EDGE_STATE_BLOCKED 
                              ? (vec3_t){1.0f, 0.0f, 0.0f}
                              : (assert(0), (vec3_t){0});

            float cost;
            bool has_path = AStar_GridPath(a, b, 
                (struct coord){chunk_r, chunk_c}, chunk->cost_base, layer, &path, &cost);
            assert(has_path);
            n_render_grid_path(chunk, chunk_model, map, &path, link_color);
        }
    }

    vec_coord_destroy(&path);
}

void N_CutoutStaticObject(void *nav_private, vec3_t map_pos, const struct obb *obb)
{
    assert(Sched_UsingBigStack());

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tds[2048];
    size_t ntiles = M_Tile_AllUnderObj(map_pos, res, obb, tds, ARR_SIZE(tds));

    for(int layer = 0; layer < NAV_LAYER_MAX; layer++) {
        /* In the current implementation, we are content to block 
         * the exact same tiles for all the existing layers */
        for(int i = 0; i < ntiles; i++) {

            priv->chunks[layer][IDX(tds[i].chunk_r, priv->width, tds[i].chunk_c)]
                .cost_base[tds[i].tile_r][tds[i].tile_c] = COST_IMPASSABLE;
        }
    }
}

void N_UpdatePortals(void *nav_private)
{
    for(int layer = 0; layer < NAV_LAYER_MAX; layer++) {
        n_update_portals(nav_private, layer);
    }
}

void N_UpdateIslandsField(void *nav_private)
{
    for(int layer = 0; layer < NAV_LAYER_MAX; layer++) {
        n_update_island_field(nav_private, layer);
    }
}

dest_id_t N_DestIDForPos(void *nav_private, vec3_t map_pos, vec2_t xz_pos, enum nav_layer layer)
{
    struct tile_desc td;
    bool result = M_Tile_DescForPoint2D(n_res(nav_private), map_pos, xz_pos, &td);
    assert(result);

    return n_dest_id(td, layer, FACTION_ID_NONE);
}

dest_id_t N_DestIDForPosAttacking(void *nav_private, vec3_t map_pos, vec2_t xz_pos, 
                                  enum nav_layer layer, int faction_id)
{
    struct tile_desc td;
    bool result = M_Tile_DescForPoint2D(n_res(nav_private), map_pos, xz_pos, &td);
    assert(result);

    return n_dest_id(td, layer, faction_id);
}

bool N_RequestPath(void *nav_private, vec2_t xz_src, vec2_t xz_dest, 
                   vec3_t map_pos, enum nav_layer layer, dest_id_t *out_dest_id)
{
    return n_request_path(nav_private, xz_src, xz_dest, FACTION_ID_NONE, 
                          map_pos, layer, out_dest_id);
}

bool N_RequestPathAttacking(void *nav_private, vec2_t xz_src, vec2_t xz_dest, 
                            int faction_id, vec3_t map_pos, enum nav_layer layer, 
                            dest_id_t *out_dest_id)
{
    return n_request_path(nav_private, xz_src, xz_dest, faction_id, 
                          map_pos, layer, out_dest_id);
}

vec2_t N_DesiredPointSeekVelocity(dest_id_t id, vec2_t curr_pos, vec2_t xz_dest, 
                                  void *nav_private, vec3_t map_pos)
{
    unsigned dir_idx;
    struct nav_private *priv = nav_private;
    enum nav_layer layer = N_DestLayer(id);
    int faction_id = N_DestFactionID(id);

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
        bool result = n_request_path(nav_private, curr_pos, xz_dest, 
            faction_id, map_pos, layer, &ret);
        if(!result)
            return (vec2_t){0.0f};
        assert(ret == id);
        N_FC_GetDestFFMapping(id, (struct coord){tile.chunk_r, tile.chunk_c}, &ffid);
    }

    const struct flow_field *ff = N_FC_FlowFieldAt(ffid);
    if(!ff || ff->field[tile.tile_r][tile.tile_c].dir_idx == FD_NONE) {

        dest_id_t ret;
        bool result = n_request_path(nav_private, curr_pos, xz_dest, 
            faction_id, map_pos, layer, &ret);
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

    const struct nav_chunk *chunk = 
        &priv->chunks[layer][IDX(tile.chunk_r, priv->width, tile.chunk_c)];
    uint16_t local_iid = chunk->local_islands[tile.tile_r][tile.tile_c];

    /*   2. The entity has somehow ended up on an impassable tile. One example
     *      where this can happen is if an adjacent entity 'stops' and occupies 
     *      the tile directly under its' neighbour. The neighbour entity can 
     *      then unexpectedly find itself positioned on a 'blocked' tile. 
     */
    if(local_iid == ISLAND_NONE) {

        struct flow_field exist_ff = *ff;
        N_FlowFieldUpdateToNearestPathable(chunk, 
            (struct coord){tile.tile_r, tile.tile_c}, faction_id, &exist_ff);
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
    N_FlowFieldUpdateIslandToNearest(local_iid, priv, layer, faction_id, &exist_ff);
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
    return N_FlowDir(dir_idx);
}

vec2_t N_DesiredEnemySeekVelocity(vec2_t curr_pos, void *nav_private, enum nav_layer layer, 
                                  vec3_t map_pos, int faction_id)
{
    PERF_ENTER();
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    n_update_dirty_local_islands(nav_private, layer);

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

    ff_id_t ffid = N_FlowFieldID(chunk, target, layer);
    struct flow_field ff;

    if(!N_FC_ContainsFlowField(ffid)) {

        N_FlowFieldInit(chunk, &ff);
        N_FlowFieldUpdate(chunk, priv, faction_id, layer, target, &ff);
        N_FC_PutFlowField(ffid, &ff);

        assert(N_FC_ContainsFlowField(ffid));
    }

    const struct flow_field *pff = N_FC_FlowFieldAt(ffid);
    assert(pff);

    const struct nav_chunk *nchunk = 
        &priv->chunks[layer][IDX(curr_tile.chunk_r, priv->width, curr_tile.chunk_c)];
    uint16_t local_iid = nchunk->local_islands[curr_tile.tile_r][curr_tile.tile_c];
    int dir_idx = pff->field[curr_tile.tile_r][curr_tile.tile_c].dir_idx;

    if(dir_idx != FD_NONE)
        goto ff_found;

    /* The entity has somehow ended up on an impassable tile. One example
     * where this can happen is if an adjacent entity 'stops' and occupies 
     * the tile directly under its' neighbour. The neighbour entity can 
     * then unexpectedly find itself positioned on a 'blocked' tile. 
     */
    if(local_iid == ISLAND_NONE) {

        struct flow_field exist_ff = *pff;
        struct coord curr = (struct coord){curr_tile.tile_r, curr_tile.tile_c};

        N_FlowFieldUpdateToNearestPathable(nchunk, curr, faction_id, &exist_ff);
        N_FC_PutFlowField(ffid, &exist_ff);

        pff = N_FC_FlowFieldAt(ffid);
        goto ff_found;
    }

    /* We are on an island that is cut off by blockers or impassable terrain from any 
     * valid enemies - do our best to get as close to the 'action' as possible.
     */
    dir_idx = pff->field[curr_tile.tile_r][curr_tile.tile_c].dir_idx;
    if(dir_idx == FD_NONE) {

        struct flow_field exist_ff = *pff;
        N_FlowFieldUpdateIslandToNearest(local_iid, priv, layer, faction_id, &exist_ff);
        N_FC_PutFlowField(ffid, &exist_ff);

        pff = N_FC_FlowFieldAt(ffid);
        goto ff_found;
    }

ff_found:
    dir_idx = pff->field[curr_tile.tile_r][curr_tile.tile_c].dir_idx;
    PERF_RETURN(N_FlowDir(dir_idx));
}

vec2_t N_DesiredSurroundVelocity(vec2_t curr_pos, void *nav_private, enum nav_layer layer, 
                                 vec3_t map_pos, uint32_t ent, int faction_id)
{
    PERF_ENTER();
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    n_update_dirty_local_islands(nav_private, layer);

    struct tile_desc curr_tile;
    bool result = M_Tile_DescForPoint2D(res, map_pos, curr_pos, &curr_tile);
    assert(result);

    struct coord chunk = (struct coord){curr_tile.chunk_r, curr_tile.chunk_c};

    struct field_target target = (struct field_target){
        .type = TARGET_ENTITY,
        .ent.target = ent,
        .ent.map_pos = map_pos,
    };

    ff_id_t ffid = N_FlowFieldID(chunk, target, layer);
    struct flow_field ff;

    if(!N_FC_ContainsFlowField(ffid)) {

        N_FlowFieldInit(chunk, &ff);
        N_FlowFieldUpdate(chunk, priv, faction_id, layer, target, &ff);
        N_FC_PutFlowField(ffid, &ff);

        assert(N_FC_ContainsFlowField(ffid));
    }

    const struct flow_field *pff = N_FC_FlowFieldAt(ffid);
    assert(pff);

    const struct nav_chunk *nchunk = 
        &priv->chunks[layer][IDX(curr_tile.chunk_r, priv->width, curr_tile.chunk_c)];
    uint16_t local_iid = nchunk->local_islands[curr_tile.tile_r][curr_tile.tile_c];
    int dir_idx = pff->field[curr_tile.tile_r][curr_tile.tile_c].dir_idx;

    /* The entity has somehow ended up on an impassable tile. One example
     * where this can happen is if an adjacent entity 'stops' and occupies 
     * the tile directly under its' neighbour. The neighbour entity can 
     * then unexpectedly find itself positioned on a 'blocked' tile. 
     */
    if(local_iid == ISLAND_NONE) {

        struct flow_field exist_ff = *pff;
        struct coord curr = (struct coord){curr_tile.tile_r, curr_tile.tile_c};

        N_FlowFieldUpdateToNearestPathable(nchunk, curr, faction_id, &exist_ff);
        N_FC_PutFlowField(ffid, &exist_ff);

        pff = N_FC_FlowFieldAt(ffid);
        goto ff_found;
    }

    /* We are on an island that is cut off by blockers or impassable terrain from any 
     * valid enemies - do our best to get as close to the 'action' as possible.
     */
    dir_idx = pff->field[curr_tile.tile_r][curr_tile.tile_c].dir_idx;
    if(dir_idx == FD_NONE) {

        struct flow_field exist_ff = *pff;
        N_FlowFieldUpdateIslandToNearest(local_iid, priv, layer, faction_id, &exist_ff);
        N_FC_PutFlowField(ffid, &exist_ff);

        pff = N_FC_FlowFieldAt(ffid);
        goto ff_found;
    }

ff_found:
    dir_idx = pff->field[curr_tile.tile_r][curr_tile.tile_c].dir_idx;
    PERF_RETURN(N_FlowDir(dir_idx));
}

void N_PrepareAsyncWork(void)
{
    vec_in_init_alloc(&s_field_work.in, vec_realloc, vec_free);
    vec_in_resize(&s_field_work.in, MAX_FIELD_TASKS);

    vec_out_init_alloc(&s_field_work.out, vec_realloc, vec_free);
    vec_out_resize(&s_field_work.out, MAX_FIELD_TASKS);
}

void N_RequestAsyncEnemySeekField(vec2_t curr_pos, void *nav_private, enum nav_layer layer,
                                  vec3_t map_pos, int faction_id)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    n_update_dirty_local_islands(nav_private, layer);

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

    ff_id_t ffid = N_FlowFieldID(chunk, target, layer);
    if(N_FC_ContainsFlowField(ffid))
       return;

    /* We'll compute the missing field on-demand later */
    if(s_field_work.nwork == MAX_FIELD_TASKS)
        return;

    for(int i = 0; i < s_field_work.nwork; i++) {
        /* We already have a job for this field */
        if(vec_AT(&s_field_work.in, i).id == ffid)
            return;
    }

    vec_in_push(&s_field_work.in, (struct field_work_in){
        .priv = priv,
        .chunk = chunk,
        .target = target,
        .faction_id = faction_id,
        .layer = layer,
        .id = ffid
    });
    size_t *arg = stalloc(&s_field_work.mem, sizeof(size_t));
    *arg = s_field_work.nwork++;

    SDL_AtomicSet(&s_field_work.futures[s_field_work.ntasks].status, FUTURE_INCOMPLETE);
    s_field_work.tids[s_field_work.ntasks] = Sched_Create(1, field_task, arg, 
        &s_field_work.futures[s_field_work.ntasks], TASK_BIG_STACK);
    if(s_field_work.tids[s_field_work.ntasks] != NULL_TID) {
        s_field_work.ntasks++;
    }
}

void N_RequestAsyncSurroundField(vec2_t curr_pos, void *nav_private, enum nav_layer layer,
                                 vec3_t map_pos, uint32_t ent, int faction_id)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    n_update_dirty_local_islands(nav_private, layer);

    struct tile_desc curr_tile;
    bool result = M_Tile_DescForPoint2D(res, map_pos, curr_pos, &curr_tile);
    assert(result);

    struct coord chunk = (struct coord){curr_tile.chunk_r, curr_tile.chunk_c};

    struct field_target target = (struct field_target){
        .type = TARGET_ENTITY,
        .ent.target = ent,
        .ent.map_pos = map_pos,
    };

    ff_id_t ffid = N_FlowFieldID(chunk, target, layer);
    if(N_FC_ContainsFlowField(ffid))
       return;

    /* We'll compute the missing field on-demand later */
    if(s_field_work.nwork == MAX_FIELD_TASKS)
        return;

    for(int i = 0; i < s_field_work.nwork; i++) {
        /* We already have a job for this field */
        if(vec_AT(&s_field_work.in, i).id == ffid)
            return;
    }

    vec_in_push(&s_field_work.in, (struct field_work_in){
        .priv = priv,
        .chunk = chunk,
        .target = target,
        .faction_id = faction_id,
        .layer = layer,
        .id = ffid
    });
    size_t *arg = stalloc(&s_field_work.mem, sizeof(size_t));
    *arg = s_field_work.nwork++;

    SDL_AtomicSet(&s_field_work.futures[s_field_work.ntasks].status, FUTURE_INCOMPLETE);
    s_field_work.tids[s_field_work.ntasks] = Sched_Create(1, field_task, arg, 
        &s_field_work.futures[s_field_work.ntasks], TASK_BIG_STACK);
    if(s_field_work.tids[s_field_work.ntasks] != NULL_TID) {
        s_field_work.ntasks++;
    }
}

void N_AwaitAsyncFields(void)
{
    field_join_work();
    for(int i = 0; i < s_field_work.ntasks; i++) {
        struct field_work_in *in = &vec_AT(&s_field_work.in, i);
        struct field_work_out *out = &vec_AT(&s_field_work.out, i);
        N_FC_PutFlowField(in->id, &out->field);
    }
    stalloc_clear(&s_field_work.mem);
    s_field_work.nwork = 0;
    s_field_work.ntasks = 0;
}

bool N_HasEntityLOS(vec2_t curr_pos, uint32_t ent, void *nav_private, 
                    enum nav_layer layer, vec3_t map_pos)
{
    vec2_t ent_pos = G_Pos_GetXZ(ent);
    bool result = false;

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct line_seg_2d line = (struct line_seg_2d){
        ent_pos.x, ent_pos.z,
        curr_pos.x, curr_pos.z
    };

    const size_t count = ceil(sqrt(pow(FIELD_RES_R, 2) + pow(FIELD_RES_C, 2)));
    STALLOC(struct tile_desc, tds, count);
    size_t ntds = M_Tile_LineSupercoverTilesSorted(res, map_pos, line, tds, count);

    for(int i = 0; i < ntds; i++) {
        if(n_tile_blocked(priv, layer, tds[i]))
            goto out;
    }
    result = true;

out:
    STFREE(tds);
    return result;
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

bool N_PositionPathable(vec2_t xz_pos, enum nav_layer layer, void *nav_private, vec3_t map_pos)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tile;
    bool result = M_Tile_DescForPoint2D(res, map_pos, xz_pos, &tile);
    assert(result);

    const struct nav_chunk *chunk = &priv->chunks[layer]
                                                 [IDX(tile.chunk_r, priv->width, tile.chunk_c)];
    return chunk->cost_base[tile.tile_r][tile.tile_c] != COST_IMPASSABLE;
}

bool N_PositionBlocked(vec2_t xz_pos, enum nav_layer layer, void *nav_private, vec3_t map_pos)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tile;
    bool result = M_Tile_DescForPoint2D(res, map_pos, xz_pos, &tile);
    assert(result);

    const struct nav_chunk *chunk = 
        &priv->chunks[layer][IDX(tile.chunk_r, priv->width, tile.chunk_c)];
    return chunk->blockers[tile.tile_r][tile.tile_c] > 0;
}

vec2_t N_ClosestReachableDest(void *nav_private, enum nav_layer layer, vec3_t map_pos, vec2_t xz_src, vec2_t xz_dst)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };
    bool result;
    (void)result;

    /* Convert source and destination positions to tile coordinates */
    struct tile_desc src_desc, dst_desc;
    result = M_Tile_DescForPoint2D(res, map_pos, xz_src, &src_desc);
    assert(result);
    result = M_Tile_DescForPoint2D(res, map_pos, xz_dst, &dst_desc);
    assert(result);

    const struct nav_chunk *src_chunk = 
        &priv->chunks[layer][src_desc.chunk_r * priv->width + src_desc.chunk_c];
    const struct nav_chunk *dst_chunk = 
        &priv->chunks[layer][dst_desc.chunk_r * priv->width + dst_desc.chunk_c];

    uint16_t src_iid = src_chunk->islands[src_desc.tile_r][src_desc.tile_c];
    uint16_t dst_iid = dst_chunk->islands[dst_desc.tile_r][dst_desc.tile_c];

    if(src_iid == dst_iid)
        return xz_dst;

    /* Get the worldspace coordinates of the tile's center */
    vec2_t tile_dims = N_TileDims(); 
    struct tile_desc closest_td;
    int ntd = n_closest_island_tiles(priv, layer, dst_desc, src_iid, true, &closest_td, 1);
    if(!ntd)
        return xz_src;
     
    vec2_t ret = {
        map_pos.x - (closest_td.chunk_c * FIELD_RES_C + closest_td.tile_c + 0.5f) * tile_dims.x,
        map_pos.z + (closest_td.chunk_r * FIELD_RES_R + closest_td.tile_r + 0.5f) * tile_dims.z,
    };
    return ret;
}

bool N_ClosestPathable(void *nav_private, enum nav_layer layer, 
                       vec3_t map_pos, vec2_t xz_src, vec2_t *out)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };
    bool result;
    (void)result;

    /* Convert source position to tile coordinates */
    struct tile_desc src_desc;
    result = M_Tile_DescForPoint2D(res, map_pos, xz_src, &src_desc);
    assert(result);

    if(!n_tile_blocked(priv, layer, src_desc)) {
        *out = xz_src;
        return true;
    }

    bool ret = false;
    queue_td_t frontier;
    queue_td_init(&frontier, 1024);
    queue_td_push(&frontier, &src_desc);

    khash_t(td) *visited = kh_init(td);

    while(queue_size(frontier) > 0) {

        struct tile_desc curr;
        queue_td_pop(&frontier, &curr);

        if(!n_tile_blocked(priv, layer, curr)) {
            struct box bounds = M_Tile_Bounds(res, map_pos, curr);
            *out = (vec2_t) { bounds.x, bounds.z };
            ret = true;
            goto done;
        }

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

            if(kh_get(td, visited, td_key(&neighb)) != kh_end(visited))
                continue;

            kh_put(td, visited, td_key(&neighb), &(int){0});
            queue_td_push(&frontier, &neighb);
        }
    }

done:
    kh_destroy(td, visited);
    queue_td_destroy(&frontier);
    return ret;
}

bool N_LocationsReachable(void *nav_private, enum nav_layer layer, 
                          vec3_t map_pos, vec2_t a, vec2_t b)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };
    bool result;
    (void)result;

    struct tile_desc src_desc, dst_desc;
    result = M_Tile_DescForPoint2D(res, map_pos, a, &src_desc);
    assert(result);
    result = M_Tile_DescForPoint2D(res, map_pos, b, &dst_desc);
    assert(result);

    const struct nav_chunk *src_chunk = &priv->chunks[layer][src_desc.chunk_r * priv->width + src_desc.chunk_c];
    const struct nav_chunk *dst_chunk = &priv->chunks[layer][dst_desc.chunk_r * priv->width + dst_desc.chunk_c];

    uint16_t src_iid = src_chunk->islands[src_desc.tile_r][src_desc.tile_c];
    uint16_t dst_iid = dst_chunk->islands[dst_desc.tile_r][dst_desc.tile_c];

    if(src_iid != dst_iid)
        return false;

    return true;
}

bool N_ClosestReachableAdjacentPosStatic(void *nav_private, enum nav_layer layer, vec3_t map_pos, 
                                         vec2_t xz_src, const struct obb *target, vec2_t *out)
{
    PERF_ENTER();
    assert(Sched_UsingBigStack());

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tds[2048];
    size_t ntiles = M_Tile_AllUnderObj(map_pos, res, target, tds, ARR_SIZE(tds));

    PERF_RETURN(n_closest_adjacent_pos(nav_private, layer, map_pos, xz_src, ntiles, tds, out));
}

bool N_ClosestReachableAdjacentPosDynamic(void *nav_private, enum nav_layer layer, vec3_t map_pos, 
                                          vec2_t xz_src, vec2_t xz_pos, float radius, vec2_t *out)
{
    PERF_ENTER();
    assert(Sched_UsingBigStack());

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tds[2048];
    size_t ntiles = M_Tile_AllUnderCircle(n_res(priv), xz_pos, radius, map_pos, tds, ARR_SIZE(tds));

    PERF_RETURN(n_closest_adjacent_pos(nav_private, layer, map_pos, xz_src, ntiles, tds, out));
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

void N_BlockersIncref(vec2_t xz_pos, float range, int faction_id, vec3_t map_pos, void *nav_private)
{
    n_update_blockers_circle(nav_private, xz_pos, range, faction_id, map_pos, +1);
}

void N_BlockersDecref(vec2_t xz_pos, float range, int faction_id, vec3_t map_pos, void *nav_private)
{
    n_update_blockers_circle(nav_private, xz_pos, range, faction_id, map_pos, -1);
}

void N_BlockersIncrefOBB(void *nav_private, int faction_id, vec3_t map_pos, const struct obb *obb)
{
    n_update_blockers_obb(nav_private, obb, faction_id, map_pos, +1);
}

void N_BlockersDecrefOBB(void *nav_private, int faction_id, vec3_t map_pos, const struct obb *obb)
{
    n_update_blockers_obb(nav_private, obb, faction_id, map_pos, -1);
}

bool N_IsMaximallyClose(void *nav_private, enum nav_layer layer, vec3_t map_pos, 
                        vec2_t xz_pos, vec2_t xz_dest, float tolerance)
{
    PERF_ENTER();

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc dest_td;
    bool result = M_Tile_DescForPoint2D(res, map_pos, xz_dest, &dest_td);
    assert(result);

    struct tile_desc tds[FIELD_RES_R*2 + FIELD_RES_C*2];
    struct nav_chunk *chunk = &priv->chunks[layer]
                                           [IDX(dest_td.chunk_r, priv->width, dest_td.chunk_c)];

    uint16_t giid = chunk->islands[dest_td.tile_r][dest_td.tile_c];
    int ntds = n_closest_island_tiles(priv, layer, dest_td, giid, false, tds, ARR_SIZE(tds));

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
            PERF_RETURN(true);
    }

    PERF_RETURN(false);
}

bool N_IsAdjacentToImpassable(void *nav_private, enum nav_layer layer, vec3_t map_pos, vec2_t xz_pos)
{
    PERF_ENTER();

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc curr_td;
    bool result = M_Tile_DescForPoint2D(res, map_pos, xz_pos, &curr_td);
    assert(result);

    for(int dr = -1; dr <= 1; dr++) {
    for(int dc = -1; dc <= 1; dc++) {
        if((dr == dc) || (dr == -dc)) /* diag */
            continue;
        struct tile_desc td = curr_td;
        if(!M_Tile_RelativeDesc(res, &td, dc, dr))
            continue;
        if(n_tile_blocked(priv, layer, td))
            PERF_RETURN(true);
    }}

    PERF_RETURN(false);
}

bool N_PortalReachableFromTile(const struct portal *port, struct coord tile, 
                               const struct nav_chunk *chunk)
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
                     struct coord out_neighbours[], float out_costs[])
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

bool N_ObjAdjacentToStatic(void *nav_private, vec3_t map_pos, uint32_t ent, const struct obb *stat)
{
    assert(Sched_UsingBigStack());

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tds_stat[2048];
    size_t ntiles_stat = M_Tile_AllUnderObj(map_pos, res, stat, tds_stat, ARR_SIZE(tds_stat));

    vec2_t pos = G_Pos_GetXZ(ent);
    float radius = G_GetSelectionRadius(ent);

    return n_objects_adjacent(nav_private, map_pos, pos, radius, ntiles_stat, tds_stat);
}

bool N_ObjAdjacentToStaticWith(void *nav_private, vec3_t map_pos, vec2_t xz_pos, float radius,
                               const struct obb *stat)
{
    assert(Sched_UsingBigStack());

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tds_stat[2048];
    size_t ntiles_stat = M_Tile_AllUnderObj(map_pos, res, stat, tds_stat, ARR_SIZE(tds_stat));
    return n_objects_adjacent(nav_private, map_pos, xz_pos, radius, ntiles_stat, tds_stat);
}

bool N_ObjAdjacentToDynamic(void *nav_private, vec3_t map_pos, uint32_t ent, 
                            vec2_t xz_pos, float radius)
{
    assert(Sched_UsingBigStack());

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tds_dyn[2048];
    size_t ntiles_dyn = M_Tile_AllUnderCircle(n_res(priv), xz_pos, radius, 
        map_pos, tds_dyn, ARR_SIZE(tds_dyn));

    vec2_t pos = G_Pos_GetXZ(ent);
    float ent_radius = G_GetSelectionRadius(ent);

    return n_objects_adjacent(nav_private, map_pos, pos, ent_radius, ntiles_dyn, tds_dyn);
}

bool N_ObjAdjacentToDynamicWith(void *nav_private, vec3_t map_pos, 
                                vec2_t xz_pos_a, float radius_a,
                                vec2_t xz_pos_b, float radius_b)
{
    assert(Sched_UsingBigStack());

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tds_dyn[2048];
    size_t ntiles_dyn = M_Tile_AllUnderCircle(n_res(priv), xz_pos_b, radius_b, 
        map_pos, tds_dyn, ARR_SIZE(tds_dyn));

    return n_objects_adjacent(nav_private, map_pos, xz_pos_a, radius_a, ntiles_dyn, tds_dyn);
}

void N_GetResolution(void *nav_private, struct map_resolution *out)
{
    struct nav_private *priv = nav_private;

    out->chunk_w = priv->width;
    out->chunk_h = priv->height;
    out->tile_w = FIELD_RES_C;
    out->tile_h = FIELD_RES_R;
}

bool N_ObjectBuildable(void *nav_private, enum nav_layer layer, 
                       vec3_t map_pos, const struct obb *obb)
{
    assert(Sched_UsingBigStack());

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    struct tile_desc tds[2048];
    size_t ntiles = M_Tile_AllUnderObj(map_pos, res, obb, tds, ARR_SIZE(tds));

    khash_t(td) *tileset = n_moving_entities_tileset(priv, map_pos, obb);
    assert(tileset);
    bool ret = false;

    for(int i = 0; i < ntiles; i++) {

        const struct nav_chunk *chunk = 
            &priv->chunks[layer][IDX(tds[i].chunk_r, priv->width, tds[i].chunk_c)];
        struct box bounds = M_Tile_Bounds(res, map_pos, tds[i]);
        vec2_t center = (vec2_t){
            bounds.x - bounds.width / 2.0f,
            bounds.z + bounds.height / 2.0f
        };
    
        if(chunk->blockers [tds[i].tile_r][tds[i].tile_c]
        || chunk->cost_base[tds[i].tile_r][tds[i].tile_c] == COST_IMPASSABLE
        || !G_Fog_PlayerExplored((vec2_t){center.x, center.z})
        || (kh_get(td, tileset, td_key(tds + i)) != kh_end(tileset))) {
            goto out;
        }
    }
    ret = true;
out:
    free(tileset);
    return ret;
}

enum nav_layer N_DestLayer(dest_id_t id)
{
    return ((id >> 4) & 0xf);
}

int N_DestFactionID(dest_id_t id)
{
    return (id & 0xf);
}

bool N_DestIDIsAttacking(dest_id_t id)
{
    return (N_DestFactionID(id) != FACTION_ID_NONE);
}

vec2_t N_ClosestReachableInRange(void *nav_private, vec3_t map_pos, 
                                 vec2_t xz_src, vec2_t xz_target, 
                                 float range, enum nav_layer layer)
{
    assert(Sched_UsingBigStack());

    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };
    bool result;
    (void)result;

    /* Convert source position to tile coordinates */
    struct tile_desc src_desc;
    result = M_Tile_DescForPoint2D(res, map_pos, xz_src, &src_desc);
    assert(result);

    const struct nav_chunk *src_chunk = 
        &priv->chunks[layer][src_desc.chunk_r * priv->width + src_desc.chunk_c];
    uint16_t src_iid = src_chunk->islands[src_desc.tile_r][src_desc.tile_c];

    /* Find the set of tiles that are within range of the target */
    struct tile_desc tds[2048];
    size_t ntiles = M_Tile_AllUnderCircle(res, xz_target, range, 
        map_pos, tds, ARR_SIZE(tds));

    /* Pick out the closest one that's on the same global island (i.e. potentially reachable) */
    const vec2_t tile_dims = N_TileDims();
    float min_dist = INFINITY;
    vec2_t nearest_pos;

    for(int i = 0; i < ntiles; i++) {

        const struct tile_desc *curr = &tds[i];
        const struct nav_chunk *curr_chunk = 
            &priv->chunks[layer][curr->chunk_r * priv->width + curr->chunk_c];
        uint16_t curr_iid = curr_chunk->islands[curr->tile_r][curr->tile_c];

        if(curr_iid != src_iid)
            continue;

        vec2_t tile_center = (vec2_t){
            map_pos.x - (curr->chunk_c * FIELD_RES_C + curr->tile_c) * tile_dims.x,
            map_pos.z + (curr->chunk_r * FIELD_RES_R + curr->tile_r) * tile_dims.z,
        };
        vec2_t delta;
        PFM_Vec2_Sub(&xz_src, &tile_center, &delta);

        if(PFM_Vec2_Len(&delta) < min_dist) {
            min_dist = PFM_Vec2_Len(&delta);
            nearest_pos = tile_center;
        }
    }

    /* If there are absolutely no tiles that are even potentially reachable, return the 
     * target location. The pathing request there will get the entity maximally close */
    if(min_dist == INFINITY) {
        return xz_target;
    }

    return nearest_pos;
}

uint16_t N_ClosestPathableLocalIsland(const struct nav_private *priv, const struct nav_chunk *chunk, 
                                      struct tile_desc target)
{
    if(chunk->local_islands[target.tile_r][target.tile_c] != ISLAND_NONE)
        return chunk->local_islands[target.tile_r][target.tile_c];

    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    queue_td_t frontier;
    queue_td_init(&frontier, 1024);
    queue_td_push(&frontier, &target);

    STALLOC(bool, visited, res.tile_h * res.tile_w);
    memset(visited, 0, res.tile_h * res.tile_w);
    visited[target.tile_r * res.tile_h + target.tile_c] = true;

    uint16_t ret = ISLAND_NONE;
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

            if(neighb.chunk_r != target.chunk_r || neighb.chunk_c != target.chunk_c)
                continue;

            if(visited[neighb.tile_r * res.tile_h + neighb.tile_c])
                continue;

            uint16_t curr = chunk->local_islands[neighb.tile_r][neighb.tile_c];
            if(curr != ISLAND_NONE) {
                ret = curr;
                goto done;
            }

            visited[neighb.tile_r * res.tile_h + neighb.tile_c] = true;
            queue_td_push(&frontier, &neighb);
        }
    }

done:
    STFREE(visited);
    queue_td_destroy(&frontier);
    return ret; 
}

size_t N_DeepCopySize(void *nav_private)
{
    struct nav_private *priv = (struct nav_private*)nav_private;
    size_t ret = sizeof(struct nav_private);

    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        ret += (priv->width * priv->height * sizeof(struct nav_chunk));
    }
    return ret;
}

void N_CopyCostsAndBlockers(void *nav_private, void *out)
{
    struct nav_private *from = (struct nav_private*)nav_private;
    struct nav_private *to = (struct nav_private*)out;

    *to = *from;
    unsigned char *cursor = (unsigned char*)(to + 1);
    size_t chunks_per_layer = from->width * from->height;
    size_t layer_size = chunks_per_layer * sizeof(struct nav_chunk);

    for(int i = 0; i < NAV_LAYER_MAX; i++) {

        size_t cost_size = sizeof(((struct nav_chunk*)0)->cost_base);
        size_t blockers_size = sizeof(((struct nav_chunk*)0)->blockers);
        to->chunks[i] = (struct nav_chunk*)cursor;

        for(int j = 0; j < chunks_per_layer; j++) {
            memcpy(to->chunks[i][j].cost_base, from->chunks[i][j].cost_base, cost_size);
            memcpy(to->chunks[i][j].blockers, from->chunks[i][j].blockers, blockers_size);
        }
        cursor += layer_size;
    }
}

