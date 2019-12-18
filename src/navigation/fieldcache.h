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

bool N_FC_Init(void);
void N_FC_Shutdown(void);

/* Invalidate all LOS and Flow fields for a particular chunk 
 */
void N_FC_InvalidateAllAtChunk(struct coord chunk);

/* Invalidate all LOS and Flow fields for paths (identified by the dest_id) which 
 * have at least one field at the specified chunk
 */
void N_FC_InvalidateAllThroughChunk(struct coord chunk);

/*###########################################################################*/
/* LOS FIELD CACHING                                                         */
/*###########################################################################*/

/* Returned pointer should not be cached, as it may become invalid after eviction. 
 */
const struct LOS_field  *N_FC_LOSFieldAt(dest_id_t id, struct coord chunk_coord);

bool N_FC_ContainsLOSField(dest_id_t id, struct coord chunk_coord);
void N_FC_PutLOSField(dest_id_t id, struct coord chunk_coord, const struct LOS_field *lf);


/*###########################################################################*/
/* FLOW FIELD CACHING                                                        */
/*###########################################################################*/

/* Returned pointer should not be cached, as it may become invalid after eviction. 
 */
const struct flow_field *N_FC_FlowFieldAt(ff_id_t ffid);

bool N_FC_ContainsFlowField(ff_id_t ffid);
void N_FC_PutFlowField(ff_id_t ffid, const struct flow_field *ff);

bool N_FC_GetDestFFMapping(dest_id_t id, struct coord chunk_coord, ff_id_t *out_ff);
void N_FC_PutDestFFMapping(dest_id_t dest_id, struct coord chunk_coord, ff_id_t ffid);


/*###########################################################################*/
/* GRID PATH CACHING                                                         */
/*###########################################################################*/

struct grid_path_desc{
    bool exists;
    vec_coord_t path;
    float cost;
};

bool N_FC_GetGridPath(struct coord local_start, struct coord local_dest,
                      struct coord chunk, struct grid_path_desc *out);
void N_FC_PutGridPath(struct coord local_start, struct coord local_dest,
                      struct coord chunk, const struct grid_path_desc *in);

#endif

