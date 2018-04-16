/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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

#ifndef VERTEX_H
#define VERTEX_H

#include <GL/glew.h>

struct vertex{
    vec3_t  pos;
    vec2_t  uv;
    vec3_t  normal;
    GLint   material_idx;
    GLint   joint_indices[4];
    GLfloat weights[4];
    /* The following attributes are used for terrain vertices. */
    GLint   blend_mode;
    GLint   adjacent_mat_indices[4];
};

#endif
