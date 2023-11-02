/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2022-2023 Eduard Permyakov 
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

#include "public/render.h"
#include "public/render_ctrl.h"
#include "gl_perf.h"
#include "gl_assert.h"
#include "gl_shader.h"

#define MIN(a, b)           ((a) < (b) ? (a) : (b))

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static GLuint s_move_ssbo;
static GLuint s_vpref_ssbo;

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_MoveUploadData(void *buff, size_t *nents, size_t *buffsize)
{
    GL_PERF_ENTER();
    assert(R_ComputeShaderSupported());

    glGenBuffers(1, &s_move_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_move_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, *buffsize, buff, GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glGenBuffers(1, &s_vpref_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_vpref_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, *nents * sizeof(GLfloat), NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_MoveInvalidateData(void)
{
    glDeleteBuffers(1, &s_move_ssbo);
    s_move_ssbo = 0;

    glDeleteBuffers(1, &s_vpref_ssbo);
    s_vpref_ssbo = 0;
}

void R_GL_MoveDispatchWork(const size_t *nents)
{
    GL_PERF_ENTER();
    assert(R_ComputeShaderSupported());
    assert(s_move_ssbo > 0);

    /* 1. bind the compute shader */
    R_GL_Shader_Install("movement");

    /* 2. Bind the approparite inputs/outputs */
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s_move_ssbo);

    GLuint pos_id_map_tex = 0;
    R_GL_PositionsGetTexture(&pos_id_map_tex);
    glBindImageTexture(1, pos_id_map_tex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_vpref_ssbo);

    /* 3. kick off the compute work */
    int max_size = 0, left = *nents;
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &max_size);

    while(left) {
        const size_t dispatch_size = MIN(left, max_size);
        glDispatchCompute(dispatch_size, 1, 1);
        left -= dispatch_size;
    }

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_MoveReadNewVelocities(void *out, const size_t *nents, const size_t *maxout)
{
    GL_PERF_ENTER();

    /* Make sure the shader has finished writing the output to the SSBO */
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_vpref_ssbo);
    size_t read_size = MIN(*nents * sizeof(GLfloat), *maxout);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, read_size, out);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}
