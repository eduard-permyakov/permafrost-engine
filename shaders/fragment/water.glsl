/* *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019 Eduard Permyakov 
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

#define WAVE_STRENGTH   (0.005)
#define WATER_TINT_CLR  vec4(0.0, 0.3, 0.5, 0.1)

/*****************************************************************************/
/* INPUTS                                                                    */
/*****************************************************************************/

in VertexToFrag {
    vec4 clip_space_pos;
    vec2 uv;
}from_vertex;

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out vec4 o_frag_color;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform sampler2D water_dudv_map;
uniform sampler2D water_normal_map;
uniform sampler2D refraction_tex;
uniform sampler2D reflection_tex;

uniform float water_move_factor;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

void main()
{
    vec2 ndc_pos = (from_vertex.clip_space_pos.xy / from_vertex.clip_space_pos.w)/2.0 + 0.5;

    vec2 dudv_uv1 = vec2(from_vertex.uv.x + water_move_factor, from_vertex.uv.y);
    vec2 distortion1 = (texture(water_dudv_map, dudv_uv1).rg * 2.0 - 1.0) * WAVE_STRENGTH;
    vec2 dudv_uv2 = vec2(-from_vertex.uv.x + water_move_factor, from_vertex.uv.y + water_move_factor);
    vec2 distortion2 = (texture(water_dudv_map, dudv_uv2).rg * 2.0 - 1.0) * WAVE_STRENGTH;
    vec2 tot_dist = distortion1 + distortion2;

    vec2 refract_uv = clamp(ndc_pos + tot_dist, 0.001, 0.999);
    vec4 refract_clr = texture(refraction_tex, refract_uv);

    vec2 reflect_uv = vec2(ndc_pos.x, -ndc_pos.y) + tot_dist;
    reflect_uv.x = clamp(reflect_uv.x, 0.001, 0.999);
    reflect_uv.y = clamp(reflect_uv.y, -0.999, -0.001);
    vec4 reflect_clr = texture(reflection_tex, reflect_uv);

    o_frag_color = mix(refract_clr, reflect_clr, 0.5);
    o_frag_color = mix(o_frag_color, WATER_TINT_CLR, 0.1);
}

