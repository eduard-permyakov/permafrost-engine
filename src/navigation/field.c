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
#include "nav_private.h"
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
                           struct coord *out_neighbours, uint8_t *out_costs)
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
            if((r == c) || (r == -c)) /* diag */
                continue;

            out_neighbours[ret] = (struct coord){abs_r, abs_c};
            out_costs[ret] = cost_field[abs_r][abs_c];
            ret++;
        }
    }
    assert(ret < 9);
    return ret;
}

static int neighbours_grid_LOS(const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C], 
                               const struct LOS_field *los, struct coord coord, 
                               struct coord *out_neighbours, uint8_t *out_costs)
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
            if((r == c) || (r == -c)) /* diag */
                continue;

            if(los->field[abs_r][abs_c].wavefront_blocked)
                continue;

            out_neighbours[ret] = (struct coord){abs_r, abs_c};
            out_costs[ret] = cost_field[abs_r][abs_c];
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

static bool is_LOS_corner(struct coord cell, const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C])
{
    if(cell.r > 0 && cell.r < FIELD_RES_R-1) {

        bool left_blocked  = cost_field[cell.r - 1][cell.c] > 1;
        bool right_blocked = cost_field[cell.r + 1][cell.c] > 1;
        if(left_blocked ^ right_blocked)
            return true;
    }

    if(cell.c > 0 && cell.c < FIELD_RES_C-1) {

        bool top_blocked = cost_field[cell.r][cell.c - 1] > 1;
        bool bot_blocked = cost_field[cell.r][cell.c + 1] > 1;
        if(top_blocked ^ bot_blocked)
            return true;
    }
    
    return false;
}

static void create_wavefront_blocked_line(struct tile_desc target, struct tile_desc corner, 
                                          const struct nav_private *priv, vec3_t map_pos, 
                                          struct LOS_field *out_los)
{
    struct map_resolution res = {
        priv->width, priv->height,
        FIELD_RES_C, FIELD_RES_R
    };

    /* First determine the slope of the LOS blocker line in the XZ plane */
    struct box target_bounds = M_Tile_Bounds(res, map_pos, target);
    struct box corner_bounds = M_Tile_Bounds(res, map_pos, corner);

    vec2_t target_center = (vec2_t){
        target_bounds.x - target_bounds.width / 2.0f,
        target_bounds.z + target_bounds.height / 2.0f
    };
    vec2_t corner_center = (vec2_t){
        corner_bounds.x - corner_bounds.width / 2.0f,
        corner_bounds.z + corner_bounds.height / 2.0f
    };

    vec2_t slope;
    PFM_Vec2_Sub(&target_center, &corner_center, &slope);
    PFM_Vec2_Normal(&slope, &slope);

    /* Now use Bresenham's line drawing algorithm to follow a line 
     * of the computed slope starting at the 'corner' until we hit the 
     * edge of the field. 
     * Multiply by 1_000 to convert slope to integer deltas, but keep 
     * 3 digits of precision after the decimal.*/
    int dx =  abs(slope.raw[0] * 1000);
    int dy = -abs(slope.raw[1] * 1000);
    int sx = slope.raw[0] > 0.0f ? 1 : -1;
    int sy = slope.raw[1] < 0.0f ? 1 : -1;
    int err = dx + dy, e2;

    struct coord curr = (struct coord){corner.tile_r, corner.tile_c};
    do {

        out_los->field[curr.r][curr.c].wavefront_blocked = 1;
        e2 = 2 * err;
        if(e2 >= dy) {
            err += dy;
            curr.c += sx;
        }
        if(e2 <= dx) {
            err += dx;
            curr.r += sy;
        }

    }while(curr.r >= 0 && curr.r < FIELD_RES_R && curr.c >= 0 && curr.c < FIELD_RES_C);
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
        uint8_t neighbour_costs[8];
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

void N_LOSFieldCreate(dest_id_t id, struct coord chunk_coord, struct tile_desc target,
                      const struct nav_private *priv, vec3_t map_pos, struct LOS_field *out_los)
{
    out_los->chunk = chunk_coord;
    memset(out_los->field, 0x00, sizeof(out_los->field));

    pq_coord_t frontier;
    pq_coord_init(&frontier);
    const struct nav_chunk *chunk = &priv->chunks[chunk_coord.r * priv->width + chunk_coord.c];

    float integration_field[FIELD_RES_R][FIELD_RES_C];
    for(int r = 0; r < FIELD_RES_R; r++)
        for(int c = 0; c < FIELD_RES_C; c++)
            integration_field[r][c] = INFINITY;

    /* Case 1: LOS for the destination chunk */
    if(chunk_coord.r == target.chunk_r && chunk_coord.c == target.chunk_c) {

        pq_coord_push(&frontier, 0.0f, (struct coord){target.tile_r, target.tile_c});
        integration_field[target.tile_r][target.tile_c] = 0.0f;
    }else{
        assert(0);
    }

    while(pq_size(&frontier) > 0) {

        struct coord curr;
        pq_coord_pop(&frontier, &curr);

        struct coord neighbours[8];
        uint8_t neighbour_costs[8];
        int num_neighbours = neighbours_grid_LOS(chunk->cost_base, out_los, curr, neighbours, neighbour_costs);

        for(int i = 0; i < num_neighbours; i++) {

            int nr = neighbours[i].r, nc = neighbours[i].c;
            if(neighbour_costs[i] > 1) {
                
                if(!is_LOS_corner(neighbours[i], chunk->cost_base))
                    continue;

                struct tile_desc src_desc = (struct tile_desc) {
                    .chunk_r = chunk_coord.r,
                    .chunk_c = chunk_coord.c,
                    .tile_r = neighbours[i].r,
                    .tile_c = neighbours[i].c
                };
                create_wavefront_blocked_line(target, src_desc, priv, map_pos, out_los);
            }else{

                float new_cost = integration_field[curr.r][curr.c] + 1;
                out_los->field[nr][nc].visible = 1;

                if(new_cost < integration_field[neighbours[i].r][neighbours[i].c]) {

                    integration_field[nr][nc] = new_cost;
                    if(!pq_coord_contains(&frontier, neighbours[i]))
                        pq_coord_push(&frontier, new_cost, neighbours[i]);
                }
            }
        }
    }
    pq_coord_destroy(&frontier);
}

