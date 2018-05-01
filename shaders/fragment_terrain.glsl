/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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
 */

#version 330 core

#define MAX_MATERIALS 8

/* TODO: Make these as material parameters */
#define SPECULAR_STRENGTH  0.5
#define SPECULAR_SHININESS 2

#define Y_COORDS_PER_TILE  4 
#define EXTRA_AMBIENT_PER_LEVEL 0.03

#define BLEND_MODE_NOBLEND  0
#define BLEND_MODE_BLUR     1

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

uniform sampler2D texture0;
uniform sampler2D texture1;
uniform sampler2D texture2;
uniform sampler2D texture3;
uniform sampler2D texture4;
uniform sampler2D texture5;
uniform sampler2D texture6;
uniform sampler2D texture7;

struct material{
    float ambient_intensity;
    vec3  diffuse_clr;
    vec3  specular_clr;
};

uniform material materials[MAX_MATERIALS];
uniform bool skip_lighting = false;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

vec4 texture_val(int mat_idx, vec2 uv)
{
    switch(mat_idx) {
    case 0: return texture2D(texture0, uv); break;
    case 1: return texture2D(texture1, uv); break;
    case 2: return texture2D(texture2, uv); break;
    case 3: return texture2D(texture3, uv); break;
    case 4: return texture2D(texture4, uv); break;
    case 5: return texture2D(texture5, uv); break;
    case 6: return texture2D(texture6, uv); break;
    case 7: return texture2D(texture7, uv); break;
    default: return vec4(0.0);
    }
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

material mixed_material_from_adj(int adjacency_mats)
{
    material ret = material(0.0, vec3(0.0), vec3(0.0));
    for(int i = 0; i < 8; i++) {
        int idx = (adjacency_mats >> (i * 4)) & 0xf;
        ret.ambient_intensity += materials[idx].ambient_intensity * (1.0f/8.0f);
        ret.diffuse_clr += materials[idx].diffuse_clr * (1.0f/8.0f);
        ret.specular_clr += materials[idx].specular_clr * (1.0f/8.0f);
    }
    return ret;
}

material mix_materials(material x, material y, float a)
{
    return material(
        x.ambient_intensity * (1.0 - a) + y.ambient_intensity * a, 
        x.diffuse_clr       * (1.0 - a) + y.diffuse_clr       * a, 
        x.specular_clr      * (1.0 - a) + y.specular_clr      * a
    );
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
    vec4 tex_color;
    material frag_material;

    switch(from_vertex.blend_mode) {
    case BLEND_MODE_NOBLEND: 
        tex_color = texture_val(from_vertex.mat_idx, from_vertex.uv);     
        frag_material = materials[from_vertex.mat_idx];
        break;
    case BLEND_MODE_BLUR:

        /* 
         * This shader will blend this tile's texture(s) with adjacent tiles' textures 
         * based on adjacency information of neighboring tiles' materials.
         *
         * Our top tile faces are made up of 4 triangles in the following configuration:
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
         * Set the fragment material 
         **********************************************************************/

        float alpha_edge = (bot)   ? (0.5f - from_vertex.uv.y)/0.5f
                         : (top)   ? 1.0f - (1.0 - from_vertex.uv.y)/0.5f
                         : (left)  ? (0.5f - from_vertex.uv.x)/0.5f
                         : /*right*/ 1.0f - (1.0 - from_vertex.uv.x)/0.5f;

        material m1 = mixed_material_from_adj(from_vertex.adjacent_mat_indices[0]);
        material m2 = mixed_material_from_adj(from_vertex.adjacent_mat_indices[1]);

        material edge_mat = mix_materials(m1, m2, (bot || top) ? from_vertex.uv.x : from_vertex.uv.y);
        material tile_mat = mix_materials(
            materials[(from_vertex.adjacent_mat_indices[3] >> 0) & 0xf], 
            materials[(from_vertex.adjacent_mat_indices[3] >> 4) & 0xf], 
            0.5
        );
        frag_material = mix_materials(tile_mat, edge_mat, alpha_edge);

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

    if(skip_lighting) {
        o_frag_color = vec4(tex_color.xyz, 1.0);
        return;
    }

    /* We increase the amount of ambient light that taller tiles get, in order to make
     * them not blend with lower terrain. */
    float height = from_vertex.world_pos.y / Y_COORDS_PER_TILE;

    /* Ambient calculations */
    vec3 ambient = (frag_material.ambient_intensity + height * EXTRA_AMBIENT_PER_LEVEL) * ambient_color;

    /* Diffuse calculations */
    vec3 light_dir = normalize(light_pos - from_vertex.world_pos);  
    float diff = max(dot(from_vertex.normal, light_dir), 0.0);
    vec3 diffuse = light_color * (diff * frag_material.diffuse_clr);

    /* Specular calculations */
    vec3 view_dir = normalize(view_pos - from_vertex.world_pos);
    vec3 reflect_dir = reflect(-light_dir, from_vertex.normal);  
    float spec = pow(max(dot(view_dir, reflect_dir), 0.0), SPECULAR_SHININESS);
    vec3 specular = SPECULAR_STRENGTH * light_color * (spec * frag_material.specular_clr);

    o_frag_color = vec4( (ambient + diffuse + specular) * tex_color.xyz, 1.0);
}

