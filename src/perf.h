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

#ifndef PERF_H
#define PERF_H

#include "mem.h"

#include <stdbool.h>
#include <stddef.h>
#include <SDL_thread.h>

#ifndef NDEBUG

#define PERF_ENTER()            \
    do{                         \
        Perf_Push(__func__);    \
    }while(0)

#define PERF_RETURN(...)        \
    do{                         \
        Perf_Pop(NULL);         \
        return (__VA_ARGS__);   \
    }while(0)

#define PERF_RETURN_VOID()      \
    do{                         \
        Perf_Pop(NULL);         \
        return;                 \
    }while(0)

#define PERF_PUSH(name)         \
    Perf_Push(name)

#define PERF_POP()              \
    Perf_Pop(NULL)

#define PERF_POP_NAME(_ptr)     \
    Perf_Pop(_ptr)

#else

#define PERF_ENTER()
#define PERF_RETURN(...) do {return (__VA_ARGS__); } while(0)
#define PERF_RETURN_VOID(...) do { return; } while(0)
#define PERF_PUSH(name)
#define PERF_POP()
#define PERF_POP_NAME(_ptr)

#endif

#define NFRAMES_LOGGED        (5)
#define PERF_GPU_STAT_COUNT   (6)
#define PERF_NAV_TICK_HISTORY (1024)
#define PERF_FRAME_HISTORY    (4096)

struct gpu_mem_accounting;

/* Curated memory snapshot mixing mimalloc internal counters with OS-level
 * accounting. The Vm* fields are populated only on Linux debug builds and
 * read as zero otherwise. 
 */
struct perf_mem_stats{
    int64_t  mi_malloc_normal_current;
    int64_t  mi_malloc_normal_total;
    int64_t  mi_pages_current;
    int64_t  mi_threads_current;
    uint64_t vm_rss_kb;
    uint64_t vm_size_kb;
};

/* GPU video memory counters (NVX_gpu_memory_info), in KB; zeroes if the
 * extension is unavailable. 
 */
struct vram_stats{
    int dedicated_kb;
    int total_available_kb;
    int current_available_kb;
    int eviction_count;
    int evicted_kb;
};

/* OpenGL pipeline-statistics counters (GL_ARB_pipeline_statistics_query) for a
 * single frame. All zero when the extension is unavailable or GPU tracing is
 * off.
 */
struct gpu_frame_stats{
    uint64_t verts_submitted;
    uint64_t prims_submitted;
    uint64_t vs_invocations;
    uint64_t clip_in_prims;
    uint64_t clip_out_prims;
    uint64_t frag_invocations;
};


/* Per-tick navigation stats for the perf window. Times are in microseconds;
 * total_us is serial_us plus the summed CPU of the parallel phases. The phase
 * wall times include any scheduler quiesce gaps at frame boundaries.
 */
struct nav_tick_sample{
    uint32_t dur_us;        /* wall-time */
    uint32_t serial_us;     /* wall-time of the serial (non-parallelised) phases */
    uint32_t total_us;      /* serial_us + summed parallel-task CPU */
    uint32_t nwork;         /* entities processed this tick */
    uint32_t budget_us;     /* per-tick budget (1000/hz) */
    uint32_t tick;          /* monotonic tick sequence number */
    /* Nav fiber phases */
    uint32_t inval_us;      /* applying deferred field invalidations */
    uint32_t los_peek_us;   /* parallel LOS cache peek */
    uint32_t los_build_us;  /* serial LOS record loop + parallel chain flood/publish */
    uint32_t cpr_async_us;  /* parallel async field build + join */
    uint32_t cpr_serial_us; /* serial path-request loop */
    uint32_t dv_us;         /* parallel desired-velocity phase */
    uint32_t vel_us;        /* velocity (ClearPath) phase */
    uint32_t upd_us;        /* parallel state-update phase */
    /* Main-thread half of the tick */
    uint32_t main_us;       /* whole of move_do_tick */
    uint32_t consume_us;    /* applying the previous tick's results */
    uint32_t copy_gs_us;    /* releasing + copying the gamestate snapshot */
    uint32_t submit_us;     /* per-entity work submission loop */
    uint32_t map_update_us; /* synchronous navigation map update */
    uint32_t drain_us;      /* main-thread inline drain of an unfinished fiber */
    uint32_t pivot_us;      /* turning of combat-held still units */
    uint32_t cmds_us;       /* command-queue drain */
    uint32_t flock_us;      /* flock disband + arrival-field refresh */
    uint32_t snaps_us;      /* flock snapshot rebuild */
    /* Field-cache miss counters */
    uint32_t nlos_builds;   /* units whose LOS field was built this tick */
    uint32_t nreq_rebuilds; /* units whose path was serviced this tick */
    /* Field churn diagnostics (see struct nav_tick_diag) */
    uint32_t nenemy_built;
    uint32_t nzone_built;
    uint32_t nsurround_built;
    uint32_t ninval_enemy;
    uint32_t ninval_surround;
    uint32_t nsvc_sync;
    uint32_t nsvc_patch;
    uint32_t nastar;
    uint32_t nastar_memo;
    uint32_t npseek_built;
};

struct perf_info{
    char threadname[64];
    size_t nentries;
    struct{
        const char *funcname; /* borrowed */
        uint64_t    pc_delta;
        double      ms_delta;
        int         parent_idx;
        /* Extra hardware performance counters for 
         * more in-depth insight. Available on Linux
         * builds. */
        float       hw_ipc;         /* Instructions per cycle */ 
        float       hw_br_miss;     /* Branch miss rate */
        float       hw_l1d_miss;    /* L1D miss rate */
        float       hw_llc_miss;    /* LLC miss rate */
    }entries[];
};

void     Perf_Push(const char *name);
void     Perf_Pop(const char **out);

void     Perf_PushGPU(const char *name, uint32_t cookie);
void     Perf_PopGPU(uint32_t cookie);

bool     Perf_IsRoot(void);

/* Per-nav-tick parallel-CPU accumulator. Thread-safe: the worker tasks add their
 * CPU time, the nav fiber resets it at tick start and reads it at the end.
 */
void     Perf_NavParallelReset(void);
void     Perf_NavParallelAddSince(uint64_t start_ticks);
uint32_t Perf_NavParallelGet(void);

/* Note that due to buffering of the frame timing data, the statistics
 * reported will be from NFRAMES_LOGGED ago. The reason for this is that
 * the GPU may be lagging a couple of frames behind the CPU. We want to get
 * far enough ahead so that the GPU is finished with the frame we're 
 * getting the statistics for. This way querying the GPU timestamps doesn't 
 * cause a CPU<->GPU synch, which would negatively impact performance.
 */

/* This returns an array of perf_info structs (one for each thread). They
 * must be 'free'd by the caller. 
 */
size_t   Perf_Report(size_t maxout, struct perf_info **out);
void     Perf_GetMemoryStats(struct perf_mem_stats *out);
void     Perf_GetVramStats(struct vram_stats *out);
void     Perf_GetGpuFrameStats(struct gpu_frame_stats *out);
void     Perf_GetMemoryAccounting(struct mem_accounting *out);
void     Perf_GetGpuMemoryAccounting(struct gpu_mem_accounting *out);
uint32_t Perf_LastFrameMS(void);
uint32_t Perf_CurrFrameMS(void);
uint64_t Perf_LastFrameAllocdBytes(void);

/* Navigation-task wall-time history (one sample per completed nav tick), feeding
 * the perf window's live graph. Recorded from the main thread at the nav-tick join.
 */
void     Perf_RecordNavTick(const struct nav_tick_sample *sample);
size_t   Perf_GetNavTickTimes(size_t maxout, struct nav_tick_sample *out);

/* Frame duration history (one entry per frame). Deep enough that a script task
 * draining it at 1Hz cannot miss frames even when the scheduler is saturated.
 */
struct frame_time_sample{
    uint32_t seq;
    uint32_t ms;
};

size_t   Perf_GetFrameTimes(size_t maxout, struct frame_time_sample *out);

/* The following can only be called from the main thread, making sure that 
 * none of the other threads are touching the Perf_ API concurrently 
 */
bool     Perf_Init(void);
void     Perf_Shutdown(void);
bool     Perf_RegisterThread(SDL_threadID tid, const char *name);
void     Perf_BeginTick(void);
void     Perf_FinishTick(void);

#endif

