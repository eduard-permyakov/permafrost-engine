/*
 *  This file is part of Permafrost Engine. 
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


#define SPECULAR_STRENGTH  0.5
#define SPECULAR_SHININESS 2

#define Y_COORDS_PER_TILE  4 
#define EXTRA_AMBIENT_PER_LEVEL 0.03

#define BLEND_MODE_NOBLEND  0
#define BLEND_MODE_BLUR     1

#define TERRAIN_AMBIENT     float(0.7)
#define TERRAIN_DIFFUSE     vec3(0.9, 0.9, 0.9)
#define TERRAIN_SPECULAR    vec3(0.1, 0.1, 0.1)

#define SHADOW_MAP_BIAS 0.002
#define SHADOW_MULTIPLIER 0.7

/*****************************************************************************/
/* INPUTS                                                                    */
/*****************************************************************************/

in VertexToFrag {
         vec2  uv;
    flat int   mat_idx;
         vec3  world_pos;
         vec3  normal;
    flat int   blend_mode;
    flat ivec4 adjacent_mat_indices;
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

uniform sampler2D shadow_map;

uniform sampler2DArray tex_array0;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

vec4 texture_val(int mat_idx, vec2 uv)
{
    return texture(tex_array0, vec3(uv, mat_idx));
}

vec4 mixed_texture_val(int adjacency_mats, vec2 uv)
{
    vec4 ret = vec4(0.0f);
    for(int i = 0; i < 8; i++) {
        int idx = (adjacency_mats >> (i * 4)) & 0xf;
        ret += texture_val(idx, uv) * (1.0/8.0);
    }
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

float shadow_factor(vec4 light_space_pos)
{
    vec3 proj_coords = (light_space_pos.xyz / light_space_pos.w) * 0.5 + 0.5;
    float closest_depth = texture(shadow_map, proj_coords.xy).r;
    float current_depth = proj_coords.z;
    if(current_depth - SHADOW_MAP_BIAS > closest_depth) {
        return 1.0;
    }else {
        return 0.0;
    }
}

void main()
{
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
         * Each of the 4 triangles has a vertex at the center of the tile. The 'adjacent_mat_indices'
         * is a 'flat' attribute, so it will be the same for all fragments of a triangle.
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
         * The first two elements of 'adjacent_mat_indices' hold the adjacency information for the 
         * two non-center vertices for this triangle. Each element has 8 4-bit indices packed into the 
         * least significant 32 bits, resulting in 8 indices for each of the two vertices. Each index
         * is the material of one of the 8 triangles touching the vertex.
         *
         * The next element of 'adjacent_mat_indices' holds the materials for the centers of the 
         * edges of the tile, with 2 4-bit indices for each edge.
         * 
         * The last element of 'adjacent_mat_indices' holds the 2 materials at the central point of 
         * the tile in the lowest 8 bits. Usually the 2 indices are the same except for some corner tiles
         * where half of the tile uses a different material.
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
        vec4 color1 = mixed_texture_val(from_vertex.adjacent_mat_indices[0], from_vertex.uv);
        vec4 color2 = mixed_texture_val(from_vertex.adjacent_mat_indices[1], from_vertex.uv);

        vec4 tile_color = mix(
            texture_val((from_vertex.adjacent_mat_indices[3] >> 0) & 0xf, from_vertex.uv), 
            texture_val((from_vertex.adjacent_mat_indices[3] >> 4) & 0xf, from_vertex.uv), 
            0.5f
        );
        vec4 left_center_color =  mix(
            texture_val((from_vertex.adjacent_mat_indices[2] >> 0) & 0xf, from_vertex.uv), 
            texture_val((from_vertex.adjacent_mat_indices[2] >> 4) & 0xf, from_vertex.uv), 
            0.5f
        );
        vec4 bot_center_color = mix(
            texture_val((from_vertex.adjacent_mat_indices[2] >> 8) & 0xf, from_vertex.uv),
            texture_val((from_vertex.adjacent_mat_indices[2] >> 12) & 0xf, from_vertex.uv),
            0.5f
        );
        vec4 right_center_color = mix(
            texture_val((from_vertex.adjacent_mat_indices[2] >> 16) & 0xf, from_vertex.uv), 
            texture_val((from_vertex.adjacent_mat_indices[2] >> 20) & 0xf, from_vertex.uv), 
            0.5f
        );
        vec4 top_center_color = mix(
            texture_val((from_vertex.adjacent_mat_indices[2] >> 24) & 0xf, from_vertex.uv), 
            texture_val((from_vertex.adjacent_mat_indices[2] >> 28) & 0xf, from_vertex.uv), 
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
        tex_color = vec4(1.0, 0.0, 1.0, 1.0);
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
    vec3 light_dir = normalize(light_pos - from_vertex.world_pos);  
    float diff = max(dot(from_vertex.normal, light_dir), 0.0);
    vec3 diffuse = light_color * (diff * TERRAIN_DIFFUSE);

    /* Specular calculations */
    vec3 view_dir = normalize(view_pos - from_vertex.world_pos);
    vec3 reflect_dir = reflect(-light_dir, from_vertex.normal);  
    float spec = pow(max(dot(view_dir, reflect_dir), 0.0), SPECULAR_SHININESS);
    vec3 specular = SPECULAR_STRENGTH * light_color * (spec * TERRAIN_SPECULAR);

    vec4 final_color = vec4( (ambient + diffuse + specular) * tex_color.xyz, 1.0);
    float shadow = shadow_factor(from_vertex.light_space_pos);
    if(shadow > 0.0) {
        o_frag_color = vec4(final_color.xyz * SHADOW_MULTIPLIER, 1.0);
    }else{
        o_frag_color = vec4(final_color.xyz, 1.0);
    }
}

