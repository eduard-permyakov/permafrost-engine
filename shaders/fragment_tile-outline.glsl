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

#define BORDER_PERCENT_WIDTH (0.05)
#define EPSILON              (1.0 / 1000000.0) 

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out vec4 o_frag_color;

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
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform vec3 color;

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

bool inside_border()
{
    if(from_vertex.uv.x < BORDER_PERCENT_WIDTH || from_vertex.uv.x > 1.0 - BORDER_PERCENT_WIDTH)
        return true;

    /* Dont' draw top and bottom edges for side faces */
    if(abs(from_vertex.normal.y) < EPSILON)
        return false;

    if(from_vertex.uv.y < BORDER_PERCENT_WIDTH || from_vertex.uv.y > 1.0 - BORDER_PERCENT_WIDTH)
        return true;
 
    return false;
}

void main()
{
    if(!inside_border())
        discard;

    o_frag_color = vec4(color, 1.0);
}

