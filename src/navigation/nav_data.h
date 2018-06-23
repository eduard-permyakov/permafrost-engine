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

#ifndef NAV_DAT_H
#define NAV_DAT_H

#include <stddef.h>
#include <stdint.h>

#define MAX_PORTALS_PER_CHUNK 64
#define FIELD_RES_R           64
#define FIELD_RES_C           64
#define COST_IMPASSABLE       0x255

struct coord{
    int x, y;
};

struct edge{
    struct portal *neighbour;
    float          distance;
};

struct portal{
    struct coord   chunk;
    struct coord   endpoints[2]; 
    size_t         num_neighbours;
    struct edge    edges[MAX_PORTALS_PER_CHUNK-1];
    struct portal *connected;
};

struct nav_chunk{
    size_t          num_portals; 
    struct   portal portals[MAX_PORTALS_PER_CHUNK];
    uint8_t         cost_base[FIELD_RES_R][FIELD_RES_C]; 
};

#endif
