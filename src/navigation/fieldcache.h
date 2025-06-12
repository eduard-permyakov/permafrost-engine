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

#ifndef FIELDCACHE_H
#define FIELDCACHE_H

#include "public/nav.h"
#include "nav_data.h"
#include "field.h"
#include "a_star.h"

#include <stdbool.h>


/*###########################################################################*/
/* FC GENERAL                                                                */
/*###########################################################################*/

struct fieldcache_ctx;

struct fieldcache_ctx *N_FC_New(void);
void N_FC_Free(struct fieldcache_ctx *ctx);

bool N_FC_Init(struct fieldcache_ctx *ctx);
void N_FC_Destroy(struct fieldcache_ctx *ctx);

/* Invalidate all LOS and Flow fields for a particular chunk 
 */
void N_FC_InvalidateAllAtChunk(struct fieldcache_ctx *ctx, 
                               struct coord chunk, 
                               enum nav_layer layer);

/* Invalidate all LOS and Flow fields for paths (identified by the dest_id) which 
 * have at least one field at the specified chunk
 */
void N_FC_InvalidateAllThroughChunk(struct fieldcache_ctx *ctx, 
                                    struct coord chunk, 
                                    enum nav_layer layer);

/* Invalidate 'enemy seek' fields in all chunks which are adjacent to the 
 * current one. This is because 'enemy seek' fields are also dependent
 * on the state of the units in adjacent chunks.
 */
void N_FC_InvalidateNeighbourEnemySeekFields(struct fieldcache_ctx *ctx, 
                                             int width, int height, 
                                             struct coord chunk, 
                                             enum nav_layer layer);

void N_FC_InvalidateDynamicSurroundFields(struct fieldcache_ctx *ctx);

/*###########################################################################*/
/* LOS FIELD CACHING                                                         */
/*###########################################################################*/

/* Returned pointer should not be cached, as it may become invalid after eviction. 
 */
const struct LOS_field  *N_FC_LOSFieldAt(struct fieldcache_ctx *ctx, 
                                         dest_id_t id, 
                                         struct coord chunk_coord);

bool                     N_FC_ContainsLOSField(struct fieldcache_ctx *ctx, 
                                               dest_id_t id, 
                                               struct coord chunk_coord);

void                     N_FC_PutLOSField(struct fieldcache_ctx *ctx, 
                                          dest_id_t id, 
                                          struct coord chunk_coord, 
                                          const struct LOS_field *lf);

/*###########################################################################*/
/* FLOW FIELD CACHING                                                        */
/*###########################################################################*/

/* Returned pointer should not be cached, as it may become invalid after eviction. 
 */
const struct flow_field *N_FC_FlowFieldAt(struct fieldcache_ctx *ctx, ff_id_t ffid);

bool                     N_FC_ContainsFlowField(struct fieldcache_ctx *ctx, ff_id_t ffid);

void                     N_FC_PutFlowField(struct fieldcache_ctx *ctx, 
                                           ff_id_t ffid, const struct flow_field *ff);

bool                     N_FC_GetDestFFMapping(struct fieldcache_ctx *ctx, 
                                               dest_id_t id, 
                                               struct coord chunk_coord, 
                                               ff_id_t *out_ff);

void                     N_FC_PutDestFFMapping(struct fieldcache_ctx *ctx, 
                                               dest_id_t dest_id, 
                                               struct coord chunk_coord, 
                                               ff_id_t ffid);

/*###########################################################################*/
/* GRID PATH CACHING                                                         */
/*###########################################################################*/

struct grid_path_desc{
    bool exists;
    vec_coord_t path;
    float cost;
};

bool N_FC_GetGridPath(struct fieldcache_ctx *ctx, 
                      struct coord local_start, 
                      struct coord local_dest,
                      struct coord chunk, 
                      enum nav_layer layer, 
                      struct grid_path_desc *out);

void N_FC_PutGridPath(struct fieldcache_ctx *ctx, 
                      struct coord local_start, 
                      struct coord local_dest,
                      struct coord chunk, 
                      enum nav_layer layer, 
                      const struct grid_path_desc *in);

/*###########################################################################*/
/* STATS                                                                     */
/*###########################################################################*/

void      N_FC_ClearStats(struct fieldcache_ctx *ctx);
void      N_FC_GetStats(struct fieldcache_ctx *ctx, struct fc_stats *out_stats);
void      N_FC_ClearAll(struct fieldcache_ctx *ctx);

#endif

