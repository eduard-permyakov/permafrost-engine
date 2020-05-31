/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#define SPECULAR_STRENGTH  0.5
#define SPECULAR_SHININESS 2

#define SHADOW_MAP_BIAS 0.002
#define SHADOW_MULTIPLIER 0.7

#define FLOATS_PER_INST (176)

/*****************************************************************************/
/* INPUTS                                                                    */
/*****************************************************************************/

in VertexToFrag {
         vec2 uv;
    flat int  mat_idx;
         vec3 world_pos;
         vec3 normal;
         vec4 light_space_pos;
    flat int  draw_id;
}from_vertex;

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out vec4 o_frag_color;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform vec3 ambient_color;
uniform vec3 light_color;
uniform vec3 light_pos;
uniform vec3 view_pos;

uniform sampler2D shadow_map;

uniform sampler2DArray tex_array0;
uniform sampler2DArray tex_array1;
uniform sampler2DArray tex_array2;
uniform sampler2DArray tex_array3;

uniform samplerBuffer attrbuff;
uniform int attrbuff_offset;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

float shadow_factor(vec4 light_space_pos)
{
    vec3 proj_coords = (light_space_pos.xyz / light_space_pos.w) * 0.5 + 0.5;
    float closest_depth = texture(shadow_map, proj_coords.xy).r;
    float current_depth = proj_coords.z;
    if(current_depth - SHADOW_MAP_BIAS > closest_depth) {
        return 1.0;
    }else {
        return 0.0;
    }
}

int inst_attr_base(int draw_id)
{
    int size = textureSize(attrbuff);
    int inst_offset = draw_id * FLOATS_PER_INST;
    return int(mod(attrbuff_offset / 4 + inst_offset, size));
}

vec4 read_vec4(int draw_id)
{
    int size = textureSize(attrbuff);
    int base = inst_attr_base(draw_id);

    return vec4(
        texelFetch(attrbuff, int(mod(base + 0, size))).r,
        texelFetch(attrbuff, int(mod(base + 1, size))).r,
        texelFetch(attrbuff, int(mod(base + 2, size))).r,
        texelFetch(attrbuff, int(mod(base + 3, size))).r
    );
}

void main()
{
    o_frag_color = read_vec4(from_vertex.draw_id);
}

