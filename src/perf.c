/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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
#include "lib/public/vec.h"
#include "lib/public/pf_string.h"

#include <assert.h>
#include <stdio.h>


#define PARENT_NONE ~((uint32_t)0)

struct perf_entry{
    uint64_t pc_delta;
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
    vec_perf_t        perf_tree;
    /* A copy of the previous tick's final perf_tree */
    vec_perf_t        perf_tree_prev;
};

KHASH_MAP_INIT_INT64(pstate, struct perf_state)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(pstate) *s_thread_state_table;

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
    vec_perf_init(&out->perf_tree);
    if(!vec_perf_resize(&out->perf_tree, 32768))
        goto fail_perf_tree;
    vec_perf_init(&out->perf_tree_prev);
    if(!vec_perf_resize(&out->perf_tree_prev, 32768))
        goto fail_perf_tree_prev;
    pf_strlcpy(out->name, name, sizeof(out->name));
    return true;

fail_perf_tree_prev:
    vec_perf_destroy(&out->perf_tree);
fail_perf_tree:
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
    vec_perf_destroy(&in->perf_tree_prev);
    vec_perf_destroy(&in->perf_tree);
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

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Perf_Init(void)
{
    s_thread_state_table = kh_init(pstate);
    if(!s_thread_state_table)
        return false;
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

    vec_perf_push(&ps->perf_tree, (struct perf_entry){
        .pc_delta = SDL_GetPerformanceCounter(),
        .parent_idx = parent_idx,
        .name_id = name_id_get(name, ps)
    });

    uint32_t new_idx = vec_size(&ps->perf_tree)-1;
    vec_idx_push(&ps->perf_stack, new_idx);
}

void Perf_Pop(void)
{
    SDL_threadID tid = SDL_ThreadID();
    khiter_t k = kh_get(pstate, s_thread_state_table, tid_to_key(tid));
    if(k == kh_end(s_thread_state_table))
        return;

    struct perf_state *ps = &kh_val(s_thread_state_table, k);
    assert(vec_size(&ps->perf_stack) > 0);

    uint32_t idx = vec_idx_pop(&ps->perf_stack);
    assert(idx < vec_size(&ps->perf_tree));
    struct perf_entry *pe = &vec_AT(&ps->perf_tree, idx);
    pe->pc_delta = SDL_GetPerformanceCounter() - pe->pc_delta;
}

void Perf_FinishTick(void)
{
    ASSERT_IN_MAIN_THREAD();

    for(khiter_t k = kh_begin(s_thread_state_table); k != kh_end(s_thread_state_table); k++) {

        if(!kh_exist(s_thread_state_table, k))
            continue;

        struct perf_state *curr = &kh_val(s_thread_state_table, k);
        assert(vec_size(&curr->perf_stack) == 0);

        vec_perf_reset(&curr->perf_tree_prev);
        vec_perf_copy(&curr->perf_tree_prev, &curr->perf_tree);
        vec_perf_reset(&curr->perf_tree);
    }
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
        struct perf_info *info = malloc(sizeof(struct perf_info) + vec_size(&ps->perf_tree_prev) * sizeof(info->entries[0]));
        if(!info)
            break;

        pf_strlcpy(info->threadname, ps->name, sizeof(info->threadname));
        info->nentries = vec_size(&ps->perf_tree_prev);

        for(int i = 0; i < vec_size(&ps->perf_tree_prev); i++) {
            const struct perf_entry *entry = &vec_AT(&ps->perf_tree_prev, i);
            const uint64_t hz = SDL_GetPerformanceFrequency();

            info->entries[i].funcname = name_for_id(ps, entry->name_id);
            info->entries[i].pc_delta = entry->pc_delta;
            info->entries[i].ms_delta = (entry->pc_delta * 1000.0 / hz);
            info->entries[i].parent_idx = entry->parent_idx;
        }
        out[ret++] = info;
    }

    PERF_RETURN(ret);
}

