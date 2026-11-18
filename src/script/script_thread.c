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

#include "public/script.h"
#include "script_thread.h"
#include "../main.h"
#include "../perf.h"
#include "../sched.h"
#include "../lib/public/lockfree.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

/* The script thread is the only thread that runs Python. The main thread
 * hands it each piece of Python work as a call and waits for it, so the two
 * never run at once: whichever one runs owns the game world and the main
 * thread's state, and each handover is a release/acquire edge. A thread
 * waiting on its call serves the calls made back to it meanwhile, which is
 * how the script thread reaches the things only the main thread may touch
 * (the window, the cursor) and how calls nest across the two.
 */

/* How long a waiting thread spins before it sleeps, in microseconds. Calls
 * come in bursts while the main thread dispatches events, so the script
 * thread keeps spinning a while after each one. */
#define SCRIPT_IDLE_SPIN_US     (50.0)
#define CALLER_SPIN_US          (200.0)
#define SCRIPT_STACK_SZ         (8 * 1024 * 1024)

struct st_call{
    void (*fn)(void *arg);
    void  *arg;
    int    done;    /* atomic */
    /* A lookup made from inside engine code, where no task may run */
    bool   query;
};

struct st_side{
    struct st_call  *inbox LF_ALIGNED;   /* atomic: the call this thread is asked to run */
    struct lf_parker parker;
    SDL_threadID     tid;
};

static SDL_Thread     *s_thread;
static bool            s_running = false;
static int             s_quit;          /* atomic */
static int             s_ready;         /* atomic */
static struct st_side  s_main;
static struct st_side  s_script;
static uint64_t        s_idle_spin_ticks;
/* Set by the main thread when it wants a frame of the scripts' windows */
static int             s_poked;         /* atomic */
static uint64_t        s_caller_spin_ticks;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct st_side *side_self(void)
{
    SDL_threadID tid = SDL_ThreadID();
    if(tid == s_script.tid)
        return &s_script;
    /* Only the two of them may hand work to each other: a third thread
     * would share the main thread's parker and lose its wakes */
    if(tid != s_main.tid) {
        fprintf(stderr, "[script-thread] call from a thread that is neither main nor script\n");
        abort();
    }
    return &s_main;
}

static struct st_side *side_other(struct st_side *self)
{
    return (self == &s_script) ? &s_main : &s_script;
}

static void run_call(struct st_side *self, struct st_call *call, bool outer)
{
    if(self == &s_script) {
        /* The main thread waits on the call, so the world is the script's */
        S_View_ApplyDeferred();
        /* A parked slice goes on before any other script code runs, as if it
         * had never stopped. A nested call comes from inside other script
         * code, which the slice must not interleave with, and a query from
         * inside engine code, where a task has no place.
         */
        if(outer && !call->query) {
            Perf_OwnTick();
            S_View_ApplyCommands();
            Sched_RunParked();
        }
    }
    call->fn(call->arg);
    /* The caller takes the world back: it resumes only after this store. It
     * is sequentially consistent, as the unpark's load of the caller's
     * parker must not pass it: a caller that prepared to sleep and then
     * missed the store would never be woken. */
    LF_STORE(&call->done, 1, LF_SC);
    lf_parker_unpark(&side_other(self)->parker);
}

/* Waits for 'flag' to be set, running the calls made to this thread meanwhile */
static void wait_serving(struct st_side *self, int *flag, uint64_t spin_ticks)
{
    uint64_t spin_end = SDL_GetPerformanceCounter() + spin_ticks;
    for(;;) {
        if(flag && LF_LOAD(flag, LF_ACQ))
            return;
        if(!flag && LF_LOAD(&s_quit, LF_ACQ))
            return;

        struct st_call *call = LF_XCHG(&self->inbox, NULL, LF_ACQ);
        if(call) {
            run_call(self, call, !flag);
            spin_end = SDL_GetPerformanceCounter() + spin_ticks;
            continue;
        }

        if(!flag && LF_XCHG(&s_poked, 0, LF_SC)) {
            S_UI_FramePump();
        }

        /* Between calls, the script thread runs its view tasks. A slice runs
         * to its yield, as script code is atomic between yields: a call made
         * meanwhile waits for it.
         */
        if(!flag && S_View_RunSlice()) {
            spin_end = SDL_GetPerformanceCounter() + spin_ticks;
            continue;
        }

        if(SDL_GetPerformanceCounter() < spin_end) {
            lf_relax();
            continue;
        }

        lf_parker_prepare(&self->parker);
        LF_FENCE(LF_SC);
        bool wake = (flag && LF_LOAD(flag, LF_SC))
                 || (!flag && LF_LOAD(&s_quit, LF_SC))
                 || (!flag && LF_LOAD(&s_poked, LF_SC))
                 || LF_LOAD(&self->inbox, LF_SC);
        if(wake) {
            lf_parker_cancel(&self->parker);
        }else{
            lf_parker_wait(&self->parker, -1);
        }
        spin_end = SDL_GetPerformanceCounter() + spin_ticks;
    }
}

static void call_on(struct st_side *target, void (*fn)(void*), void *arg, bool query)
{
    struct st_side *self = side_self();
    struct st_call call = {.fn = fn, .arg = arg, .done = 0, .query = query};

    SDL_threadID prev = __atomic_load_n(&g_world_owner_id, __ATOMIC_RELAXED);
    __atomic_store_n(&g_world_owner_id, target->tid, __ATOMIC_RELAXED);

    /* One call is outstanding to a thread at a time: its caller waits on it,
     * and a nested call is only made by the thread running the outer one. */
    struct st_call *was = LF_XCHG(&target->inbox, &call, LF_SC);
    assert(was == NULL);
    (void)was;
    lf_parker_unpark(&target->parker);

    wait_serving(self, &call.done, s_caller_spin_ticks);
    __atomic_store_n(&g_world_owner_id, prev, __ATOMIC_RELAXED);
}

static int script_threadfn(void *arg)
{
    (void)arg;
    s_script.tid = SDL_ThreadID();
    Sched_RegisterScriptThread();
    LF_STORE(&s_ready, 1, LF_REL);
    lf_parker_unpark(&s_main.parker);

    wait_serving(&s_script, NULL, s_idle_spin_ticks);
    return 0;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool S_Thread_Start(void)
{
    ASSERT_IN_MAIN_THREAD();
    uint64_t freq = SDL_GetPerformanceFrequency();
    s_idle_spin_ticks = (uint64_t)(SCRIPT_IDLE_SPIN_US * freq / 1e6);
    s_caller_spin_ticks = (uint64_t)(CALLER_SPIN_US * freq / 1e6);

    s_main.tid = g_main_thread_id;
    s_main.parker.sem = SDL_CreateSemaphore(0);
    s_script.parker.sem = SDL_CreateSemaphore(0);
    if(!s_main.parker.sem || !s_script.parker.sem)
        goto fail;

    /* Python handlers run on this thread's own stack, not on a fiber's */
    s_thread = SDL_CreateThreadWithStackSize(script_threadfn, "script", SCRIPT_STACK_SZ, NULL);
    if(!s_thread)
        goto fail;

    while(!LF_LOAD(&s_ready, LF_ACQ)) {
        lf_parker_prepare(&s_main.parker);
        LF_FENCE(LF_SC);
        if(LF_LOAD(&s_ready, LF_SC)) {
            lf_parker_cancel(&s_main.parker);
            break;
        }
        lf_parker_wait(&s_main.parker, -1);
    }
    Perf_RegisterThread(s_script.tid, "script");
    /* Its view slices run on through the main thread's frame end */
    Perf_SetOwnTicks(s_script.tid);
    s_running = true;
    return true;

fail:
    if(s_main.parker.sem)
        SDL_DestroySemaphore(s_main.parker.sem);
    if(s_script.parker.sem)
        SDL_DestroySemaphore(s_script.parker.sem);
    s_main.parker.sem = NULL;
    s_script.parker.sem = NULL;
    return false;
}

void S_Thread_Stop(void)
{
    if(!s_running)
        return;
    ASSERT_IN_MAIN_THREAD();
    LF_STORE(&s_quit, 1, LF_SC);
    lf_parker_unpark(&s_script.parker);
    SDL_WaitThread(s_thread, NULL);
    s_running = false;
    SDL_DestroySemaphore(s_main.parker.sem);
    SDL_DestroySemaphore(s_script.parker.sem);
}

bool S_Thread_Running(void)
{
    return s_running;
}

bool S_Thread_OnScript(void)
{
    return s_running && SDL_ThreadID() == s_script.tid;
}

void S_Thread_Call(void (*fn)(void *arg), void *arg)
{
    if(!s_running || SDL_ThreadID() == s_script.tid) {
        fn(arg);
        return;
    }
    assert(Engine_OwnsWorld());
    call_on(&s_script, fn, arg, false);
}

void S_Thread_Poke(void)
{
    if(!s_running)
        return;
    LF_STORE(&s_poked, 1, LF_SC);
    lf_parker_unpark(&s_script.parker);
}

void S_Thread_Query(void (*fn)(void *arg), void *arg)
{
    if(!s_running || SDL_ThreadID() == s_script.tid) {
        fn(arg);
        return;
    }
    assert(Engine_OwnsWorld());
    call_on(&s_script, fn, arg, true);
}

void S_Thread_CallMain(void (*fn)(void *arg), void *arg)
{
    if(!s_running || SDL_ThreadID() == s_main.tid) {
        fn(arg);
        return;
    }
    assert(Engine_OwnsWorld());
    call_on(&s_main, fn, arg, false);
}

struct handler_call{
    void (*handler)(void *user, void *event);
    void  *user;
    void  *event;
};

static void handler_thunk(void *arg)
{
    struct handler_call *call = arg;
    call->handler(call->user, call->event);
}

void S_Thread_CallHandler(void (*handler)(void *user, void *event), void *user, void *event)
{
    struct handler_call call = {handler, user, event};
    S_Thread_Call(handler_thunk, &call);
}
