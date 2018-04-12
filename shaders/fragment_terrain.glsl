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

#define MAX_MATERIALS 16

/* TODO: Make these as material parameters */
#define SPECULAR_STRENGTH  2
#define SPECULAR_SHININESS 4

#define Y_COORDS_PER_TILE  4 
#define EXTRA_AMBIENT_PER_LEVEL 0.03

/*****************************************************************************/
/* INPUTS                                                                    */
/*****************************************************************************/

in VertexToFrag {
         vec2  uv;
    flat int   mat_idx;
         vec3  world_pos;
         vec3  normal;
    flat int   blend_mode;
    flat ivec2 adjacent_mat_indices;
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

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

vec4 texture_val(int mat_idx, vec2 uv)
{
    switch(mat_idx) {
    case 0: return vec4(1.0, 0.0, 0.0, 1.0); break;
    case 1: return vec4(0.0, 1.0, 0.0, 1.0); break;
    case 2: return vec4(0.0, 0.0, 1.0, 1.0); break;
    case 3: return vec4(0.0, 1.0, 1.0, 1.0); break;
    case 4: return texture2D(texture4, uv); break;
    case 5: return texture2D(texture5, uv); break;
    case 6: return texture2D(texture6, uv); break;
    case 7: return texture2D(texture7, uv); break;
    default: return vec4(0.0);
    }
}

void main()
{
    vec4 tex_color;

    if(0 == from_vertex.blend_mode) {
        tex_color = texture_val(from_vertex.mat_idx, from_vertex.uv);     
    }else {
        tex_color = texture_val(from_vertex.adjacent_mat_indices[0], from_vertex.uv);
    }

    /* Simple alpha test to reject transparent pixels */
    if(tex_color.a == 0.0)
        discard;

    /* We increase the amount of ambient light that taller tiles get, in order to make
     * them not blend with lower terrain. */
    float height = from_vertex.world_pos.y / Y_COORDS_PER_TILE;

    /* Ambient calculations */
    vec3 ambient = (materials[from_vertex.mat_idx].ambient_intensity + height * EXTRA_AMBIENT_PER_LEVEL) * ambient_color;

    /* Diffuse calculations */
    vec3 light_dir = normalize(light_pos - from_vertex.world_pos);  
    float diff = max(dot(from_vertex.normal, light_dir), 0.0);
    vec3 diffuse = light_color * (diff * materials[from_vertex.mat_idx].diffuse_clr);

    /* Specular calculations */
    vec3 view_dir = normalize(view_pos - from_vertex.world_pos);
    vec3 reflect_dir = reflect(-light_dir, from_vertex.normal);  
    float spec = pow(max(dot(view_dir, reflect_dir), 0.0), SPECULAR_SHININESS);
    vec3 specular = SPECULAR_STRENGTH * light_color * (spec * materials[from_vertex.mat_idx].specular_clr);

    o_frag_color = vec4( (ambient + diffuse + specular) * tex_color.xyz, 1.0);
}

