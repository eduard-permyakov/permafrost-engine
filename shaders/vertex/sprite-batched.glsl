/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2025 Eduard Permyakov 
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

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;

#define MAX_SPRITES (1024)

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out VertexToFrag{
         vec2 uv;
    flat uint frame;
}to_fragment;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

/* Set up for worldspace rendering */
uniform mat4 view;
uniform mat4 projection;
uniform vec3 view_dir;

struct sprite_desc{
    vec3 ws_pos; /* 1 float padding after */
    vec2 ws_size;  /* 2 float padding after */
    uint frame_idx; /* 3 float padding after */
};

layout (std140) uniform sprites
{
    sprite_desc descs[MAX_SPRITES];
};

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

void main()
{
    /* Find a vector that is orthogonal to 'view_dir' in the XZ plane */
    vec3 xz = vec3(view_dir.z, 0.0, -view_dir.x);
    vec3 cam_up_worldspace = normalize(cross(view_dir, xz));
    vec3 cam_right_worldspace = normalize(cross(view_dir, cam_up_worldspace));
    vec3 center_worldspace = descs[gl_InstanceID].ws_pos;
    vec2 size_worldspace = descs[gl_InstanceID].ws_size;
    vec3 out_pos = center_worldspace
                 + cam_right_worldspace * in_pos.x * (size_worldspace.x/2.0)
                 + cam_up_worldspace    * in_pos.y * (size_worldspace.y/2.0);

    to_fragment.uv = in_uv;
    to_fragment.frame = descs[gl_InstanceID].frame_idx;
    gl_Position = projection * view * vec4(out_pos, 1);
}

