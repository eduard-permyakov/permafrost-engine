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
#include "gl_ringbuffer.h"
#include "gl_assert.h"
#include "../main.h"
#include "../perf.h"
#include "../map/public/tile.h"

#include <assert.h>

#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct texture_arr s_map_textures;
static bool               s_map_ctx_active = false;
static struct gl_ring    *s_fog_ring;

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_MapInit(const char map_texfiles[][256], const size_t *num_textures, const size_t *nchunks)
{
    PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    s_fog_ring = R_GL_RingbufferInit(*nchunks * TILES_PER_CHUNK_WIDTH * TILES_PER_CHUNK_HEIGHT * 3);
    assert(s_fog_ring);

    bool status = R_GL_Texture_MakeArrayMap(map_texfiles, *num_textures, &s_map_textures);
    assert(status);

    PERF_RETURN_VOID();
}

void R_GL_MapUpdateFog(void *buff, const size_t *size)
{
    PERF_ENTER();
    R_GL_RingbufferPush(s_fog_ring, buff, *size);
    PERF_RETURN_VOID();
}

void R_GL_MapShutdown(void)
{
    R_GL_Texture_ArrayFree(s_map_textures);
    R_GL_RingbufferDestroy(s_fog_ring);
}

void R_GL_MapBegin(const bool *shadows)
{
    PERF_ENTER();
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

    PERF_RETURN_VOID();
}

void R_GL_MapEnd(void)
{
    PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    assert(s_map_ctx_active);
    s_map_ctx_active = false;

    PERF_RETURN_VOID();
}

void R_GL_MapInvalidate(void)
{
    PERF_ENTER();
    R_GL_RingbufferSyncLast(s_fog_ring);
    PERF_RETURN_VOID();
}

void R_GL_MapFogBindLast(GLuint tunit, GLuint shader_prog, const char *uname)
{
    R_GL_RingbufferBindLast(s_fog_ring, tunit, shader_prog, uname);
}

