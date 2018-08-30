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

bool                     N_FC_Init(void);
void                     N_FC_Shutdown(void);

/*###########################################################################*/
/* LOS FIELD CACHING                                                         */
/*###########################################################################*/

bool                     N_FC_ContainsLOSField(dest_id_t id, struct coord chunk_coord);

/* ------------------------------------------------------------------------
 * Updates the 'age' of the entry, which is used for determining which 
 * entries to evict. Returned pointer should not be stored, as it may become 
 * invalid after eviction.
 * ------------------------------------------------------------------------
 */
const struct LOS_field  *N_FC_LOSFieldAt(dest_id_t id, struct coord chunk_coord);
void                     N_FC_SetLOSField(dest_id_t id, struct coord chunk_coord, 
                                          const struct LOS_field *lf);

/*###########################################################################*/
/* FLOW FIELD CACHING                                                        */
/*###########################################################################*/

bool                     N_FC_ContainsFlowField(dest_id_t id, struct coord chunk_coord,
                                                ff_id_t *out_ffid);

/* ------------------------------------------------------------------------
 * Updates the 'age' of the entry, which is used for determining which 
 * entries to evict. Returned pointer should not be stored, as it may become 
 * invalid after eviction.
 * ------------------------------------------------------------------------
 */
const struct flow_field *N_FC_FlowFieldAt(dest_id_t id, struct coord chunk_coord);
void                     N_FC_SetFlowField(dest_id_t id, struct coord chunk_coord, 
                                           ff_id_t field_id, const struct flow_field *ff);

#endif

