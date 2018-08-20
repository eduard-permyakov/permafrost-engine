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


typedef uint32_t ff_id_t;

struct LOS_field{
    struct coord chunk;
    struct{
        unsigned int visible : 1;
    }field[FIELD_RES_R][FIELD_RES_C];
};

struct flow_field{
    struct coord chunk;
    struct{
        uint8_t x; 
        uint8_t z;
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

ff_id_t N_Field_ID(struct coord chunk, struct field_target target);

#endif

