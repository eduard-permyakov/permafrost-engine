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

#include "a_star.h"
#include "nav_private.h"
#include "../perf.h"
#include "../lib/public/pqueue.h"
#include "../lib/public/khash.h"
#include "fieldcache.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>


PQUEUE_TYPE(coord, struct coord)
PQUEUE_IMPL(static, coord, struct coord)

PQUEUE_TYPE(portal, struct portal_hop)
PQUEUE_IMPL(static, portal, struct portal_hop)

KHASH_MAP_INIT_INT64(key_coord, struct coord)
KHASH_MAP_INIT_INT64(key_portal, struct portal_hop)
KHASH_MAP_INIT_INT64(key_float, float)

#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)         (sizeof(a)/sizeof(a[0]))
#define MAX_PORTAL_NEIGHBS  (256)

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
    return (((uint64_t)c.r) << 32) | (((uint64_t)c.c) & ~((uint32_t)0));
}

static uint64_t phop_to_key(const struct portal_hop *ph)
{
    return (((uint64_t)ph->liid                   & 0xffff) << 48)
         | (((uint64_t)ph->portal->chunk.r        & 0xff  ) << 40)
         | (((uint64_t)ph->portal->chunk.c        & 0xff  ) << 32)
         | (((uint64_t)ph->portal->endpoints[0].r & 0xff  ) << 24)
         | (((uint64_t)ph->portal->endpoints[0].c & 0xff  ) << 16)
         | (((uint64_t)ph->portal->endpoints[1].r & 0xff  ) <<  8)
         | (((uint64_t)ph->portal->endpoints[1].c & 0xff  ) <<  0);
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
    }}
    assert(ret < 9);
    return ret;
}

static bool portal_reachable_from_island(const struct nav_chunk *chunk, 
                                         const struct portal *port, uint16_t liid)
{
    for(int r = port->endpoints[0].r; r <= port->endpoints[1].r; r++) {
    for(int c = port->endpoints[0].c; c <= port->endpoints[1].c; c++) {

        uint16_t curr_liid = chunk->local_islands[r][c];
        if(curr_liid == liid)
            return true;
    }}
    return false;
}

static size_t portal_connected_liids(const struct nav_private *priv, enum nav_layer layer,
                                     const struct portal *port, uint16_t liid, uint16_t *out, size_t maxout)
{
    assert(maxout > 0);
    uint16_t ret = 0;
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    const struct portal *conn = port->connected;
    const struct nav_chunk *pchunk = &priv->chunks[layer][port->chunk.r * priv->width + port->chunk.c];

    /* Take the first tile in the current chunk with the matching liid and 
     * return the liid of the tile directly beside the matching tile in the 
     * adjacent chunk.
     */
    for(int r1 = port->endpoints[0].r; r1 <= port->endpoints[1].r; r1++) {
    for(int c1 = port->endpoints[0].c; c1 <= port->endpoints[1].c; c1++) {

        uint16_t curr_liid = pchunk->local_islands[r1][c1];
        if(curr_liid != liid)
            continue;

        for(int r2 = conn->endpoints[0].r; r2 <= conn->endpoints[1].r; r2++) {
        for(int c2 = conn->endpoints[0].c; c2 <= conn->endpoints[1].c; c2++) {

            const struct nav_chunk *cchunk = &priv->chunks[layer][conn->chunk.r * priv->width + conn->chunk.c];
            struct tile_desc tda = (struct tile_desc){port->chunk.r, port->chunk.c, r1, c1};
            struct tile_desc tdb = (struct tile_desc){conn->chunk.r, conn->chunk.c, r2, c2};

            if(ret == maxout)
                return ret;

            int dr, dc;
            M_Tile_Distance(res, &tda, &tdb, &dr, &dc);
            if(abs(dr) + abs(dc) == 1) {
                uint16_t neighb_liid = cchunk->local_islands[r2][c2];
                bool contains = false;
                for(int i = 0; i < ret; i++) {
                    if(out[i] == neighb_liid) {
                        contains = true;
                        break;
                    }
                }
                if(!contains && neighb_liid != ISLAND_NONE) {
                    out[ret++] = neighb_liid;
                }
            }
        }}
    }}
    return ret;
}

/* The 'liid' is the local island ID on the chunk that 'portal' is on 
 * from which we reached the portal. It is used for discriminating 
 * some portals which may not be reachable from that specific local island
 * and for computing the local IID of the 'next hop' portal. The 
 * local IID of the island on which we reach the neighbour portal will
 * be written to the corresponding index in the 'out_enter_liids' array.
 */
static int neighbours_portal_graph(const struct nav_private *priv, enum nav_layer layer,
                                   const struct portal *portal, uint16_t liid, 
                                   const struct portal **out_neighbours, float *out_costs, 
                                   uint16_t *out_enter_liids, size_t maxout)
{
    int ret = 0;
    const struct nav_chunk *chunk = 
        &priv->chunks[layer][portal->chunk.r * priv->width + portal->chunk.c];

    for(int i = 0; i < portal->num_neighbours; i++) {

        if(ret == maxout)
            return ret;

        const struct edge *edge = &portal->edges[i];
        if(edge->es == EDGE_STATE_BLOCKED)
            continue;

        /* If the portal is not reachable from our source local island, then 
         * we can't use it 
         */
        if(!portal_reachable_from_island(chunk, edge->neighbour, liid))
            continue;

        out_neighbours[ret] = edge->neighbour;
        out_costs[ret] = edge->cost;
        out_enter_liids[ret] = liid;
        ret++;
    }

    uint16_t conn_liids[MAX_PORTAL_NEIGHBS];
    size_t nconn = portal_connected_liids(priv, layer, portal, liid, conn_liids, ARR_SIZE(conn_liids));

    for(int i = 0; i < nconn; i++) {

        if(ret == maxout)
            return ret;

        out_neighbours[ret] = portal->connected;
        out_costs[ret] = 1;
        out_enter_liids[ret] = conn_liids[i];
        ret++;
    }

    return ret;
}

static bool portal_path_found(const struct nav_private *priv, enum nav_layer layer,
                              khash_t(key_portal) *came_from, const struct portal *finish,
                              uint16_t end_liid, struct portal_hop *out_first)
{
    khiter_t k;
    const struct nav_chunk *chunk = 
        &priv->chunks[layer][finish->chunk.r * priv->width + finish->chunk.c];

    struct portal_hop hop = (struct portal_hop){finish, end_liid};
    if((k = kh_get(key_portal, came_from, phop_to_key(&hop))) != kh_end(came_from)) {
        *out_first = hop;
        return true;
    }
    return false;
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

/* Add a constant pentalty to every portal node on top of the existing 
 * cost of the edge between two portals. This will prioritize paths
 * with the fewest number of hops over paths with the shortest distance,
 * unless the pentalty for doing this is significant. If this is increased 
 * such that the edge cost is insignificant in comparison, the pathfinding 
 * will essentially find the path with the fewest number of hops.
 * Since our costs are distances are between portal centers and thus not 
 * precise, this typically gives better behaviour overall.  */
static float portal_node_penalty(void)
{
    return sqrt(pow(FIELD_RES_R, 2.0f) + pow(FIELD_RES_C, 2.0f));
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool AStar_GridPath(struct coord start, struct coord finish, struct coord chunk,
                    const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C], 
                    enum nav_layer layer, vec_coord_t *out_path, float *out_cost)
{
    PERF_ENTER();

    struct grid_path_desc gp = {0};
    vec_coord_init(&gp.path);
    vec_coord_resize(&gp.path, 512);

    if(N_FC_GetGridPath(start, finish, chunk, layer, &gp)) {

        if(!gp.exists)
            PERF_RETURN(false);

        *out_cost = gp.cost;
        vec_coord_copy(out_path, &gp.path);
        PERF_RETURN(true);
    }

    pq_coord_t          frontier;
    khash_t(key_coord) *came_from;
    khash_t(key_float) *running_cost;
    
    pq_coord_init(&frontier);
    if(NULL == (came_from = kh_init(key_coord)))
        goto fail_came_from;
    if(NULL == (running_cost = kh_init(key_float)))
        goto fail_running_cost;

    kh_resize(key_coord, came_from, 1024);
    kh_resize(key_float, running_cost, 1024);

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

    vec_coord_reset(out_path);

    /* We have our path at this point. Walk backwards along the path to build a 
     * vector of the nodes along the path. */
    struct coord curr = finish;
    while(0 != memcmp(&curr, &start, sizeof(struct coord))) {

        vec_coord_push(out_path, curr);
        khiter_t k = kh_get(key_coord, came_from, coord_to_key(curr));
        assert(k != kh_end(came_from));
        curr = kh_value(came_from, k);
    }
    vec_coord_push(out_path, start);

    /* Reverse the path vector */
    for(int i = 0, j = vec_size(out_path) - 1; i < j; i++, j--) {
        struct coord tmp = vec_AT(out_path, i);
        vec_AT(out_path, i) = vec_AT(out_path, j);
        vec_AT(out_path, j) = tmp;
    }

    khiter_t k = kh_get(key_float, running_cost, coord_to_key(finish));
    assert(k != kh_end(running_cost));
    *out_cost = kh_value(running_cost, k);

    pq_coord_destroy(&frontier);
    kh_destroy(key_float, running_cost);
    kh_destroy(key_coord, came_from);

    /* Cache the result */
    gp.exists = true;
    vec_coord_copy(&gp.path, out_path);
    gp.cost = *out_cost;
    N_FC_PutGridPath(start, finish, chunk, layer, &gp);
    PERF_RETURN(true);

fail_find_path:
    gp.exists = false;
    N_FC_PutGridPath(start, finish, chunk, layer, &gp);

    pq_coord_destroy(&frontier);
    kh_destroy(key_float, running_cost);
fail_running_cost:
    kh_destroy(key_coord, came_from);
fail_came_from:
    PERF_RETURN(false);
}

bool AStar_PortalGraphPath(struct tile_desc start_tile, struct tile_desc end_tile, 
                           const struct portal *finish, const struct nav_private *priv, 
                           enum nav_layer layer, vec_portal_t *out_path, float *out_cost)
{
    PERF_ENTER();

    pq_portal_t          frontier;
    khash_t(key_portal) *came_from;
    khash_t(key_float)  *running_cost;
    
    pq_portal_init(&frontier);
    if(NULL == (came_from = kh_init(key_portal)))
        goto fail_came_from;
    if(NULL == (running_cost = kh_init(key_float)))
        goto fail_running_cost;

    const struct nav_chunk *bchunk = &priv->chunks[layer][start_tile.chunk_r * priv->width + start_tile.chunk_c];
    uint16_t start_liid = N_ClosestPathableLocalIsland(priv, bchunk, start_tile);
    if(start_liid == ISLAND_NONE)
        goto fail_find_path;

    const struct nav_chunk *echunk = &priv->chunks[layer][end_tile.chunk_r * priv->width + end_tile.chunk_c];
    uint16_t end_liid = N_ClosestPathableLocalIsland(priv, echunk, end_tile);
    if(end_liid == ISLAND_NONE)
        goto fail_find_path;

    /* Intitialize the frontier with all the portals in the source chunk that are 
     * reachable from the source tile. */
    for(int i = 0; i < bchunk->num_portals; i++) {

        const struct portal *port = &bchunk->portals[i];
        struct coord tile_coord = (struct coord){start_tile.tile_r, start_tile.tile_c};

        if(N_PortalReachableFromTile(port, tile_coord, bchunk)) {

            float cost = bchunk->portal_travel_costs[i][tile_coord.r][tile_coord.c];
            if(cost != FLT_MAX) {

                struct portal_hop hop = (struct portal_hop){port, start_liid};
                kh_put_val(key_float, running_cost, phop_to_key(&hop), cost);
                pq_portal_push(&frontier, cost, hop);
            }
        }
    }

    while(pq_size(&frontier) > 0) {

        struct portal_hop curr;
        pq_portal_pop(&frontier, &curr);

        if(curr.portal == finish && curr.liid == end_liid)
            break;

        const struct portal *neighbours[MAX_PORTAL_NEIGHBS];
        float neighbour_costs[MAX_PORTAL_NEIGHBS];
        uint16_t neighb_enter_liids[MAX_PORTAL_NEIGHBS];

        int num_neighbours = neighbours_portal_graph(priv, layer, curr.portal, curr.liid, neighbours, 
            neighbour_costs, neighb_enter_liids, ARR_SIZE(neighbours));

        for(int i = 0; i < num_neighbours; i++) {

            const struct portal *next = neighbours[i];
            struct portal_hop next_hop = (struct portal_hop){next, neighb_enter_liids[i]};

            khiter_t k = kh_get(key_float, running_cost, phop_to_key(&curr));
            assert(k != kh_end(running_cost));
            float new_cost = kh_value(running_cost, k) + neighbour_costs[i] + portal_node_penalty();

            if((k = kh_get(key_float, running_cost, phop_to_key(&next_hop))) == kh_end(running_cost)
            || new_cost < kh_value(running_cost, k)) {

                kh_put_val(key_float, running_cost, phop_to_key(&next_hop), new_cost);
                /* No heuristic used - effectively Dijkstra's algorithm */
                float priority = new_cost;
                pq_portal_push(&frontier, priority, next_hop);
                kh_put_val(key_portal, came_from, phop_to_key(&next_hop), curr);
            }
        }
    }
    
    struct portal_hop last;
    if(!portal_path_found(priv, layer, came_from, finish, end_liid, &last))
        goto fail_find_path;

    vec_portal_reset(out_path);

    /* We have our path at this point. Walk backwards along the path to build a 
     * vector of the nodes along the path. */
    struct portal_hop curr = last;
    while(true) {

        vec_portal_push(out_path, curr);
        khiter_t k = kh_get(key_portal, came_from, phop_to_key(&curr));
        if(k == kh_end(came_from))
            break;
        curr = kh_value(came_from, k);
    }

    /* Reverse the path vector */
    for(int i = 0, j = vec_size(out_path) - 1; i < j; i++, j--) {
        struct portal_hop tmp = vec_AT(out_path, i);
        vec_AT(out_path, i) = vec_AT(out_path, j);
        vec_AT(out_path, j) = tmp;
    }

    khiter_t k = kh_get(key_float, running_cost, phop_to_key(&last));
    assert(k != kh_end(running_cost));
    *out_cost = kh_value(running_cost, k);

    pq_portal_destroy(&frontier);
    kh_destroy(key_float, running_cost);
    kh_destroy(key_portal, came_from);

    PERF_RETURN(true);

fail_find_path:
    pq_portal_destroy(&frontier);
    kh_destroy(key_float, running_cost);
fail_running_cost:
    kh_destroy(key_portal, came_from);
fail_came_from:
    PERF_RETURN(false);
}

