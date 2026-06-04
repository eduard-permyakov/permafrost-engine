/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2023 Eduard Permyakov 
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


#define SPECULAR_STRENGTH  0.9
#define SPECULAR_SHININESS 12.0
#define SPECULAR_DEPTH_LO  0.20
#define SPECULAR_DEPTH_HI  0.42
#define SPECULAR_VIEW_EDGE 0.92   /* glint fades from the view centre to 0 at this view-axis cos */

#define Y_COORDS_PER_TILE  4 
#define X_COORDS_PER_TILE  8 
#define Z_COORDS_PER_TILE  8 

#define EXTRA_AMBIENT_PER_LEVEL 0.03

#define BLEND_MODE_NOBLEND  0
#define BLEND_MODE_BLUR     1
#define BLEND_MODE_EDGE     2

#define TERRAIN_AMBIENT     float(0.7)
#define TERRAIN_DIFFUSE     vec3(0.9, 0.9, 0.9)
#define TERRAIN_SPECULAR    vec3(1.0, 0.96, 0.88)

#define SHADOW_MAP_BIAS 0.002
#define SHADOW_MULTIPLIER 0.55

#define STATE_UNEXPLORED 0
#define STATE_IN_FOG     1
#define STATE_VISIBLE    2

#define HEIGHT_MAP_WEIGHT 0.107
#define NORMAL_MAP_WEIGHT 0.7
#define NORMAL_DIFFUSE_GAIN 2.8   /* steepen the relief for diffuse shading -> wider light/shadow range */
#define PARALLAX_SCALE      0.18  /* UV-offset depth for parallax from the alpha height map */
#define MAX_TEXTURES      (256)
#define SPLAT_NONE        (-1)
#define MAX_MATERIALS     (16)
#define TILESET_LAYERS    (9)   /* array layers per material: 8 Wang tiles + 1 edge-seam mask */
#define TILESET_WANG      (8)

/*****************************************************************************/
/* INPUTS                                                                    */
/*****************************************************************************/

in VertexToFrag {
         vec2  uv;
    flat int   mat_idx;
         vec3  world_pos;
         vec3  normal;
    flat int   blend_mode;
    flat int   no_bump_map;
    flat int   mid_indices;
    flat ivec2 c1_indices;
    flat ivec2 c2_indices; 
    flat int   tb_indices;
    flat int   lr_indices;
    flat int   wang_index;
         vec4  light_space_pos;
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
uniform mat4 view;

uniform samplerBuffer height_map;
uniform samplerBuffer splat_map;

uniform int shadows_on;
uniform sampler2D shadow_map;

uniform sampler2DArray tex_array0;
uniform sampler2DArray tex_array1;
uniform sampler2DArray tex_array2;
uniform sampler2DArray tex_array3;

uniform sampler2DArray norm_array0;
uniform sampler2DArray norm_array1;
uniform sampler2DArray norm_array2;
uniform sampler2DArray norm_array3;

uniform usamplerBuffer visbuff;
uniform int visbuff_offset;

uniform ivec4 map_resolution;
uniform vec2 map_pos;

uniform int splats[MAX_TEXTURES];

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

vec4 splatted_color_at_pos(vec3 ws_pos, int base_mat_idx, vec4 tex_color);

/*
 * x = chunk_r
 * y = chunk_c
 * z = tile_r
 * a = tile_c
 */
ivec4 tile_desc_at(vec3 ws_pos)
{
    int chunk_w = map_resolution[0];
    int chunk_h = map_resolution[1];
    int tile_w = map_resolution[2];
    int tile_h = map_resolution[3];
    int tiles_per_chunk = tile_w * tile_h;

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

float tf_for_state(uint state)
{
    if(state == uint(STATE_UNEXPLORED))
        return 0.0;
    else if(state == uint(STATE_IN_FOG))
        return 0.5;
    return 1.0;
}

/* (0,0) is in the bottom-left corner, and (1,1) is in the top right corner */
float bilinear_interp_unit_square(float tl, float tr, float bl, float br, vec2 coord)
{
    return bl * (1.0 - coord.x) * (1.0 - coord.y) 
         + br * (coord.x) * (1.0 - coord.y)
         + tl * (1.0 - coord.x) * (coord.y)
         + tr * (coord.x) * (coord.y);
}

uvec4 fetch_safe(usamplerBuffer buff, int idx)
{
    int batch_size = map_resolution[0] * map_resolution[1] * map_resolution[2] * map_resolution[3];
    int buff_size = textureSize(buff);
    int begin_idx = visbuff_offset;
    int end_idx = int(mod(visbuff_offset + batch_size, buff_size));

    if(end_idx > begin_idx) {
        idx = clamp(idx, begin_idx, end_idx-1);
    }else{
        int dist = begin_idx - end_idx;
        if(idx < end_idx + dist/2)
            idx = clamp(idx, 0, end_idx-1);
        else
            idx = clamp(idx, begin_idx, buff_size-1);
    }
    return texelFetch(buff, idx);
}

/* The tint factor is in the range of [0,1]. It is a color multiplier based on 
 * the fog-of-war state of the current and adjacent tiles. */
float tint_factor(ivec4 td, vec2 uv)
{
    float c  = tf_for_state(fetch_safe(visbuff, visbuff_idx(td)).r);
    float tl = tf_for_state(fetch_safe(visbuff, visbuff_idx(tile_relative_desc(td, -1, -1))).r);
    float tr = tf_for_state(fetch_safe(visbuff, visbuff_idx(tile_relative_desc(td, -1, +1))).r);
    float l  = tf_for_state(fetch_safe(visbuff, visbuff_idx(tile_relative_desc(td,  0, -1))).r);
    float r  = tf_for_state(fetch_safe(visbuff, visbuff_idx(tile_relative_desc(td,  0, +1))).r);
    float bl = tf_for_state(fetch_safe(visbuff, visbuff_idx(tile_relative_desc(td, +1, -1))).r);
    float br = tf_for_state(fetch_safe(visbuff, visbuff_idx(tile_relative_desc(td, +1, +1))).r);
    float t  = tf_for_state(fetch_safe(visbuff, visbuff_idx(tile_relative_desc(td, -1,  0))).r);
    float b  = tf_for_state(fetch_safe(visbuff, visbuff_idx(tile_relative_desc(td, +1,  0))).r);

    float tl_corner = (c + t + l + tl) / 4.0;
    float tr_corner = (c + t + r + tr) / 4.0;
    float bl_corner = (c + l + b + bl) / 4.0;
    float br_corner = (c + r + b + br) / 4.0;

    return bilinear_interp_unit_square(tl_corner, tr_corner, bl_corner, br_corner, uv);
}

vec4 wang_color(vec2 uv, vec4 left_color, vec4 top_color, vec4 right_color, vec4 bot_color)
{
    bool left =  (uv.x <  uv.y) && (1.0 - uv.x >= uv.y);
    bool top =   (uv.x <  uv.y) && (1.0 - uv.x <  uv.y);
    bool right = (uv.x >= uv.y) && (1.0 - uv.x <  uv.y);
    bool bot =   (uv.x >= uv.y) && (1.0 - uv.x >= uv.y);

    if(left)
        return left_color;
    else if(top)
        return top_color;
    else if(right)
        return right_color;
    else
        return bot_color;
}

/* Debug rendering of Wang tile indices */
vec4 wang_texture_val(int wang_idx, vec2 uv)
{
    vec4 red = vec4(1.0, 0.0, 0.0, 1.0);
    vec4 green = vec4(0.0, 1.0, 0.0, 1.0);
    vec4 blue = vec4(0.0, 0.0, 1.0, 1.0);
    vec4 yellow = vec4(1.0, 1.0, 0.0, 1.0);

    switch(wang_idx) {
    case 0:
        return wang_color(uv, blue, red, yellow, green);
    case 1:
        return wang_color(uv, blue, green, blue, green);
    case 2:
        return wang_color(uv, yellow, red, yellow, red);
    case 3:
        return wang_color(uv, yellow, green, blue, red);
    case 4:
        return wang_color(uv, yellow, red, blue, green);
    case 5:
        return wang_color(uv, yellow, green, yellow, green);
    case 6:
        return wang_color(uv, blue, red, blue, red);
    case 7:
        return wang_color(uv, blue, green, yellow, red);
    default:
        return vec4(1.0, 0.0, 1.0, 1.0);
    }
}

vec4 texture_val_raw(int mat_idx, int wang_idx, vec2 uv)
{
    int idx = mat_idx * TILESET_LAYERS;
    int size = textureSize(tex_array0, 0).z;
    if(idx < size) {
        return texture(tex_array0, vec3(uv.x, 1.0 - uv.y, idx + wang_idx));
    }
    idx -= size;
    size = textureSize(tex_array1, 0).z;
    if(idx < size) {
        return texture(tex_array1, vec3(uv.x, 1.0 - uv.y, idx + wang_idx));
    }
    idx -= size;
    size = textureSize(tex_array2, 0).z;
    if(idx < size) {
        return texture(tex_array2, vec3(uv.x, 1.0 - uv.y, idx + wang_idx));
    }
    idx -= size;
    size = textureSize(tex_array3, 0).z;
    if(idx < size) {
        return texture(tex_array3, vec3(uv.x, 1.0 - uv.y, idx + wang_idx));
    }
    return vec4(0, 0, 0, 0);
}

/* Sample the per-texture normal map: rgb = tangent-space normal, a = parallax height.
 * Materials without a normal map return a flat normal at mid height (no perturbation). */
vec4 normal_val_raw(int mat_idx, int wang_idx, vec2 uv)
{
    int idx = mat_idx * TILESET_LAYERS;
    int size = textureSize(norm_array0, 0).z;
    if(idx < size)
        return texture(norm_array0, vec3(uv.x, 1.0 - uv.y, idx + wang_idx));
    idx -= size;
    size = textureSize(norm_array1, 0).z;
    if(idx < size)
        return texture(norm_array1, vec3(uv.x, 1.0 - uv.y, idx + wang_idx));
    idx -= size;
    size = textureSize(norm_array2, 0).z;
    if(idx < size)
        return texture(norm_array2, vec3(uv.x, 1.0 - uv.y, idx + wang_idx));
    idx -= size;
    size = textureSize(norm_array3, 0).z;
    if(idx < size)
        return texture(norm_array3, vec3(uv.x, 1.0 - uv.y, idx + wang_idx));
    return vec4(0.5, 0.5, 1.0, 0.5);
}

/* Sample a material's edge-seam mask (the final layer of its tileset block). The
 * 4 channels hold the per-direction fringe masks (R=north, G=east, B=south,
 * A=west); a channel is ~1 inside the material's fringe near that edge. 'img_uv'
 * is in image space (y = 0 is the top/north row), matching how the masks are baked. */
vec4 edge_mask_at(int mat_idx, vec2 img_uv)
{
    int idx = mat_idx * TILESET_LAYERS + TILESET_WANG;
    int size = textureSize(tex_array0, 0).z;
    if(idx < size)
        return texture(tex_array0, vec3(img_uv.x, img_uv.y, idx));
    idx -= size;
    size = textureSize(tex_array1, 0).z;
    if(idx < size)
        return texture(tex_array1, vec3(img_uv.x, img_uv.y, idx));
    idx -= size;
    size = textureSize(tex_array2, 0).z;
    if(idx < size)
        return texture(tex_array2, vec3(img_uv.x, img_uv.y, idx));
    idx -= size;
    size = textureSize(tex_array3, 0).z;
    if(idx < size)
        return texture(tex_array3, vec3(img_uv.x, img_uv.y, idx));
    return vec4(0.0);
}

vec4 texture_val(int mat_idx, int wang_idx, vec2 uv)
{
    vec4 tex_clr = texture_val_raw(mat_idx, wang_idx, uv);
    return splatted_color_at_pos(from_vertex.world_pos, mat_idx, tex_clr);
}

vec4 mixed_texture_val(ivec2 adjacency_mats, int wang_idx, vec2 uv)
{
    vec4 ret = vec4(0.0f);
    for(int i = 0; i < 2; i++) {
    for(int j = 0; j < 4; j++) {
        int idx = (adjacency_mats[i] >> (j * 8)) & 0xff;
        ret += texture_val(idx, wang_idx, uv) * (1.0/8.0);
    }}
    return ret;
}

float splat_alpha_at_pos(vec3 ws_pos)
{
    int chunk_w = map_resolution[0];
    int chunk_h = map_resolution[1];
    int tile_w = map_resolution[2];
    int tile_h = map_resolution[3];
    int tiles_per_chunk = tile_w * tile_h;

    int chunk_x_dist = tile_w * X_COORDS_PER_TILE;
    int chunk_z_dist = tile_h * Z_COORDS_PER_TILE;

    int chunk_r = int(abs(map_pos.y - ws_pos.z) / chunk_z_dist);
    int chunk_c = int(abs(map_pos.x - ws_pos.x) / chunk_x_dist);

    float chunk_base_x = map_pos.x - (chunk_c * chunk_x_dist);
    float chunk_base_z = map_pos.y + (chunk_r * chunk_z_dist);

    float percentu = clamp((chunk_base_x - ws_pos.x) / chunk_x_dist, 0, 1.0);
    float percentv = clamp((ws_pos.z - chunk_base_z) / chunk_z_dist, 0, 1.0);

    int res = int(sqrt(textureSize(splat_map)));
    int buffx = int(percentu * (res - 1));
    int buffy = int(percentv * (res - 1));
    int idx = clamp(buffx * res + buffy, 0, res * res - 1);

    return clamp(texelFetch(splat_map, idx).r + 0.2, 0.0, 1.0);
}

float bilinear_interp_float
(
    float q11, float q12, float q21, float q22, 
    float x1, float x2, 
    float y1, float y2, 
    float x, float y
)
{
    float x2x1, y2y1, x2x, y2y, yy1, xx1;

    x2x1 = x2 - x1;
    y2y1 = y2 - y1;
    x2x = x2 - x;
    y2y = y2 - y;
    yy1 = y - y1;
    xx1 = x - x1;

    return 1.0 / (x2x1 * y2y1) * (
        q11 * x2x * y2y +
        q21 * xx1 * y2y +
        q12 * x2x * yy1 +
        q22 * xx1 * yy1
    );
}

vec4 splatted_color_at_pos(vec3 ws_pos, int base_mat_idx, vec4 tex_color)
{
    int splat_idx = splats[base_mat_idx];
    if(splat_idx == -1)
        return tex_color;

    float splat_alpha = splat_alpha_at_pos(ws_pos);
    float tex_alpha = tex_color.w;
    float total = splat_alpha + tex_alpha;

    float splat_frac = splat_alpha / total;
    vec4 splat_color = texture_val_raw(splat_idx, from_vertex.wang_index, from_vertex.uv);
    return mix(splat_color, tex_color, 1.0 - splat_alpha);
}

vec4 bilinear_interp_vec4
(
    vec4 q11, vec4 q12, vec4 q21, vec4 q22, 
    float x1, float x2, 
    float y1, float y2, 
    float x, float y
)
{
    float x2x1, y2y1, x2x, y2y, yy1, xx1;

    x2x1 = x2 - x1;
    y2y1 = y2 - y1;
    x2x = x2 - x;
    y2y = y2 - y;
    yy1 = y - y1;
    xx1 = x - x1;

    return 1.0 / (x2x1 * y2y1) * (
        q11 * x2x * y2y +
        q21 * xx1 * y2y +
        q12 * x2x * yy1 +
        q22 * xx1 * yy1
    );
}

float shadow_factor(vec4 light_space_pos)
{
    vec3 proj_coords = (light_space_pos.xyz / light_space_pos.w) * 0.5 + 0.5;
    if(proj_coords.x < 0 || proj_coords.x >= textureSize(shadow_map, 0).x)
        return 0.0;
    if(proj_coords.y < 0 || proj_coords.y >= textureSize(shadow_map, 0).y)
        return 0.0;
    if(proj_coords.z > 0.95)
        return 0.0;

    float closest_depth = texture(shadow_map, proj_coords.xy).r;
    float current_depth = proj_coords.z;
    if(current_depth - SHADOW_MAP_BIAS > closest_depth) {
        return 1.0;
    }else {
        return 0.0;
    }
}

float shadow_factor_pcf(vec4 light_space_pos)
{
    vec3 proj_coords = (light_space_pos.xyz / light_space_pos.w) * 0.5 + 0.5;
    if(proj_coords.x < 0 || proj_coords.x >= textureSize(shadow_map, 0).x)
        return 0.0;
    if(proj_coords.y < 0 || proj_coords.y >= textureSize(shadow_map, 0).y)
        return 0.0;
    if(proj_coords.z > 0.95)
        return 0.0;

    float shadow = 0.0;
    vec2 texel_size = 1.0 / textureSize(shadow_map, 0);
    float current_depth = proj_coords.z;

    for(int x = -1; x <= 1; x++) {
    for(int y = -1; y <= 1; y++) {

        float pcf_depth = texture(shadow_map, proj_coords.xy + vec2(x, y) * texel_size).r; 
        shadow += (current_depth - SHADOW_MAP_BIAS > pcf_depth ? 1.0 : 0.0);
    }}

    shadow /= 9.0;
    return shadow;
}

float shadow_factor_poisson(vec4 light_space_pos)
{
    vec2 poisson_disk[4] = vec2[](
        vec2( -0.94201624,  -0.39906216 ),
        vec2(  0.94558609,  -0.76890725 ),
        vec2( -0.094184101, -0.92938870 ),
        vec2(  0.34495938,   0.29387760 )
    );

    vec3 proj_coords = (light_space_pos.xyz / light_space_pos.w) * 0.5 + 0.5;
    if(proj_coords.x < 0 || proj_coords.x >= textureSize(shadow_map, 0).x)
        return 0.0;
    if(proj_coords.y < 0 || proj_coords.y >= textureSize(shadow_map, 0).y)
        return 0.0;
    if(proj_coords.z > 0.95)
        return 0.0;

    float current_depth = proj_coords.z;
    float closest_depth = texture(shadow_map, proj_coords.xy).r;
    float shadow = (current_depth - SHADOW_MAP_BIAS > closest_depth) ? 1.0 : 0.0;
    float visibility = 1.0;

    for(int i = 0; i < 4; i++) {
    
        float depth = texture(shadow_map, proj_coords.xy + poisson_disk[i]/256.0).r; 
        if(current_depth - SHADOW_MAP_BIAS <= depth)
            visibility -= 0.25;
    }
    return shadow * visibility;
}

float height_at_pos(vec3 ws_pos)
{
    int chunk_w = map_resolution[0];
    int chunk_h = map_resolution[1];
    int tile_w = map_resolution[2];
    int tile_h = map_resolution[3];
    int tiles_per_chunk = tile_w * tile_h;

    int chunk_x_dist = tile_w * X_COORDS_PER_TILE;
    int chunk_z_dist = tile_h * Z_COORDS_PER_TILE;

    int chunk_r = int(abs(map_pos.y - ws_pos.z) / chunk_z_dist);
    int chunk_c = int(abs(map_pos.x - ws_pos.x) / chunk_x_dist);

    float chunk_base_x = map_pos.x - (chunk_c * chunk_x_dist);
    float chunk_base_z = map_pos.y + (chunk_r * chunk_z_dist);

    float percentu = clamp((chunk_base_x - ws_pos.x) / chunk_x_dist, 0, 1.0);
    float percentv = clamp((ws_pos.z - chunk_base_z) / chunk_z_dist, 0, 1.0);

    int res = int(sqrt(textureSize(height_map)));
    int buffx = int(percentu * (res - 1));
    int buffy = int(percentv * (res - 1));
    int idx = clamp(buffx * res + buffy, 0, res * res - 1);

    return texelFetch(height_map, idx).r;
}

vec3 normal_at_pos(vec3 ws_pos)
{
    int chunk_w = map_resolution[0];
    int chunk_h = map_resolution[1];
    int tile_w = map_resolution[2];
    int tile_h = map_resolution[3];
    int tiles_per_chunk = tile_w * tile_h;

    int chunk_x_dist = tile_w * X_COORDS_PER_TILE;
    int chunk_z_dist = tile_h * Z_COORDS_PER_TILE;

    int chunk_r = int(abs(map_pos.y - ws_pos.z) / chunk_z_dist);
    int chunk_c = int(abs(map_pos.x - ws_pos.x) / chunk_x_dist);

    float chunk_base_x = map_pos.x - (chunk_c * chunk_x_dist);
    float chunk_base_z = map_pos.y + (chunk_r * chunk_z_dist);

    float percentu = clamp((chunk_base_x - ws_pos.x) / chunk_x_dist, 0, 1.0);
    float percentv = clamp((ws_pos.z - chunk_base_z) / chunk_z_dist, 0, 1.0);

    int res = int(sqrt(textureSize(height_map)));
    int buffx = int(percentu * (res - 1));
    int buffy = int(percentv * (res - 1));
    int idx = clamp(buffx * res + buffy, 0, res * res - 1);

    /* Compute the derivative at the point using the finite difference approximation
     * (central difference).
     */
    int backx = (buffx - 1) % res;
    int backx_idx = clamp(backx * res + buffy, 0, res * res - 1);

    int frontx = (buffx + 1) % res;
    int front_idx = clamp(frontx * res + buffy, 0, res * res - 1);

    int backz = (buffy - 1) % res;
    int backz_idx = clamp(buffx * res + backz, 0, res * res - 1);

    int frontz = (buffy + 1) % res;
    int frontz_idx = clamp(buffx * res + frontz, 0, res * res - 1);

    float x0 = texelFetch(height_map, backx_idx).r;
    float x1 = texelFetch(height_map, front_idx).r;
    float dx = (x1 - x0) / 2.0;

    float z0 = texelFetch(height_map, backz_idx).r;
    float z1 = texelFetch(height_map, front_idx).r;
    float dz = (z1 - z0) / 2.0;

    vec2 xz_normal = normalize(vec2(dx, dz));
    return vec3(xz_normal.x, 1.0, xz_normal.y);
}

vec4 blended_texture_val()
{
    /* 
     * This shader will blend this tile's texture(s) with adjacent tiles' textures 
     * based on adjacency information of neighboring tiles' materials.
     *
     * Our top tile faces are made up of 4 triangles in the following configuration:
     * (Note that the 4 "major" triangles may be further subdivided. In that case, the 
     * triangles it is subdivided to must inherit the flat adjacency attributes. The
     * other attributes will be interpolated. This is a detail not discussed further on.)
     *
     *  +----+----+
     *  | \ top / |
     *  |  \   /  |
     *  + l -+- r +
     *  |  /   \  |
     *  | / bot \ |
     *  +----+----+
     *
     * Each of the 4 triangles has a vertex at the center of the tile. The material indices
     * are 'flat' attributes, so they will be the same for all fragments of a triangle.
     *
     * The UV coordinates for a tile go from (0.0, 1.0) to (1.0, 1.0) in the diagonal corner so
     * we are able to determine which of the 4 triangles this fragment is in by checking 
     * the interpolated UV coordinate.
     *
     * For a single tile, there are 9 reference points on the face of the tile: The 4 corners
     * of the tile, the midpoints of the 4 edges, and the center point.
     *
     *  +---+---+
     *  | 1 | 2 |
     *  +---+---+
     *  | 4 | 3 |
     *  +---+---+ 
     *
     * Based on which quadrant we're in (which can be determined from UV), we will select the 
     * closest 4 points and use bilinear interpolation to select the texture color for this 
     * fragment using the UV coordinate.
     *
     * 'c1_indices' and 'c2_indices' hold the adjacency information for the two non-center vertices 
     * for this triangle. Each element has 8 8-bit indices packed into 64 bits, resulting in 8 
     * indices for each of the two vertices. Each index is the material of one of the 8 triangles 
     * touching the vertex.
     *
     * 'tb_indices' and 'lr_indices' hold the materials for the centers of the edges of the 
     * tile, with 2 8-bit indices for each edge.
     * 
     * 'mid_indices' holds the 2 materials at the central point of the tile in the lowest 8 bits. 
     * Usually the 2 indices are the same except for some corner tiles where half of the tile uses 
     * a different material.
     *
     */

    vec4 tex_color;

    bool bot   = (from_vertex.uv.x > from_vertex.uv.y) && (1.0 - from_vertex.uv.x > from_vertex.uv.y);
    bool top   = (from_vertex.uv.x < from_vertex.uv.y) && (1.0 - from_vertex.uv.x < from_vertex.uv.y);
    bool left  = (from_vertex.uv.x < from_vertex.uv.y) && (1.0 - from_vertex.uv.x > from_vertex.uv.y);
    bool right = (from_vertex.uv.x > from_vertex.uv.y) && (1.0 - from_vertex.uv.x < from_vertex.uv.y);

    bool left_half = from_vertex.uv.x < 0.5f;
    bool bot_half = from_vertex.uv.y < 0.5f;

    /***********************************************************************
     * Set the fragment texture color
     **********************************************************************/
    vec4 color1 = mixed_texture_val(from_vertex.c1_indices, from_vertex.wang_index, from_vertex.uv);
    vec4 color2 = mixed_texture_val(from_vertex.c2_indices, from_vertex.wang_index, from_vertex.uv);

    vec4 tile_color = mix(
        texture_val((from_vertex.mid_indices >> 0) & 0xff, from_vertex.wang_index, from_vertex.uv),
        texture_val((from_vertex.mid_indices >> 8) & 0xff, from_vertex.wang_index, from_vertex.uv),
        0.5f
    );
    vec4 left_center_color =  mix(
        texture_val((from_vertex.lr_indices >> 16) & 0xff, from_vertex.wang_index, from_vertex.uv),
        texture_val((from_vertex.lr_indices >> 24) & 0xff, from_vertex.wang_index, from_vertex.uv),
        0.5f
    );
    vec4 bot_center_color = mix(
        texture_val((from_vertex.tb_indices >> 0) & 0xff, from_vertex.wang_index, from_vertex.uv),
        texture_val((from_vertex.tb_indices >> 8) & 0xff, from_vertex.wang_index, from_vertex.uv),
        0.5f
    );
    vec4 right_center_color = mix(
        texture_val((from_vertex.lr_indices >> 0) & 0xff, from_vertex.wang_index, from_vertex.uv),
        texture_val((from_vertex.lr_indices >> 8) & 0xff, from_vertex.wang_index, from_vertex.uv),
        0.5f
    );
    vec4 top_center_color = mix(
        texture_val((from_vertex.tb_indices >> 16) & 0xff, from_vertex.wang_index, from_vertex.uv),
        texture_val((from_vertex.tb_indices >> 24) & 0xff, from_vertex.wang_index, from_vertex.uv),
        0.5f
    );

    if(top){

        if(left_half)
            tex_color = bilinear_interp_vec4(left_center_color, color1, tile_color, top_center_color,
                0.0f, 0.5f, 0.5f, 1.0f, from_vertex.uv.x, from_vertex.uv.y);        
        else
            tex_color = bilinear_interp_vec4(tile_color, top_center_color, right_center_color, color2,
                0.5f, 1.0f, 0.5f, 1.0f, from_vertex.uv.x, from_vertex.uv.y);
    }else if(bot){

        if(left_half)
            tex_color = bilinear_interp_vec4(color1, left_center_color, bot_center_color, tile_color,
                0.0f, 0.5f, 0.0f, 0.5f, from_vertex.uv.x, from_vertex.uv.y);        
        else
            tex_color = bilinear_interp_vec4(bot_center_color, tile_color, color2, right_center_color,
                0.5f, 1.0f, 0.0f, 0.5f, from_vertex.uv.x, from_vertex.uv.y);
    }else if(left){

        if(bot_half)
            tex_color = bilinear_interp_vec4(color1, left_center_color, bot_center_color, tile_color,
                0.0f, 0.5f, 0.0f, 0.5f, from_vertex.uv.x, from_vertex.uv.y);        
        else
            tex_color = bilinear_interp_vec4(left_center_color, color2, tile_color, top_center_color,
                0.0f, 0.5f, 0.5f, 1.0f, from_vertex.uv.x, from_vertex.uv.y);
    }else if(right){

        if(bot_half)
            tex_color = bilinear_interp_vec4(bot_center_color, tile_color, color1, right_center_color,
                0.5f, 1.0f, 0.0f, 0.5f, from_vertex.uv.x, from_vertex.uv.y);        
        else
            tex_color = bilinear_interp_vec4(tile_color, top_center_color, right_center_color, color2,
                0.5f, 1.0f, 0.5f, 1.0f, from_vertex.uv.x, from_vertex.uv.y);
    }
    return tex_color;
}

void main()
{
    ivec4 td = tile_desc_at(from_vertex.world_pos);
    float tf = tint_factor(td, from_vertex.uv);

    if(tf == 0.0) {
        o_frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    /* Parallax: offset the sampling UV along the tangent-space view by the alpha height,
     * so raised relief occludes recessed. The terrain UV maps to world XZ (normal = +Y),
     * so the tangent-space view is a swizzle of the world view direction. */
    vec3 pview = normalize(view_pos - from_vertex.world_pos);
    float pheight = normal_val_raw(from_vertex.mat_idx, from_vertex.wang_index, from_vertex.uv).a;
    vec2 puv = from_vertex.uv - (vec2(pview.x, pview.z) / max(pview.y, 0.3)) * (pheight - 0.5) * PARALLAX_SCALE;

    vec4 tex_color;

    switch(from_vertex.blend_mode & 0x3) {
    case BLEND_MODE_NOBLEND:
        tex_color = texture_val(from_vertex.mat_idx, from_vertex.wang_index, puv);
        break;
    case BLEND_MODE_BLUR:
        tex_color = blended_texture_val();
        break;
    case BLEND_MODE_EDGE:
        /* This tile keeps its own material crisp; the noisy boundary is drawn as
         * the neighbour's material spilling in (handled below). */
        tex_color = texture_val(from_vertex.mat_idx, from_vertex.wang_index, puv);
        break;
    default:
        o_frag_color = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }

    /* Noisy hard edge: where the cardinally-adjacent tile across this side has EDGE
     * mode (the upper bits of blend_mode), its material spills over the shared edge
     * along the baked minimum-error seam, hard-overwriting this tile's colour in a
     * thin fringe. The side is the top-face quadrant the fragment falls in. */
    if(((from_vertex.blend_mode >> 2) & 0x3) == BLEND_MODE_EDGE) {

        vec2 uv = from_vertex.uv;
        bool e_bot  = (uv.x > uv.y) && (1.0 - uv.x > uv.y);
        bool e_top  = (uv.x < uv.y) && (1.0 - uv.x < uv.y);
        bool e_left = (uv.x < uv.y) && (1.0 - uv.x > uv.y);

        int nbr_mat;
        float seam;
        if(e_top) {
            nbr_mat = (from_vertex.tb_indices >> 16) & 0xff;  /* north neighbour edge-centre */
            seam = edge_mask_at(nbr_mat, vec2(uv.x, uv.y)).b; /* its south fringe */
        }else if(e_bot) {
            nbr_mat = (from_vertex.tb_indices >> 0) & 0xff;   /* south neighbour */
            seam = edge_mask_at(nbr_mat, vec2(uv.x, uv.y)).r; /* its north fringe */
        }else if(e_left) {
            nbr_mat = (from_vertex.lr_indices >> 16) & 0xff;  /* west neighbour */
            seam = edge_mask_at(nbr_mat, vec2(1.0 - uv.x, uv.y)).g; /* its east fringe */
        }else {
            nbr_mat = (from_vertex.lr_indices >> 0) & 0xff;   /* east neighbour */
            seam = edge_mask_at(nbr_mat, vec2(1.0 - uv.x, uv.y)).a; /* its west fringe */
        }
        /* Single seam: only the higher-indexed material overlaps across the edge,
         * so the two textures don't both spill into each other and create stripes.
         * The neighbour shows its own texture cleanly up to this one seam. */
        if(seam > 0.5 && nbr_mat > from_vertex.mat_idx)
            tex_color = texture_val(nbr_mat, from_vertex.wang_index, puv);
    }

    /* Simple alpha test to reject transparent pixels (with mipmapping) */
    tex_color.rgb *= tex_color.a;
    if(tex_color.a <= 0.5)
        discard;

    /* We increase the amount of ambient light that taller tiles get, in order to make
     * them not blend with lower terrain. */
    float height = from_vertex.world_pos.y / Y_COORDS_PER_TILE;

    /* Ambient calculations */
    vec3 ambient = (TERRAIN_AMBIENT + height * EXTRA_AMBIENT_PER_LEVEL) * ambient_color;

    /* Perturb the geometric normal with the heightmap slope (gated by no_bump_map); the
     * per-texture relief is blended in below. A flat (0,0,1) sample is a no-op. */
    vec3 normal = from_vertex.normal;
    if(from_vertex.no_bump_map == 0) {
        vec3 heightmap_normal = normal_at_pos(from_vertex.world_pos);
        normal = normalize(normal + heightmap_normal * HEIGHT_MAP_WEIGHT);
    }

    vec3 tn = normal_val_raw(from_vertex.mat_idx, from_vertex.wang_index, puv).rgb * 2.0 - 1.0;
    /* Tangent frame for the Y-up terrain surface (uv maps to world XZ), built from the
     * current normal so it also works on ramps. */
    vec3 tangent = normalize(cross(vec3(0.0, 0.0, 1.0), normal));
    vec3 bitangent = normalize(cross(normal, tangent));
    vec3 tex_normal = normalize(tangent * tn.x + bitangent * tn.y + normal * tn.z);
    /* Steepen the relief for the diffuse normal to widen its light/shadow range; the
     * specular below uses the unscaled tex_normal. */
    vec3 diff_normal = normalize(tangent * (tn.x * NORMAL_DIFFUSE_GAIN)
                               + bitangent * (tn.y * NORMAL_DIFFUSE_GAIN) + normal * tn.z);
    normal = normalize(mix(normal, diff_normal, NORMAL_MAP_WEIGHT));

    /* Diffuse calculations */
    /* Always use light direction relative to world origin. Otherwise different parts of a
     * large map have too distinct differences in lighting */
    vec3 light_dir = normalize(light_pos);
    float diff = max(dot(normal, light_dir), 0.0);
    vec3 diffuse = light_color * (diff * TERRAIN_DIFFUSE);

    /* Stylised crevice glint: a Blinn-Phong highlight off the relief, gated to the deep
     * (steep) parts of the normal map so it lands on grooves and cracks, not flat areas. */
    vec3 view_dir = normalize(view_pos - from_vertex.world_pos);
    vec3 halfway = normalize(light_dir + view_dir);
    float spec = pow(max(dot(tex_normal, halfway), 0.0), SPECULAR_SHININESS);
    float crevice = smoothstep(SPECULAR_DEPTH_LO, SPECULAR_DEPTH_HI, length(tn.xy));
    /* Fade by angle from the camera's view axis: full at the screen centre, zero past
     * SPECULAR_VIEW_EDGE. */
    vec3 cam_fwd = -normalize(vec3(view[0][2], view[1][2], view[2][2]));
    float centered = dot(cam_fwd, normalize(from_vertex.world_pos - view_pos));
    float center_falloff = smoothstep(SPECULAR_VIEW_EDGE, 1.0, centered);
    vec3 specular = SPECULAR_STRENGTH * light_color * spec * crevice * center_falloff * TERRAIN_SPECULAR;

    vec4 final_color = vec4( (ambient + diffuse) * tex_color.xyz + specular, 1.0);
    if(!bool(shadows_on)) {
        o_frag_color = final_color * tf;        
        return;
    }

    /* Shadow caclulations */
    float shadow = shadow_factor_poisson(from_vertex.light_space_pos);
    if(shadow > 0.0) {
        o_frag_color = vec4(final_color.xyz * (SHADOW_MULTIPLIER + (1.0 - shadow) * (1.0 - SHADOW_MULTIPLIER)), 1.0);
    }else{
        o_frag_color = vec4(final_color.xyz, 1.0);
    }

    o_frag_color = o_frag_color * tf;
}

