/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2020 Eduard Permyakov 
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

#include "gl_render.h"
#include "gl_texture.h"
#include "gl_shader.h"
#include "../main.h"

#include <assert.h>

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct texture_arr s_map_textures;
static bool               s_map_ctx_active = false;

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_MapInit(const char map_texfiles[][256], const size_t *num_textures)
{
    ASSERT_IN_RENDER_THREAD();

    bool ret = R_GL_Texture_MakeArrayMap(map_texfiles, *num_textures, &s_map_textures);
    assert(ret);
}

void R_GL_MapBegin(const bool *shadows)
{
    ASSERT_IN_RENDER_THREAD();
    assert(!s_map_ctx_active);

    GLuint shader_prog;
    if(*shadows) {
      shader_prog = R_GL_Shader_GetProgForName("terrain-shadowed");
    }else {
        shader_prog = R_GL_Shader_GetProgForName("terrain");
    }
    assert(shader_prog != -1);
    glUseProgram(shader_prog);
    R_GL_Texture_ActivateArray(&s_map_textures, shader_prog);
    s_map_ctx_active = true;
}

void R_GL_MapEnd(void)
{
    ASSERT_IN_RENDER_THREAD();

    assert(s_map_ctx_active);
    s_map_ctx_active = false;
}

