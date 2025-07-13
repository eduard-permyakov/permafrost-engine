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
#include "gl_state.h"
#include "../map/public/tile.h"
#include "../main.h"

#define MIN(a, b)           ((a) < (b) ? (a) : (b))

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static GLuint s_dispatch_ssbo;
static GLuint s_moveattr_ssbo;
static GLuint s_flock_ssbo;
static GLuint s_vout_ssbo;
static GLuint s_cost_base_ssbo;
static GLuint s_blockers_ssbo;
static GLsync s_move_fence = 0;

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_MoveUpdateUniforms(const struct map_resolution *res, vec2_t *map_pos, int *ticks_hz)
{
    R_GL_StateSet(GL_U_MAP_RES, (struct uval){
        .type = UTYPE_IVEC4,
        .val.as_ivec4[0] = res->chunk_w, 
        .val.as_ivec4[1] = res->chunk_h,
        .val.as_ivec4[2] = res->tile_w,
        .val.as_ivec4[3] = res->tile_h
    });
    R_GL_StateSet(GL_U_MAP_POS, (struct uval){
        .type = UTYPE_VEC2,
        .val.as_vec2 = *map_pos
    });
    R_GL_StateSet(GL_U_TICKS_HZ, (struct uval){ 
        .type = UTYPE_INT, 
        .val.as_int = *ticks_hz
    });
}

void R_GL_MoveUploadData(void *gpuid_buff, size_t *ndynamic_ents, 
                         void *attr_buff, size_t *attr_buffsize,
                         void *flock_buff, size_t *flock_buffsize,
                         void *cost_base_buff, size_t *cost_base_size,
                         void *blockers_buff, size_t *blockers_size)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();
    assert(R_ComputeShaderSupported());

    glGenBuffers(1, &s_dispatch_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_dispatch_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, *ndynamic_ents * sizeof(uint32_t), gpuid_buff, GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glGenBuffers(1, &s_moveattr_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_moveattr_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, *attr_buffsize, attr_buff, GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glGenBuffers(1, &s_flock_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_flock_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, *flock_buffsize, flock_buff, GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glGenBuffers(1, &s_vout_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_vout_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, *ndynamic_ents * sizeof(vec2_t), NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glGenBuffers(1, &s_cost_base_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_cost_base_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, *cost_base_size, cost_base_buff, GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glGenBuffers(1, &s_blockers_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_blockers_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, *blockers_size, blockers_buff, GL_STREAM_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_MoveInvalidateData(void)
{
    ASSERT_IN_RENDER_THREAD();

    glDeleteBuffers(1, &s_dispatch_ssbo);
    s_dispatch_ssbo = 0;

    glDeleteBuffers(1, &s_moveattr_ssbo);
    s_moveattr_ssbo = 0;

    glDeleteBuffers(1, &s_flock_ssbo);
    s_flock_ssbo = 0;

    glDeleteBuffers(1, &s_vout_ssbo);
    s_vout_ssbo = 0;

    glDeleteBuffers(1, &s_cost_base_ssbo);
    s_cost_base_ssbo = 0;

    glDeleteBuffers(1, &s_blockers_ssbo);
    s_cost_base_ssbo = 0;

    GL_ASSERT_OK();
}

void R_GL_MoveDispatchWork(const size_t *nents)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    assert(R_ComputeShaderSupported());
    assert(s_moveattr_ssbo > 0);

    enum{
        GPUIDS_UNIT = 0,
        MOVEATTRS_UNIT = 1,
        FLOCKS_UNIT = 2,
        POSMAP_UNIT = 3,
        COST_BASE_UNIT = 4,
        BLOCKERS_UNIT = 5,
        VOUT_UNIT = 6,
    };

    /* 1. bind the compute shader */
    R_GL_Shader_Install("movement");

    /* 2. Bind the approparite inputs/outputs */
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, GPUIDS_UNIT, s_dispatch_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, MOVEATTRS_UNIT, s_moveattr_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FLOCKS_UNIT, s_flock_ssbo);

    GLuint pos_id_map_tex = 0;
    R_GL_PositionsGetTexture(&pos_id_map_tex);
    glBindImageTexture(POSMAP_UNIT, pos_id_map_tex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, COST_BASE_UNIT, s_cost_base_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BLOCKERS_UNIT, s_blockers_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, VOUT_UNIT, s_vout_ssbo);

    /* 3. kick off the compute work */
    int max_size = 0, left = *nents;
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &max_size);

    while(left) {
        const size_t dispatch_size = MIN(left, max_size);
        glDispatchCompute(dispatch_size, 1, 1);
        left -= dispatch_size;
    }

    assert(s_move_fence == 0);
    s_move_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_MoveReadNewVelocities(void *out, const size_t *nwork, const size_t *maxout)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    /* Make sure the shader has finished writing the output to the SSBO */
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_vout_ssbo);
    size_t read_size = MIN(*nwork * sizeof(vec2_t), *maxout);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, read_size, out);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    assert(s_move_fence != 0);
    glDeleteSync(s_move_fence);
    s_move_fence = 0;

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_MovePollCompletion(SDL_atomic_t *out)
{
    ASSERT_IN_RENDER_THREAD();

    if(!s_move_fence)
        return;

    GLenum result = glClientWaitSync(s_move_fence, 0, 0);
    if(result == GL_ALREADY_SIGNALED
    || result == GL_CONDITION_SATISFIED) {
        SDL_AtomicSet(out, 1);
    }
    GL_ASSERT_OK();
}

void R_GL_MoveClearState(void)
{
    ASSERT_IN_RENDER_THREAD();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    if(s_dispatch_ssbo) {
        glDeleteBuffers(1, &s_dispatch_ssbo);
        s_moveattr_ssbo = 0;
    }
    if(s_moveattr_ssbo) {
        glDeleteBuffers(1, &s_moveattr_ssbo);
        s_moveattr_ssbo = 0;
    }
    if(s_flock_ssbo) {
        glDeleteBuffers(1, &s_flock_ssbo);
        s_flock_ssbo = 0;
    }
    if(s_vout_ssbo) {
        glDeleteBuffers(1, &s_vout_ssbo);
        s_vout_ssbo = 0;
    }
    if(s_move_fence) {
        glDeleteSync(s_move_fence);
        s_move_fence = 0;
    }
    if(s_cost_base_ssbo) {
        glDeleteBuffers(1, &s_cost_base_ssbo);
        s_cost_base_ssbo = 0;
    }
    if(s_blockers_ssbo) {
        glDeleteBuffers(1, &s_blockers_ssbo);
        s_blockers_ssbo = 0;
    }
    GL_ASSERT_OK();
}

