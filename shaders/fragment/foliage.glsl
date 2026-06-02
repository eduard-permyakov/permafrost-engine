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

#define X_COORDS_PER_TILE  8
#define Z_COORDS_PER_TILE  8

#define STATE_UNEXPLORED 0
#define STATE_IN_FOG     1
#define STATE_VISIBLE    2

uniform usamplerBuffer visbuff;
uniform int            visbuff_offset;
uniform ivec4          map_resolution;
uniform vec2           map_pos;

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

/* Returns the index into the fog-of-war visibility buffer for the tile under
 * the given world-space position.
 * (x = chunk_r, y = chunk_c, z = tile_r, a = tile_c) */
ivec4 tile_desc_at(vec3 ws_pos)
{
    int tile_w = map_resolution[2];
    int tile_h = map_resolution[3];

    int chunk_x_dist = tile_w * X_COORDS_PER_TILE;
    int chunk_z_dist = tile_h * Z_COORDS_PER_TILE;

    int chunk_r = int(abs(map_pos.y - ws_pos.z) / chunk_z_dist);
    int chunk_c = int(abs(map_pos.x - ws_pos.x) / chunk_x_dist);

    int chunk_base_x = int(map_pos.x - (chunk_c * chunk_x_dist));
    int chunk_base_z = int(map_pos.y + (chunk_r * chunk_z_dist));

    int tile_c = int(abs(chunk_base_x - ws_pos.x) / X_COORDS_PER_TILE);
    int tile_r = int(abs(chunk_base_z - ws_pos.z) / Z_COORDS_PER_TILE);

    return ivec4(chunk_r, chunk_c, tile_r, tile_c);
}

int visbuff_idx(ivec4 td)
{
    int chunk_w = map_resolution[0];
    int tile_w = map_resolution[2];
    int tile_h = map_resolution[3];
    int tiles_per_chunk = tile_w * tile_h;

    return visbuff_offset + (td.x * tiles_per_chunk * chunk_w)
                          + (td.y * tiles_per_chunk)
                          + (td.z * tile_w)
                          + td.a;
}

/* Color multiplier for the tile's fog-of-war state: hidden when unexplored,
 * dimmed in fog, full when visible. */
float tf_for_state(uint state)
{
    if(state == uint(STATE_UNEXPLORED))
        return 0.0;
    else if(state == uint(STATE_IN_FOG))
        return 0.5;
    return 1.0;
}

/* Clamps on the absolute tile index, so a neighbour past the map edge resolves
 * to the edge tile itself rather than wrapping to a far tile in the same chunk.
 * This keeps an unexplored map border reading as unexplored. */
ivec4 tile_relative_desc(ivec4 desc, int dr, int dc)
{
    int abs_r = desc.x * map_resolution.z + desc.z + dr;
    int abs_c = desc.y * map_resolution.a + desc.a + dc;

    abs_r = clamp(abs_r, 0, map_resolution.x * map_resolution.z - 1);
    abs_c = clamp(abs_c, 0, map_resolution.y * map_resolution.a - 1);

    return ivec4(
        abs_r / map_resolution.z,
        abs_c / map_resolution.a,
        int(mod(abs_r, map_resolution.z)),
        int(mod(abs_c, map_resolution.a))
    );
}

/* (0,0) is the bottom-left corner, (1,1) the top-right. */
float bilinear_interp_unit_square(float tl, float tr, float bl, float br, vec2 coord)
{
    return bl * (1.0 - coord.x) * (1.0 - coord.y)
         + br * (coord.x) * (1.0 - coord.y)
         + tl * (1.0 - coord.x) * (coord.y)
         + tr * (coord.x) * (coord.y);
}

/* The fragment's fractional position within its tile, oriented to match the
 * corner layout above: x runs left->right (toward increasing column), y is 1
 * at the lower-row edge. Derived from the world position the same way as
 * tile_desc_at, since the foliage uv is the billboard uv, not a tile uv. */
vec2 tile_local_uv(vec3 ws_pos)
{
    int tile_w = map_resolution[2];
    int tile_h = map_resolution[3];

    int chunk_x_dist = tile_w * X_COORDS_PER_TILE;
    int chunk_z_dist = tile_h * Z_COORDS_PER_TILE;

    int chunk_r = int(abs(map_pos.y - ws_pos.z) / chunk_z_dist);
    int chunk_c = int(abs(map_pos.x - ws_pos.x) / chunk_x_dist);

    float chunk_base_x = map_pos.x - (chunk_c * chunk_x_dist);
    float chunk_base_z = map_pos.y + (chunk_r * chunk_z_dist);

    float u = mod(abs(chunk_base_x - ws_pos.x), float(X_COORDS_PER_TILE)) / float(X_COORDS_PER_TILE);
    float v = mod(abs(chunk_base_z - ws_pos.z), float(Z_COORDS_PER_TILE)) / float(Z_COORDS_PER_TILE);

    return vec2(u, 1.0 - v);
}

/* Smoothly-blended fog tint: samples the current tile and its 8 neighbours and
 * bilinearly interpolates, so the foliage fades across tile boundaries exactly
 * like the terrain beneath it (no hard edge between light and dark tiles). */
float fog_tint_factor(vec3 ws_pos)
{
    ivec4 td = tile_desc_at(ws_pos);

    float c  = tf_for_state(texelFetch(visbuff, visbuff_idx(td)).r);
    float t  = tf_for_state(texelFetch(visbuff, visbuff_idx(tile_relative_desc(td, -1,  0))).r);
    float b  = tf_for_state(texelFetch(visbuff, visbuff_idx(tile_relative_desc(td, +1,  0))).r);
    float l  = tf_for_state(texelFetch(visbuff, visbuff_idx(tile_relative_desc(td,  0, -1))).r);
    float r  = tf_for_state(texelFetch(visbuff, visbuff_idx(tile_relative_desc(td,  0, +1))).r);
    float tl = tf_for_state(texelFetch(visbuff, visbuff_idx(tile_relative_desc(td, -1, -1))).r);
    float tr = tf_for_state(texelFetch(visbuff, visbuff_idx(tile_relative_desc(td, -1, +1))).r);
    float bl = tf_for_state(texelFetch(visbuff, visbuff_idx(tile_relative_desc(td, +1, -1))).r);
    float br = tf_for_state(texelFetch(visbuff, visbuff_idx(tile_relative_desc(td, +1, +1))).r);

    float tl_corner = (c + t + l + tl) / 4.0;
    float tr_corner = (c + t + r + tr) / 4.0;
    float bl_corner = (c + l + b + bl) / 4.0;
    float br_corner = (c + r + b + br) / 4.0;

    return bilinear_interp_unit_square(tl_corner, tr_corner, bl_corner, br_corner, tile_local_uv(ws_pos));
}

void main()
{
    /* Clip foliage to the map bounds. Billboards on the edge tiles can extend a
     * little past the map, where they would otherwise poke through the fog. */
    vec2 map_delta = vec2(map_pos.x - from_vertex.world_pos.x,
                          from_vertex.world_pos.z - map_pos.y);
    vec2 map_extent = vec2(map_resolution[0] * map_resolution[2] * X_COORDS_PER_TILE,
                           map_resolution[1] * map_resolution[3] * Z_COORDS_PER_TILE);
    if(map_delta.x < 0.0 || map_delta.x > map_extent.x
    || map_delta.y < 0.0 || map_delta.y > map_extent.y)
        discard;

    /* Foliage is terrain decoration: hide it in unexplored areas and dim it in
     * fog, blended across tile boundaries to mirror the terrain beneath it. */
    float fog_tf = fog_tint_factor(from_vertex.world_pos);
    if(fog_tf == 0.0)
        discard;

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
    final_color.rgb *= fog_tf;
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
