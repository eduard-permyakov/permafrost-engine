/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2023 Eduard Permyakov 
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

layout (location = 0) in vec3  in_pos;
layout (location = 1) in vec2  in_uv;
layout (location = 2) in vec3  in_normal;
layout (location = 3) in int   in_material_idx;

layout (location = 4) in int   in_blend_mode;
layout (location = 5) in int   in_mid_indices;
layout (location = 6) in ivec2 in_c1_indices;
layout (location = 7) in ivec2 in_c2_indices; 
layout (location = 8) in int   in_tb_indices;
layout (location = 9) in int   in_lr_indices;
layout (location = 10) in int  in_wang_index;

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out VertexToFrag {
         vec2  uv;
         vec3  world_pos;
         vec3  normal;
    flat int   mat_idx;
    flat int   blend_mode;
    flat int   mid_indices;
    flat ivec2 c1_indices;
    flat ivec2 c2_indices; 
    flat int   tb_indices;
    flat int   lr_indices;
    flat int   wang_index;
}to_fragment;

out VertexToGeo {
    vec3 normal;
}to_geometry;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec4 clip_plane0;

/*****************************************************************************/
/* PROGRAM
/*****************************************************************************/

void main()
{
    to_fragment.uv = in_uv;
    to_fragment.mat_idx = in_material_idx;
    to_fragment.world_pos = (model * vec4(in_pos, 1.0)).xyz;
    to_fragment.normal = normalize(mat3(model) * in_normal);
    to_fragment.blend_mode = in_blend_mode;
    to_fragment.mid_indices = in_mid_indices;
    to_fragment.c1_indices = in_c1_indices;
    to_fragment.c2_indices = in_c2_indices;
    to_fragment.tb_indices = in_tb_indices;
    to_fragment.lr_indices = in_lr_indices;
    to_fragment.wang_index = in_wang_index;

    to_geometry.normal = normalize(mat3(projection * view * model) * in_normal);

    gl_Position = projection * view * model * vec4(in_pos, 1.0);
    gl_ClipDistance[0] = dot(model * vec4(in_pos, 1.0), clip_plane0);
}

