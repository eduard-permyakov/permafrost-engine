/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2022-2023 Eduard Permyakov 
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

#version 430 core

/*****************************************************************************/
/* DEFINES                                                                   */
/*****************************************************************************/

#define STATE_MOVING                0
#define STATE_MOVING_IN_FORMATION   1
#define STATE_ARRIVED               2
#define STATE_SEEK_ENEMIES          3
#define STATE_WAITING               4
#define STATE_SURROUND_ENTITY       5
#define STATE_ENTER_ENTITY_RANGE    6
#define STATE_TURNING               7
#define STATE_ARRIVING_TO_CELL      8

#define ATTR(_gpuid, _name) \
    moveattrs[gpuid - 1]._name

#define MAX_FLOCK_MEMBERS           (1024)

/*****************************************************************************/
/* INPUT/OUTPUT                                                              */
/*****************************************************************************/

struct move_input{
    uint flock_id;
    uint movestate;
    uint has_dest_los;
    /* We don't use a vec2 because it has greater alignment 
     * requirements, and may cause padding. */
    float dest_x;
    float dest_y;
};

struct flock{
    uint ents[MAX_FLOCK_MEMBERS];
    uint nmembers;
    vec2 target_xz;
};

layout(local_size_x = 1) in;
layout(std430, binding = 0) readonly buffer in_movedata
{
    move_input moveattrs[];
};
layout(std430, binding = 1) readonly buffer in_flocks
{
    flock flocks[];
};
layout(r32ui, binding = 2) uniform readonly uimage2D in_pos_id_map;
layout(std430, binding = 3) writeonly buffer o_data
{
    vec2 vpref[];
};

/*****************************************************************************/
/* IMPLEMENTATION                                                            */
/*****************************************************************************/

vec2 enemy_seek_vpref(uint gpuid, float speed, vec2 vdes)
{
    return vec2(0.0, 0.0);
}

vec2 cell_arrival_seek_vpref(uint gpuid, vec2 cell_pos, float speed, vec2 vdes,
                             vec2 cohesion, vec2 alignment, vec2 drag)
{
    return vec2(0.0, 0.0);
}

vec2 formation_seek_vpref(uint gpuid, uint flockid, float speed, vec2 vdes, vec2 cohesion, 
                          vec2 alignment, vec2 drag, uint has_dest_los)
{
    return vec2(0.0, 0.0);
}

vec2 point_seek_vpref(uint gpuid, uint flockid, vec2 vdes, uint has_dest_los, float speed)
{
    return vec2(0.0, 0.0);
}

vec2 compute_vpref(uint gpuid)
{
    return vec2(0.0, 0.0);
}

bool ent_still(uint gpuid)
{
    uint state = ATTR(gpuid, movestate);
    return (state == STATE_ARRIVED || state == STATE_WAITING);
}

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    uint gpuid = idx + 1;

    if(ent_still(gpuid))
        return;

    vec2 out_vpref = vec2(0.0, 0.0);
    uint state = ATTR(gpuid, movestate);

    if(state == STATE_TURNING) {
    }else if(state == STATE_SEEK_ENEMIES) {
    }else if(state == STATE_ARRIVING_TO_CELL) {
    }else if(state == STATE_MOVING_IN_FORMATION) {
    }else{
    }

    vpref[idx] = vec2(0.0, 0.0);
}

