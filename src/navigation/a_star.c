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

PQUEUE_TYPE(coord, struct coord)
PQUEUE_IMPL(static, coord, struct coord)

KHASH_MAP_INIT_INT64(coord_coord, struct coord)
KHASH_MAP_INIT_INT64(coord_uint, float)

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

uint64_t coord_to_key(struct coord c)
{
    return (((uint64_t)c.r) << 32 | ((uint64_t)c.c) & ~((uint32_t)0));
}

int coord_get_neighbours(const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C],
                         struct coord coord, struct coord *out_neighbours)
{
    int ret = 0;
    for(int r = coord.r - 1; r <= coord.r + 1; r++) {
        for(int c = coord.c - 1; c <= coord.c + 1; c++) {

            if(r < 0 || r >= FIELD_RES_R)
                continue;
            if(c < 0 || c >= FIELD_RES_C)
                continue;
            if(cost_field[r][c] == COST_IMPASSABLE)
                continue;
            out_neighbours[ret++] = (struct coord){r, c};
        }
    }
    return ret;
}

float heuristic(struct coord a, struct coord b)
{
    /* diagonal distance */
    const int D = 1;
    const int D2 = 1;

    float dx = abs(a.r - b.r);
    float dy = abs(a.c - b.c);

    return D * (dx + dy) + (D2 - 2 * D) * MIN(dx, dy);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool AStar_GridPath(struct coord start, struct coord finish, 
                    const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C], 
                    coord_vec_t *out_path)
{
    pq_coord_t            frontier;
    khash_t(coord_coord) *came_from;
    khash_t(coord_uint)  *running_cost;
    
    pq_coord_init(&frontier);
    if(NULL == (came_from = kh_init(coord_coord)))
        goto fail_came_from;
    if(NULL == (running_cost = kh_init(coord_uint)))
        goto fail_running_cost;

    kh_put_val(coord_uint, running_cost, coord_to_key(start), 0);
    pq_coord_push(&frontier, 0, start);

    while(pq_size(&frontier) > 0) {

        struct coord curr;
        pq_coord_pop(&frontier, &curr);

        if(0 == memcmp(&curr, &finish, sizeof(struct coord)))
            break;

        struct coord neighbours[8];
        int num_neighbours = coord_get_neighbours(cost_field, curr, neighbours);
        for(struct coord *next = neighbours; next < neighbours + num_neighbours; next++) {
            
            khiter_t k = kh_get(coord_uint, running_cost, coord_to_key(curr));
            float new_cost = kh_value(running_cost, k) + cost_field[next->r][next->c];

            if((k = kh_get(coord_uint, running_cost, coord_to_key(*next))) == kh_end(running_cost)
            || new_cost < kh_value(running_cost, k)) {

                kh_put_val(coord_uint, running_cost, coord_to_key(*next), new_cost);
                int priority = new_cost + heuristic(finish, *next);
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

    pq_coord_destroy(&frontier);
    kh_destroy(coord_uint, running_cost);
    kh_destroy(coord_coord, came_from);
    return true;

fail_find_path:
    pq_coord_destroy(&frontier);
    kh_destroy(coord_uint, running_cost);
fail_running_cost:
    kh_destroy(coord_coord, came_from);
fail_came_from:
    return false;
}

