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

#define SHADOW_MAP_BIAS   0.002
#define SHADOW_MULTIPLIER 0.55

/* The mesh shaders' lighting, which the sprites reproduce */
#define SPECULAR_SHININESS 2
#define AMBIENT_SCALE      0.55
#define DIRECT_SCALE       1.5

/*****************************************************************************/
/* INPUTS                                                                    */
/*****************************************************************************/

in VertexToFrag{
         vec2  uv;
         vec3  ws_pos;
    flat int   layer;
    flat float depth_scale;
    flat float shadow_offset;
    flat vec3  forward;
    flat float bake_turn;
}from_vertex;

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out vec4 o_frag_color;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform mat4 view;
uniform mat4 projection;

uniform sampler2DArray tex_array0;
/* The bake's window depths, linear in the view distance */
uniform sampler2DArray tex_array1;
/* The baked surface: octahedral normal, then the ambient and specular
 * strengths relative to the diffuse colour, a quarter scale
 */
uniform sampler2DArray tex_array2;

/* Decodes a baked depth into the offset of the surface from the card plane
 * along the view direction, in model units: depth * x + y
 */
uniform vec2  bb_depth_decode;
uniform float bb_depth_extent;

/* The shadows cast by the rest of the scene, received like a mesh receives
 * them
 */
uniform vec3      light_pos;
uniform vec3      light_color;
uniform vec3      ambient_color;
uniform vec3      view_pos;
uniform mat4      light_space_transform;
uniform int       shadows_on;
uniform sampler2D shadow_map;

/* The look the world is graded to, for billboards drawn over the graded world */
uniform sampler3D post_grade_lut;
uniform int       post_grade_on;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

bool in_shadow(vec3 ws_pos)
{
    vec4 light_space_pos = light_space_transform * vec4(ws_pos, 1.0);
    vec3 proj_coords = (light_space_pos.xyz / light_space_pos.w) * 0.5 + 0.5;
    if(any(lessThan(proj_coords.xy, vec2(0.0))) || any(greaterThan(proj_coords.xy, vec2(1.0))))
        return false;
    if(proj_coords.z > 0.95)
        return false;
    return (proj_coords.z - SHADOW_MAP_BIAS > texture(shadow_map, proj_coords.xy).r);
}

float receive_offset()
{
    vec3 ndc_per_wu = vec3(light_space_transform[0][0], light_space_transform[1][0],
                           light_space_transform[2][0]);
    float texel = 2.0 / (length(ndc_per_wu) * float(textureSize(shadow_map, 0).x));
    return max(from_vertex.shadow_offset, 2.0 * texel);
}

vec2 sign_not_zero(vec2 v)
{
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec3 octahedral_decode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if(n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * sign_not_zero(n.xy);
    }
    return normalize(n);
}

/* The bake turned about the vertical to the world: increasing every azimuth
 * atan(x, z) by 'turn'
 */
vec3 turn_azimuth(vec3 v, float turn)
{
    float c = cos(turn), s = sin(turn);
    return vec3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
}

void main()
{
    vec4 texel = texture(tex_array0, vec3(from_vertex.uv, float(from_vertex.layer)));
    if(texel.a < 0.5)
        discard;

    /* The baked surface point; texels the bake left clear lie on the card plane */
    float depth = texture(tex_array1, vec3(from_vertex.uv, float(from_vertex.layer))).r;
    float off = (depth < 1.0)
              ? clamp(depth * bb_depth_decode.x + bb_depth_decode.y, -bb_depth_extent, bb_depth_extent)
              : 0.0;
    off *= from_vertex.depth_scale;
    vec3 surface = from_vertex.ws_pos + from_vertex.forward * off;

    vec4 clip = projection * view * vec4(surface, 1.0);
    gl_FragDepth = 0.5 * (clip.z / clip.w) + 0.5;

    /* The sprite's own silhouette in the shadow map is as coarse as its own
     * texels or the shadow map's, whichever is larger; looking up from that
     * far toward the light keeps the surface from shadowing itself, leaving
     * the self-shadow to the bake
     */
    /* The cells are cleared to an empty surface, so minified texels carry
     * theirs scaled by their coverage
     */
    vec4 baked = texture(tex_array2, vec3(from_vertex.uv, float(from_vertex.layer)));
    vec3 albedo = texel.rgb / texel.a;
    vec3 normal = turn_azimuth(octahedral_decode((baked.xy - 0.5) * 2.0 / texel.a),
                               from_vertex.bake_turn);
    float ambient = baked.z * 4.0 / texel.a;
    float specular = baked.w * 4.0 / texel.a;

    vec3 light_dir = normalize(light_pos);
    float diff = max(dot(normal, light_dir), 0.0);
    vec3 view_dir = normalize(view_pos - surface);
    float spec = pow(max(dot(view_dir, reflect(-light_dir, normal)), 0.0), SPECULAR_SHININESS);
    vec3 color = albedo * (AMBIENT_SCALE * ambient * ambient_color
                         + DIRECT_SCALE * light_color * (diff + spec * specular));

    if(shadows_on != 0 && in_shadow(surface + light_dir * receive_offset())) {
        color *= SHADOW_MULTIPLIER;
    }
    if(post_grade_on != 0) {
        vec3 dim = vec3(textureSize(post_grade_lut, 0));
        color = texture(post_grade_lut, clamp(color, 0.0, 1.0) * ((dim - 1.0) / dim) + 0.5 / dim).rgb;
    }
    o_frag_color = vec4(color, 1.0);
}
