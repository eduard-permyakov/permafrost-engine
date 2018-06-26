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

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec4 in_color;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out VertexToFrag {
         vec4 color;
}to_fragment;

void main()
{
    to_fragment.color = in_color;
    gl_Position = projection * view * model * vec4(in_pos, 1.0);
}

