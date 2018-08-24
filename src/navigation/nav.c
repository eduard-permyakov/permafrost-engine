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

#include "public/nav.h"
#include "nav_private.h"
#include "a_star.h"
#include "field.h"
#include "fieldcache.h"
#include "../map/public/tile.h"
#include "../render/public/render.h"
#include "../pf_math.h"
#include "../collision.h"
#include "../entity.h"

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>


#define IDX(r, width, c)   ((r) * (width) + (c))
#define CURSOR_OFF(cursor, base) ((ptrdiff_t)((cursor) - (base)))
#define ARR_SIZE(a)              (sizeof(a)/sizeof(a[0]))

#define MIN(a, b)                ((a) < (b) ? (a) : (b))
#define MAX(a, b)                ((a) > (b) ? (a) : (b))

#define EPSILON                  (1.0f / 1024)
#define MAX_TILES_PER_LINE       (128)

struct row_desc{
    int chunk_r;
    int tile_r;
};

struct col_desc{
    int chunk_c;
    int tile_c;
};

#define HIGHER(tile_desc, row_desc) \
       ((tile_desc).chunk_r < (row_desc).chunk_r) \
    || ((tile_desc).chunk_r == (row_desc).chunk_r && (tile_desc).tile_r < (row_desc).tile_r)

#define LOWER(tile_desc, row_desc) \
       ((tile_desc).chunk_r > (row_desc).chunk_r) \
    || ((tile_desc).chunk_r == (row_desc).chunk_r && (tile_desc).tile_r > (row_desc).tile_r)

#define MORE_LEFT(tile_desc, col_desc) \
       ((tile_desc).chunk_c < (col_desc).chunk_c) \
    || ((tile_desc).chunk_c == (col_desc).chunk_c && (tile_desc).tile_c < (col_desc).tile_c)

#define MORE_RIGHT(tile_desc, col_desc) \
       ((tile_desc).chunk_c > (col_desc).chunk_c) \
    || ((tile_desc).chunk_c == (col_desc).chunk_c && (tile_desc).tile_c > (col_desc).tile_c)

#define MIN_ROW(a, b)          HIGHER((a), (b)) ? (a) : (b)
#define MIN_ROW_4(a, b, c, d)  MIN_ROW(MIN_ROW((a), (b)), MIN_ROW((c), (d)))

#define MAX_ROW(a, b)          LOWER((a), (b)) ? (a) : (b)
#define MAX_ROW_4(a, b, c, d)  MAX_ROW(MAX_ROW((a), (b)), MAX_ROW((c), (d)))

#define MIN_COL(a, b)          MORE_LEFT((a), (b)) ? (a) : (b)
#define MIN_COL_4(a, b, c, d)  MIN_COL(MIN_COL((a), (b)), MIN_COL((c), (d)))

#define MAX_COL(a, b)          MORE_RIGHT((a), (b)) ? (a) : (b)
#define MAX_COL_4(a, b, c, d)  MAX_COL(MAX_COL((a), (b)), MAX_COL((c), (d)))

enum edge_type{
    EDGE_BOT   = (1 << 0),
    EDGE_LEFT  = (1 << 1),
    EDGE_RIGHT = (1 << 2),
    EDGE_TOP   = (1 << 3),
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool n_tile_pathable(const struct tile *tile)
{
    if(!tile->pathable)
        return false;

    if(!tile->type == TILETYPE_FLAT && tile->ramp_height > 1)
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

    const int (*tile_path_map)[2];

    switch(tile->type) {
    case TILETYPE_FLAT:
    case TILETYPE_RAMP_SN:
    case TILETYPE_RAMP_NS:
    case TILETYPE_RAMP_EW:
    case TILETYPE_RAMP_WE:
        tile_path_map = (int[2][2]){
            {0,0}, 
            {0,0}
        };  break;
    case TILETYPE_CORNER_CONCAVE_SW:
    case TILETYPE_CORNER_CONVEX_NE:
        tile_path_map = (int[2][2]){
            {0,0}, 
            {1,0}
        };  break;
    case TILETYPE_CORNER_CONCAVE_SE:
    case TILETYPE_CORNER_CONVEX_NW:
        tile_path_map = (int[2][2]){
            {0,0}, 
            {0,1}
        };  break;
    case TILETYPE_CORNER_CONCAVE_NW:
    case TILETYPE_CORNER_CONVEX_SE:
        tile_path_map = (int[2][2]){
            {1,0}, 
            {0,0}
        };  break;
    case TILETYPE_CORNER_CONCAVE_NE:
    case TILETYPE_CORNER_CONVEX_SW:
        tile_path_map = (int[2][2]){
            {0,1}, 
            {0,0}
        };  break;
    default: assert(0);
    }

    size_t r_base = tile_r * 2;
    size_t c_base = tile_c * 2;

    for(int r = 0; r < 2; r++) {
        for(int c = 0; c < 2; c++) {

            chunk->cost_base[r_base + r][c_base + c] = n_tile_pathable(tile) ? 1 
                                                     : tile_path_map[r][c]   ? 1
                                                     : COST_IMPASSABLE;
        }
    }
}

static void n_set_cost_edge(struct nav_chunk *chunk,
                            size_t chunk_w, size_t chunk_h,
                            size_t tile_r,  size_t tile_c,
                            enum edge_type edge)
{
    assert(FIELD_RES_R / chunk_h == 2);
    assert(FIELD_RES_C / chunk_w == 2);

    const int (*tile_path_map)[2];

    switch(edge){
    case EDGE_BOT:
        tile_path_map = (int[2][2]){
            {1,1}, 
            {0,0}
        };  break;
    case EDGE_TOP:
        tile_path_map = (int[2][2]){
            {0,0}, 
            {1,1}
        };  break;
    case EDGE_LEFT:
        tile_path_map = (int[2][2]){
            {0,1}, 
            {0,1}
        };  break;
    case EDGE_RIGHT:
        tile_path_map = (int[2][2]){
            {1,0}, 
            {1,0}
        };  break;
    }

    size_t r_base = tile_r * 2;
    size_t c_base = tile_c * 2;

    for(int r = 0; r < 2; r++) {
        for(int c = 0; c < 2; c++) {

            if(!tile_path_map[r][c])
                chunk->cost_base[r_base + r][c_base + c] = COST_IMPASSABLE;
        }
    }
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
                }
            }

        }
    }
}

static void n_link_chunks(struct nav_chunk *a, enum edge_type a_type, struct coord a_coord,
                          struct nav_chunk *b, enum edge_type b_type, struct coord b_coord)
{
    assert((a_type | b_type == EDGE_BOT | EDGE_TOP) || (a_type | b_type == EDGE_LEFT | EDGE_RIGHT));
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
                .chunk          = a_coord,
                .endpoints[0]   = (a_type & (EDGE_TOP | EDGE_BOT))    ? (struct coord){a_fixed_idx, i}
                                : (a_type & (EDGE_LEFT | EDGE_RIGHT)) ? (struct coord){i, a_fixed_idx}
                                : (assert(0), (struct coord){0}),
                .num_neighbours = 0,
                .connected      = &b->portals[b->num_portals]
            };
            b->portals[b->num_portals] = (struct portal) {
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

    assert(n_links == (priv->width)*(priv->width-1) + (priv->height)*(priv->height-1));
}

static void n_link_chunk_portals(struct nav_chunk *chunk)
{
    coord_vec_t path;
    kv_init(path);

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
            bool has_path = AStar_GridPath(a, b, chunk->cost_base, &path, &cost);
            if(has_path) {
                port->edges[port->num_neighbours] = (struct edge){link_candidate, cost};
                port->num_neighbours++;    
            }
        }
    }

    kv_destroy(path);
}

static void n_render_grid_path(struct nav_chunk *chunk, mat4x4_t *chunk_model,
                               const struct map *map, coord_vec_t *path)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    vec2_t corners_buff[4 * kv_size(*path)];
    vec3_t colors_buff[kv_size(*path)];

    vec2_t *corners_base = corners_buff;
    vec3_t *colors_base = colors_buff; 

    for(int i = 0, r = kv_A(*path, i).r, c = kv_A(*path, i).c; 
        i < kv_size(*path); 
        i++, r = kv_A(*path, i).r, c = kv_A(*path, i).c) {

        /* Subtract EPSILON to make sure every coordinate is strictly within the map bounds */
        float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim - EPSILON;
        float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim - EPSILON;
        float square_x = -(((float)c) / FIELD_RES_C) * chunk_x_dim;
        float square_z =  (((float)r) / FIELD_RES_R) * chunk_z_dim;

        *corners_base++ = (vec2_t){square_x, square_z};
        *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

        *colors_base++ = (vec3_t){0.0f, 0.0f, 1.0f};
    }

    assert(colors_base == colors_buff + ARR_SIZE(colors_buff));
    assert(corners_base == corners_buff + ARR_SIZE(corners_buff));
    R_GL_DrawMapOverlayQuads(corners_buff, colors_buff, kv_size(*path), chunk_model, map);
}

static void n_render_portals(const struct nav_chunk *chunk, mat4x4_t *chunk_model,
                             const struct map *map)
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

                /* Subtract EPSILON to make sure every coordinate is strictly within the map bounds */
                float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim - EPSILON;
                float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim - EPSILON;
                float square_x = -(((float)c) / FIELD_RES_C) * chunk_x_dim;
                float square_z =  (((float)r) / FIELD_RES_R) * chunk_z_dim;

                *corners_base++ = (vec2_t){square_x, square_z};
                *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
                *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
                *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

                *colors_base++ = (vec3_t){1.0f, 1.0f, 0.0f};
                num_tiles++;
            }
        }
    }

    R_GL_DrawMapOverlayQuads(corners_buff, colors_buff, num_tiles, chunk_model, map);
}

static dest_id_t n_dest_id(struct tile_desc dst_desc)
{
    return (((uint32_t)dst_desc.chunk_r & 0xf) << 24)
         | (((uint32_t)dst_desc.chunk_c & 0xf) << 16)
         | (((uint32_t)dst_desc.tile_r  & 0xf) <<  8)
         | (((uint32_t)dst_desc.tile_c  & 0xf) <<  0);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool N_Init(void)
{
    if(!N_FC_Init())
        return false;

    return true;
}

void N_Shutdown(void)
{
    N_FC_Shutdown();
}

void *N_BuildForMapData(size_t w, size_t h, 
                        size_t chunk_w, size_t chunk_h,
                        const struct tile **chunk_tiles)
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
        for(int chunk_c = 0; chunk_c < ret->width; chunk_c++){

            struct nav_chunk *curr_chunk = &ret->chunks[IDX(chunk_r, ret->width, chunk_c)];
            const struct tile *curr_tiles = chunk_tiles[IDX(chunk_r, ret->width, chunk_c)];
            curr_chunk->num_portals = 0;

            for(int tile_r = 0; tile_r < chunk_h; tile_r++) {
                for(int tile_c = 0; tile_c < chunk_w; tile_c++) {

                    const struct tile *curr_tile = &curr_tiles[tile_r * chunk_w + tile_c];
                    n_set_cost_for_tile(curr_chunk, chunk_w, chunk_h, tile_r, tile_c, curr_tile);
                }
            }
        }
    }

    n_make_cliff_edges(ret, chunk_tiles, chunk_w, chunk_h);
    N_UpdatePortals(ret);
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
    n_render_portals(chunk, chunk_model, map);

    vec2_t *corners_base = corners_buff;
    vec3_t *colors_base = colors_buff; 

    for(int r = 0; r < FIELD_RES_R; r++) {
        for(int c = 0; c < FIELD_RES_C; c++) {

            /* Subtract EPSILON to make sure every coordinate is strictly within the map bounds */
            float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim - EPSILON;
            float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim - EPSILON;
            float square_x = -(((float)c) / FIELD_RES_C) * chunk_x_dim;
            float square_z =  (((float)r) / FIELD_RES_R) * chunk_z_dim;

            *corners_base++ = (vec2_t){square_x, square_z};
            *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
            *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
            *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

            *colors_base++ = chunk->cost_base[r][c] == COST_IMPASSABLE ? (vec3_t){1.0f, 0.0f, 0.0f}
                                                                       : (vec3_t){0.0f, 1.0f, 0.0f};
        }
    }

    assert(colors_base == colors_buff + ARR_SIZE(colors_buff));
    assert(corners_base == corners_buff + ARR_SIZE(corners_buff));
    R_GL_DrawMapOverlayQuads(corners_buff, colors_buff, FIELD_RES_R * FIELD_RES_C, chunk_model, map);
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
    if(!N_FC_ContainsFlowField(id, (struct coord){chunk_r, chunk_c}, &field_id))
        return;
    const struct flow_field *ff = N_FC_FlowFieldAt(id, (struct coord){chunk_r, chunk_c});

    for(int r = 0; r < FIELD_RES_R; r++) {
        for(int c = 0; c < FIELD_RES_C; c++) {

            /* Subtract EPSILON to make sure every coordinate is strictly within the map bounds */
            float square_x_len = (1.0f / FIELD_RES_C) * chunk_x_dim - EPSILON;
            float square_z_len = (1.0f / FIELD_RES_R) * chunk_z_dim - EPSILON;
            float square_x = -(((float)c) / FIELD_RES_C) * chunk_x_dim;
            float square_z =  (((float)r) / FIELD_RES_R) * chunk_z_dim;

            positions_buff[r * FIELD_RES_C + c] = (vec2_t){
                square_x - square_x_len / 2.0f,
                square_z + square_z_len / 2.0f
            };
            dirs_buff[r * FIELD_RES_C + c] = g_flow_dir_lookup[ff->field[r][c].dir_idx];
        }
    }

    /* we bout to draw it team */
    R_GL_DrawFlowField(positions_buff, dirs_buff, FIELD_RES_R * FIELD_RES_C, chunk_model, map);
}

void N_CutoutStaticObject(void *nav_private, vec3_t map_pos, const struct obb *obb)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    /* Corners ordered to make a loop */
    vec3_t bot_corners[4] = {obb->corners[0], obb->corners[1], obb->corners[5], obb->corners[4]};
    vec2_t bot_corners_2d[4] = {
        (vec2_t){bot_corners[0].x, bot_corners[0].z},
        (vec2_t){bot_corners[1].x, bot_corners[1].z},
        (vec2_t){bot_corners[2].x, bot_corners[2].z},
        (vec2_t){bot_corners[3].x, bot_corners[3].z},
    };
    struct line_seg_2d xz_line_segs[4] = {
        (struct line_seg_2d){bot_corners[0].x, bot_corners[0].z, bot_corners[1].x, bot_corners[1].z},
        (struct line_seg_2d){bot_corners[1].x, bot_corners[1].z, bot_corners[2].x, bot_corners[2].z},
        (struct line_seg_2d){bot_corners[2].x, bot_corners[2].z, bot_corners[3].x, bot_corners[3].z},
        (struct line_seg_2d){bot_corners[3].x, bot_corners[3].z, bot_corners[0].x, bot_corners[0].z},
    };

    struct row_desc min_rows[4], max_rows[4];
    struct col_desc min_cols[4], max_cols[4];

    /* For each line segment of the bottom face of the OBB, find the supercover (set of all tiles which
     * intersect the line segment) and update their cost. Keep track of the top-most, bottom-most,
     * left-most and right-most tiles of the outline.
     */
    struct tile_desc descs[MAX_TILES_PER_LINE];
    for(int i = 0; i < ARR_SIZE(xz_line_segs); i++) {

        min_rows[i] = (struct row_desc){0,0};
        max_rows[i] = (struct row_desc){priv->height-1, FIELD_RES_R-1};
        min_cols[i] = (struct col_desc){0,0};
        max_cols[i] = (struct col_desc){priv->width-1, FIELD_RES_C-1};

        size_t num_tiles = M_Tile_LineSupercoverTilesSorted(res, map_pos, xz_line_segs[i], descs);
        for(int j = 0; j < num_tiles; j++) {

            priv->chunks[IDX(descs[j].chunk_r, priv->width, descs[j].chunk_c)]
                .cost_base[descs[j].tile_r][descs[j].tile_c] = COST_IMPASSABLE;

            if(HIGHER(descs[j], min_rows[i]))
                min_rows[i] = (struct row_desc){descs[j].chunk_r, descs[j].tile_r};

            if(LOWER(descs[j], max_rows[i]))
                max_rows[i] = (struct row_desc){descs[j].chunk_r, descs[j].tile_r};

            if(MORE_LEFT(descs[j], max_cols[i]))
                min_cols[i] = (struct col_desc){descs[j].chunk_c, descs[j].tile_c};

            if(MORE_RIGHT(descs[j], max_cols[i]))
                max_cols[i] = (struct col_desc){descs[j].chunk_c, descs[j].tile_c};
        }
    }

    struct row_desc min_row = MIN_ROW_4(min_rows[0], min_rows[1], min_rows[2], min_rows[3]);
    struct row_desc max_row = MAX_ROW_4(max_rows[0], max_rows[1], max_rows[2], max_rows[3]);

    struct col_desc min_col = MIN_COL_4(min_cols[0], min_cols[1], min_cols[2], min_cols[3]);
    struct col_desc max_col = MAX_COL_4(max_cols[0], max_cols[1], max_cols[2], max_cols[3]);

    /* Now iterate over the square region of tiles defined by the extrema of the outline 
     * and check whether the tiles of this box fall within the OBB and should have their
     * cost updated. 
     */
    for(int r = min_row.chunk_r * FIELD_RES_R + min_row.tile_r; 
            r < max_row.chunk_r * FIELD_RES_R + max_row.tile_r; r++) {
        for(int c = min_col.chunk_c * FIELD_RES_C + min_col.tile_c; 
                c < max_col.chunk_c * FIELD_RES_C + max_col.tile_c; c++) {

            struct tile_desc desc = {
                .chunk_r = r / FIELD_RES_R,
                .chunk_c = c / FIELD_RES_C,
                .tile_r  = r % FIELD_RES_R,
                .tile_c  = c % FIELD_RES_C,
            };
            struct box bounds = M_Tile_Bounds(res, map_pos, desc);
            vec2_t center = (vec2_t){
                bounds.x - bounds.width/2.0f,
                bounds.z + bounds.height/2.0f
            };

            if(C_PointInsideRect2D(center, bot_corners_2d[0], bot_corners_2d[1], 
                                               bot_corners_2d[2], bot_corners_2d[3])) {
                priv->chunks[IDX(desc.chunk_r, priv->width, desc.chunk_c)]
                    .cost_base[desc.tile_r][desc.tile_c] = COST_IMPASSABLE;
            }
        }
    }
}

void N_UpdatePortals(void *nav_private)
{
    struct nav_private *priv = nav_private;

    for(int chunk_r = 0; chunk_r < priv->height; chunk_r++){
        for(int chunk_c = 0; chunk_c < priv->width; chunk_c++){
            
            struct nav_chunk *curr_chunk = &priv->chunks[IDX(chunk_r, priv->width, chunk_c)];
            curr_chunk->num_portals = 0;
        }
    }
    
    n_create_portals(priv);

    for(int chunk_r = 0; chunk_r < priv->height; chunk_r++){
        for(int chunk_c = 0; chunk_c < priv->width; chunk_c++){
            
            struct nav_chunk *curr_chunk = &priv->chunks[IDX(chunk_r, priv->width, chunk_c)];
            n_link_chunk_portals(curr_chunk);
        }
    }
}

bool N_RequestPath(void *nav_private, vec2_t xz_src, vec2_t xz_dest, 
                   vec3_t map_pos, dest_id_t *out_dest_id)
{
    struct nav_private *priv = nav_private;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    /* Convert source and destination positions to tile coordinates */
    bool result;
    struct tile_desc src_desc, dst_desc;
    result = M_Tile_DescForPoint2D(res, map_pos, xz_src, &src_desc);
    assert(result);
    result = M_Tile_DescForPoint2D(res, map_pos, xz_dest, &dst_desc);
    assert(result);

    dest_id_t ret = n_dest_id(dst_desc);

    /* Generate the flow field for the destination chunk, if necessary */
    ff_id_t id;
    if(!N_FC_ContainsFlowField(ret, (struct coord){dst_desc.chunk_r, dst_desc.chunk_c}, &id)){

        struct field_target target = (struct field_target){
            .type = TARGET_TILE,
            .tile = (struct coord){dst_desc.tile_r, dst_desc.tile_c}
        };

        const struct nav_chunk *chunk = &priv->chunks[IDX(dst_desc.chunk_r, priv->width, dst_desc.chunk_c)];
        struct flow_field ff;
        id = N_FlowField_ID((struct coord){dst_desc.chunk_r, dst_desc.chunk_c}, target);

        N_FlowFieldInit((struct coord){dst_desc.chunk_r, dst_desc.chunk_c}, &ff);
        N_FlowFieldUpdate(chunk, target, &ff);
        N_FC_SetFlowField(ret, (struct coord){dst_desc.chunk_r, dst_desc.chunk_c}, id, &ff);
    }

    /* Source and destination positions are in the same chunk, and a path exists
     * between them. In this case, we only need a single flow field. .*/
    if(src_desc.chunk_r == dst_desc.chunk_r && src_desc.chunk_c == dst_desc.chunk_c
    && AStar_TilesLinked((struct coord){src_desc.tile_r, src_desc.tile_c}, 
                         (struct coord){dst_desc.tile_r, dst_desc.tile_c}, 
                         priv->chunks[IDX(src_desc.chunk_r, priv->width, src_desc.chunk_c)].cost_base)) {

        *out_dest_id = ret;
        return true;
    }

    const struct portal *src_port, *dst_port;
    src_port = AStar_NearestPortal((struct coord){src_desc.tile_r, src_desc.tile_c}, 
        &priv->chunks[IDX(src_desc.chunk_r, priv->width, src_desc.chunk_c)]);
    dst_port = AStar_NearestPortal((struct coord){dst_desc.tile_r, dst_desc.tile_c}, 
        &priv->chunks[IDX(dst_desc.chunk_r, priv->width, dst_desc.chunk_c)]);

    if(!src_port || !dst_port) {
        return false; 
    }

    float cost;
    portal_vec_t path;
    kv_init(path);

    bool path_exists = AStar_PortalGraphPath(src_port, dst_port, priv, &path, &cost);
    if(!path_exists) {

        kv_destroy(path);
        return false; 
    }

    /* Traverse the portal path and generate the required fields, if they are not already 
     * cached. Add the results to the fieldcache. */
    for(int i = 0; i < kv_size(path)-1; i++) {

        const struct portal *curr_node = kv_A(path, i);
        const struct portal *next_hop = kv_A(path, i + 1);

        /* If the very first hop takes us into another chunk, that means that the 'nearest portal'
         * to the source borders the 'next' chunk already. In this case, we must remember to
         * still generate a flow field for the current chunk steering to this portal. */
        if(i == 0 && (next_hop->chunk.r != src_desc.chunk_r || next_hop->chunk.c != src_desc.chunk_c))
            next_hop = src_port;

        if(curr_node->connected == next_hop)
            continue;

        /* Since we are moving from 'closest portal' to 'closest portal', it 
         * may be possible that the very last hop takes us from another portal in the 
         * destination chunk to the destination portal. This is not needed and will
         * overwrite the destination flow field made earlier. */
        if(curr_node->chunk.r == dst_desc.chunk_r 
        && curr_node->chunk.c == dst_desc.chunk_c
        && next_hop == dst_port)
            break;

        struct coord chunk_coord = curr_node->chunk;
        struct field_target target = (struct field_target){
            .type = TARGET_PORTAL,
            .port = next_hop
        };

        const struct nav_chunk *chunk = &priv->chunks[IDX(chunk_coord.r, priv->width, chunk_coord.c)];
        ff_id_t new_id = N_FlowField_ID(chunk_coord, target);
        ff_id_t exist_id;
        struct flow_field ff;

        if(N_FC_ContainsFlowField(ret, chunk_coord, &exist_id)) {

            /* The exact flow field we need has already been made */
            if(new_id == exist_id)
                continue;

            /* This is the edge case when a path to a particular target takes us through
             * the same chunk more than once. This can happen if a chunk is divided into
             * 'islands' by unpathable barriers. */
            const struct flow_field *exist_ff  = N_FC_FlowFieldAt(ret, chunk_coord);
            memcpy(&ff, exist_ff, sizeof(struct flow_field));

            N_FlowFieldUpdate(chunk, target, &ff);
            /* We set the updated flow field for the new (least recently used) key. Since in 
             * this case more than one flowfield ID maps to the same field but we only keep 
             * one of the IDs, it may be possible that the same flowfield will be redundantly 
             * updated at a later time. However, this is largely inconsequential. */
            N_FC_SetFlowField(ret, chunk_coord, new_id, &ff);
            continue;
        }

        N_FlowFieldInit(chunk_coord, &ff);
        N_FlowFieldUpdate(chunk, target, &ff);
        N_FC_SetFlowField(ret, chunk_coord, new_id, &ff);
    }
    kv_destroy(path);

    *out_dest_id = ret; 
    return true;
}

