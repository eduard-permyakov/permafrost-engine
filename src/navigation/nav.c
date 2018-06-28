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
#include "../map/public/tile.h"
#include "../render/public/render.h"

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>


#define CHUNK_IDX(r, width, c)   ((r) * (width) + (c))
#define CURSOR_OFF(cursor, base) ((ptrdiff_t)((cursor) - (base)))
#define EPSILON                  (1.0f/10000.0f)
#define ARR_SIZE(a)              (sizeof(a)/sizeof(a[0]))

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
    size_t field_per_map_r = FIELD_RES_R / chunk_h;
    size_t field_per_map_c = FIELD_RES_C / chunk_w;

    size_t r_base = tile_r * field_per_map_r;
    size_t c_base = tile_c * field_per_map_c;

    uint8_t cost = n_tile_pathable(tile) ? 0 : COST_IMPASSABLE;

    for(int r = 0; r < field_per_map_r; r++) {
        for(int c = 0; c < field_per_map_c; c++) {

            chunk->cost_base[r_base + r][c_base + c] = cost;
        }
    }
}

static void n_link_chunks(struct nav_chunk *a, enum edge_type a_type, struct coord a_coord,
                          struct nav_chunk *b, enum edge_type b_type, struct coord b_coord)
{
    assert((a_type | b_type == EDGE_BOT | EDGE_TOP) || (a_type | b_type == EDGE_LEFT | EDGE_RIGHT));
    size_t stride = (a_type & (EDGE_BOT | EDGE_TOP)) ? 1 : FIELD_RES_C;
    size_t line_len = (a_type & (EDGE_BOT | EDGE_TOP)) ? FIELD_RES_C : FIELD_RES_R;

    uint8_t *a_cursor = &a->cost_base[a_type == EDGE_BOT ? FIELD_RES_R-1 : 0][a_type == EDGE_RIGHT ? FIELD_RES_C-1 : 0];
    uint8_t *b_cursor = &b->cost_base[b_type == EDGE_BOT ? FIELD_RES_R-1 : 0][b_type == EDGE_RIGHT ? FIELD_RES_C-1 : 0];

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

        assert(CURSOR_OFF(a_cursor, &a->cost_base[0][0]) >= 0 && CURSOR_OFF(a_cursor, &a->cost_base[0][0]) < (FIELD_RES_R*FIELD_RES_C));
        assert(CURSOR_OFF(b_cursor, &b->cost_base[0][0]) >= 0 && CURSOR_OFF(a_cursor, &b->cost_base[0][0]) < (FIELD_RES_R*FIELD_RES_C));

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
        }else if(!can_cross && in_portal) {

            in_portal = false;
            a->portals[a->num_portals].endpoints[1] 
                = (a_type & (EDGE_TOP | EDGE_BOT))    ? (struct coord){a_fixed_idx, i-1}
                : (a_type & (EDGE_LEFT | EDGE_RIGHT)) ? (struct coord){i-1, a_fixed_idx}
                : (assert(0), (struct coord){0});
            b->portals[b->num_portals].endpoints[1] 
                = (b_type & (EDGE_TOP | EDGE_BOT))    ? (struct coord){b_fixed_idx, i-1}
                : (b_type & (EDGE_LEFT | EDGE_RIGHT)) ? (struct coord){i-1, b_fixed_idx}
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
            
            struct nav_chunk *curr = &priv->chunks[CHUNK_IDX(r, priv->width, c)];
            struct nav_chunk *bot = (r < priv->height-1) ? &priv->chunks[CHUNK_IDX(r+1, priv->width, c)] : NULL;
            struct nav_chunk *right = (c < priv->width-1) ? &priv->chunks[CHUNK_IDX(r, priv->width, c+1)] : NULL;

            if(bot)   n_link_chunks(curr, EDGE_BOT, (struct coord){r, c}, bot, EDGE_TOP, (struct coord){r+1, c});
            if(right) n_link_chunks(curr, EDGE_RIGHT, (struct coord){r, c}, right, EDGE_LEFT, (struct coord){r, c+1});
    
            n_links += (!!bot + !!right);
        }
    }

    assert(n_links == (priv->width)*(priv->width-1) + (priv->height)*(priv->height-1));
}

void n_render_grid_path(void *nav_private, mat4x4_t *chunk_model,
                        const struct map *map, coord_vec_t *path,
                        int chunk_r, int chunk_c, 
                        int chunk_x_dim, int chunk_z_dim)
{
    const struct nav_private *priv = nav_private;
    assert(chunk_r < priv->height);
    assert(chunk_c < priv->width);

    vec2_t corners_buff[4 * kv_size(*path)];
    vec3_t colors_buff[kv_size(*path)];

    const struct nav_chunk *chunk = &priv->chunks[CHUNK_IDX(chunk_r, priv->width, chunk_c)];
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

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void *N_BuildForMapData(size_t w, size_t h, 
                        size_t chunk_w, size_t chunk_h,
                        struct tile **chunk_tiles)
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

    /* First build the base cost field based on terrain, reset portals */
    for(int chunk_r = 0; chunk_r < ret->height; chunk_r++){
        for(int chunk_c = 0; chunk_c < ret->width; chunk_c++){

            struct nav_chunk *curr_chunk = &ret->chunks[CHUNK_IDX(chunk_r, ret->width, chunk_c)];
            struct tile *curr_tiles = chunk_tiles[CHUNK_IDX(chunk_r, ret->width, chunk_c)];
            curr_chunk->num_portals = 0;

            for(int tile_r = 0; tile_r < chunk_h; tile_r++) {
                for(int tile_c = 0; tile_c < chunk_w; tile_c++) {

                    const struct tile *curr_tile = &curr_tiles[tile_r * chunk_w + tile_c];
                    n_set_cost_for_tile(curr_chunk, chunk_w, chunk_h, tile_r, tile_c, curr_tile);
                }
            }
        }
    }

    /* Next define the portals between chunks */
    n_create_portals(ret);
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
                           int chunk_r, int chunk_c, 
                           int chunk_x_dim, int chunk_z_dim)
{
    const struct nav_private *priv = nav_private;
    assert(chunk_r < priv->height);
    assert(chunk_c < priv->width);

    vec2_t corners_buff[4 * FIELD_RES_R * FIELD_RES_C];
    vec3_t colors_buff[FIELD_RES_R * FIELD_RES_C];

    const struct nav_chunk *chunk = &priv->chunks[CHUNK_IDX(chunk_r, priv->width, chunk_c)];
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

