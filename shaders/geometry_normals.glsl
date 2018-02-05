/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017 Eduard Permyakov 
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

#define MAGNITUDE 2.0

layout (triangles) in;
layout (line_strip) out;
layout (max_vertices = 6) out;

in VertexToGeo {
    vec3 normal;
}from_vertex[];

void main()
{
    int i;
    for(i = 0; i < gl_in.length(); i++) {

        gl_Position = gl_in[i].gl_Position;
        EmitVertex();

        gl_Position = gl_in[i].gl_Position + vec4(from_vertex[i].normal, 0.0) * MAGNITUDE;
        EmitVertex();

        EndPrimitive();
    }
}

