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

/*****************************************************************************/
/* INPUTS                                                                    */
/*****************************************************************************/

in VertexToFrag {
    vec2 uv;
    vec3 world_pos;
    vec3 normal;
    vec4 light_space_pos;
} from_vertex;

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out vec4 o_frag_color;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

#define SHADOW_MAP_BIAS  0.002
#define SHADOW_MULTIPLIER 0.55

uniform sampler2D texture0;
uniform vec3      ambient_color;
uniform vec3      light_pos;
uniform vec3      light_color;
uniform int       shadows_on;
uniform sampler2D shadow_map;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

float shadow_factor_poisson(vec4 light_space_pos)
{
    vec2 poisson_disk[4] = vec2[](
        vec2( -0.94201624,  -0.39906216 ),
        vec2(  0.94558609,  -0.76890725 ),
        vec2( -0.094184101, -0.92938870 ),
        vec2(  0.34495938,   0.29387760 )
    );

    vec3 proj_coords = (light_space_pos.xyz / light_space_pos.w) * 0.5 + 0.5;
    if(proj_coords.x < 0.0 || proj_coords.x >= float(textureSize(shadow_map, 0).x))
        return 0.0;
    if(proj_coords.y < 0.0 || proj_coords.y >= float(textureSize(shadow_map, 0).y))
        return 0.0;
    if(proj_coords.z > 0.95)
        return 0.0;

    float current_depth = proj_coords.z;
    float closest_depth = texture(shadow_map, proj_coords.xy).r;
    float shadow = (current_depth - SHADOW_MAP_BIAS > closest_depth) ? 1.0 : 0.0;
    float visibility = 1.0;

    for(int i = 0; i < 4; i++) {
        float depth = texture(shadow_map, proj_coords.xy + poisson_disk[i] / 256.0).r;
        if(current_depth - SHADOW_MAP_BIAS <= depth)
            visibility -= 0.25;
    }
    return shadow * visibility;
}

void main()
{
    vec4 tex_color = texture(texture0, from_vertex.uv);
    if(tex_color.a < 0.1)
        discard;

    /* Texture is stored premultiplied; recover original color for lighting */
    vec3 base_color = tex_color.rgb / tex_color.a;

    /* Ambient */
    vec3 ambient = ambient_color * base_color;

    /* Diffuse - use abs(dot) so both faces of the cross-quad are lit */
    vec3 light_dir = normalize(light_pos - from_vertex.world_pos);
    float diff     = abs(dot(from_vertex.normal, light_dir));
    vec3 diffuse   = diff * light_color * base_color;

    vec4 final_color = vec4(ambient + diffuse, tex_color.a);
    if(!bool(shadows_on)) {
        o_frag_color = final_color;
        return;
    }

    float shadow = shadow_factor_poisson(from_vertex.light_space_pos);
    if(shadow > 0.0) {
        o_frag_color = vec4(final_color.rgb * (SHADOW_MULTIPLIER + (1.0 - shadow) * (1.0 - SHADOW_MULTIPLIER)), final_color.a);
    } else {
        o_frag_color = final_color;
    }
}
