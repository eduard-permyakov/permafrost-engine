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

#include "a_star.h"
#include "nav_private.h"
#include "../lib/public/pqueue.h"
#include "../lib/public/khash.h"
#include "fieldcache.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

PQUEUE_TYPE(coord, struct coord)
PQUEUE_IMPL(static, coord, struct coord)

PQUEUE_TYPE(portal, const struct portal*)
PQUEUE_IMPL(static, portal, const struct portal*)

KHASH_MAP_INIT_INT64(key_coord, struct coord)
KHASH_MAP_INIT_INT64(key_portal, const struct portal*)
KHASH_MAP_INIT_INT64(key_float, float)

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

static uint64_t portal_to_key(const struct portal *p)
{
    return (((uint64_t)p->chunk.r & 0xffff)      << 48)
         | (((uint64_t)p->chunk.c & 0xffff)      << 32)
         | (((uint64_t)p->endpoints[0].r & 0xff) << 24)
         | (((uint64_t)p->endpoints[0].c & 0xff) << 16)
         | (((uint64_t)p->endpoints[1].r & 0xff) <<  8)
         | (((uint64_t)p->endpoints[1].c & 0xff) <<  0);
}

static int neighbours_grid(const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C], struct coord coord, 
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

static int neighbours_portal_graph(const struct portal *portal,
                                   const struct portal **out_neighbours, float *out_costs)
{
    int ret = 0;

    for(int i = 0; i < portal->num_neighbours; i++) {

        out_neighbours[ret] = portal->edges[i].neighbour;
        out_costs[ret] = portal->edges[i].cost;
        ret++;
    }

    out_neighbours[ret] = portal->connected;
    out_costs[ret] = 1;
    ret++;

    assert(ret <= MAX_PORTALS_PER_CHUNK);
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

bool AStar_GridPath(struct coord start, struct coord finish, struct coord chunk,
                    const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C], 
                    coord_vec_t *out_path, float *out_cost)
{
    struct grid_path_desc gp = {0};
    kv_init(gp.path);

    if(N_FC_GetGridPath(start, finish, chunk, &gp)) {

        if(!gp.exists)
            return false;

        *out_cost = gp.cost;
        kv_copy(struct coord, *out_path, gp.path);
        return true;
    }

    pq_coord_t          frontier;
    khash_t(key_coord) *came_from;
    khash_t(key_float) *running_cost;
    
    pq_coord_init(&frontier);
    if(NULL == (came_from = kh_init(key_coord)))
        goto fail_came_from;
    if(NULL == (running_cost = kh_init(key_float)))
        goto fail_running_cost;

    kh_put_val(key_float, running_cost, coord_to_key(start), 0.0f);
    pq_coord_push(&frontier, 0.0f, start);

    while(pq_size(&frontier) > 0) {

        struct coord curr;
        pq_coord_pop(&frontier, &curr);

        if(0 == memcmp(&curr, &finish, sizeof(struct coord)))
            break;

        struct coord neighbours[8];
        float neighbour_costs[8];
        int num_neighbours = neighbours_grid(cost_field, curr, neighbours, neighbour_costs);

        for(int i = 0; i < num_neighbours; i++) {

            struct coord *next = &neighbours[i];
            khiter_t k = kh_get(key_float, running_cost, coord_to_key(curr));
            assert(k != kh_end(running_cost));
            float new_cost = kh_value(running_cost, k) + neighbour_costs[i];

            if((k = kh_get(key_float, running_cost, coord_to_key(*next))) == kh_end(running_cost)
            || new_cost < kh_value(running_cost, k)) {

                kh_put_val(key_float, running_cost, coord_to_key(*next), new_cost);
                float priority = new_cost + heuristic(finish, *next);
                pq_coord_push(&frontier, priority, *next);
                kh_put_val(key_coord, came_from, coord_to_key(*next), curr);
            }
        }
    }
    
    if(kh_get(key_coord, came_from, coord_to_key(finish)) == kh_end(came_from))
        goto fail_find_path;

    kv_reset(*out_path);

    /* We have our path at this point. Walk backwards along the path to build a 
     * vector of the nodes along the path. */
    struct coord curr = finish;
    while(0 != memcmp(&curr, &start, sizeof(struct coord))) {

        kv_push(struct coord, *out_path, curr);
        khiter_t k = kh_get(key_coord, came_from, coord_to_key(curr));
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

    khiter_t k = kh_get(key_float, running_cost, coord_to_key(finish));
    assert(k != kh_end(running_cost));
    *out_cost = kh_value(running_cost, k);

    pq_coord_destroy(&frontier);
    kh_destroy(key_float, running_cost);
    kh_destroy(key_coord, came_from);

    /* Cache the result */
    gp.exists = true;
    kv_copy(struct coord, gp.path, *out_path);
    gp.cost = *out_cost;
    N_FC_PutGridPath(start, finish, chunk, &gp);
    return true;

fail_find_path:
    gp.exists = false;
    N_FC_PutGridPath(start, finish, chunk, &gp);

    pq_coord_destroy(&frontier);
    kh_destroy(key_float, running_cost);
fail_running_cost:
    kh_destroy(key_coord, came_from);
fail_came_from:
    return false;
}

bool AStar_PortalGraphPath(struct tile_desc start_tile, const struct portal *finish, 
                           const struct nav_private *priv, 
                           portal_vec_t *out_path, float *out_cost)
{
    pq_portal_t          frontier;
    khash_t(key_portal) *came_from;
    khash_t(key_float)  *running_cost;
    
    pq_portal_init(&frontier);
    if(NULL == (came_from = kh_init(key_portal)))
        goto fail_came_from;
    if(NULL == (running_cost = kh_init(key_float)))
        goto fail_running_cost;

    const struct nav_chunk *chunk = &priv->chunks[start_tile.chunk_r * priv->width + start_tile.chunk_c];
    coord_vec_t path;
    kv_init(path);

    /* Intitialize the frontier with all the portals in the source chunk that are 
     * reachable from the source tile. */
    for(int i = 0; i < chunk->num_portals; i++) {

        const struct portal *port = &chunk->portals[i];
        struct coord port_center = (struct coord){
            (port->endpoints[0].r + port->endpoints[1].r) / 2,
            (port->endpoints[0].c + port->endpoints[1].c) / 2,
        };

        struct coord chunk_coord = (struct coord){start_tile.chunk_r, start_tile.chunk_c};
        struct coord tile_coord = (struct coord){start_tile.tile_r, start_tile.tile_c};

        float cost;
        bool found = AStar_GridPath(tile_coord, port_center, chunk_coord, chunk->cost_base, &path, &cost);
		if(found){
			
            kh_put_val(key_float, running_cost, portal_to_key(port), cost);
            pq_portal_push(&frontier, cost, port);
		}
    }
	kv_destroy(path);

    while(pq_size(&frontier) > 0) {

        const struct portal *curr;
        pq_portal_pop(&frontier, &curr);

        if(curr == finish)
            break;

        const struct portal *neighbours[MAX_PORTALS_PER_CHUNK];
        float neighbour_costs[MAX_PORTALS_PER_CHUNK];
        int num_neighbours = neighbours_portal_graph(curr, neighbours, neighbour_costs);

        for(int i = 0; i < num_neighbours; i++) {

            const struct portal *next = neighbours[i];
            khiter_t k = kh_get(key_float, running_cost, portal_to_key(curr));
            assert(k != kh_end(running_cost));
            float new_cost = kh_value(running_cost, k) + neighbour_costs[i];

            if((k = kh_get(key_float, running_cost, portal_to_key(next))) == kh_end(running_cost)
            || new_cost < kh_value(running_cost, k)) {

                kh_put_val(key_float, running_cost, portal_to_key(next), new_cost);
                /* No heuristic used - effectively Dijkstra's algorithm */
                float priority = new_cost;
                pq_portal_push(&frontier, priority, next);
                kh_put_val(key_portal, came_from, portal_to_key(next), curr);
            }
        }
    }
    
    if(kh_get(key_portal, came_from, portal_to_key(finish)) == kh_end(came_from))
        goto fail_find_path;

    kv_reset(*out_path);

    /* We have our path at this point. Walk backwards along the path to build a 
     * vector of the nodes along the path. */
    const struct portal *curr = finish;
    while(true) {

        kv_push(const struct portal*, *out_path, curr);
        khiter_t k = kh_get(key_portal, came_from, portal_to_key(curr));
        if(k == kh_end(came_from))
            break;
        curr = kh_value(came_from, k);
    }

    /* Reverse the path vector */
    for(int i = 0, j = kv_size(*out_path) - 1; i < j; i++, j--) {
        const struct portal *tmp = kv_A(*out_path, i);
        kv_A(*out_path, i) = kv_A(*out_path, j);
        kv_A(*out_path, j) = tmp;
    }

    khiter_t k = kh_get(key_float, running_cost, portal_to_key(finish));
    assert(k != kh_end(running_cost));
    *out_cost = kh_value(running_cost, k);

    pq_portal_destroy(&frontier);
    kh_destroy(key_float, running_cost);
    kh_destroy(key_portal, came_from);
    return true;

fail_find_path:
    pq_portal_destroy(&frontier);
    kh_destroy(key_float, running_cost);
fail_running_cost:
    kh_destroy(key_portal, came_from);
fail_came_from:
    return false;
}

const struct portal *AStar_ReachablePortal(struct coord start, struct coord chunk,
                                           const struct nav_chunk *nchunk)
{
    coord_vec_t path;
    kv_init(path);

    for(int i = 0; i < nchunk->num_portals; i++) {

        const struct portal *port = &nchunk->portals[i];
        struct coord port_center = (struct coord){
            (port->endpoints[0].r + port->endpoints[1].r) / 2,
            (port->endpoints[0].c + port->endpoints[1].c) / 2,
        };
        float cost;
        bool found = AStar_GridPath(start, port_center, chunk, nchunk->cost_base, &path, &cost);
		if(found){
    		kv_destroy(path);
			return port;
		}
    }

	kv_destroy(path);
    return NULL;
}

bool AStar_TilesLinked(struct coord start, struct coord finish, struct coord chunk,
                       const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C])
{
    coord_vec_t path;
    kv_init(path);

    float cost;
    bool ret = AStar_GridPath(start, finish, chunk, cost_field, &path, &cost);

    kv_destroy(path);
    return ret;    
}

