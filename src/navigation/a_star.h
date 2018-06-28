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


typedef kvec_t(struct coord) coord_vec_t;

bool AStar_GridPath(struct coord start, struct coord finish, 
                    const uint8_t cost_field[FIELD_RES_R][FIELD_RES_C], 
                    coord_vec_t *out_path);

#endif

