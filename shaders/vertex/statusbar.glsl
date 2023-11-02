/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2023 Eduard Permyakov 
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

#version 330 core

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;

#define MAX_HBS   (256)

/* Must match the definition in the fragment shader */
#define CURR_HB_HEIGHT  (max(4.0/1080 * curr_res.y, 4.0))
#define CURR_HB_WIDTH   (40.0/1080 * curr_res.y)

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out VertexToFrag {
         vec2 uv;
        float health_pc;
}to_fragment;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

/* Should be set up for screenspace rendering */
uniform mat4 view;
uniform mat4 projection;

uniform ivec2 curr_res;

uniform vec2  ent_top_offsets_ss[MAX_HBS];
uniform float ent_health_pc[MAX_HBS];

/*****************************************************************************/
/* PROGRAM
/*****************************************************************************/

void main()
{
    to_fragment.uv = in_uv;
    to_fragment.health_pc = ent_health_pc[gl_InstanceID];

    vec2 ss_pos = vec2(in_pos.x * CURR_HB_WIDTH, in_pos.y * CURR_HB_HEIGHT);
    ss_pos += ent_top_offsets_ss[gl_InstanceID];
    gl_Position = projection * view * vec4(ss_pos, 0.0, 1.0);
}

