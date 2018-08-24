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

#include "field.h"
#include "../lib/public/pqueue.h"

#include <string.h>
#include <assert.h>
#include <math.h>

PQUEUE_TYPE(coord, struct coord)
PQUEUE_IMPL(static, coord, struct coord)

/*****************************************************************************/
/* GLOBAL VARIABLES                                                          */
/*****************************************************************************/

vec2_t g_flow_dir_lookup[9] = {
    [FD_NONE] = (vec2_t){  0.0f,               0.0f              },
    [FD_NW]   = (vec2_t){  1.0f / sqrt(2.0f), -1.0f / sqrt(2.0f) },
    [FD_N]    = (vec2_t){  0.0f,              -1.0f              },
    [FD_NE]   = (vec2_t){ -1.0f / sqrt(2.0f), -1.0f / sqrt(2.0f) },
    [FD_W]    = (vec2_t){  1.0f,               0.0f              },
    [FD_E]    = (vec2_t){ -1.0f,               0.0f              },
    [FD_SW]   = (vec2_t){  1.0f / sqrt(2.0f),  1.0f / sqrt(2.0f) },
    [FD_S]    = (vec2_t){  0.0f,               1.0f              },
    [FD_SE]   = (vec2_t){ -1.0f / sqrt(2.0f),  1.0f / sqrt(2.0f) },
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

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

static enum flow_dir flow_dir(const float integration_field[FIELD_RES_R][FIELD_RES_C], 
                              struct coord coord)
{
    float min_cost = INFINITY;

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

            if(integration_field[abs_r][abs_c] < min_cost)
                min_cost = integration_field[abs_r][abs_c];
        }
    }
    assert(min_cost < INFINITY);

    /* Prioritize the cardinal directions over the diagonal ones */
    if(coord.r > 0 
    && integration_field[coord.r-1][coord.c] == min_cost)
        return FD_N; 
    else if(coord.r < (FIELD_RES_R-1) 
    && integration_field[coord.r+1][coord.c] == min_cost)
        return FD_S;
    else if(coord.c < (FIELD_RES_C-1) 
    && integration_field[coord.r][coord.c+1] == min_cost)
        return FD_E;
    else if(coord.c > 0 
    && integration_field[coord.r][coord.c-1] == min_cost)
        return FD_W;
    else if(coord.r > 0 && coord.c > 0 
    && integration_field[coord.r-1][coord.c-1] == min_cost)
        return FD_NW; 
    else if(coord.r > 0 && coord.c < (FIELD_RES_C-1) 
    && integration_field[coord.r-1][coord.c+1] == min_cost)
        return FD_NE;
    else if(coord.r < (FIELD_RES_R-1) && coord.c > 0 
    && integration_field[coord.r+1][coord.c-1] == min_cost)
        return FD_SW;
    else if(coord.r < (FIELD_RES_R-1) && coord.c < (FIELD_RES_R-1) 
    && integration_field[coord.r+1][coord.c+1] == min_cost)
        return FD_SE;
    else
        assert(0);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

ff_id_t N_FlowField_ID(struct coord chunk, struct field_target target)
{
    if(target.type == TARGET_PORTAL) {

        return (((uint64_t)target.type)                 << 48)
             | (((uint64_t)target.port->endpoints[0].r) << 40)
             | (((uint64_t)target.port->endpoints[0].c) << 32)
             | (((uint64_t)target.port->endpoints[1].r) << 24)
             | (((uint64_t)target.port->endpoints[1].c) << 16)
             | (((uint64_t)chunk.r)                     <<  8)
             | (((uint64_t)chunk.c)                     <<  0);
    }else{

        return (((uint64_t)target.type)                 << 48)
             | (((uint64_t)target.tile.r)               << 24)
             | (((uint64_t)target.tile.c)               << 16)
             | (((uint64_t)chunk.r)                     <<  8)
             | (((uint64_t)chunk.c)                     <<  0);
    }
}

void N_FlowFieldInit(struct coord chunk, struct flow_field *out)
{
    for(int r = 0; r < FIELD_RES_R; r++) {
        for(int c = 0; c < FIELD_RES_C; c++) {

            out->field[r][c].dir_idx = FD_NONE;
        }
    }
    out->chunk = chunk;
}

void N_FlowFieldUpdate(const struct nav_chunk *chunk, struct field_target target, 
                       struct flow_field *inout_flow)
{
    pq_coord_t frontier;
    pq_coord_init(&frontier);

    float integration_field[FIELD_RES_R][FIELD_RES_C];
    for(int r = 0; r < FIELD_RES_R; r++)
        for(int c = 0; c < FIELD_RES_C; c++)
            integration_field[r][c] = INFINITY;

    switch(target.type) {
    case TARGET_PORTAL: {
        
        for(int r = target.port->endpoints[0].r; r <= target.port->endpoints[1].r; r++) {
            for(int c = target.port->endpoints[0].c; c <= target.port->endpoints[1].c; c++) {

                pq_coord_push(&frontier, 0.0f, (struct coord){r, c});
                integration_field[r][c] = 0.0f;
            }
        }
        break;
    }
    case TARGET_TILE: {
        pq_coord_push(&frontier, 0.0f, target.tile);
        integration_field[target.tile.r][target.tile.c] = 0.0f;
        break;
    }
    default: assert(0);
    }

    /* Build the integration field */
    while(pq_size(&frontier) > 0) {

        struct coord curr;
        pq_coord_pop(&frontier, &curr);

        struct coord neighbours[8];
        float neighbour_costs[8];
        int num_neighbours = neighbours_grid(chunk->cost_base, curr, neighbours, neighbour_costs);

        for(int i = 0; i < num_neighbours; i++) {

            float total_cost = integration_field[curr.r][curr.c] + neighbour_costs[i];
            if(total_cost < integration_field[neighbours[i].r][neighbours[i].c]) {

                integration_field[neighbours[i].r][neighbours[i].c] = total_cost;
                if(!pq_coord_contains(&frontier, neighbours[i]))
                    pq_coord_push(&frontier, total_cost, neighbours[i]);
            }
        }
    }
    pq_coord_destroy(&frontier);

    /* Build the flow field from the integration field. Don't touch any impassable tiles
     * as they may have already been set in the case that a single chunk is divided into
     * multiple passable 'islands', but a computed path takes us through more than one of
     * these 'islands'. */
    for(int r = 0; r < FIELD_RES_R; r++) {
        for(int c = 0; c < FIELD_RES_C; c++) {

            if(integration_field[r][c] == INFINITY)
                continue;

            if(integration_field[r][c] == 0.0f) {

                if(target.type != TARGET_PORTAL) {

                    inout_flow->field[r][c].dir_idx = FD_NONE;
                    continue;
                }

                bool up    = target.port->connected->chunk.r < target.port->chunk.r;
                bool down  = target.port->connected->chunk.r > target.port->chunk.r;
                bool left  = target.port->connected->chunk.c < target.port->chunk.c;
                bool right = target.port->connected->chunk.c > target.port->chunk.c;
                assert(up ^ down ^ left ^ right);

                if(up)
                    inout_flow->field[r][c].dir_idx = FD_N;
                else if(down)
                    inout_flow->field[r][c].dir_idx = FD_S;
                else if(left)
                    inout_flow->field[r][c].dir_idx = FD_W;
                else if(right)
                    inout_flow->field[r][c].dir_idx = FD_E;
                else
                    assert(0);
                continue;
            }

            inout_flow->field[r][c].dir_idx = flow_dir(integration_field, (struct coord){r, c});
        }
    }
}

