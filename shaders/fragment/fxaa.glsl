/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2026 Eduard Permyakov 
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

/* Fast approximate antialiasing, after Timothy Lottes' FXAA: where the 
 * picture's brightness steps sharply, blend along the edge the step follows.
 */

#define EDGE_MIN        (0.0312)
#define EDGE_RELATIVE   (0.125)
#define REDUCE_MUL      (0.125)
#define REDUCE_MIN      (1.0 / 128.0)
#define SPAN_MAX        (8.0)

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out vec4 o_frag_color;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform sampler2D post_world_tex;
uniform vec2      post_out_size;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

float luma(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main()
{
    vec2 texel = 1.0 / vec2(textureSize(post_world_tex, 0));
    vec2 uv = gl_FragCoord.xy / post_out_size;

    vec3 mid = texture(post_world_tex, uv).rgb;
    float l_nw = luma(texture(post_world_tex, uv + vec2(-0.5,  0.5) * texel).rgb);
    float l_ne = luma(texture(post_world_tex, uv + vec2( 0.5,  0.5) * texel).rgb);
    float l_sw = luma(texture(post_world_tex, uv + vec2(-0.5, -0.5) * texel).rgb);
    float l_se = luma(texture(post_world_tex, uv + vec2( 0.5, -0.5) * texel).rgb);
    float l_mid = luma(mid);

    float l_min = min(l_mid, min(min(l_nw, l_ne), min(l_sw, l_se)));
    float l_max = max(l_mid, max(max(l_nw, l_ne), max(l_sw, l_se)));

    /* Flat areas are left alone, so textures keep their detail */
    if(l_max - l_min < max(EDGE_MIN, l_max * EDGE_RELATIVE)) {
        o_frag_color = vec4(mid, 1.0);
        return;
    }

    /* Perpendicular to the brightness gradient, which is along the edge */
    vec2 dir = vec2(-((l_nw + l_ne) - (l_sw + l_se)), (l_nw + l_sw) - (l_ne + l_se));
    float reduce = max((l_nw + l_ne + l_sw + l_se) * 0.25 * REDUCE_MUL, REDUCE_MIN);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -SPAN_MAX, SPAN_MAX) * texel;

    vec3 near = 0.5 * (texture(post_world_tex, uv + dir * (1.0 / 3.0 - 0.5)).rgb
                     + texture(post_world_tex, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 far = 0.5 * near + 0.25 * (texture(post_world_tex, uv - dir * 0.5).rgb
                                  + texture(post_world_tex, uv + dir * 0.5).rgb);

    /* The wider blend is dropped where it reaches past the edge */
    float l_far = luma(far);
    o_frag_color = vec4((l_far < l_min || l_far > l_max) ? near : far, 1.0);
}
