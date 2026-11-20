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

#define LUT_DIM 64.0

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out vec4 o_frag_color;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform sampler2D post_world_tex;
uniform sampler3D post_grade_lut;
uniform sampler3D post_palette_lut;
uniform vec2      post_out_size;
uniform ivec2     post_dither_offset;
uniform float     post_dither;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

const float BAYER[16] = float[16](
     0.0,  8.0,  2.0, 10.0,
    12.0,  4.0, 14.0,  6.0,
     3.0, 11.0,  1.0,  9.0,
    15.0,  7.0, 13.0,  5.0
);

float threshold(ivec2 p)
{
    ivec2 q = p & 3;
    return (BAYER[q.y * 4 + q.x] + 0.5) / 16.0 - 0.5;
}

vec3 srgb_to_linear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 linear_to_srgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 srgb_to_oklab(vec3 c)
{
    vec3 lin = srgb_to_linear(c);
    vec3 lms = vec3(
        dot(lin, vec3(0.4122214708, 0.5363325363, 0.0514459929)),
        dot(lin, vec3(0.2119034982, 0.6806995451, 0.1073969566)),
        dot(lin, vec3(0.0883024619, 0.2817188376, 0.6299787005)));
    lms = pow(max(lms, 0.0), vec3(1.0 / 3.0));
    return vec3(
        dot(lms, vec3(0.2104542553,  0.7936177850, -0.0040720468)),
        dot(lms, vec3(1.9779984951, -2.4285922050,  0.4505937099)),
        dot(lms, vec3(0.0259040371,  0.7827717662, -0.8086757660)));
}

vec3 oklab_to_srgb(vec3 lab)
{
    vec3 lms = vec3(
        lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z,
        lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z,
        lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z);
    lms = lms * lms * lms;
    return linear_to_srgb(vec3(
        dot(lms, vec3( 4.0767416621, -3.3077115913,  0.2309699292)),
        dot(lms, vec3(-1.2684380046,  2.6097574011, -0.3413193965)),
        dot(lms, vec3(-0.0041960863, -0.7034186147,  1.7076147010))));
}

vec3 lut_coord(vec3 c)
{
    return clamp(c, 0.0, 1.0) * ((LUT_DIM - 1.0) / LUT_DIM) + 0.5 / LUT_DIM;
}

/* A texel of the world: graded, dithered and snapped to the palette */
vec3 shade(ivec2 texel, ivec2 size)
{
    texel = clamp(texel, ivec2(0), size - 1);
    vec3 world = texelFetch(post_world_tex, texel, 0).rgb;
    vec3 graded = texture(post_grade_lut, lut_coord(world)).rgb;

    /* The pattern is laid out from the top row down, and rides with the ground */
    ivec2 cell = ivec2(texel.x, size.y - 1 - texel.y) + post_dither_offset;
    vec3 lab = srgb_to_oklab(graded);
    lab.x += threshold(cell) * post_dither;
    lab.y += threshold(cell - ivec2(2, 0)) * post_dither * 0.25;
    lab.z += threshold(cell - ivec2(0, 2)) * post_dither * 0.25;

    return texture(post_palette_lut, lut_coord(oklab_to_srgb(lab))).rgb;
}

void main()
{
    ivec2 size = textureSize(post_world_tex, 0);
    vec2 scale = post_out_size / vec2(size);

    /* Nearest sampling, but for a blend across the one output pixel a texel
     * edge falls inside. At a whole scale no edge does, and none blend.
     */
    vec2 pos = gl_FragCoord.xy / scale - 0.5;
    ivec2 base = ivec2(floor(pos));
    vec2 w = clamp((fract(pos) - 0.5) * scale + 0.5, 0.0, 1.0);

    vec3 bot = shade(base, size);
    if(w.x > 0.0)
        bot = mix(bot, shade(base + ivec2(1, 0), size), w.x);
    vec3 color = bot;
    if(w.y > 0.0) {
        vec3 top = shade(base + ivec2(0, 1), size);
        if(w.x > 0.0)
            top = mix(top, shade(base + ivec2(1, 1), size), w.x);
        color = mix(bot, top, w.y);
    }
    o_frag_color = vec4(color, 1.0);
}
