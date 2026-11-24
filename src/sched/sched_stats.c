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

#define MEM_FILE_SYS MEM_SYS_SCHED
#define MEM_FILE_SUB MEM_SUB_SCHED_STATS

#include "sched_stats.h"
#include "../settings.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/lockfree.h"

#include <SDL.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>
#if defined(__linux__)
#include <time.h>
#endif


#define MAX_STAT_THREADS    (65)
#define TASK_TABLE_SZ       (256)
#define BATCH_TABLE_SZ      (64)
#define LAT_BUCKETS         (40)
#define SAMPLE_PERIOD_NS    (200 * 1000)
#define REPORT_PERIOD_S     (1.0)
#define TASK_DUMP_PERIOD_S  (10.0)
#define TRACE_CAP           (1 << 20)
#define TRACE_FRAME_MARK    (0xff)

struct task_entry{
    void     *code;
    char      name[48];
    uint64_t  slices;
    uint64_t  ticks;
};

struct batch_entry{
    void     *func;
    uint64_t  count;
    uint64_t  nitems;
    uint64_t  wall;
    uint64_t  work_self;
    uint64_t  work_others;
    uint64_t  threads;
};

struct trace_entry{
    uint64_t  tsc;
    void     *code;
    uint32_t  state;
    uint32_t  pad;
};

struct LF_ALIGNED thread_stats{
    int                state;
    void              *running;
    uint64_t           t_last;
    uint64_t           acc[SS_COUNT];
    uint64_t           cnt[SC_COUNT];
    uint64_t           tim[ST_COUNT];
    int                lock_depth[SLK_COUNT];
    uint64_t           lock_since[SLK_COUNT];
    uint64_t           lat[SL_COUNT][LAT_BUCKETS];
    struct task_entry  tasks[TASK_TABLE_SZ];
    struct batch_entry batches[BATCH_TABLE_SZ];
    struct trace_entry *trace;
    size_t             ntrace;
};

struct totals{
    uint64_t acc_main[SS_COUNT];
    uint64_t acc_workers[SS_COUNT];
    uint64_t cnt[SC_COUNT];
    uint64_t tim[ST_COUNT];
    uint64_t lat[SL_COUNT][LAT_BUCKETS];
    uint64_t idle_work;
    uint64_t conc[MAX_STAT_THREADS + 1];
    uint64_t main_busy_samples;
    uint64_t window_samples;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

bool                        g_sched_stats_on;

static __thread int         t_idx = -1;
static int                  s_nthreads;
static struct thread_stats  s_threads[MAX_STAT_THREADS];
static struct totals        s_prev;
static double               s_ticks_per_us;
static uint64_t             s_start_tsc;
static uint64_t             s_last_report_tsc;
static uint64_t             s_last_dump_tsc;
static uint64_t             s_frames;
static bool                 s_header_printed;

/* pf.debug.sched_trace_file records every state change in [pf.debug.sched_trace_from_s,
 * + pf.debug.sched_trace_len_s) and writes it at shutdown
 */
static char                 s_trace_path[SETT_NAME_LEN];
static uint64_t             s_trace_from;
static uint64_t             s_trace_to;

/* Written under the scheduler's ready lock */
static uint64_t             s_rq_last;
static int                  s_rq_queued;
static int                  s_rq_idle;
static uint64_t             s_idle_work;

/* Written by the sampler thread only, read with atomic loads */
static SDL_Thread          *s_sampler;
static int                  s_sampler_quit;
static uint64_t             s_conc[MAX_STAT_THREADS + 1];
static uint64_t             s_main_busy_samples;
static uint64_t             s_window_samples;
static void                *s_serial_code[TASK_TABLE_SZ];
static uint64_t             s_serial_samples[TASK_TABLE_SZ];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static const char *code_name(void *code);

static bool bool_val_validate(const struct sval *new_val)
{
    return (new_val->type == ST_TYPE_BOOL);
}

static bool string_val_validate(const struct sval *new_val)
{
    return (new_val->type == ST_TYPE_STRING);
}

static bool seconds_validate(const struct sval *new_val)
{
    return (new_val->type == ST_TYPE_FLOAT && new_val->as_float >= 0.0f);
}

/* Read once, when the scheduler starts */
static void create_settings(void)
{
    ss_e status;
    (void)status;

    status = Settings_Create((struct setting){
        .name = "pf.debug.log_sched_stats",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.debug.sched_trace_file",
        .val = (struct sval) {
            .type = ST_TYPE_STRING,
            .as_string = ""
        },
        .prio = 0,
        .validate = string_val_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.debug.sched_trace_from_s",
        .val = (struct sval) {
            .type = ST_TYPE_FLOAT,
            .as_float = 60.0f
        },
        .prio = 0,
        .validate = seconds_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.debug.sched_trace_len_s",
        .val = (struct sval) {
            .type = ST_TYPE_FLOAT,
            .as_float = 0.5f
        },
        .prio = 0,
        .validate = seconds_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);
}

/* Never inlined, so that the thread-local address is not computed once and
 * carried across a fiber's context switch onto another thread.
 */
__attribute__((noinline)) static struct thread_stats *stats_self(void)
{
    int idx = t_idx;
    if(idx < 0)
        return NULL;
    return &s_threads[idx];
}

static void trace_note(struct thread_stats *ts, uint64_t now, int state)
{
    if(now < s_trace_from || now >= s_trace_to || ts->ntrace == TRACE_CAP)
        return;
    ts->trace[ts->ntrace++] = (struct trace_entry){ now, ts->running, (uint32_t)state, 0 };
}

static void trace_write(void)
{
    FILE *file = fopen(s_trace_path, "wb");
    if(!file)
        return;
    fprintf(file, "PFSCHEDTRACE %d %.6f\n", s_nthreads, s_ticks_per_us);
    for(int t = 0; t < s_nthreads; t++) {
        struct thread_stats *ts = &s_threads[t];
        for(size_t i = 0; i < ts->ntrace; i++) {
            const struct trace_entry *te = &ts->trace[i];
            fprintf(file, "%d %llu %u %s\n", t, (unsigned long long)te->tsc, te->state,
                te->code ? code_name(te->code) : "-");
        }
    }
    fclose(file);
}

static double ticks_to_ms(uint64_t ticks)
{
    return ticks / s_ticks_per_us / 1000.0;
}

static int lat_bucket(uint64_t ticks)
{
    uint64_t ns = (uint64_t)(ticks * 1000.0 / s_ticks_per_us);
    int b = 0;
    while(ns > 1 && b < LAT_BUCKETS - 1) {
        ns >>= 1;
        b++;
    }
    return b;
}

/* Upper bound of the bucket holding the given percentile, in microseconds */
static double lat_percentile(const uint64_t *hist, double pct)
{
    uint64_t total = 0;
    for(int i = 0; i < LAT_BUCKETS; i++)
        total += hist[i];
    if(total == 0)
        return 0.0;

    uint64_t target = (uint64_t)(total * pct);
    uint64_t seen = 0;
    for(int i = 0; i < LAT_BUCKETS; i++) {
        seen += hist[i];
        if(seen > target)
            return (double)(1ull << i) / 1000.0;
    }
    return (double)(1ull << (LAT_BUCKETS - 1)) / 1000.0;
}

static bool busy_state(int state)
{
    return (state == SS_TASK || state == SS_RANGE);
}

static void sampler_note_serial(void *code)
{
    uint64_t h = ((uintptr_t)code >> 4) * 0x9E3779B97F4A7C15ull;
    for(int i = 0; i < TASK_TABLE_SZ; i++) {
        int slot = (h + i) % TASK_TABLE_SZ;
        if(s_serial_code[slot] == code || s_serial_code[slot] == NULL) {
            LF_STORE(&s_serial_code[slot], code, LF_RLX);
            LF_FADD(&s_serial_samples[slot], 1, LF_RLX);
            return;
        }
    }
}

static int sampler_threadfn(void *arg)
{
#if defined(__linux__)
    struct timespec period = { 0, SAMPLE_PERIOD_NS };
    while(!LF_LOAD(&s_sampler_quit, LF_RLX)) {

        nanosleep(&period, NULL);

        int nbusy = 0;
        int noutside = 0;
        void *last_busy = NULL;
        for(int i = 1; i < s_nthreads; i++) {
            int state = LF_LOAD(&s_threads[i].state, LF_RLX);
            if(busy_state(state)) {
                nbusy++;
                last_busy = LF_LOAD(&s_threads[i].running, LF_RLX);
            }
            if(state == SS_OUTSIDE)
                noutside++;
        }
        /* Only count the frame window, when the workers are released */
        if(noutside == s_nthreads - 1)
            continue;

        LF_FADD(&s_window_samples, 1, LF_RLX);
        LF_FADD(&s_conc[nbusy], 1, LF_RLX);
        int main_state = LF_LOAD(&s_threads[0].state, LF_RLX);
        if(main_state == SS_OUTSIDE || busy_state(main_state))
            LF_FADD(&s_main_busy_samples, 1, LF_RLX);
        if(nbusy == 1 && last_busy)
            sampler_note_serial(last_busy);
    }
#endif
    return 0;
}

static void collect(struct totals *out)
{
    memset(out, 0, sizeof(*out));
    for(int t = 0; t < s_nthreads; t++) {
        struct thread_stats *ts = &s_threads[t];
        for(int i = 0; i < SS_COUNT; i++) {
            if(t == 0)
                out->acc_main[i] += ts->acc[i];
            else
                out->acc_workers[i] += ts->acc[i];
        }
        for(int i = 0; i < SC_COUNT; i++)
            out->cnt[i] += ts->cnt[i];
        for(int i = 0; i < ST_COUNT; i++)
            out->tim[i] += ts->tim[i];
        for(int c = 0; c < SL_COUNT; c++) {
            for(int b = 0; b < LAT_BUCKETS; b++)
                out->lat[c][b] += ts->lat[c][b];
        }
    }
    out->idle_work = s_idle_work;
    for(int i = 0; i <= MAX_STAT_THREADS; i++)
        out->conc[i] = LF_LOAD(&s_conc[i], LF_RLX);
    out->main_busy_samples = LF_LOAD(&s_main_busy_samples, LF_RLX);
    out->window_samples = LF_LOAD(&s_window_samples, LF_RLX);
}

static const char *code_name(void *code)
{
    for(int t = 0; t < s_nthreads; t++) {
        for(int i = 0; i < TASK_TABLE_SZ; i++) {
            if(s_threads[t].tasks[i].code == code)
                return s_threads[t].tasks[i].name;
        }
    }
    return "?";
}

static void print_header(void)
{
    fprintf(stdout, "[sched-csv-hdr] t_s,frames,nworkers"
        ",w_outside,w_task,w_range,w_sched,w_lreq,w_lready,w_idle,w_join,w_spin,w_idle_work"
        ",m_outside,m_task,m_range,m_sched,m_lreq,m_lready,m_idle,m_barrier,m_join,m_spin"
        ",slices,migrated,under_req,under_req_ms"
        ",r_create,r_tid,r_ptid,r_yield,r_send,r_recv,r_reply,r_await,r_dtor,r_wait,r_resched,r_free,r_runsync"
        ",signals,wakeups,futile"
        ",lreq_acq,lreq_cont,lreq_hold_ms,lready_acq,lready_cont,lready_hold_ms"
        ",inversions,rs_hit,rs_miss,await_polls,ranges,lowprio_in_batch,lowprio_ms"
        ",rt_p50,rt_p99,script_p50,script_p99,main_p50,main_p99,gen_p50,gen_p99"
        ",samples,main_busy_pct");
    for(int i = 0; i < s_nthreads; i++)
        fprintf(stdout, ",conc%d", i);
    fputc('\n', stdout);
    s_header_printed = true;
}

static void print_line(const struct totals *now, const struct totals *prev, double t_s)
{
    #define D_ACC_W(i) ticks_to_ms(now->acc_workers[i] - prev->acc_workers[i])
    #define D_ACC_M(i) ticks_to_ms(now->acc_main[i] - prev->acc_main[i])
    #define D_CNT(i)   (unsigned long long)(now->cnt[i] - prev->cnt[i])
    #define D_TIM(i)   ticks_to_ms(now->tim[i] - prev->tim[i])

    fprintf(stdout, "[sched-csv] %.1f,%llu,%d", t_s, (unsigned long long)s_frames, s_nthreads - 1);
    for(int i = 0; i < SS_COUNT; i++) {
        if(i == SS_BARRIER)
            continue;
        fprintf(stdout, ",%.1f", D_ACC_W(i));
    }
    fprintf(stdout, ",%.1f", ticks_to_ms(now->idle_work - prev->idle_work));
    for(int i = 0; i < SS_COUNT; i++)
        fprintf(stdout, ",%.1f", D_ACC_M(i));

    fprintf(stdout, ",%llu,%llu,%llu,%.1f", D_CNT(SC_SLICES), D_CNT(SC_SLICES_MIGRATED),
        D_CNT(SC_SLICES_UNDER_REQ), D_TIM(ST_SLICE_UNDER_REQ));
    for(int i = 0; i < 13; i++)
        fprintf(stdout, ",%llu", D_CNT(SC_REQ_FIRST + i));
    fprintf(stdout, ",%llu,%llu,%llu", D_CNT(SC_SIGNALS), D_CNT(SC_WAKEUPS), D_CNT(SC_FUTILE_WAKES));
    fprintf(stdout, ",%llu,%llu,%.1f,%llu,%llu,%.1f",
        D_CNT(SC_LOCK_REQ_ACQ), D_CNT(SC_LOCK_REQ_CONT), D_TIM(ST_REQ_HOLD),
        D_CNT(SC_LOCK_READY_ACQ), D_CNT(SC_LOCK_READY_CONT), D_TIM(ST_READY_HOLD));
    fprintf(stdout, ",%llu,%llu,%llu,%llu,%llu,%llu,%.1f",
        D_CNT(SC_MAIN_INVERSIONS), D_CNT(SC_RUNSYNC_HIT), D_CNT(SC_RUNSYNC_MISS),
        D_CNT(SC_AWAIT_POLLS), D_CNT(SC_RANGES), D_CNT(SC_LOWPRIO_IN_BATCH),
        D_TIM(ST_LOWPRIO_IN_BATCH));

    for(int c = 0; c < SL_COUNT; c++) {
        uint64_t hist[LAT_BUCKETS];
        for(int b = 0; b < LAT_BUCKETS; b++)
            hist[b] = now->lat[c][b] - prev->lat[c][b];
        fprintf(stdout, ",%.1f,%.1f", lat_percentile(hist, 0.5), lat_percentile(hist, 0.99));
    }

    uint64_t samples = now->window_samples - prev->window_samples;
    uint64_t main_busy = now->main_busy_samples - prev->main_busy_samples;
    fprintf(stdout, ",%llu,%.1f", (unsigned long long)samples,
        samples ? 100.0 * main_busy / samples : 0.0);
    for(int i = 0; i < s_nthreads; i++)
        fprintf(stdout, ",%llu", (unsigned long long)(now->conc[i] - prev->conc[i]));
    fputc('\n', stdout);

    #undef D_ACC_W
    #undef D_ACC_M
    #undef D_CNT
    #undef D_TIM
}

/* Cumulative per-task and per-batch totals since start */
static void dump_tables(double t_s)
{
    struct task_entry merged[TASK_TABLE_SZ * 2];
    uint64_t main_ticks[TASK_TABLE_SZ * 2];
    uint64_t main_slices[TASK_TABLE_SZ * 2];
    int nmerged = 0;

    for(int t = 0; t < s_nthreads; t++) {
        for(int i = 0; i < TASK_TABLE_SZ; i++) {
            const struct task_entry *te = &s_threads[t].tasks[i];
            if(!te->code)
                continue;
            int j;
            for(j = 0; j < nmerged; j++) {
                if(merged[j].code == te->code)
                    break;
            }
            if(j == nmerged) {
                if(nmerged == TASK_TABLE_SZ * 2)
                    continue;
                merged[j] = *te;
                merged[j].slices = 0;
                merged[j].ticks = 0;
                main_ticks[j] = 0;
                main_slices[j] = 0;
                nmerged++;
            }
            merged[j].slices += te->slices;
            merged[j].ticks += te->ticks;
            if(t == 0) {
                main_slices[j] += te->slices;
                main_ticks[j] += te->ticks;
            }
        }
    }
    for(int j = 0; j < nmerged; j++) {
        fprintf(stdout, "[sched-task] %.1f,%s,%llu,%.1f,%llu,%.1f\n", t_s, merged[j].name,
            (unsigned long long)merged[j].slices, ticks_to_ms(merged[j].ticks),
            (unsigned long long)main_slices[j], ticks_to_ms(main_ticks[j]));
    }

    for(int t = 0; t < s_nthreads; t++) {
        for(int i = 0; i < BATCH_TABLE_SZ; i++) {
            const struct batch_entry *be = &s_threads[t].batches[i];
            if(!be->func)
                continue;
            fprintf(stdout, "[sched-batch] %.1f,%p,%d,%llu,%llu,%.1f,%.1f,%.1f,%llu\n", t_s,
                be->func, t, (unsigned long long)be->count, (unsigned long long)be->nitems,
                ticks_to_ms(be->wall), ticks_to_ms(be->work_self), ticks_to_ms(be->work_others),
                (unsigned long long)be->threads);
        }
    }

    for(int i = 0; i < TASK_TABLE_SZ; i++) {
        void *code = LF_LOAD(&s_serial_code[i], LF_RLX);
        if(!code)
            continue;
        fprintf(stdout, "[sched-serial] %.1f,%s,%llu\n", t_s, code_name(code),
            (unsigned long long)LF_LOAD(&s_serial_samples[i], LF_RLX));
    }
    fprintf(stdout, "[sched-anchor] %p\n", (void*)SchedStats_Init);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool SchedStats_Init(int nthreads)
{
    create_settings();

    struct sval val;
    Settings_Get("pf.debug.log_sched_stats", &val);
    g_sched_stats_on = val.as_bool;
    if(!g_sched_stats_on)
        return true;

    if(nthreads > MAX_STAT_THREADS)
        nthreads = MAX_STAT_THREADS;
    s_nthreads = nthreads;

    uint64_t pc0 = SDL_GetPerformanceCounter();
    uint64_t tsc0 = __rdtsc();
    SDL_Delay(50);
    uint64_t pc1 = SDL_GetPerformanceCounter();
    uint64_t tsc1 = __rdtsc();
    double us = (pc1 - pc0) * 1e6 / SDL_GetPerformanceFrequency();
    s_ticks_per_us = (tsc1 - tsc0) / us;

    s_start_tsc = tsc1;
    s_last_report_tsc = tsc1;
    s_last_dump_tsc = tsc1;
    s_rq_last = tsc1;
    for(int i = 0; i < s_nthreads; i++) {
        s_threads[i].t_last = tsc1;
        s_threads[i].state = SS_OUTSIDE;
    }

    Settings_Get("pf.debug.sched_trace_file", &val);
    pf_strlcpy(s_trace_path, val.as_string, sizeof(s_trace_path));
    if(s_trace_path[0]) {
        Settings_Get("pf.debug.sched_trace_from_s", &val);
        double from_s = val.as_float;
        Settings_Get("pf.debug.sched_trace_len_s", &val);
        double len_s = val.as_float;
        s_trace_from = tsc1 + (uint64_t)(from_s * 1e6 * s_ticks_per_us);
        s_trace_to = s_trace_from + (uint64_t)(len_s * 1e6 * s_ticks_per_us);
        for(int i = 0; i < s_nthreads; i++)
            s_threads[i].trace = calloc(TRACE_CAP, sizeof(struct trace_entry));
    }

    s_sampler = SDL_CreateThread(sampler_threadfn, "sched-sampler", NULL);
    return true;
}

void SchedStats_Shutdown(void)
{
    if(!g_sched_stats_on)
        return;
    LF_STORE(&s_sampler_quit, 1, LF_RLX);
    if(s_sampler)
        SDL_WaitThread(s_sampler, NULL);
    dump_tables((__rdtsc() - s_start_tsc) / s_ticks_per_us / 1e6);
    if(s_trace_path[0])
        trace_write();
    fflush(stdout);
}

void SchedStats_RegisterThread(int idx)
{
    if(!g_sched_stats_on)
        return;
    if(idx >= s_nthreads)
        return;
    t_idx = idx;
}

uint64_t SchedStats_Now(void)
{
    return __rdtsc();
}

int SchedStats_Self(void)
{
    if(!g_sched_stats_on)
        return -1;
    struct thread_stats *ts = stats_self();
    return ts ? (int)(ts - s_threads) : -1;
}

static void lock_held(struct thread_stats *ts, int lock)
{
    if(ts->lock_depth[lock]++ == 0)
        ts->lock_since[lock] = __rdtsc();
}

static void lock_dropped(struct thread_stats *ts, int lock)
{
    assert(ts->lock_depth[lock] > 0);
    if(--ts->lock_depth[lock] == 0)
        ts->tim[lock == SLK_REQ ? ST_REQ_HOLD : ST_READY_HOLD] += __rdtsc() - ts->lock_since[lock];
}

void SchedStats_Lock(SDL_mutex *mutex, int lock)
{
    struct thread_stats *ts = g_sched_stats_on ? stats_self() : NULL;
    if(!ts) {
        SDL_LockMutex(mutex);
        return;
    }

    ts->cnt[lock == SLK_REQ ? SC_LOCK_REQ_ACQ : SC_LOCK_READY_ACQ]++;
    if(SDL_TryLockMutex(mutex) != 0) {
        ts->cnt[lock == SLK_REQ ? SC_LOCK_REQ_CONT : SC_LOCK_READY_CONT]++;
        int prev = SchedStats_Set(lock == SLK_REQ ? SS_LOCK_REQ : SS_LOCK_READY);
        SDL_LockMutex(mutex);
        SchedStats_Set(prev);
    }
    lock_held(ts, lock);
}

void SchedStats_Unlock(SDL_mutex *mutex, int lock)
{
    struct thread_stats *ts = g_sched_stats_on ? stats_self() : NULL;
    if(ts)
        lock_dropped(ts, lock);
    SDL_UnlockMutex(mutex);
}

int SchedStats_CondWait(SDL_cond *cond, SDL_mutex *mutex, int lock, int state, int timeout_ms)
{
    struct thread_stats *ts = g_sched_stats_on ? stats_self() : NULL;
    if(!ts) {
        return (timeout_ms < 0) ? SDL_CondWait(cond, mutex)
                                : SDL_CondWaitTimeout(cond, mutex, timeout_ms);
    }

    /* The wait releases a lock taken once; the hold clock stops for its length */
    int depth = ts->lock_depth[lock];
    ts->lock_depth[lock] = 1;
    lock_dropped(ts, lock);
    int prev = SchedStats_Set(state);
    int ret = (timeout_ms < 0) ? SDL_CondWait(cond, mutex)
                               : SDL_CondWaitTimeout(cond, mutex, timeout_ms);
    SchedStats_Set(prev);
    lock_held(ts, lock);
    ts->lock_depth[lock] = depth;
    return ret;
}

bool SchedStats_HoldsLock(int lock)
{
    if(!g_sched_stats_on)
        return false;
    struct thread_stats *ts = stats_self();
    return ts && ts->lock_depth[lock] > 0;
}

int SchedStats_Set(int state)
{
    if(!g_sched_stats_on)
        return SS_OUTSIDE;
    struct thread_stats *ts = stats_self();
    if(!ts)
        return SS_OUTSIDE;

    uint64_t now = __rdtsc();
    int prev = ts->state;
    ts->acc[prev] += now - ts->t_last;
    ts->t_last = now;
    LF_STORE(&ts->state, state, LF_RLX);
    if(ts->trace && prev != state)
        trace_note(ts, now, state);
    return prev;
}

void SchedStats_Count(int counter, uint64_t n)
{
    if(!g_sched_stats_on)
        return;
    struct thread_stats *ts = stats_self();
    if(!ts)
        return;
    ts->cnt[counter] += n;
}

void SchedStats_Time(int timer, uint64_t ticks)
{
    if(!g_sched_stats_on)
        return;
    struct thread_stats *ts = stats_self();
    if(!ts)
        return;
    ts->tim[timer] += ticks;
}

void SchedStats_Latency(int lat_class, uint64_t ticks)
{
    if(!g_sched_stats_on)
        return;
    struct thread_stats *ts = stats_self();
    if(!ts)
        return;
    ts->lat[lat_class][lat_bucket(ticks)]++;
}

void SchedStats_Slice(void *code, const char *name, uint64_t ticks)
{
    if(!g_sched_stats_on)
        return;
    struct thread_stats *ts = stats_self();
    if(!ts)
        return;

    uint64_t h = ((uintptr_t)code >> 4) * 0x9E3779B97F4A7C15ull;
    for(int i = 0; i < TASK_TABLE_SZ; i++) {
        struct task_entry *te = &ts->tasks[(h + i) % TASK_TABLE_SZ];
        if(te->code == NULL) {
            te->code = code;
            snprintf(te->name, sizeof(te->name), "%s", name ? name : "?");
        }
        if(te->code == code) {
            te->slices++;
            te->ticks += ticks;
            return;
        }
    }
}

void SchedStats_SetRunning(void *code)
{
    if(!g_sched_stats_on)
        return;
    struct thread_stats *ts = stats_self();
    if(!ts)
        return;
    LF_STORE(&ts->running, code, LF_RLX);
}

void SchedStats_ReadyIntegrate(int queued, int idle_workers)
{
    if(!g_sched_stats_on)
        return;
    uint64_t now = __rdtsc();
    int lost = s_rq_queued < s_rq_idle ? s_rq_queued : s_rq_idle;
    if(lost > 0)
        s_idle_work += lost * (now - s_rq_last);
    s_rq_last = now;
    s_rq_queued = queued;
    s_rq_idle = idle_workers;
}

void SchedStats_Batch(void *func, int nitems, uint64_t wall, uint64_t work_self,
                      uint64_t work_others, int nthreads)
{
    if(!g_sched_stats_on)
        return;
    struct thread_stats *ts = stats_self();
    if(!ts)
        return;

    for(int i = 0; i < BATCH_TABLE_SZ; i++) {
        struct batch_entry *be = &ts->batches[i];
        if(be->func == NULL)
            be->func = func;
        if(be->func == func) {
            be->count++;
            be->nitems += nitems;
            be->wall += wall;
            be->work_self += work_self;
            be->work_others += work_others;
            be->threads += nthreads;
            return;
        }
    }
}

void SchedStats_FrameReport(void)
{
    if(!g_sched_stats_on)
        return;

    s_frames++;
    uint64_t now = __rdtsc();
    if(s_threads[0].trace)
        trace_note(&s_threads[0], now, TRACE_FRAME_MARK);
    double since = (now - s_last_report_tsc) / s_ticks_per_us / 1e6;
    if(since < REPORT_PERIOD_S)
        return;

    /* Bring every thread's clock up to now, so that the interval is whole */
    for(int t = 0; t < s_nthreads; t++) {
        struct thread_stats *ts = &s_threads[t];
        ts->acc[ts->state] += now - ts->t_last;
        ts->t_last = now;
    }

    if(!s_header_printed)
        print_header();

    double t_s = (now - s_start_tsc) / s_ticks_per_us / 1e6;
    struct totals curr;
    collect(&curr);
    print_line(&curr, &s_prev, t_s);
    s_prev = curr;
    s_last_report_tsc = now;
    s_frames = 0;

    if((now - s_last_dump_tsc) / s_ticks_per_us / 1e6 >= TASK_DUMP_PERIOD_S) {
        dump_tables(t_s);
        s_last_dump_tsc = now;
    }
}

