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

#ifndef A_STAR_H
#define A_STAR_H

#include "../lib/public/kvec.h"
#include "nav_data.h"

#include <stdbool.h>

struct nav_private;

typedef kvec_t(struct coord) coord_vec_t;
typedef kvec_t(const struct portal*) portal_vec_t;

/* ------------------------------------------------------------------------
 * Finds the shortest path in a rectangular cost field. Returns true if a 
 * path is found, false otherwise. If returning true, 'out_path' holds the
 * tiles to be traversed, in order.
 * ------------------------------------------------------------------------
 */
bool AStar_GridPath(struct coord start, struct coord finish, 
                    const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C], 
                    coord_vec_t *out_path, float *out_cost);

/* ------------------------------------------------------------------------
 * Finds the shortest path between two nodes in a portal graph. Returns true
 * if a path is found, false otherwise. If returning true, 'out_path' holds 
 * the portal nodes to be traversed, in order.
 * ------------------------------------------------------------------------
 */
bool AStar_PortalGraphPath(const struct portal *start, const struct portal *finish, 
                           const struct nav_private *priv, 
                           portal_vec_t *out_path, float *out_cost);

/* ------------------------------------------------------------------------
 * Returns true if there exists a path between 2 tiles in the same chunk.
 * ------------------------------------------------------------------------
 */
bool AStar_TilesLinked(struct coord start, struct coord finish,
                       const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C]);

/* ------------------------------------------------------------------------
 * Returns the nearest portal to a tile, NULL if there is no reachable portal.
 * ------------------------------------------------------------------------
 */
const struct portal *AStar_NearestPortal(struct coord start,
                                         const struct nav_chunk *chunk);

#endif

