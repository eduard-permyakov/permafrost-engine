/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2023 Eduard Permyakov 
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

#ifndef GL_PERF_H
#define GL_PERF_H

#include "../perf.h"
#include <GL/glew.h>

#include "gl_assert.h"

#include <stdbool.h>
#include <stdint.h>

/*****************************************************************************/
/* GPU-STALL INSTRUMENTATION                                                 */
/*****************************************************************************/
/*
 * Unlike the GL_PERF_* macros below, this is compiled into release builds as
 * well (it issues no GL profiling calls of its own) and is gated at runtime by
 * 'g_trace_stalls'. Its purpose is to measure how much render-thread time is
 * lost blocking inside glClientWaitSync - i.e. the CPU waiting for the GPU to
 * release a buffer or framebuffer.
 */

enum gl_stall_site{
    GL_STALL_RING,        /* gl_ringbuffer.c : ring_wait_one()   */
    GL_STALL_SWAPCHAIN,   /* gl_swapchain.c  : wait_frame_done() */
    GL_STALL_NUM_SITES
};

extern bool g_trace_stalls;

/* Called on the render thread immediately after a glClientWaitSync.
 * 'first_result' is the return value of the (first) wait call;
 * 'elapsed_ticks' is an SDL_GetPerformanceCounter() delta. */
void R_GL_PerfStallRecordWait(enum gl_stall_site site, GLenum first_result,
                              uint64_t elapsed_ticks);

/* Called once per frame on the render thread. Logs the accumulated per-frame
 * stall stats (when g_trace_stalls is set) and resets the counters. */
void R_GL_PerfStallFrameReport(void);

/* Default threshold (milliseconds) above which a single instrumented GL call
 * is logged immediately as a likely implicit-synchronization stall. */
#define GL_CALL_STALL_THRESHOLD_MS  (0.2)

/*
 * Wraps a single GL call (or any statement) to measure its CPU wall-clock
 * cost. Use it to catch driver-side implicit synchronization hidden inside an
 * otherwise cheap GL call - e.g. a glTexImage2D or glDeleteTextures that
 * blocks because the driver had to wait on an in-flight resource or perform
 * an internal allocation. For calls that return a value, assign the result
 * into a variable declared beforehand:
 *
 *     GLenum status;
 *     R_GL_CALL(status = glCheckFramebufferStatus(GL_FRAMEBUFFER));
 *
 * The file using this macro must #include <SDL.h>.
 *
 * Zero-overhead when tracing is off: it expands to the bare statement plus a
 * single predicted branch on g_trace_stalls. SDL_GetPerformanceCounter() and
 * R_GL_PerfCallRecord() are only invoked while tracing is enabled.
 */
#define R_GL_CALL(_stmt)                                          \
    do{                                                           \
        uint64_t _glc_t0 = g_trace_stalls                         \
                         ? SDL_GetPerformanceCounter() : 0;       \
        _stmt;                                                    \
        if(g_trace_stalls) {                                      \
            R_GL_PerfCallRecord(#_stmt, __FILE__, __LINE__,       \
                SDL_GetPerformanceCounter() - _glc_t0);           \
        }                                                         \
    }while(0)

void R_GL_PerfCallRecord(const char *expr, const char *file, int line,
                         uint64_t elapsed_ticks);

#ifndef NDEBUG

extern bool g_trace_gpu;

#define GL_GPU_PERF_PUSH(name)                  \
    do{                                         \
        if(!g_trace_gpu)                        \
            break;                              \
        GLuint cookie;                          \
        glGenQueries(1, &cookie);               \
        glQueryCounter(cookie, GL_TIMESTAMP);   \
        GL_ASSERT_OK(); \
        Perf_PushGPU(name, cookie);             \
    }while(0)

#define GL_GPU_PERF_POP()                       \
    do{                                         \
        if(!g_trace_gpu)                        \
            break;                              \
        GLuint cookie;                          \
        glGenQueries(1, &cookie);               \
        glQueryCounter(cookie, GL_TIMESTAMP);   \
        GL_ASSERT_OK(); \
        Perf_PopGPU(cookie);                    \
    }while(0)

#define GL_PERF_ENTER()                         \
    do{                                         \
        Perf_Push(__func__);                    \
        GL_GPU_PERF_PUSH(__func__);             \
    }while(0)

#define GL_PERF_RETURN(...)                     \
    do{                                         \
        Perf_Pop(NULL);                         \
        GL_GPU_PERF_POP();                      \
        return (__VA_ARGS__);                   \
    }while(0)

#define GL_PERF_RETURN_VOID()                   \
    do{                                         \
        Perf_Pop(NULL);                         \
        GL_GPU_PERF_POP();                      \
        return;                                 \
    }while(0)

#define GL_PERF_PUSH_GROUP(id, message)                     \
    do{                                                     \
        if(GLEW_KHR_debug) {                                \
            glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION,   \
                id, strlen(message), message);              \
        }                                                   \
    }while(0)

#define GL_PERF_POP_GROUP()                                 \
    do{                                                     \
        if(GLEW_KHR_debug) {                                \
            glPopDebugGroup();                              \
        }                                                   \
    }while(0)
    

#else

#define GL_GPU_PERF_PUSH(name)
#define GL_GPU_PERF_POP()

#define GL_PERF_ENTER()
#define GL_PERF_RETURN(...) do {return (__VA_ARGS__); } while(0)
#define GL_PERF_RETURN_VOID(...) do { return; } while(0)

#define GL_PERF_PUSH_GROUP(id, message) /* No-op */
#define GL_PERF_POP_GROUP() /* No-op */

#endif //NDEBUG

#endif

