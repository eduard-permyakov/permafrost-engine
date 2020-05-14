/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2020 Eduard Permyakov 
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


#define SPECULAR_STRENGTH  0.5
#define SPECULAR_SHININESS 2

#define Y_COORDS_PER_TILE  4 
#define X_COORDS_PER_TILE  8 
#define Z_COORDS_PER_TILE  8 

#define EXTRA_AMBIENT_PER_LEVEL 0.03

#define BLEND_MODE_NOBLEND  0
#define BLEND_MODE_BLUR     1

#define TERRAIN_AMBIENT     float(0.7)
#define TERRAIN_DIFFUSE     vec3(0.9, 0.9, 0.9)
#define TERRAIN_SPECULAR    vec3(0.1, 0.1, 0.1)

#define STATE_UNEXPLORED 0
#define STATE_IN_FOG     1
#define STATE_VISIBLE    2

/*****************************************************************************/
/* INPUTS                                                                    */
/*****************************************************************************/

in VertexToFrag {
         vec2  uv;
         vec3  world_pos;
         vec3  normal;
    flat int   mat_idx;
    flat int   blend_mode;
    flat int   mid_indices;
    flat ivec2 c1_indices;
    flat ivec2 c2_indices; 
    flat int   tb_indices;
    flat int   lr_indices;
}from_vertex;

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

layout(location = 0) out vec4 o_frag_color;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform vec3 ambient_color;
uniform vec3 light_color;
uniform vec3 light_pos;
uniform vec3 view_pos;

uniform sampler2DArray tex_array0;

uniform usamplerBuffer visbuff;
uniform int visbuff_offset;

uniform ivec4 map_resolution;
uniform vec2 map_pos;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

int visbuff_idx(vec3 ws_pos)
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

    return visbuff_offset + (chunk_r * tiles_per_chunk * chunk_w) 
                          + (chunk_c * tiles_per_chunk) 
                          + (tile_r * tile_w) 
                          + tile_c;
}

vec4 texture_val(int mat_idx, vec2 uv)
{
    return texture(tex_array0, vec3(uv, mat_idx));
}

vec4 mixed_texture_val(ivec2 adjacency_mats, vec2 uv)
{
    vec4 ret = vec4(0.0f);
    for(int i = 0; i < 2; i++) {
    for(int j = 0; j < 4; j++) {
        int idx = (adjacency_mats[i] >> (j * 8)) & 0xff;
        ret += texture_val(idx, uv) * (1.0/8.0);
    }}
    return ret;
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

void main()
{
    int idx = visbuff_idx(from_vertex.world_pos);
    int frag_state = int(texelFetch(visbuff, idx).r);

    if(frag_state == STATE_UNEXPLORED) {
        o_frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec4 tex_color;

    switch(from_vertex.blend_mode) {
    case BLEND_MODE_NOBLEND:
        tex_color = texture_val(from_vertex.mat_idx, from_vertex.uv);
        break;
    case BLEND_MODE_BLUR:

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
         * Based on which quadrant we're in (which can be determined from UV), we will select the closest 
         * 4 points and use bilinear interpolation to select the texture color for this fragment using 
         * the UV coordinate.
         *
         * 'c1_indices' and 'c2_indices' hold the adjacency information for the two non-center vertices 
         * for this triangle. Each element has 8 8-bit indices packed into 64 bits, resulting in 8 indices 
         * for each of the two vertices. Each index is the material of one of the 8 triangles touching the 
         * vertex.
         *
         * 'tb_indices' and 'lr_indices' hold the materials for the centers of the edges of the tile, with 
         * 2 8-bit indices for each edge.
         * 
         * 'mid_indices' holds the 2 materials at the central point of the tile in the lowest 8 bits. 
         * Usually the 2 indices are the same except for some corner tiles where half of the tile uses 
         * a different material.
         *
         */

        bool bot   = (from_vertex.uv.x > from_vertex.uv.y) && (1.0 - from_vertex.uv.x > from_vertex.uv.y);
        bool top   = (from_vertex.uv.x < from_vertex.uv.y) && (1.0 - from_vertex.uv.x < from_vertex.uv.y);
        bool left  = (from_vertex.uv.x < from_vertex.uv.y) && (1.0 - from_vertex.uv.x > from_vertex.uv.y);
        bool right = (from_vertex.uv.x > from_vertex.uv.y) && (1.0 - from_vertex.uv.x < from_vertex.uv.y);

        bool left_half = from_vertex.uv.x < 0.5f;
        bool bot_half = from_vertex.uv.y < 0.5f;

        /***********************************************************************
         * Set the fragment texture color
         **********************************************************************/
        vec4 color1 = mixed_texture_val(from_vertex.c1_indices, from_vertex.uv);
        vec4 color2 = mixed_texture_val(from_vertex.c2_indices, from_vertex.uv);

        vec4 tile_color = mix(
            texture_val((from_vertex.mid_indices >> 0) & 0xff, from_vertex.uv),
            texture_val((from_vertex.mid_indices >> 8) & 0xff, from_vertex.uv),
            0.5f
        );
        vec4 left_center_color =  mix(
            texture_val((from_vertex.lr_indices >> 16) & 0xff, from_vertex.uv),
            texture_val((from_vertex.lr_indices >> 24) & 0xff, from_vertex.uv),
            0.5f
        );
        vec4 bot_center_color = mix(
            texture_val((from_vertex.tb_indices >> 0) & 0xff, from_vertex.uv),
            texture_val((from_vertex.tb_indices >> 8) & 0xff, from_vertex.uv),
            0.5f
        );
        vec4 right_center_color = mix(
            texture_val((from_vertex.lr_indices >> 0) & 0xff, from_vertex.uv),
            texture_val((from_vertex.lr_indices >> 8) & 0xff, from_vertex.uv),
            0.5f
        );
        vec4 top_center_color = mix(
            texture_val((from_vertex.tb_indices >> 16) & 0xff, from_vertex.uv),
            texture_val((from_vertex.tb_indices >> 24) & 0xff, from_vertex.uv),
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

        break;
    default:
        o_frag_color = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }

    /* Simple alpha test to reject transparent pixels */
    if(tex_color.a == 0.0)
        discard;

    /* We increase the amount of ambient light that taller tiles get, in order to make
     * them not blend with lower terrain. */
    float height = from_vertex.world_pos.y / Y_COORDS_PER_TILE;

    /* Ambient calculations */
    vec3 ambient = (TERRAIN_AMBIENT + height * EXTRA_AMBIENT_PER_LEVEL) * ambient_color;

    /* Diffuse calculations */
    /* Always use light direction relative to world origin. Otherwise different parts of a
     * large map have too distinct differences in lighting */
    vec3 light_dir = normalize(light_pos);  
    float diff = max(dot(from_vertex.normal, light_dir), 0.0);
    vec3 diffuse = light_color * (diff * TERRAIN_DIFFUSE);

    /* Specular calculations */
    vec3 view_dir = normalize(view_pos - from_vertex.world_pos);
    vec3 reflect_dir = reflect(-light_dir, from_vertex.normal);  
    float spec = pow(max(dot(view_dir, reflect_dir), 0.0), SPECULAR_SHININESS);
    vec3 specular = SPECULAR_STRENGTH * light_color * (spec * TERRAIN_SPECULAR);

    o_frag_color = vec4( (ambient + diffuse + specular) * tex_color.xyz, 1.0);

    if(frag_state == STATE_IN_FOG) {
        o_frag_color = o_frag_color * 0.5;
    }
}

