/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#ifndef GL_STATE_H
#define GL_STATE_H

#include "../pf_math.h"
#include <stdbool.h>
#include <GL/glew.h>

struct uval{
    enum{
        UTYPE_FLOAT,
        UTYPE_VEC2,
        UTYPE_VEC3,
        UTYPE_VEC4,
        UTYPE_INT,
        UTYPE_IVEC2,
        UTYPE_IVEC3,
        UTYPE_IVEC4,
        UTYPE_MAT3,
        UTYPE_MAT4,
    }type;
    union{
        GLfloat  as_float;
        vec2_t   as_vec2;
        vec3_t   as_vec3;
        vec4_t   as_vec4;
        GLint    as_int;
        GLint    as_ivec2[2];
        GLint    as_ivec3[3];
        GLint    as_ivec4[4];
        mat3x3_t as_mat3;
        mat4x4_t as_mat4;
    }val;
};

bool R_GL_StateInit(void);
void R_GL_StateShutdown(void);
void R_GL_StateSet(const char *uname, struct uval val);
void R_GL_StateInstall(const char *uname, GLuint shader_prog);

#endif

