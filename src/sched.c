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

#include "sched.h"
#include "config.h"
#include "perf.h"
#include "lib/public/pqueue.h"
#include "lib/public/queue.h"
#include "lib/public/pf_string.h"

#include <SDL.h>


enum taskstate{
    TASK_STATE_ACTIVE,
    TASK_STATE_READY,
    TASK_STATE_SEND_BLOCKED,
    TASK_STATE_RECV_BLOCKED,
    TASK_STATE_REPLY_BLOCKED,
    TASK_STATE_EVENT_BLOCKED,
    TASK_STATE_ZOMBIE,
};

#if defined(__x86_64__)
struct context{
    uint64_t rbx;
    uint64_t rsp;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint32_t mxcsr;
    uint16_t fpucw;
};
#else
#error "Unsupported architecture"
#endif

enum{
    TASK_MAIN_THREAD_AFFINITY = (1 << 0),
};

struct task{
    enum taskstate state;
    struct context ctx;
    int            prio;
    uint32_t       tid;
    uint32_t       parent_tid;
    uint32_t       flags;
};

#define MAX_TASKS               (512)
#define MAX_WORKER_THREADS      (64)
#define STACK_SZ                (64 * 1024)
#define SCHED_TICK_TIMESLICE_MS (1.0f / CONFIG_SCHED_TARGET_FPS * 1000.0f)

PQUEUE_TYPE(task, struct task*)
PQUEUE_IMPL(static, task, struct task*)

QUEUE_TYPE(tid, uint32_t)
QUEUE_IMPL(static, tid, uint32_t)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static uint32_t      s_curr_tid;

static bool          s_freelist[MAX_TASKS];
static struct task   s_tasks[MAX_TASKS];
static char          s_stacks[MAX_TASKS][STACK_SZ];

static SDL_SpinLock  s_ready_queue_lock;
static pq_task_t     s_ready_queue;

static SDL_SpinLock  s_msg_queue_locks[MAX_TASKS];
static queue_tid_t   s_msg_queues[MAX_TASKS];

static size_t        s_nworkers;
static SDL_Thread   *s_worker_threads[MAX_WORKER_THREADS];
static SDL_atomic_t  s_worker_idle[MAX_WORKER_THREADS];

static bool          s_worker_work_ready[MAX_WORKER_THREADS];
static bool          s_worker_should_exit[MAX_WORKER_THREADS];
static uint32_t      s_worker_work_tids[MAX_WORKER_THREADS];

static SDL_mutex    *s_worker_locks[MAX_WORKER_THREADS];
static SDL_cond     *s_worker_conds[MAX_WORKER_THREADS];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

#if defined(__x86_64__)

void sched_save_ctx(struct context *out)
{
    __asm__("mov %0, %%rbx;" :"=m"(out->rbx)   :: "memory");
    __asm__("mov %0, %%rsp;" :"=m"(out->rsp)   :: "memory");
    __asm__("mov %0, %%rbp;" :"=m"(out->rbp)   :: "memory");
    __asm__("mov %0, %%r12;" :"=m"(out->r12)   :: "memory");
    __asm__("mov %0, %%r13;" :"=m"(out->r13)   :: "memory");
    __asm__("mov %0, %%r14;" :"=m"(out->r14)   :: "memory");
    __asm__("mov %0, %%r15;" :"=m"(out->r15)   :: "memory");
    __asm__("stmxcsr %0;"    :"=m"(out->mxcsr) :: "memory");
    __asm__("fstcw %0;"      :"=m"(out->fpucw) :: "memory");
}

void sched_load_ctx(struct context *in)
{
    __asm__("mov %%rbx, %0;" :: "m"(in->rbx)   :);
    __asm__("mov %%rsp, %0;" :: "m"(in->rsp)   :);
    __asm__("mov %%rbp, %0;" :: "m"(in->rbp)   :);
    __asm__("mov %%r12, %0;" :: "m"(in->r12)   :);
    __asm__("mov %%r13, %0;" :: "m"(in->r13)   :);
    __asm__("mov %%r14, %0;" :: "m"(in->r14)   :);
    __asm__("mov %%r15, %0;" :: "m"(in->r15)   :);
    __asm__("ldmxcsr %0;"    :: "m"(in->mxcsr) :);
    __asm__("fldcw %0;"      :: "m"(in->fpucw) :);
}

#endif

static struct task *sched_task_alloc(void)
{
    return NULL;
}

static void sched_task_free(void)
{

}

static void sched_task_activate(const struct task *task)
{

}

static void sched_signal_worker_quit(int id)
{
    SDL_LockMutex(s_worker_locks[id]);
    s_worker_should_exit[id] = true;
    SDL_CondSignal(s_worker_conds[id]);
    SDL_UnlockMutex(s_worker_locks[id]);
}

static void worker_wait_on_work(int id)
{
    SDL_LockMutex(s_worker_locks[id]);
    while(!s_worker_work_ready[id] && !s_worker_should_exit[id])
        SDL_CondWait(s_worker_conds[id], s_worker_locks[id]);
    s_worker_work_ready[id] = false;
    SDL_UnlockMutex(s_worker_locks[id]);
}

static void worker_set_done(int id)
{
    SDL_AtomicIncRef(&s_worker_idle[id]);
}

static void worker_do_work(int id)
{
    printf("ESKEETIT [%lx]\n", (long)SDL_ThreadID());
    fflush(stdout);
}

static int worker_threadfn(void *arg)
{
    int id = (uintptr_t)arg;

    while(true) {

        worker_wait_on_work(id);
        if(s_worker_should_exit[id])
            break;
        worker_do_work(id);
        worker_set_done(id);
    }
    return 0;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Sched_Init(void)
{
    for(int i = 0; i < MAX_TASKS; i++) {
        s_freelist[i] = true;
    }
    s_ready_queue_lock = (SDL_SpinLock){0};
    pq_task_init(&s_ready_queue);

    for(int i = 0; i < MAX_TASKS; i++) {
        s_msg_queue_locks[i] = (SDL_SpinLock){0};
        SDL_AtomicSet(s_worker_idle + i, 0);
        if(!queue_tid_init(s_msg_queues + i, 512))
            goto fail_msg_queue;
    }

    /* On a single-core system, all the tasks will just be run on the main thread */
    s_nworkers = SDL_GetCPUCount() - 1;

    for(int i = 0; i < s_nworkers; i++) {

        s_worker_locks[i] = SDL_CreateMutex();
        if(!s_worker_locks[i])
            goto fail_worker_sync;

        s_worker_conds[i] = SDL_CreateCond();
        if(!s_worker_conds[i])
            goto fail_worker_sync;

        SDL_AtomicSet(s_worker_idle + i, 0);
        s_worker_work_ready[i] = false;
        s_worker_should_exit[i] = false;
    }

    for(int i = 0; i < s_nworkers; i++) {

        char threadname[128];
        pf_snprintf(threadname, sizeof(threadname), "worker-%d", i);
        s_worker_threads[i] = SDL_CreateThread(worker_threadfn, threadname, (void*)((uintptr_t)i));
        if(!s_worker_threads[i])
            goto fail_workers;
        Perf_RegisterThread(SDL_GetThreadID(s_worker_threads[i]), threadname);
    }

    s_curr_tid = MAIN_THREAD_TID;
    return true;

fail_workers:
    for(int i = 0; i < s_nworkers; i++) {
        if(!s_worker_threads[i])
            continue;
        sched_signal_worker_quit(i);
        SDL_WaitThread(s_worker_threads[i], NULL);
    }
fail_worker_sync:
    for(int i = 0; i < s_nworkers; i++) {
        if(s_worker_locks[i])
            SDL_DestroyMutex(s_worker_locks[i]);
        if(s_worker_conds[i])
            SDL_DestroyCond(s_worker_conds[i]);
    }
fail_msg_queue:
    for(int i = 0; i < MAX_TASKS; i++) {
        queue_tid_destroy(s_msg_queues + i);
    }
    return false;
}

void Sched_Shutdown(void)
{
    pq_task_destroy(&s_ready_queue);

    for(int i = 0; i < s_nworkers; i++) {
        sched_signal_worker_quit(i);
        SDL_WaitThread(s_worker_threads[i], NULL);
    }
    for(int i = 0; i < s_nworkers; i++) {
        SDL_DestroyMutex(s_worker_locks[i]);
        SDL_DestroyCond(s_worker_conds[i]);
    }
    for(int i = 0; i < MAX_TASKS; i++) {
        queue_tid_destroy(s_msg_queues + i);
    }
}

void Sched_HandleEvent(int event, void *arg)
{

}

void Sched_Tick(void)
{

}

uint32_t Sched_Create(int prio, void (*code)(void *), void *arg, struct future *result)
{
    return 0;
}

uint32_t Sched_CreateJob(int prio, void (*code)(void *), void *arg, struct future *result)
{
    return 0;
}

void Sched_Destroy(uint32_t tid)
{

}

void Sched_Send(uint32_t tid, void *msg, size_t msglen)
{

}

void Sched_Receive(uint32_t tid)
{

}

void Sched_Reply(uint32_t tid)
{

}

void Sched_AwaitEvent(uint32_t tid)
{

}

