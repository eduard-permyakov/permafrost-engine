/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2020 Eduard Permyakov 
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

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out VertexToFrag {
    vec4 clip_space_pos;
    vec2 uv;
    vec3 view_dir;
    vec3 light_dir;
}to_fragment;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

uniform vec3 view_pos;
uniform vec3 light_pos;

uniform vec2 water_tiling;

/*****************************************************************************/
/* PROGRAM
/*****************************************************************************/

void main()
{
    vec4 ws_pos = model * vec4(in_pos, 1.0);
    vec4 clip_space_pos = projection * view * ws_pos;

    to_fragment.clip_space_pos = clip_space_pos;
    to_fragment.uv = vec2(in_pos.x/2.0 + 0.5, in_pos.z/2.0 + 0.5) * water_tiling;
    to_fragment.view_dir = normalize(view_pos - ws_pos.xyz);
    to_fragment.light_dir = normalize(light_pos - ws_pos.xyz);

    gl_Position = clip_space_pos;
}

