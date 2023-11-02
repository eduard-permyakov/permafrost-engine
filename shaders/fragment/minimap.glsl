/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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

#define STATE_UNEXPLORED 0
#define STATE_IN_FOG     1
#define STATE_VISIBLE    2

/*****************************************************************************/
/* INPUTS                                                                    */
/*****************************************************************************/

in VertexToFrag {
         vec2 uv;
    flat int  mat_idx;
         vec3 world_pos;
         vec3 normal;
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

uniform sampler2D texture0;

uniform usamplerBuffer visbuff;
uniform int visbuff_offset;

uniform ivec4 map_resolution;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

int visbuff_idx(vec2 uv)
{
    int chunk_w = map_resolution[0];
    int chunk_h = map_resolution[1];
    int tile_w = map_resolution[2];
    int tile_h = map_resolution[3];
    int tiles_per_chunk = tile_w * tile_h;

    int chunk_r = int(uv.y * chunk_h);
    int chunk_c = int(uv.x * chunk_w);

    float chunk_height = 1.0 / chunk_h;
    float chunk_width = 1.0 / chunk_w;

    int tile_r = int(mod(uv.y, chunk_height)/chunk_height * tile_h);
    int tile_c = int(mod(uv.x, chunk_width)/chunk_width * tile_w);

    return visbuff_offset + (chunk_r * tiles_per_chunk * chunk_w) 
                          + (chunk_c * tiles_per_chunk) 
                          + (tile_r * tile_w) 
                          + tile_c;
}

void main()
{
    vec4 tex_color = texture(texture0,  from_vertex.uv);
    int idx = visbuff_idx(from_vertex.uv);
    int frag_state = int(texelFetch(visbuff, idx).r);
    
    if(frag_state == STATE_UNEXPLORED) {
        o_frag_color = vec4(0.0, 0.0, 0.0, 1.0);
    }else if(frag_state == STATE_IN_FOG) {
        o_frag_color = vec4(tex_color.xyz * 0.5, 1.0);
    }else if(frag_state == STATE_VISIBLE) {
        o_frag_color = vec4(tex_color.xyz, 1.0);
    }
}

