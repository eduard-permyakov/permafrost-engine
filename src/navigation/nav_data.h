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

#ifndef NAV_DAT_H
#define NAV_DAT_H

#include <stddef.h>
#include <stdint.h>

#define MAX_PORTALS_PER_CHUNK 64
#define FIELD_RES_R           64
#define FIELD_RES_C           64
#define COST_IMPASSABLE       0xff
#define ISLAND_NONE           0xffff

struct coord{
    int r, c;
};

enum edge_state{
    EDGE_STATE_ACTIVE,
    EDGE_STATE_BLOCKED,
};

struct edge{
    enum edge_state es;
    struct portal  *neighbour;
    /* Cost of moving from the center of one portal to the center
     * of the next. */
    float           cost;
};

struct portal{
    /* This identifies which component of the global portal graph the 
     * portal is a part of. Portals with the same component_id are
     * reachable from one another, The component_id calculation takes
     * 'blocked' portals into account. */
    int               component_id;
    struct coord      chunk;
    struct coord      endpoints[2]; 
    size_t            num_neighbours;
    struct edge       edges[MAX_PORTALS_PER_CHUNK-1];
    struct portal    *connected;
};

struct nav_chunk{
    size_t          num_portals; 
    struct portal   portals[MAX_PORTALS_PER_CHUNK];
    /* The per-tile cost of traversal. Tiles with 'COST_IMPASSABLE'
     * cost may never be reached.
     */
    uint8_t         cost_base[FIELD_RES_R][FIELD_RES_C]; 
    /* Holds the cost to travel from every tile to every portal,
     * when the portal is reachable from the tile. This field is 
     * synchronized with the 'cost_base' field.
     */
    float           portal_travel_costs[MAX_PORTALS_PER_CHUNK][FIELD_RES_R][FIELD_RES_C];
    /* Every tile in the 'blockers' holds a reference count for
     * how many stationary entities are currently 'retaining' that 
     * tile by being positioned on it. 'Blocked' tiles are treated 
     * as impassable when computing flow fields. 
     */
    uint8_t         blockers[FIELD_RES_R][FIELD_RES_C];
    /* An 'island' is a collection of tiles that are all reachable 
     * from one another. Each island has a unique ID. These are
     * synchronized with the 'cost_base' field, and are not
     * affected by blockers. These islands IDs are 'global' 
     * (shared by all chunks)
     */
    uint16_t        islands[FIELD_RES_R][FIELD_RES_C];
    /* This field uses chunk-local island IDs and accounts for
     * the blockers, but does not account for any part of the
     * map outside the local chunk. This field is synchronized
     * with the 'blockers' field. Two tiles having the same 
     * 'islands' values may have different 'local_islands' 
     * values if the only path between them is blocked by 
     * stationary entities, for example.
     */
    uint16_t        local_islands[FIELD_RES_R][FIELD_RES_C];
};

#endif
