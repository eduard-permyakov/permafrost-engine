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

#include "perf.h"
#include "main.h"
#include "lib/public/khash.h"
#include "lib/public/vec2.h"
#include "lib/public/pf_string.h"
#include "render/public/render.h"
#include "render/public/render_ctrl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>


#define PARENT_NONE     ~((uint32_t)0)
#define GPU_STATE_NAME  "GPU"
#define GPU_STATE_KEY   UINT64_MAX
#define GPU_TIMER_HZ    (1 * 1000 * 1000 * 1000)

struct perf_entry{
    union{
        uint64_t pc_delta;
        struct{
            union{
                uint32_t gpu_cookie;
                uint64_t gpu_ts;
            }begin, end;
        };
    };
    uint32_t parent_idx;
    uint32_t name_id;
};

KHASH_MAP_INIT_STR(name_id, uint32_t)
KHASH_MAP_INIT_INT(id_name, const char *)

VEC_TYPE(perf, struct perf_entry)
VEC_IMPL(static inline, perf, struct perf_entry)

VEC_TYPE(idx, uint32_t)
VEC_IMPL(static inline, idx, uint32_t)

struct perf_state{
    char              name[64];
    /* The next name ID to hand out 
     */
    uint32_t          next_id;
    /* Keep a mapping of function_name: unique_ID and a reverse-mapping. 
     * These should (mostly) get populated during the first frame.
     */
    khash_t(name_id) *name_id_table;
    khash_t(id_name) *id_name_table;
    /* The callstack of profiled functions. As enties are popped, the
     * entries for the corresponding index are updated in the perf tree. 
     */
    vec_idx_t         perf_stack;
    /* The perf tree gets a new entry for each profiled function call.
     * As such, the function calls are added in depth-first fashion.
     */
    int               perf_tree_idx;
    vec_perf_t        perf_trees[NFRAMES_LOGGED];
};

KHASH_MAP_INIT_INT64(pstate, struct perf_state)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(pstate) *s_thread_state_table;

static int              s_last_idx = 0;
static unsigned         s_last_frames_ms[NFRAMES_LOGGED];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static uint64_t tid_to_key(SDL_threadID tid)
{
    union{
        SDL_threadID as_tid;
        uint64_t     as_u64;
    }ret = {0};
    ret.as_tid = tid;
    return ret.as_u64;
}

static uint32_t name_id_get(const char *name, struct perf_state *ps)
{
    khiter_t k = kh_get(name_id, ps->name_id_table, name);
    if(k != kh_end(ps->name_id_table))
        return kh_val(ps->name_id_table, k);

    int status;
    uint32_t new_id = ps->next_id++;
    const char *copy = pf_strdup(name);
    assert(copy);

    kh_put(id_name, ps->id_name_table, new_id, &status);
    assert(status != -1);
    k = kh_get(id_name, ps->id_name_table, new_id);
    kh_val(ps->id_name_table, k) = copy;

    kh_put(name_id, ps->name_id_table, copy, &status);
    assert(status != -1);
    k = kh_get(name_id, ps->name_id_table, copy);
    kh_val(ps->name_id_table, k) = new_id;

    return new_id;
}

const char *name_for_id(struct perf_state *ps, uint32_t id)
{
    khiter_t k = kh_get(id_name, ps->id_name_table, id);
    if(k != kh_end(ps->id_name_table))
        return kh_val(ps->id_name_table, k);
    return NULL;
}

static bool pstate_init(struct perf_state *out, const char *name)
{
    out->next_id = 0;
    out->name_id_table = kh_init(name_id);
    if(!out->name_id_table)
        goto fail_name_id;
    out->id_name_table = kh_init(id_name);
    if(!out->id_name_table)
        goto fail_id_name;
    vec_idx_init(&out->perf_stack);
    if(!vec_idx_resize(&out->perf_stack, 4096))
        goto fail_perf_stack;

    for(int i = 0; i < NFRAMES_LOGGED; i++) {
    
        vec_perf_init(&out->perf_trees[i]);
        if(!vec_perf_resize(&out->perf_trees[i], 32768))
            goto fail_perf_trees;
    }

    pf_strlcpy(out->name, name, sizeof(out->name));
    out->perf_tree_idx = 0;
    return true;

fail_perf_trees:
    for(int i = 0; i < NFRAMES_LOGGED; i++) {

        if(!out->perf_trees[i].array)
            continue;
        vec_perf_destroy(&out->perf_trees[i]);
    }
    vec_idx_destroy(&out->perf_stack);
fail_perf_stack:
    kh_destroy(id_name, out->id_name_table);
fail_id_name:
    kh_destroy(name_id, out->name_id_table);
fail_name_id:
    return false;
}

static void pstate_destroy(struct perf_state *in)
{
    for(int i = 0; i < NFRAMES_LOGGED; i++) {
        vec_perf_destroy(&in->perf_trees[i]);
    }
    vec_idx_destroy(&in->perf_stack);

    uint32_t key;
    const char *curr;
    (void)key;

    kh_foreach(in->id_name_table, key, curr, {
        free((char*)curr);
    });

    kh_destroy(id_name, in->id_name_table);
    kh_destroy(name_id, in->name_id_table);
}

static bool register_gpu_state(void)
{
    khiter_t k = kh_get(pstate, s_thread_state_table, GPU_STATE_KEY);
    assert(k == kh_end(s_thread_state_table));

    int status;
    k = kh_put(pstate, s_thread_state_table, GPU_STATE_KEY, &status);
    assert(status != -1 && status != 0);

    if(!pstate_init(&kh_val(s_thread_state_table, k), GPU_STATE_NAME))
        return false;
    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Perf_Init(void)
{
    s_thread_state_table = kh_init(pstate);
    if(!s_thread_state_table)
        return false;
    if(!register_gpu_state()) {
        kh_destroy(pstate, s_thread_state_table);
        return false;
    }
    assert(NFRAMES_LOGGED >= 3);
    return true;
}

void Perf_Shutdown(void)
{
    uint64_t key;
    struct perf_state curr;
    (void)key;

    kh_foreach(s_thread_state_table, key, curr, {
        pstate_destroy(&curr);
    });
    kh_destroy(pstate, s_thread_state_table);
}

bool Perf_RegisterThread(SDL_threadID tid, const char *name)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(pstate, s_thread_state_table, tid_to_key(tid));
    if(k != kh_end(s_thread_state_table))
        return false;

    int status;
    kh_put(pstate, s_thread_state_table, tid_to_key(tid), &status);
    if(status == -1)
        return false;

    k = kh_get(pstate, s_thread_state_table, tid_to_key(tid));
    if(!pstate_init(&kh_val(s_thread_state_table, k), name))
        return false;

    return true;
}

void Perf_Push(const char *name)
{
    SDL_threadID tid = SDL_ThreadID();
    khiter_t k = kh_get(pstate, s_thread_state_table, tid_to_key(tid));
    if(k == kh_end(s_thread_state_table))
        return;

    struct perf_state *ps = &kh_val(s_thread_state_table, k);
    const size_t ssize = vec_size(&ps->perf_stack);
    uint32_t parent_idx = ssize > 0 ? vec_AT(&ps->perf_stack, ssize-1) : PARENT_NONE;

    vec_perf_push(&ps->perf_trees[ps->perf_tree_idx], (struct perf_entry){
        .pc_delta = SDL_GetPerformanceCounter(),
        .parent_idx = parent_idx,
        .name_id = name_id_get(name, ps)
    });

    uint32_t new_idx = vec_size(&ps->perf_trees[ps->perf_tree_idx])-1;
    vec_idx_push(&ps->perf_stack, new_idx);
}

void Perf_Pop(const char **out)
{
    if(out)
        *out = NULL;

    SDL_threadID tid = SDL_ThreadID();
    khiter_t k = kh_get(pstate, s_thread_state_table, tid_to_key(tid));
    if(k == kh_end(s_thread_state_table))
        return;

    struct perf_state *ps = &kh_val(s_thread_state_table, k);
    assert(vec_size(&ps->perf_stack) > 0);

    uint32_t idx = vec_idx_pop(&ps->perf_stack);
    assert(idx < vec_size(&ps->perf_trees[ps->perf_tree_idx]));
    struct perf_entry *pe = &vec_AT(&ps->perf_trees[ps->perf_tree_idx], idx);
    pe->pc_delta = abs(SDL_GetPerformanceCounter() - pe->pc_delta);

    if(out)
        *out = name_for_id(ps, pe->name_id);
}

int Perf_StackSize(void)
{
    SDL_threadID tid = SDL_ThreadID();
    khiter_t k = kh_get(pstate, s_thread_state_table, tid_to_key(tid));
    if(k == kh_end(s_thread_state_table))
        return -1;

    struct perf_state *ps = &kh_val(s_thread_state_table, k);
    return vec_size(&ps->perf_stack);
}

bool Perf_IsRoot(void)
{
    SDL_threadID tid = SDL_ThreadID();
    khiter_t k = kh_get(pstate, s_thread_state_table, tid_to_key(tid));
    if(k == kh_end(s_thread_state_table))
        return false;

    struct perf_state *ps = &kh_val(s_thread_state_table, k);
    const size_t ssize = vec_size(&ps->perf_stack);

    if(ssize == 0)
        return true;

    uint32_t idx = vec_AT(&ps->perf_stack, vec_size(&ps->perf_stack) - 1);
    struct perf_entry *pe = &vec_AT(&ps->perf_trees[ps->perf_tree_idx], idx);
    const char *name = name_for_id(ps, pe->name_id);
    if(0 == strncmp(name, "Task ", 5))
        return true;
    return false;
}

void Perf_PushGPU(const char *name, uint32_t cookie)
{
    khiter_t k = kh_get(pstate, s_thread_state_table, GPU_STATE_KEY);
    assert(k != kh_end(s_thread_state_table));

    struct perf_state *ps = &kh_val(s_thread_state_table, k);
    const size_t ssize = vec_size(&ps->perf_stack);
    uint32_t parent_idx = ssize > 0 ? vec_AT(&ps->perf_stack, ssize-1) : PARENT_NONE;

    vec_perf_push(&ps->perf_trees[ps->perf_tree_idx], (struct perf_entry){
        .begin.gpu_cookie = cookie,
        .parent_idx = parent_idx,
        .name_id = name_id_get(name, ps)
    });

    uint32_t new_idx = vec_size(&ps->perf_trees[ps->perf_tree_idx])-1;
    vec_idx_push(&ps->perf_stack, new_idx);
}

void Perf_PopGPU(uint32_t cookie)
{
    khiter_t k = kh_get(pstate, s_thread_state_table, GPU_STATE_KEY);
    if(k != kh_end(s_thread_state_table));

    struct perf_state *ps = &kh_val(s_thread_state_table, k);
    assert(vec_size(&ps->perf_stack) > 0);

    uint32_t idx = vec_idx_pop(&ps->perf_stack);
    assert(idx < vec_size(&ps->perf_trees[ps->perf_tree_idx]));
    struct perf_entry *pe = &vec_AT(&ps->perf_trees[ps->perf_tree_idx], idx);
    pe->end.gpu_cookie = cookie;
}

void Perf_BeginTick(void)
{
    ASSERT_IN_MAIN_THREAD();
    s_last_frames_ms[s_last_idx] = SDL_GetTicks();

    khiter_t k = kh_get(pstate, s_thread_state_table, GPU_STATE_KEY);
    if(k != kh_end(s_thread_state_table));

    /* commands are just queued now, to be executed next tick when the 
     * perf_tree_idx moves forward by 1 */
    struct perf_state *gpu_ps = &kh_val(s_thread_state_table, k);
    int write_idx = (gpu_ps->perf_tree_idx + 3) % NFRAMES_LOGGED;

    for(int i = 0; i < vec_size(&gpu_ps->perf_trees[write_idx]); i++) {
    
        struct perf_entry *pe = &vec_AT(&gpu_ps->perf_trees[write_idx], i);

        R_PushCmd((struct rcmd){
            .func = R_GL_TimestampForCookie,
            .nargs = 2,
            .args = {
                &pe->begin.gpu_cookie,
                &pe->begin.gpu_ts,
            }
        });
        R_PushCmd((struct rcmd){
            .func = R_GL_TimestampForCookie,
            .nargs = 2,
            .args = {
                &pe->end.gpu_cookie,
                &pe->end.gpu_ts,
            }
        });
    }
}

void Perf_FinishTick(void)
{
    ASSERT_IN_MAIN_THREAD();

    for(khiter_t k = kh_begin(s_thread_state_table); k != kh_end(s_thread_state_table); k++) {

        if(!kh_exist(s_thread_state_table, k))
            continue;

        struct perf_state *curr = &kh_val(s_thread_state_table, k);
        assert(vec_size(&curr->perf_stack) == 0);

        curr->perf_tree_idx = (curr->perf_tree_idx + 1) % NFRAMES_LOGGED;
        vec_perf_reset(&curr->perf_trees[curr->perf_tree_idx]);
    }

    uint32_t curr_time = SDL_GetTicks();
    uint32_t last_ts = s_last_frames_ms[s_last_idx];
    s_last_frames_ms[s_last_idx] = curr_time - last_ts;
    s_last_idx = (s_last_idx + 1) % NFRAMES_LOGGED;
}

size_t Perf_Report(size_t maxout, struct perf_info **out)
{
    PERF_ENTER();

    size_t ret = 0;
    for(khiter_t k = kh_begin(s_thread_state_table); k != kh_end(s_thread_state_table); k++) {
    
        if(!kh_exist(s_thread_state_table, k))
            continue;
        if(ret == maxout)
            break;

        struct perf_state *ps = &kh_val(s_thread_state_table, k);
        int read_idx = (ps->perf_tree_idx + 1) % NFRAMES_LOGGED;
        struct perf_info *info = malloc(sizeof(struct perf_info) + vec_size(&ps->perf_trees[read_idx]) * sizeof(info->entries[0]));
        if(!info)
            break;

        pf_strlcpy(info->threadname, ps->name, sizeof(info->threadname));
        info->nentries = vec_size(&ps->perf_trees[read_idx]);

        for(int i = 0; i < vec_size(&ps->perf_trees[read_idx]); i++) {

            const struct perf_entry *entry = &vec_AT(&ps->perf_trees[read_idx], i);

            if(k == kh_get(pstate, s_thread_state_table, GPU_STATE_KEY)) {
                uint64_t hz = GPU_TIMER_HZ;
                uint64_t delta = abs(entry->end.gpu_ts - entry->begin.gpu_ts);
                info->entries[i].pc_delta = delta;
                info->entries[i].ms_delta = (delta * 1000.0 / hz);
            }else{
                uint64_t hz = SDL_GetPerformanceFrequency();
                info->entries[i].pc_delta = entry->pc_delta;
                info->entries[i].ms_delta = (entry->pc_delta * 1000.0 / hz);
            }

            info->entries[i].funcname = name_for_id(ps, entry->name_id);
            info->entries[i].parent_idx = entry->parent_idx;
        }
        out[ret++] = info;
    }

    PERF_RETURN(ret);
}

uint32_t Perf_LastFrameMS(void)
{
    int read_idx = (s_last_idx + 1) % NFRAMES_LOGGED;
    return s_last_frames_ms[read_idx];
}

uint32_t Perf_CurrFrameMS(void)
{
    uint32_t curr_time = SDL_GetTicks();
    uint32_t last_ts = s_last_frames_ms[s_last_idx];
    return curr_time - last_ts;
}

