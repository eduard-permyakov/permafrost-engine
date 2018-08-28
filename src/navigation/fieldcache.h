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

bool                     N_FC_ContainsLOSField(dest_id_t id, struct coord chunk_coord);

/*###########################################################################*/
/* LOS FIELD CACHING                                                         */
/*###########################################################################*/

/* ------------------------------------------------------------------------
 * Updates the 'age' of the entry, which is used for determining which 
 * entries to evict. Returned pointer should not be stored, as it may become 
 * invalid after eviction.
 * ------------------------------------------------------------------------
 */
const struct LOS_field  *N_FC_LOSFieldAt(dest_id_t id, struct coord chunk_coord);
void                     N_FC_SetLOSField(dest_id_t id, struct coord chunk_coord, 
                                          const struct LOS_field *lf);

bool                     N_FC_ContainsFlowField(dest_id_t id, struct coord chunk_coord,
                                                ff_id_t *out_ffid);

/*###########################################################################*/
/* FLOW FIELD CACHING                                                        */
/*###########################################################################*/

/* ------------------------------------------------------------------------
 * Updates the 'age' of the entry, which is used for determining which 
 * entries to evict. Returned pointer should not be stored, as it may become 
 * invalid after eviction.
 * ------------------------------------------------------------------------
 */
const struct flow_field *N_FC_FlowFieldAt(dest_id_t id, struct coord chunk_coord);
void                     N_FC_SetFlowField(dest_id_t id, struct coord chunk_coord, 
                                           ff_id_t field_id, const struct flow_field *ff);

/*###########################################################################*/
/* PORTAL PATH CACHING                                                       */
/*###########################################################################*/

bool                     N_FC_ContainsPortalPath(const struct portal *src, 
                                                 const struct portal *dst);
/* ------------------------------------------------------------------------
 * Updates the 'age' of the entry, which is used for determining which 
 * entries to evict. Returned pointer should not be stored, as it may become 
 * invalid after eviction.
 * Returns true if a path exists. In this case, 'out_path' is set to the path.
 * ------------------------------------------------------------------------
 */
bool                     N_FC_PortalPathForNodes(const struct portal *src, 
                                                 const struct portal *dst,
                                                 portal_vec_t *out_path);

/* ------------------------------------------------------------------------
 * 'path' argument is NULL if no path exists between 2 nodes. Knowing that
 * no path exists will still save time traversing the portal graph.
 * Once set, the cache will 'own' the path vector and will be responsible
 * for freeing it.
 * ------------------------------------------------------------------------
 */
void                     N_FC_SetPortalPath(const struct portal *src, 
                                            const struct portal *dst,
                                            const portal_vec_t *path);

#endif

