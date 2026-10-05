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

/* BC3 (DXT5) block encoder: one fragment per 4x4 texel block of a mip level of
 * the source. The colour endpoints are the inset bounding box of the block, the
 * alpha endpoints its minimum and maximum.
 */

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out uvec4 o_block;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform sampler2D bc3_src_tex;
uniform int bc3_src_level;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

uint pack565(vec3 c)
{
    uvec3 q = uvec3(round(c * vec3(31.0, 63.0, 31.0)));
    return (q.r << 11) | (q.g << 5) | q.b;
}

vec3 unpack565(uint c)
{
    return vec3(float((c >> 11) & 31u) / 31.0, float((c >> 5) & 63u) / 63.0, float(c & 31u) / 31.0);
}

void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * 4;
    ivec2 size = textureSize(bc3_src_tex, bc3_src_level);

    vec4 texels[16];
    vec3 cmin = vec3(1.0), cmax = vec3(0.0);
    float amin = 1.0, amax = 0.0;
    for(int i = 0; i < 16; i++) {
        /* Levels smaller than a block repeat their edge texels */
        ivec2 p = min(base + ivec2(i & 3, i >> 2), size - 1);
        texels[i] = texelFetch(bc3_src_tex, p, bc3_src_level);
        cmin = min(cmin, texels[i].rgb);
        cmax = max(cmax, texels[i].rgb);
        amin = min(amin, texels[i].a);
        amax = max(amax, texels[i].a);
    }

    /* Insetting the box leaves fewer texels clamped at the endpoints */
    vec3 inset = (cmax - cmin) / 16.0;
    uint c0 = pack565(clamp(cmax - inset, 0.0, 1.0));
    uint c1 = pack565(clamp(cmin + inset, 0.0, 1.0));
    vec3 e0 = unpack565(c0), e1 = unpack565(c1);
    vec3 palette[4] = vec3[4](e0, e1, mix(e0, e1, 1.0 / 3.0), mix(e0, e1, 2.0 / 3.0));

    /* Eight-alpha mode (a0 > a1): the six interpolants sit between the endpoints */
    uint a0 = uint(round(amax * 255.0)), a1 = uint(round(amin * 255.0));
    float alphas[8];
    alphas[0] = float(a0) / 255.0;
    alphas[1] = float(a1) / 255.0;
    for(int i = 1; i <= 6; i++) {
        alphas[i + 1] = mix(alphas[0], alphas[1], float(i) / 7.0);
    }

    uint cidx = 0u;
    uint aidx_lo = 0u, aidx_hi = 0u;
    for(int i = 0; i < 16; i++) {

        uint best = 0u;
        float best_dist = 1e9;
        for(uint j = 0u; j < 4u; j++) {
            vec3 d = texels[i].rgb - palette[j];
            float dist = dot(d, d);
            if(dist < best_dist) {
                best_dist = dist;
                best = j;
            }
        }
        cidx |= best << uint(2 * i);

        best = 0u;
        best_dist = 1e9;
        for(uint j = 0u; j < 8u; j++) {
            float dist = abs(texels[i].a - alphas[j]);
            if(dist < best_dist) {
                best_dist = dist;
                best = j;
            }
        }
        /* 3-bit fields of a 48-bit little-endian value, split into two words */
        uint shift = uint(3 * i);
        if(shift < 32u) {
            aidx_lo |= best << shift;
            if(shift > 29u) {
                aidx_hi |= best >> (32u - shift);
            }
        }else{
            aidx_hi |= best << (shift - 32u);
        }
    }

    o_block = uvec4(
        a0 | (a1 << 8) | ((aidx_lo & 0xffffu) << 16),
        (aidx_lo >> 16) | (aidx_hi << 16),
        c0 | (c1 << 16),
        cidx
    );
}
