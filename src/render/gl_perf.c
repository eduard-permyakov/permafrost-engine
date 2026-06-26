/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2025 Eduard Permyakov
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

#include "gl_perf.h"
#include "public/render.h"

#include <SDL.h>
#include <string.h>
#include <stdio.h>

/*****************************************************************************/
/* GLOBAL VARIABLES                                                          */
/*****************************************************************************/

bool g_trace_stalls = false;

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

struct stall_acc{
    uint64_t total_ticks;  /* SDL_GetPerformanceCounter ticks spent waiting    */
    uint64_t max_ticks;    /* longest single wait this frame                   */
    uint32_t nwaits;       /* number of glClientWaitSync calls                 */
    uint32_t nblocked;     /* subset where the GPU was not already finished    */
};

static struct stall_acc s_sites[GL_STALL_NUM_SITES];

/* Per-frame aggregate of all calls wrapped in R_GL_CALL(). */
static uint64_t s_call_total_ticks;
static uint32_t s_call_count;
static struct{
    const char *expr;      /* borrowed: a string literal, static lifetime      */
    const char *file;      /* borrowed: __FILE__, static lifetime              */
    int         line;
    uint64_t    ticks;
}s_call_slowest;

static unsigned long s_frame;

/* Pipeline-statistics query targets, in the field order of struct
 * gpu_frame_stats. 's_pipeline_stats_active' guards the begin/end pairing so a
 * frame that never opened a span (extension absent, or tracing toggled) is not
 * closed.
 */
static const GLenum s_pipeline_stat_targets[PERF_GPU_STAT_COUNT] = {
    GL_VERTICES_SUBMITTED,
    GL_PRIMITIVES_SUBMITTED,
    GL_VERTEX_SHADER_INVOCATIONS,
    GL_CLIPPING_INPUT_PRIMITIVES,
    GL_CLIPPING_OUTPUT_PRIMITIVES,
    GL_FRAGMENT_SHADER_INVOCATIONS,
};
static bool s_pipeline_stats_active;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? (slash + 1) : path;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_PerfStallRecordWait(enum gl_stall_site site, GLenum first_result,
                              uint64_t elapsed_ticks)
{
    assert(site >= 0 && site < GL_STALL_NUM_SITES);
    struct stall_acc *acc = &s_sites[site];

    acc->total_ticks += elapsed_ticks;
    if(elapsed_ticks > acc->max_ticks)
        acc->max_ticks = elapsed_ticks;
    acc->nwaits++;

    /* GL_ALREADY_SIGNALED means the fence was complete before we asked: no
     * stall. Anything else (GL_CONDITION_SATISFIED, or in pathological cases
     * GL_TIMEOUT_EXPIRED) means the render thread actually blocked, waiting
     * for the GPU to release the buffer. */
    if(first_result != GL_ALREADY_SIGNALED)
        acc->nblocked++;
}

void R_GL_PerfCallRecord(const char *expr, const char *file, int line,
                         uint64_t elapsed_ticks)
{
    s_call_total_ticks += elapsed_ticks;
    s_call_count++;

    if(elapsed_ticks > s_call_slowest.ticks) {
        s_call_slowest.expr  = expr;
        s_call_slowest.file  = file;
        s_call_slowest.line  = line;
        s_call_slowest.ticks = elapsed_ticks;
    }

    /* A single ordinary GL call exceeding the threshold is almost always the
     * driver performing implicit synchronization (waiting on an in-flight
     * resource, or doing an internal allocation/relocation). Log it the
     * moment it happens so it can be attributed to the exact call site. */
    if(g_trace_stalls) {
        double ms = elapsed_ticks * 1000.0 / SDL_GetPerformanceFrequency();
        if(ms >= GL_CALL_STALL_THRESHOLD_MS) {
            fprintf(stderr, "[gl-call] %.3fms  %s:%d  %s\n",
                ms, basename_of(file), line, expr);
            fflush(stderr);
        }
    }
}

void R_GL_ReadVramStats(struct vram_stats *out)
{
    memset(out, 0, sizeof(*out));
    if(!GLEW_NVX_gpu_memory_info)
        return;

    glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX,         &out->dedicated_kb);
    glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX,   &out->total_available_kb);
    glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &out->current_available_kb);
    glGetIntegerv(GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX,           &out->eviction_count);
    glGetIntegerv(GL_GPU_MEMORY_INFO_EVICTED_MEMORY_NVX,           &out->evicted_kb);
}

void R_GL_PipelineStatsBegin(uint32_t *cookies)
{
    if(!GLEW_ARB_pipeline_statistics_query) {
        memset(cookies, 0, sizeof(uint32_t) * PERF_GPU_STAT_COUNT);
        s_pipeline_stats_active = false;
        return;
    }

    glGenQueries(PERF_GPU_STAT_COUNT, (GLuint*)cookies);
    for(int i = 0; i < PERF_GPU_STAT_COUNT; i++)
        glBeginQuery(s_pipeline_stat_targets[i], cookies[i]);

    s_pipeline_stats_active = true;
    GL_ASSERT_OK();
}

void R_GL_PipelineStatsEnd(void)
{
    if(!s_pipeline_stats_active)
        return;

    for(int i = 0; i < PERF_GPU_STAT_COUNT; i++)
        glEndQuery(s_pipeline_stat_targets[i]);

    s_pipeline_stats_active = false;
    GL_ASSERT_OK();
}

void R_GL_ResolvePipelineStats(uint32_t *cookies, struct gpu_frame_stats *out)
{
    uint64_t vals[PERF_GPU_STAT_COUNT] = {0};

    for(int i = 0; i < PERF_GPU_STAT_COUNT; i++) {
        if(cookies[i] == 0)
            continue;
        GLint avail = GL_FALSE;
        glGetQueryObjectiv(cookies[i], GL_QUERY_RESULT_AVAILABLE, &avail);
        if(avail)
            glGetQueryObjectui64v(cookies[i], GL_QUERY_RESULT, &vals[i]);
        glDeleteQueries(1, (GLuint*)&cookies[i]);
        cookies[i] = 0;
    }

    out->verts_submitted  = vals[0];
    out->prims_submitted  = vals[1];
    out->vs_invocations   = vals[2];
    out->clip_in_prims    = vals[3];
    out->clip_out_prims   = vals[4];
    out->frag_invocations = vals[5];
}

void R_GL_PerfStallFrameReport(void)
{
    if(g_trace_stalls) {

        uint64_t freq = SDL_GetPerformanceFrequency();
        const struct stall_acc *ring = &s_sites[GL_STALL_RING];
        const struct stall_acc *swap = &s_sites[GL_STALL_SWAPCHAIN];

        /* CPU<->GPU clock correlation anchor. GL_TIMESTAMP is sampled from the
         * same GPU clock as the Perf_PushGPU queries, so pairing it with the
         * CPU performance counter lets the GPU-timeline traces be overlaid on
         * the render thread's CPU timeline. */
        GLint64 gpu_ts = 0;
        glGetInteger64v(GL_TIMESTAMP, &gpu_ts);
        uint64_t cpu_ctr = SDL_GetPerformanceCounter();

        fprintf(stderr,
            "[stall] frame %lu | "
            "ring %.3fms %uw/%ub (max %.3fms) | "
            "swapchain %.3fms %uw/%ub (max %.3fms) | "
            "gl-calls %.3fms x%u",
            s_frame,
            ring->total_ticks * 1000.0 / freq, ring->nwaits, ring->nblocked,
            ring->max_ticks * 1000.0 / freq,
            swap->total_ticks * 1000.0 / freq, swap->nwaits, swap->nblocked,
            swap->max_ticks * 1000.0 / freq,
            s_call_total_ticks * 1000.0 / freq, s_call_count);

        if(s_call_slowest.expr) {
            fprintf(stderr, " (slowest %.3fms %s:%d %s)",
                s_call_slowest.ticks * 1000.0 / freq,
                basename_of(s_call_slowest.file), s_call_slowest.line,
                s_call_slowest.expr);
        }

        fprintf(stderr, " | clk cpu=%llu gpu=%lld\n",
            (unsigned long long)cpu_ctr, (long long)gpu_ts);
        fflush(stderr);
    }

    memset(s_sites, 0, sizeof(s_sites));
    s_call_total_ticks = 0;
    s_call_count = 0;
    memset(&s_call_slowest, 0, sizeof(s_call_slowest));
    s_frame++;
}

