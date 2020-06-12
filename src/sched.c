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
#include "main.h"
#include "lib/public/pqueue.h"
#include "lib/public/queue.h"
#include "lib/public/khash.h"
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

enum{
    _SCHED_REQ_FREE = _SCHED_REQ_COUNT + 1,
};

struct task{
    enum taskstate state;
    struct context ctx;
    int            prio;
    uint32_t       tid;
    uint32_t       parent_tid;
    uint32_t       flags;
    struct request req;
    uint64_t       retval;
    void          *arg;
    void          (*destructor)(void*);
    void          *darg;
    struct task   *prev, *next;
};

enum worker_cmd{
    WORKER_CMD_NONE = 0,
    WORKER_CMD_WORK,
    WORKER_CMD_QUIESCE,
    WORKER_CMD_QUIT,
};

#define MAX_TASKS               (512)
#define MAX_WORKER_THREADS      (64)
#define STACK_SZ                (64 * 1024)
#define SCHED_TICK_MS           (1.0f / CONFIG_SCHED_TARGET_FPS * 1000.0f)
#define ALIGNED(val, align)     (((val) + ((align) - 1)) & ~((align) - 1))

PQUEUE_TYPE(task, struct task*)
PQUEUE_IMPL(static, task, struct task*)

QUEUE_TYPE(tid, uint32_t)
QUEUE_IMPL(static, tid, uint32_t)

KHASH_MAP_INIT_INT64(tid, uint32_t)
KHASH_MAP_INIT_INT(tqueue, queue_tid_t)


uint64_t sched_switch_ctx(struct context *save, struct context *restore, int retval, void *arg);
static void sched_task_exit(void);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

/* After initialization, no new keys are added to the map, so it's safe to 
 * read from different threads. */
static khash_t(tid)    *s_thread_tid_map;
static khash_t(tid)    *s_thread_worker_id_map;

static struct context   s_main_ctx;
static struct task     *s_freehead;

static struct task      s_tasks[MAX_TASKS];
static char             s_stacks[MAX_TASKS][STACK_SZ];
static queue_tid_t      s_msg_queues[MAX_TASKS];
static khash_t(tqueue) *s_event_queues;

static SDL_SpinLock     s_request_queue_lock;
static queue_tid_t      s_request_queue;

static pq_task_t        s_ready_queue;
static SDL_SpinLock     s_ready_queue_lock;

static size_t           s_nworkers;
static SDL_Thread      *s_worker_threads[MAX_WORKER_THREADS];
static struct context   s_worker_contexts[MAX_WORKER_THREADS];

/* Used by the main thread to kick off some processing on the worker threads */
static bool             s_worker_start[MAX_WORKER_THREADS];
static bool             s_worker_quit[MAX_WORKER_THREADS];
static SDL_mutex       *s_worker_locks[MAX_WORKER_THREADS];
static SDL_cond        *s_worker_conds[MAX_WORKER_THREADS];

/* Used by the main thread to wait until all the workers have quiesced */
static int              s_idle_workers;
static SDL_mutex       *s_idle_workers_lock;
static SDL_cond        *s_idle_workers_cond;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

#if defined(__x86_64__)

__asm__(
".text                                  \n"
"                                       \n"
".type sched_switch_ctx, @function      \n"
"                                       \n"
"# parameter 0 (%rdi) - save ctx ptr    \n"
"# parameter 1 (%rsi) - load ctx ptr    \n"
"# parameter 2 (%rdx) - return value    \n"
"# parameter 3 (%rcx) - arg passed to   \n"
"#                      context code    \n"
"                                       \n"
"sched_switch_ctx:                      \n"
"Lsave_ctx:                             \n"
"   lea Lback(%rip), %r8                \n" 
"   push %r8                            \n" 
"   mov %rbx,  0x0(%rdi)                \n"
"   mov %rsp,  0x8(%rdi)                \n"
"   mov %rbp, 0x10(%rdi)                \n"
"   mov %r12, 0x18(%rdi)                \n"
"   mov %r13, 0x20(%rdi)                \n"
"   mov %r14, 0x28(%rdi)                \n"
"   mov %r15, 0x30(%rdi)                \n"
"   stmxcsr 0x38(%rdi)                  \n"
"   fstcw   0x3c(%rdi)                  \n"
"Lload_ctx:                             \n"
"   mov  0x0(%rsi), %rbx                \n"
"   mov  0x8(%rsi), %rsp                \n"
"   mov 0x10(%rsi), %rbp                \n"
"   mov 0x18(%rsi), %r12                \n"
"   mov 0x20(%rsi), %r13                \n"
"   mov 0x28(%rsi), %r14                \n"
"   mov 0x30(%rsi), %r15                \n"
"   ldmxcsr 0x38(%rsi)                  \n"
"   fldcw   0x3c(%rsi)                  \n"
"   mov %rcx, %rdi                      \n"
"   mov %rdx, %rax                      \n"
"Lback:                                 \n"
"   ret                                 \n"
"                                       \n"
);

void sched_init_ctx(struct task *task, void *code)
{
    char *stack_end = s_stacks[task->tid - 1];
    char *stack_base = stack_end + STACK_SZ;
    stack_base = (char*)ALIGNED((uintptr_t)stack_base, 16);
    if(stack_base >= stack_end + STACK_SZ)
        stack_base -= 16;

    /* This is the address where we will jump to upon 
     * returning from the task function. This routine
     * is responsible for context-switching out of the 
     * task context. */
    stack_base -= 8;
    *(uint64_t*)stack_base = (uintptr_t)sched_task_exit;

    /* This is where we will jump to the next time we 
     * will context switch to this task. */
    stack_base -= 8;
    *(uint64_t*)stack_base = (uintptr_t)code;

    memset(&task->ctx, 0, sizeof(task->ctx));
    task->ctx.rsp = (uintptr_t)stack_base;
}

#endif

static uint64_t thread_id_to_key(SDL_threadID tid)
{
    union{
        SDL_threadID as_tid;
        uint64_t     as_u64;
    }ret = {0};
    ret.as_tid = tid;
    return ret.as_u64;
}

static void sched_set_thread_tid(SDL_threadID id, uint32_t tid)
{
    uint64_t key = thread_id_to_key(id);
    khiter_t k = kh_get(tid, s_thread_tid_map, key);
    assert(k != kh_end(s_thread_tid_map));
    kh_val(s_thread_tid_map, k) = tid;
}

static uint32_t sched_curr_thread_tid(void)
{
    uint64_t key = thread_id_to_key(SDL_ThreadID());
    khiter_t k = kh_get(tid, s_thread_tid_map, key);
    assert(k != kh_end(s_thread_tid_map));
    return kh_val(s_thread_tid_map, k);
}

static struct task *sched_task_alloc(void)
{
    if(!s_freehead)
        return NULL;

    struct task *ret = s_freehead;
    if(ret->prev)
        ret->prev->next = ret->next;
    if(ret->next)
        ret->next->prev = ret->prev;

    s_freehead = s_freehead->next;
    return ret;
}

static void sched_task_free(struct task *task)
{
    task->next = s_freehead;
    task->prev = NULL;
    if(s_freehead)
        s_freehead->prev = task;
    s_freehead = task;
}

static void sched_task_init(struct task *task, int prio, uint32_t flags, void *code, void *arg)
{
    task->state = TASK_STATE_READY;
    task->prio = prio;
    task->parent_tid = NULL_TID;
    task->flags = flags;
    task->retval = 0;
    task->arg = arg;
    task->destructor = NULL;
    task->darg = NULL;
    sched_init_ctx(task, code);

    SDL_AtomicLock(&s_ready_queue_lock); 
    pq_task_push(&s_ready_queue, prio, task);
    SDL_AtomicUnlock(&s_ready_queue_lock);
}

static void sched_enqueue_request(uint32_t tid)
{
    SDL_AtomicLock(&s_request_queue_lock); 
    queue_tid_push(&s_request_queue, &tid);
    SDL_AtomicUnlock(&s_request_queue_lock);
}

static size_t sched_num_requests(void)
{
    SDL_AtomicLock(&s_request_queue_lock); 
    size_t ret = queue_size(s_request_queue);
    SDL_AtomicUnlock(&s_request_queue_lock);
    return ret;
}

static void sched_task_exit(void)
{
    uint32_t tid = sched_curr_thread_tid();
    struct task *task = &s_tasks[tid - 1];

    if(task->destructor) {
        task->destructor(task->darg);
    }

    task->req.type = _SCHED_REQ_FREE;
    sched_enqueue_request(task->tid);

    if(SDL_ThreadID() == g_main_thread_id) {
        sched_switch_ctx(&task->ctx, &s_main_ctx, 0, NULL);
    }else{
        //TODO:
        assert(0);
    }
}

static void sched_reactivate(struct task *task)
{
    task->state = TASK_STATE_REPLY_BLOCKED;

    SDL_AtomicLock(&s_ready_queue_lock); 
    pq_task_push(&s_ready_queue, task->prio, task);
    SDL_AtomicUnlock(&s_ready_queue_lock);
}

static void sched_send(struct task *task, uint32_t tid, void *msg, size_t msglen)
{
    ASSERT_IN_MAIN_THREAD();
    struct task *recv_task = &s_tasks[tid - 1];

    /* write data to blocked send-blocked task to unblock it */
    if(recv_task->state == TASK_STATE_SEND_BLOCKED) {

        uint32_t *out_send = (uint32_t*)recv_task->req.argv[0];
        void *dst = (void*)recv_task->req.argv[1];
        size_t dstlen = (size_t)recv_task->req.argv[2];

        assert(dstlen == msglen);
        memcpy(dst, msg, msglen);

        task->state = TASK_STATE_REPLY_BLOCKED;
        sched_reactivate(recv_task);

    }else{

        task->state = TASK_STATE_RECV_BLOCKED;
        queue_tid_push(&s_msg_queues[tid - 1], &task->tid);
    }
}

static void sched_receive(struct task *task, uint32_t *out_tid, void *msg, size_t msglen)
{
    ASSERT_IN_MAIN_THREAD();

    if(queue_size(s_msg_queues[task->tid - 1]) > 0) {
    
        uint32_t send_tid;
        assert(task->state != TASK_STATE_SEND_BLOCKED);
        queue_tid_pop(&s_msg_queues[task->tid - 1], &send_tid);

        struct task *send_task = &s_tasks[send_tid - 1];
        void *src = (void*)send_task->req.argv[1];
        size_t srclen = (size_t)send_task->req.argv[2];

        assert(srclen == msglen);
        memcpy(msg, src, msglen);
        *out_tid = send_tid;

        assert(send_task->state == TASK_STATE_RECV_BLOCKED);
        send_task->state = TASK_STATE_REPLY_BLOCKED;
        sched_reactivate(task);

    }else{

        task->state = TASK_STATE_SEND_BLOCKED;
    }
}

static void sched_reply(struct task *task, uint32_t tid, void *reply, size_t replylen)
{
    ASSERT_IN_MAIN_THREAD();

    struct task *send_task = &s_tasks[tid - 1];
    assert(send_task->state == TASK_STATE_REPLY_BLOCKED);

    void *dst = (void*)send_task->req.argv[3];
    size_t dstlen = (size_t)send_task->req.argv[4];

    assert(dstlen == replylen);
    memcpy(dst, reply, replylen);

    sched_reactivate(send_task);
    sched_reactivate(task);
}

static void sched_await_event(struct task *task, int event)
{
    ASSERT_IN_MAIN_THREAD();

    task->state = TASK_STATE_EVENT_BLOCKED;

    int status;
    khiter_t k = kh_get(tqueue, s_event_queues, event);

    if(k == kh_end(s_event_queues)) {

        k = kh_put(tqueue, s_event_queues, event, &status);
        assert(status != -1 && status != 0);
        queue_tid_init(&kh_val(s_event_queues, k), 32);
    }
    queue_tid_push(&kh_val(s_event_queues, k), &task->tid);
}

static void sched_task_run(struct task *task)
{
    sched_set_thread_tid(SDL_ThreadID(), task->tid);
    task->state = TASK_STATE_ACTIVE;

    if(SDL_ThreadID() == g_main_thread_id) {
        sched_switch_ctx(&s_main_ctx, &task->ctx, task->retval, task->arg);
    }else{
        //TODO:
        assert(0);
    }
}

static void sched_task_service_request(struct task *task)
{
    ASSERT_IN_MAIN_THREAD();

    switch((int)task->req.type) {
    case SCHED_REQ_CREATE:
        break;
    case SCHED_REQ_MY_TID:
        task->retval = task->tid;
        sched_reactivate(task);
        break;
    case SCHED_REQ_MY_PARENT_TID:
        task->retval = task->parent_tid;
        sched_reactivate(task);
        break;
    case SCHED_REQ_YIELD:
        sched_reactivate(task);
        break;
    case SCHED_REQ_SEND:
        sched_send(task, (uint32_t)task->req.argv[0], (void*)task->req.argv[1], (size_t)task->req.argv[2]);
        break;
    case SCHED_REQ_RECEIVE:
        sched_receive(task, (uint32_t*)task->req.argv[0], (void*)task->req.argv[1], (size_t)task->req.argv[2]);
        break;
    case SCHED_REQ_REPLY:
        sched_reply(task, (uint32_t)task->req.argv[0], (void*)task->req.argv[1], (size_t)task->req.argv[2]);
        break;
    case SCHED_REQ_AWAIT_EVENT:
        sched_await_event(task, (int)task->req.argv[0]);
        break;
    case SCHED_REQ_SET_DESTRUCTOR:
        task->destructor = (void (*)(void*))task->req.argv[0];
        task->darg = (void*)task->req.argv[1];
        sched_reactivate(task);
        break;
    case _SCHED_REQ_FREE:
        sched_task_free(task);
        break;
    default: assert(0);    
    }
}

static void sched_service_requests(void)
{
    ASSERT_IN_MAIN_THREAD();

    while(sched_num_requests() > 0) {
    
        uint32_t curr = 0;

        SDL_AtomicLock(&s_request_queue_lock);
        queue_tid_pop(&s_request_queue, &curr);
        SDL_AtomicUnlock(&s_request_queue_lock);

        assert(curr > 0);
        sched_task_service_request(&s_tasks[curr - 1]);
    }
}

static size_t sched_ready_queue_size(void)
{
    SDL_AtomicLock(&s_ready_queue_lock); 
    size_t ret = pq_size(&s_ready_queue);
    SDL_AtomicUnlock(&s_ready_queue_lock);
    return ret;
}

static void sched_signal_worker_quit(int id)
{
    SDL_LockMutex(s_worker_locks[id]);
    s_worker_quit[id] = true;
    SDL_CondSignal(s_worker_conds[id]);
    SDL_UnlockMutex(s_worker_locks[id]);
}

static void sched_init_thread_tid_map(void)
{
    int status;
    uint64_t main_key = thread_id_to_key(g_main_thread_id);

    kh_put(tid, s_thread_tid_map, g_main_thread_id, &status);
    assert(status != -1 && status != 0);

    for(int i = 0; i < s_nworkers; i++) {

        uint64_t key = thread_id_to_key(SDL_GetThreadID(s_worker_threads[i]));
        kh_put(tid, s_thread_tid_map, key, &status);
        assert(status != -1 && status != 0);
    }
}

static void sched_init_thread_worker_id_map(void)
{
    for(int i = 0; i < s_nworkers; i++) {

        int status;
        uint64_t key = thread_id_to_key(SDL_GetThreadID(s_worker_threads[i]));

        khiter_t k = kh_put(tid, s_thread_worker_id_map, key, &status);
        assert(status != -1 && status != 0);
        kh_val(s_thread_worker_id_map, k) = i;
    }
}

static void sched_wait_workers_done(void)
{
    SDL_LockMutex(s_idle_workers_lock);
    while(s_idle_workers < s_nworkers)
        SDL_CondWait(s_idle_workers_cond, s_idle_workers_lock);
    SDL_UnlockMutex(s_idle_workers_lock);
}

static void worker_wait_on_work(int id)
{
    SDL_LockMutex(s_worker_locks[id]);
    while(!s_worker_start[id] && !s_worker_quit[id])
        SDL_CondWait(s_worker_conds[id], s_worker_locks[id]);
    s_worker_start[id] = false;
    SDL_UnlockMutex(s_worker_locks[id]);
}

static void worker_notify_done(int id)
{
    SDL_LockMutex(s_idle_workers_lock);
    s_idle_workers++;
    SDL_UnlockMutex(s_idle_workers_lock);
}

static void worker_do_work(int id)
{
    //printf("ESKEETIT [%lx]\n", (long)SDL_ThreadID());
    //fflush(stdout);
}

static int worker_threadfn(void *arg)
{
    int id = (uintptr_t)arg;

    while(true) {

        worker_wait_on_work(id);
        if(s_worker_quit[id])
            break;
        worker_do_work(id);
        worker_notify_done(id);
    }
    return 0;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Sched_Init(void)
{
    ASSERT_IN_MAIN_THREAD();

    s_thread_tid_map = kh_init(tid);
    if(!s_thread_tid_map)
        goto fail_thread_tid_map;

    s_thread_worker_id_map = kh_init(tid);
    if(!s_thread_worker_id_map)
        goto fail_thread_worker_id_map;

    s_request_queue_lock = (SDL_SpinLock){0};
    if(!queue_tid_init(&s_request_queue, MAX_TASKS))
        goto fail_req_queue;

    s_event_queues = kh_init(tqueue);
    if(!s_event_queues)
        goto fail_event_queue;

    s_ready_queue_lock = (SDL_SpinLock){0};
    pq_task_init(&s_ready_queue);
    if(!pq_task_reserve(&s_ready_queue, MAX_TASKS))
        goto fail_ready_queue;

    assert(MAX_TASKS >= 2);
    s_tasks[0].prev = NULL;
    s_tasks[0].next = &s_tasks[1];
    s_tasks[MAX_TASKS-1].prev = &s_tasks[MAX_TASKS - 2];
    s_tasks[MAX_TASKS-1].next = NULL;

    for(int i = 1; i < MAX_TASKS-1; i++) {
        s_tasks[i].next = &s_tasks[i + 1];
        s_tasks[i].prev = &s_tasks[i - 1];
    }
    s_freehead = s_tasks;

    for(int i = 0; i < MAX_TASKS; i++) {

        s_tasks[i].tid = i + 1;
        assert(s_msg_queues + i > &s_request_queue + 1);
        if(!queue_tid_init(&s_msg_queues[i], MAX_TASKS))
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

        s_worker_start[i] = false;
        s_worker_quit[i] = false;
    }

    for(int i = 0; i < s_nworkers; i++) {

        char threadname[128];
        pf_snprintf(threadname, sizeof(threadname), "worker-%d", i);
        s_worker_threads[i] = SDL_CreateThread(worker_threadfn, threadname, (void*)((uintptr_t)i));
        if(!s_worker_threads[i])
            goto fail_workers;
        Perf_RegisterThread(SDL_GetThreadID(s_worker_threads[i]), threadname);
    }

    s_idle_workers_lock = SDL_CreateMutex();
    if(!s_idle_workers_lock)
        goto fail_workers;

    s_idle_workers_cond = SDL_CreateCond();
    if(!s_idle_workers_cond)
        goto fail_idle_workers_cond;

    sched_init_thread_tid_map();
    sched_init_thread_worker_id_map();
    return true;

fail_idle_workers_cond:
    SDL_DestroyMutex(s_idle_workers_lock);
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
    pq_task_destroy(&s_ready_queue);
fail_ready_queue:
    kh_destroy(tqueue, s_event_queues);
fail_event_queue:
    queue_tid_destroy(&s_request_queue);
fail_req_queue:
    kh_destroy(tid, s_thread_worker_id_map);
fail_thread_worker_id_map:
    kh_destroy(tid, s_thread_tid_map);
fail_thread_tid_map:
    return false;
}

void Sched_Shutdown(void)
{
    ASSERT_IN_MAIN_THREAD();

    uint32_t key;
    queue_tid_t curr;
    (void)key;

    kh_foreach(s_event_queues, key, curr, {
        queue_tid_destroy(&curr);
    });
    kh_destroy(tqueue, s_event_queues);

    kh_destroy(tid, s_thread_tid_map);
    kh_destroy(tid, s_thread_worker_id_map);
    queue_tid_destroy(&s_request_queue);
    pq_task_destroy(&s_ready_queue);

    SDL_DestroyMutex(s_idle_workers_lock);
    SDL_DestroyCond(s_idle_workers_cond);

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
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(tqueue, s_event_queues, event);
    if(k == kh_end(s_event_queues))
        return;

    queue_tid_t *waiters = &kh_val(s_event_queues, k);
    while(queue_size(*waiters) > 0) {

        uint32_t tid;
        queue_tid_pop(waiters, &tid);

        struct task *task = &s_tasks[tid - 1];
        assert(task->state == TASK_STATE_EVENT_BLOCKED);

        task->retval = (uint64_t)arg;
        sched_reactivate(task);
    }
}

void Sched_StartBackgroundTasks(void)
{
    ASSERT_IN_MAIN_THREAD();

    SDL_LockMutex(s_idle_workers_lock);
    s_idle_workers = 0;
    SDL_UnlockMutex(s_idle_workers_lock);

    for(int i = 0; i < s_nworkers; i++) {
    
        SDL_LockMutex(s_worker_locks[i]);
        s_worker_start[i] = true;
        SDL_CondSignal(s_worker_conds[i]);
        SDL_UnlockMutex(s_worker_locks[i]);
    }
}

void Sched_Tick(void)
{
    ASSERT_IN_MAIN_THREAD();
    PERF_ENTER();

    sched_service_requests();
    //TODO: change top-level condition to be timeslice-based
    while(sched_ready_queue_size() > 0) {

        struct task *curr = NULL;

        SDL_AtomicLock(&s_ready_queue_lock);
        pq_task_pop(&s_ready_queue, &curr);
        SDL_AtomicUnlock(&s_ready_queue_lock);

        assert(curr);
        sched_task_run(curr);
        sched_service_requests();
    }

    sched_wait_workers_done();
    PERF_RETURN_VOID();
}

uint32_t Sched_Create(int prio, void (*code)(void *), void *arg, struct future *result)
{
    ASSERT_IN_MAIN_THREAD();

    struct task *task = sched_task_alloc();
    if(!task)
        return 0;

    //TODO: handle arg, result....
    sched_task_init(task, prio, TASK_MAIN_THREAD_AFFINITY, code, arg);
    printf("created task with tid: %u\n", task->tid);
    return task->tid;
}

uint32_t Sched_CreateJob(int prio, void (*code)(void *), void *arg, struct future *result)
{
    ASSERT_IN_MAIN_THREAD();

    return 0;
}

uint64_t Sched_Request(struct request req)
{
    uint32_t tid = sched_curr_thread_tid();
    struct task *task = &s_tasks[tid - 1];

    task->req = req;
    sched_enqueue_request(tid);

    if(SDL_ThreadID() == g_main_thread_id) {
        return sched_switch_ctx(&task->ctx, &s_main_ctx, 0, NULL);
    }else{
        //TODO: need to be able to get the worker index here...
        assert(0);
    }
}

