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

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;
layout (location = 2) in vec3 in_normal;
layout (location = 3) in int  in_material_idx;
layout (location = 4) in int  in_draw_id;

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out VertexToFrag {
         vec2 uv;
    flat int  mat_idx;
         vec3 world_pos;
         vec3 normal;
         vec4 light_space_pos;
    flat int  draw_id;
}to_fragment;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform mat4 view;
uniform mat4 projection;

uniform mat4 light_space_transform;
uniform vec4 clip_plane0;

/* Per-instance buffer contents:
 *  +--------------------------------------------------+ <-- base
 *  | mat4x4_t (16 floats)                             | (model matrix)
 *  +--------------------------------------------------+
 *  | vec2_t[16] (32 floats)                           | (material:texture mapping)
 *  +--------------------------------------------------+
 *  | {float, float, vec3_t, vec3_t}[16] (128 floats)  | (material properties)
 *  +--------------------------------------------------+
 */

uniform samplerBuffer attrbuff;
uniform int attrbuff_offset;
uniform int attr_stride;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

int inst_attr_base(int draw_id)
{
    int size = textureSize(attrbuff);
    int inst_offset = draw_id * attr_stride;
    return int(mod(attrbuff_offset / 4 + inst_offset, size));
}

mat4 model_from_attrbuff(int draw_id)
{
    int size = textureSize(attrbuff);
    int base = inst_attr_base(draw_id);

    return mat4(
        vec4(
            texelFetch(attrbuff, int(mod(base +  0, size))).r,
            texelFetch(attrbuff, int(mod(base +  1, size))).r,
            texelFetch(attrbuff, int(mod(base +  2, size))).r,
            texelFetch(attrbuff, int(mod(base +  3, size))).r
        ),
        vec4(
            texelFetch(attrbuff, int(mod(base +  4, size))).r,
            texelFetch(attrbuff, int(mod(base +  5, size))).r,
            texelFetch(attrbuff, int(mod(base +  6, size))).r,
            texelFetch(attrbuff, int(mod(base +  7, size))).r
        ),
        vec4(
            texelFetch(attrbuff, int(mod(base +  8, size))).r,
            texelFetch(attrbuff, int(mod(base +  9, size))).r,
            texelFetch(attrbuff, int(mod(base + 10, size))).r,
            texelFetch(attrbuff, int(mod(base + 11, size))).r
        ),
        vec4(
            texelFetch(attrbuff, int(mod(base + 12, size))).r,
            texelFetch(attrbuff, int(mod(base + 13, size))).r,
            texelFetch(attrbuff, int(mod(base + 14, size))).r,
            texelFetch(attrbuff, int(mod(base + 15, size))).r
        )
    );
}

void main()
{
    mat4 model = model_from_attrbuff(in_draw_id);

    to_fragment.uv = in_uv;
    to_fragment.mat_idx = in_material_idx;
    to_fragment.world_pos = (model * vec4(in_pos, 1.0)).xyz;
    to_fragment.normal = normalize(mat3(model) * in_normal);
    to_fragment.light_space_pos = light_space_transform * vec4(to_fragment.world_pos, 1.0);
    to_fragment.draw_id = in_draw_id;

    gl_Position = projection * view * model * vec4(in_pos, 1.0);
    gl_ClipDistance[0] = dot(model * vec4(in_pos, 1.0), clip_plane0);
}

