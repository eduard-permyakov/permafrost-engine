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

#ifndef FIELD_H
#define FIELD_H

#include "public/nav.h"
#include "nav_data.h"
#include "../pf_math.h"
#include "../map/public/tile.h"
#include <stdbool.h>

typedef uint64_t ff_id_t;
struct nav_private;

struct LOS_field{
    struct coord chunk;
    struct{
        unsigned int visible : 1;
        unsigned int wavefront_blocked : 1;
    }field[FIELD_RES_R][FIELD_RES_C];
};

struct flow_field{
    struct coord chunk;
    struct{
        unsigned dir_idx : 4;
    }field[FIELD_RES_R][FIELD_RES_C];
};

struct field_target{
    enum{
        TARGET_PORTAL,
        TARGET_TILE,
    }type;
    union{
        const struct portal *port;
        struct coord         tile;
    };
};

enum flow_dir{
    FD_NONE = 0,
    FD_NW,
    FD_N,
    FD_NE,
    FD_W,
    FD_E,
    FD_SW,
    FD_S,
    FD_SE
};

extern vec2_t g_flow_dir_lookup[];

ff_id_t N_FlowField_ID(struct coord chunk, struct field_target target);
void    N_FlowFieldInit(struct coord chunk_coord, const void *nav_private, struct flow_field *out);
void    N_FlowFieldUpdate(const struct nav_chunk *chunk, struct field_target target, 
                          struct flow_field *inout_flow);

/* ------------------------------------------------------------------------
 * Create a line of sight field, indicating which tiles in this chunk are 
 * directly visible from the 'target' tile. If the 'target' tile is not in
 * the current chunk, the previous LOS field along the path must be 
 * supplied in the 'prev_los' argument, so that the visibility information
 * can be carried accross the chunk border. This means that the LOS fields 
 * must be generated starting at the destination chunk (where 'prev_los' is 
 * NULL) and and moving backwards along the path back to the 'source' chunk.
 * ------------------------------------------------------------------------
 */
void    N_LOSFieldCreate(dest_id_t id, struct coord chunk_coord, struct tile_desc target,
                         const struct nav_private *priv, vec3_t map_pos, 
                         struct LOS_field *out_los, const struct LOS_field *prev_los);

#endif

