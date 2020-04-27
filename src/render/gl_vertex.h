/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2020 Eduard Permyakov 
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

#ifndef GL_VERTEX_H
#define GL_VERTEX_H

#include "../pf_math.h"
#include <stdint.h>
#include <GL/glew.h>


#define PACK_32(name, ...)      \
    union{                      \
        struct{                 \
            __VA_ARGS__         \
        };                      \
        uint32_t name;          \
    }

#define VERTEX_BASE             \
    vec3_t  pos;                \
    vec2_t  uv;                 \
    vec3_t  normal;             \
    GLint   material_idx;       \


struct vertex{
    VERTEX_BASE
};

struct anim_vert{
    VERTEX_BASE
    GLubyte joint_indices[6];
    GLfloat weights[6];
};

struct terrain_vert{
    VERTEX_BASE
    uint16_t  blend_mode;
    uint16_t  middle_indices; 
    /* Each uint32_t holds 4 8-bit indices */
    uint32_t  c1_indices[2]; /* corner 1 */
    uint32_t  c2_indices[2]; /* corner 2 */
    uint32_t  tb_indices;
    uint32_t  lr_indices;
};

struct colored_vert{
    vec3_t pos;
    vec4_t color;
};

struct textured_vert{
    vec3_t pos;
    vec2_t uv;
};

#endif
