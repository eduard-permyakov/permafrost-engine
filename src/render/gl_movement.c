/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2022-2026 Eduard Permyakov 
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

#define MEM_FILE_SYS MEM_SYS_RENDER
#define MEM_FILE_SUB MEM_SUB_RENDER_GL_MOVEMENT

#include "public/render.h"
#include "public/render_ctrl.h"
#include "gl_perf.h"
#include "gl_assert.h"

#define GPU_MEM_FILE_SYS GPU_MEM_SYS_GL_MOVEMENT
#include "gl_mem.h"
#include "gl_shader.h"
#include "gl_state.h"
#include "../map/public/tile.h"
#include "../main.h"

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_MOVEMENT)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_MOVEMENT)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_MOVEMENT)

/* Workgroups per dispatch row: the minimum GL_MAX_COMPUTE_WORK_GROUP_COUNT */
#define MAX_GROUPS_X        (65535)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

/* The velocity solve's buffers, in the binding order of movement.glsl: the
 * inputs mapped for writing, the results for reading, all persistently.
 */
enum{
    BUF_UNITS,
    BUF_NBS,
    BUF_PATCH,
    BUF_RESULTS,
    NBUFS
};

static GLuint        s_bufs[NBUFS];
static void         *s_maps[NBUFS];
static size_t        s_caps[NBUFS];
/* The solves posted by the movement task, the latest of which the render
 * thread takes at its next frame boundary. Posts alternate between the
 * slots, so a post never rewrites the one being dispatched.
 */
struct move_post{
    struct map_resolution  res;
    vec2_t                 nav_pos;
    int                    nwork;
    SDL_atomic_t          *done;
    int                    seq;
    struct gpu_move_times *times;
};
static struct move_post s_posts[2];
static int              s_next_post;
static void            *s_posted;

/* The solve in flight, and the flag set to its sequence number once its
 * results are visible */
static GLsync        s_fence;
static SDL_atomic_t *s_done;
static int           s_done_seq;
/* Timestamps around the solve in flight, and where its times go */
static GLuint                 s_queries[2];
static struct gpu_move_times *s_times;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void move_free_buffer(int i)
{
    if(!s_bufs[i])
        return;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_bufs[i]);
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glDeleteBuffers(1, &s_bufs[i]);
    s_bufs[i] = 0;
    s_maps[i] = NULL;
    s_caps[i] = 0;
}

static bool move_alloc_buffer(int i, size_t size)
{
    GLbitfield access = GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT
                      | ((i == BUF_RESULTS) ? GL_MAP_READ_BIT : GL_MAP_WRITE_BIT);
    /* Results are read by the CPU, so they are best kept in its memory */
    GLbitfield storage = access | ((i == BUF_RESULTS) ? GL_CLIENT_STORAGE_BIT : 0);

    glGenBuffers(1, &s_bufs[i]);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_bufs[i]);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, size, NULL, storage);
    s_maps[i] = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, size, access);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if(!s_maps[i]) {
        glDeleteBuffers(1, &s_bufs[i]);
        s_bufs[i] = 0;
        return false;
    }
    s_caps[i] = size;
    return true;
}

static void move_signal(void)
{
    if(s_times) {
        GLuint64 begin, end;
        glGetQueryObjectui64v(s_queries[0], GL_QUERY_RESULT, &begin);
        glGetQueryObjectui64v(s_queries[1], GL_QUERY_RESULT, &end);
        s_times->gpu_us = (uint32_t)((end - begin) / 1000);
        s_times->signalled = SDL_GetPerformanceCounter();
        s_times = NULL;
    }
    glDeleteSync(s_fence);
    s_fence = 0;
    SDL_AtomicSet(s_done, s_done_seq);
    s_done = NULL;
}

static void move_dispatch(const struct move_post *post)
{
    GL_PERF_ENTER();

    R_GL_StateSet(GL_U_NAV_RES, (struct uval){
        .type = UTYPE_IVEC4,
        .val.as_ivec4[0] = post->res.chunk_w,
        .val.as_ivec4[1] = post->res.chunk_h,
        .val.as_ivec4[2] = post->res.tile_w,
        .val.as_ivec4[3] = post->res.tile_h
    });
    R_GL_StateSet(GL_U_NAV_POS, (struct uval){
        .type = UTYPE_VEC2,
        .val.as_vec2 = post->nav_pos
    });
    R_GL_StateSet(GL_U_NUM_SIM_ENTS, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = post->nwork
    });
    R_GL_Shader_Install("movement");

    for(int i = 0; i < NBUFS; i++) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, s_bufs[i]);
    }

    if(post->times && !s_queries[0]) {
        glGenQueries(2, s_queries);
    }
    if(post->times) {
        post->times->dispatched = SDL_GetPerformanceCounter();
        glQueryCounter(s_queries[0], GL_TIMESTAMP);
    }
    /* One workgroup per unit */
    if(post->nwork > 0) {
        GLuint nx = (post->nwork < MAX_GROUPS_X) ? post->nwork : MAX_GROUPS_X;
        GLuint ny = (post->nwork + MAX_GROUPS_X - 1) / MAX_GROUPS_X;
        glDispatchCompute(nx, ny, 1);
    }
    if(post->times) {
        glQueryCounter(s_queries[1], GL_TIMESTAMP);
    }
    s_times = post->times;
    /* The results are read through the persistent mapping */
    glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
    s_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    s_done = post->done;
    s_done_seq = post->seq;

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_MoveReserve(const size_t *caps, struct gpu_move_bufs *out, SDL_atomic_t *done)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();
    assert(R_ComputeShaderSupported());

    /* A solve still in flight reads the buffers about to be replaced */
    if(s_fence) {
        while(glClientWaitSync(s_fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000) == GL_TIMEOUT_EXPIRED)
            ;
        move_signal();
    }

    bool ok = true;
    for(int i = 0; i < NBUFS; i++) {
        if(s_caps[i] >= caps[i])
            continue;
        move_free_buffer(i);
        ok = ok && move_alloc_buffer(i, caps[i]);
    }

    *out = (struct gpu_move_bufs){
        .units       = ok ? s_maps[BUF_UNITS]   : NULL,
        .nbs         = ok ? s_maps[BUF_NBS]     : NULL,
        .patch       = ok ? s_maps[BUF_PATCH]   : NULL,
        .results     = ok ? s_maps[BUF_RESULTS] : NULL,
        .units_cap   = ok ? s_caps[BUF_UNITS]   : 0,
        .nbs_cap     = ok ? s_caps[BUF_NBS]     : 0,
        .patch_cap   = ok ? s_caps[BUF_PATCH]   : 0,
        .results_cap = ok ? s_caps[BUF_RESULTS] : 0,
    };
    SDL_AtomicSet(done, 1);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_MovePost(const struct map_resolution *res, vec2_t nav_pos, int nwork,
                   SDL_atomic_t *done, int seq, struct gpu_move_times *out_times)
{
    struct move_post *post = &s_posts[s_next_post];
    s_next_post = !s_next_post;
    *post = (struct move_post){*res, nav_pos, nwork, done, seq, out_times};
    SDL_AtomicSetPtr(&s_posted, post);
}

void R_GL_MovePoll(bool boundary)
{
    ASSERT_IN_RENDER_THREAD();

    if(s_fence) {
        GLenum result = glClientWaitSync(s_fence, 0, 0);
        if(result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
            move_signal();
        }
    }
    /* Between commands, the program one installed may be live for the next */
    if(boundary && !s_fence && SDL_AtomicGetPtr(&s_posted)) {
        assert(R_ComputeShaderSupported());
        move_dispatch(SDL_AtomicSetPtr(&s_posted, NULL));
    }
    GL_ASSERT_OK();
}

void R_GL_MoveClearState(void)
{
    ASSERT_IN_RENDER_THREAD();

    if(s_fence) {
        while(glClientWaitSync(s_fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000) == GL_TIMEOUT_EXPIRED)
            ;
        glDeleteSync(s_fence);
        s_fence = 0;
    }
    s_done = NULL;
    s_times = NULL;
    SDL_AtomicSetPtr(&s_posted, NULL);
    if(s_queries[0]) {
        glDeleteQueries(2, s_queries);
        s_queries[0] = s_queries[1] = 0;
    }
    for(int i = 0; i < NBUFS; i++) {
        move_free_buffer(i);
    }
    GL_ASSERT_OK();
}
