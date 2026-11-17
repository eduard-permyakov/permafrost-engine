/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2026 Eduard Permyakov 
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

#ifndef SCHED_STATS_H
#define SCHED_STATS_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL_mutex.h>

/* Where each scheduler thread spends its time, measured on the TSC. Enabled by
 * the PF_SCHED_STATS environment variable; every hook is a no-op otherwise.
 */

enum sched_state{
    SS_OUTSIDE,     /* main: engine work, worker: between frames */
    SS_TASK,        /* a task body */
    SS_RANGE,       /* a parallel-for range */
    SS_SCHED,       /* scheduler bookkeeping: picking, switching, servicing */
    SS_LOCK_REQ,    /* acquiring the request lock */
    SS_LOCK_READY,  /* acquiring the ready lock */
    SS_IDLE,        /* nothing to take */
    SS_BARRIER,     /* main: waiting for the workers to quiesce */
    SS_JOIN,        /* blocked until a parallel-for batch completes */
    SS_SPIN,        /* polling for a task another thread is running */
    SS_COUNT
};

enum sched_counter{
    SC_SLICES,
    SC_SLICES_MIGRATED,     /* ran on another thread than its last slice */
    SC_SLICES_UNDER_REQ,    /* ran with the request lock held by this thread */
    SC_REQ_FIRST,           /* one counter per request type follows */
    SC_REQ_LAST = SC_REQ_FIRST + 16,
    SC_SIGNALS,             /* cond signals and broadcasts sent */
    SC_WAKEUPS,             /* worker returns from a cond wait */
    SC_FUTILE_WAKES,        /* ... that found nothing to run */
    SC_LOCK_REQ_ACQ,
    SC_LOCK_REQ_CONT,
    SC_LOCK_READY_ACQ,
    SC_LOCK_READY_CONT,
    SC_MAIN_INVERSIONS,     /* main took a less urgent task than one it could run */
    SC_RUNSYNC_HIT,
    SC_RUNSYNC_MISS,
    SC_AWAIT_POLLS,         /* AwaitAll yields while children are unfinished */
    SC_RANGES,
    SC_LOWPRIO_IN_BATCH,    /* worker took a task while a batch had ranges left */
    SC_COUNT
};

enum sched_timer{
    ST_REQ_HOLD,            /* request lock held, outermost level */
    ST_READY_HOLD,
    ST_SLICE_UNDER_REQ,
    ST_LOWPRIO_IN_BATCH,
    ST_COUNT
};

enum sched_lock{
    SLK_REQ,
    SLK_READY,
    SLK_COUNT
};

enum sched_lat_class{
    SL_REALTIME,
    SL_SCRIPT,
    SL_MAIN,
    SL_GENERAL,
    SL_COUNT
};

extern bool g_sched_stats_on;

bool     SchedStats_Init(int nthreads);
void     SchedStats_Shutdown(void);
/* Index 0 is the main thread, 1..n the workers */
void     SchedStats_RegisterThread(int idx);
uint64_t SchedStats_Now(void);
int      SchedStats_Self(void);
int      SchedStats_Set(int state);
void     SchedStats_Count(int counter, uint64_t n);
void     SchedStats_Time(int timer, uint64_t ticks);
void     SchedStats_Latency(int lat_class, uint64_t ticks);
void     SchedStats_Slice(void *code, const char *name, uint64_t ticks);
void     SchedStats_SetRunning(void *code);
void     SchedStats_Lock(SDL_mutex *mutex, int lock);
void     SchedStats_Unlock(SDL_mutex *mutex, int lock);
/* Returns the SDL_CondWait(Timeout) status; timeout_ms < 0 waits without one */
int      SchedStats_CondWait(SDL_cond *cond, SDL_mutex *mutex, int lock,
                             int state, int timeout_ms);
bool     SchedStats_HoldsLock(int lock);
/* Called under the ready lock after the general queue or the waiter count changed */
void     SchedStats_ReadyIntegrate(int queued, int idle_workers);
void     SchedStats_Batch(void *func, int nitems, uint64_t wall, uint64_t work_self,
                          uint64_t work_others, int nthreads);
/* Main thread, with the workers quiesced */
void     SchedStats_FrameReport(void);

#endif

