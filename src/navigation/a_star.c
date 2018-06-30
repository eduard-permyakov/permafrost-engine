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

#include "a_star.h"
#include "../lib/public/pqueue.h"
#include "../lib/public/khash.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

PQUEUE_TYPE(coord, struct coord)
PQUEUE_IMPL(static, coord, struct coord)

KHASH_MAP_INIT_INT64(coord_coord, struct coord)
KHASH_MAP_INIT_INT64(coord_float, float)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define kh_put_val(name, table, key, val)               \
    do{                                                 \
        int ret;                                        \
        khiter_t k = kh_put(name, table, key, &ret);    \
        assert(ret != -1);                              \
        kh_value(table, k) = val;                       \
    }while(0)

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static uint64_t coord_to_key(struct coord c)
{
    return (((uint64_t)c.r) << 32 | ((uint64_t)c.c) & ~((uint32_t)0));
}

static int coord_get_neighbours(const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C], struct coord coord, 
                                struct coord *out_neighbours, float *out_costs)
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
        }
    }
    assert(ret < 9);
    return ret;
}

static float heuristic(struct coord a, struct coord b)
{
    /* Octile Distance:
     * Compute the number of steps you can take if you can't take
     * a diagonal, then subtract the steps you save by using the 
     * diagonal. Uses cost 'D' for orthogonal traversal of one tile.*/
    const float D = 1.0f;
    const float D2 = sqrt(2) * D;

    int dx = abs(a.r - b.r);
    int dy = abs(a.c - b.c);

    return D * (dx + dy) + (D2 - 2 * D) * MIN(dx, dy);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool AStar_GridPath(struct coord start, struct coord finish, 
                    const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C], 
                    coord_vec_t *out_path, float *out_cost)
{
    pq_coord_t            frontier;
    khash_t(coord_coord) *came_from;
    khash_t(coord_float) *running_cost;
    
    pq_coord_init(&frontier);
    if(NULL == (came_from = kh_init(coord_coord)))
        goto fail_came_from;
    if(NULL == (running_cost = kh_init(coord_float)))
        goto fail_running_cost;

    kh_put_val(coord_float, running_cost, coord_to_key(start), 0.0f);
    pq_coord_push(&frontier, 0.0f, start);

    while(pq_size(&frontier) > 0) {

        struct coord curr;
        pq_coord_pop(&frontier, &curr);

        if(0 == memcmp(&curr, &finish, sizeof(struct coord)))
            break;

        struct coord neighbours[8];
        float neighbour_costs[8];
        int num_neighbours = coord_get_neighbours(cost_field, curr, neighbours, neighbour_costs);

        for(int i = 0; i < num_neighbours; i++) {

            struct coord *next = &neighbours[i];
            khiter_t k = kh_get(coord_float, running_cost, coord_to_key(curr));
            assert(k != kh_end(running_cost));
            float new_cost = kh_value(running_cost, k) + neighbour_costs[i];

            if((k = kh_get(coord_float, running_cost, coord_to_key(*next))) == kh_end(running_cost)
            || new_cost < kh_value(running_cost, k)) {

                kh_put_val(coord_float, running_cost, coord_to_key(*next), new_cost);
                float priority = new_cost + heuristic(finish, *next);
                pq_coord_push(&frontier, priority, *next);
                kh_put_val(coord_coord, came_from, coord_to_key(*next), curr);
            }
        }
    }
    
    if(kh_get(coord_coord, came_from, coord_to_key(finish)) == kh_end(came_from))
        goto fail_find_path;

    kv_reset(*out_path);

    /* We have our path at this point. Walk backwards along the path to build a 
     * vector of the nodes along the path. */
    struct coord curr = finish;
    while(0 != memcmp(&curr, &start, sizeof(struct coord))) {

        kv_push(struct coord, *out_path, curr);
        khiter_t k = kh_get(coord_coord, came_from, coord_to_key(curr));
        assert(k != kh_end(came_from));
        curr = kh_value(came_from, k);
    }
    kv_push(struct coord, *out_path, start);

    /* Reverse the path vector */
    for(int i = 0, j = kv_size(*out_path) - 1; i < j; i++, j--) {
        struct coord tmp = kv_A(*out_path, i);
        kv_A(*out_path, i) = kv_A(*out_path, j);
        kv_A(*out_path, j) = tmp;
    }

    khiter_t k = kh_get(coord_float, running_cost, coord_to_key(finish));
    assert(k != kh_end(running_cost));
    *out_cost = kh_value(running_cost, k);

    pq_coord_destroy(&frontier);
    kh_destroy(coord_float, running_cost);
    kh_destroy(coord_coord, came_from);
    return true;

fail_find_path:
    pq_coord_destroy(&frontier);
    kh_destroy(coord_float, running_cost);
fail_running_cost:
    kh_destroy(coord_coord, came_from);
fail_came_from:
    return false;
}

