/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2020-2026 Eduard Permyakov
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
#define MEM_FILE_SUB 0

#include "sched.h"
#include "sched_stats.h"
#include "config.h"
#include "perf.h"
#include "main.h"
#include "task.h"
#include "event.h"
#include "game/public/game.h"
#include "phys/public/phys.h"
#include "script/public/script.h"
#include "lib/public/pqueue.h"
#include "lib/public/khash.h"
#include "lib/public/lockfree.h"
#include "lib/public/pf_string.h"
#include "mem.h"
#include "settings.h"

#include <SDL.h>
#include <mimalloc-stats.h>

#include <inttypes.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_SCHED, 0)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_SCHED, 0)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_SCHED, 0)

#if !defined(NDEBUG) || defined(PF_PERF_MARKERS)
#define SCHED_PERF 1
#else
#define SCHED_PERF 0
#endif

/* Ownership and ordering
 *
 * Every task carries one atomic state word: its kind and a sequence number
 * bumped by every change. A task is at any time either owned by exactly one
 * thread (ACTIVE: running, being serviced after it switched out, or claimed
 * by a waker), or ready, or blocked and owned by nobody. Only the owner
 * touches the task's plain fields. Ownership moves in two ways only, and
 * every other field rides on them:
 *
 *  - the owner publishes a new state with a seq_cst store, and a claimer
 *    takes the task with a CAS from the exact (kind, sequence) it read, so
 *    one claimer wins and a claim based on a stale reading fails;
 *  - a hint travels through a ring, an inbox or a mailbox whose push is a
 *    release and whose pop is an acquire.
 *
 * A block and its wake always meet on a store-then-load pair on each side,
 * both seq_cst: the blocker publishes its state and re-reads the condition
 * (mailbox, child, join count, queues); the waker publishes the condition and
 * reads the state. One of the two sees the other, so no wake is lost, and the
 * CAS lets only one of them go on with the task. Parking pairs the same way:
 * a pusher writes its queue and reads the idle mask, a parking thread sets its
 * idle bit and re-reads the queues.
 */

enum taskkind{
    TK_FREE,
    TK_ACTIVE,
    TK_READY,           /* hinted in its level's ring; taken by CAS */
    TK_READY_MAIN,      /* in the main inbox or a main heap; only the main thread takes it */
    TK_SEND_BLOCKED,    /* in Task_Receive, waiting for a sender */
    TK_RECV_BLOCKED,    /* in Task_Send, waiting for the receiver to take the message */
    TK_REPLY_BLOCKED,
    TK_EVENT_BLOCKED,
    TK_WAIT_BLOCKED,    /* in Task_Wait, waiting for the child to exit */
    TK_JOIN_BLOCKED,    /* waiting for futures or a parallel batch */
    TK_ZOMBIE,
};

#define ST(_kind, _seq)     ((((uint64_t)(_seq)) << 8) | (uint64_t)(_kind))
#define ST_KIND(_st)        ((int)((_st) & 0xff))
#define ST_SEQ(_st)         ((_st) >> 8)

#ifdef _MSC_VER
#define INT128(_name) uint64_t _name[2]
#else
#define INT128(_name) __int128 _name
#endif

#if defined(__x86_64__) || defined(_WIN64)

#if defined(_WIN32)

struct context{
    uint64_t rbx;
    uint64_t rsp;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint32_t mxcsr;
    uint16_t fpucw;
    uint16_t __pad0;
    INT128(xmm6);
    INT128(xmm7);
    INT128(xmm8);
    INT128(xmm9);
    INT128(xmm10);
    INT128(xmm11);
    INT128(xmm12);
    INT128(xmm13);
    INT128(xmm14);
    INT128(xmm15);
} __attribute__((packed, aligned(16)));

#else // System V ABI

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
    uint16_t __pad0;
} __attribute__((packed, aligned(8)));

#endif

#else
#error "Unsupported architecture"
#endif

enum{
    _SCHED_REQ_FREE = _SCHED_REQ_COUNT + 1,
    _SCHED_REQ_RUN_SYNC,
    _SCHED_REQ_JOIN,
};

struct task{
    struct context ctx;
    uint64_t       state;       /* atomic */
    uint32_t       inc;         /* atomic: bumped by every allocation of the slot */
    uint32_t       queued;      /* atomic: the incarnation whose hint waits in a ring */
    uint32_t       inbox_next;
    uint32_t       reg_next;
    int            reg_event;
    uint64_t       reg_seq;
    uint32_t       mbox_head;   /* atomic: pushed message nodes, newest first */
    uint32_t       mbox_first;  /* the owner's FIFO of taken nodes */
    uint32_t       mbox_last;
    uint32_t       reply_to;    /* atomic */
    uint64_t       exit_word;   /* atomic */
    int            prio;
    int            level;
    uint32_t       tid;
    uint32_t       parent_tid;  /* atomic where a non-owner reads it */
    uint32_t       flags;       /* atomic where a non-owner reads it */
    bool           on_main;
    bool           needs_world; /* a view task's next slice runs in a world window */
    bool           park_request; /* a view task's yield, to finish its slice with the world */
    struct request req;
    uint64_t       retval;
    void          *stackmem;
    void          *bigstack;    /* kept with the slot for its next big-stack task */
    struct future *future;
    const char    *name;
    void          *arg;
    void         (*destructor)(void*);
    void          *darg;
    void          *earg;
    void         (*erelease)(void*);
    void          *code;
    uint64_t       ready_ts;
    int            last_thread;
    uint64_t       rt_pass;
#if SCHED_PERF
    char           perf_name[48];
#endif
#if defined(__SANITIZE_ADDRESS__)
    void          *fake_stack;   /* ASan fiber fake-stack save slot */
    void          *entry_code;   /* real task fn, invoked via the ASan entry shim */
#endif
#if defined(__SANITIZE_THREAD__)
    void          *tsan_fiber;
#endif
} LF_ALIGNED;

struct sched_thread{
    struct context   ctx;
    int              idx;       /* 0 for the main thread, workers from 1, SCRIPT_IDX */
    struct task     *curr;
    struct lf_parker parker;
    SDL_Thread      *thread;
#if defined(__SANITIZE_ADDRESS__)
    void            *fake_stack;
    const void      *stack_lo;
    size_t           stack_sz;
#endif
#if defined(__SANITIZE_THREAD__)
    void            *tsan_fiber;
#endif
} LF_ALIGNED;

/* A message waiting in a receiver's mailbox: the sender and the sequence of
 * its blocked state, so that a sender aborted in the meantime is told apart */
struct mnode{
    uint64_t seq;
    uint32_t tid;
    uint32_t next;
};

/* Waits for the arrival of a count of parties. The waiter holds one token
 * itself, so the count cannot reach zero before it has blocked.
 */
struct join{
    int      remaining;     /* atomic */
    uint32_t tid;           /* the waiting task, or NULL_TID for a thread */
    int      thread;
    int      woken;         /* atomic: the last arrival is done with the record */
};

/* A join's registration on one future. It lives on the waiter's stack, so the
 * completer reads its link before arriving: the arrival may let it return. */
struct join_node{
    struct join      *join;
    struct join_node *next;
};

struct ev_waiter{
    uint64_t seq;
    uint32_t tid;
};

struct ev_waiters{
    struct ev_waiter *ents;
    int               size;
    int               cap;
};

#define MAX_TASKS               (8192)
#define MAX_WORKER_THREADS      (63)
#define MAX_MNODES              (2 * MAX_TASKS)
#define NLEVELS                 (8)
#define MAX_BATCHES             (4)
#define RING_CAP                (2 * MAX_TASKS)
#define STACK_SZ                (16 * 1024)
#define SLOW_SLICE_US           (3000.0)
#define SLOW_RT_SLICE_US        (1000.0)
#define BIG_STACK_SZ            (8 * 1024 * 1024)
#define SCHED_TICK_MS           (1.0f / CONFIG_SCHED_TARGET_FPS * 1000.0f)
/* The least of the frame budget the task loop keeps when the frame's tail
 * (quiesce, render wait, buffer swap) is long. */
#define SCHED_MIN_LOOP_US       (10000.0)
/* The least the task loop runs for in a frame that reached it past its
 * deadline (a GPU-bound frame), so that the ready tasks are not cut to one a
 * frame and the main thread's queue does not grow without end.
 */
#define SCHED_MIN_SLICE_US      (1000.0)
/* How long a worker out of work keeps looking before it sleeps */
#define WORKER_SPIN_US          (30.0)
/* How long a parallel-for submitter waits on the last ranges before it blocks */
#define BATCH_SPIN_US           (20.0)
#define ALIGNED(val, align)     (((val) + ((align) - 1)) & ~((align) - 1))
#define JOIN_DONE               ((uintptr_t)1)
#define BATCH_CLOSED            ((uint64_t)0x7fffffff)
#define EXIT_DONE               ((uint64_t)1)
#define MAIN_PARKER             (&s_threads[0].parker)
#define MAIN_HOME               (&s_homes[HOME_MAIN])
#define SCRIPT_HOME             (&s_homes[HOME_SCRIPT])
#define VIEW_HOME               (&s_homes[HOME_VIEW])
#define SCRIPT_IDX              (MAX_WORKER_THREADS + 1)

PQUEUE_TYPE(task, struct task*)
PQUEUE_IMPL(static, task, struct task*)

KHASH_MAP_INIT_INT(evw, struct ev_waiters)


uint64_t    sched_switch_ctx(struct context *save, struct context *restore, uint64_t retval, void *arg);
void        sched_task_exit_trampoline(void);
void        sched_task_exit(struct result ret);
static void run_task(struct sched_thread *self, struct task *task);
static void run_slice(struct sched_thread *self, struct task *task);
static struct task *service(struct sched_thread *self, struct task *task);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct task             s_tasks[MAX_TASKS];
static char                    s_stacks[MAX_TASKS][STACK_SZ];
static struct lf_istack        s_free_tasks;
static uint32_t                s_free_task_links[MAX_TASKS];

static struct mnode            s_mnodes[MAX_MNODES];
static struct lf_istack        s_free_mnodes;
static uint32_t                s_free_mnode_links[MAX_MNODES];

static struct sched_thread     s_threads[MAX_WORKER_THREADS + 2];
static __thread struct sched_thread *t_self;
static size_t                  s_nworkers;

/* Ready unpinned tasks, one ring per priority level, most urgent first */
static struct lf_ring          s_rings[NLEVELS];
static struct lf_cell          s_ring_cells[NLEVELS][RING_CAP];

static struct{
    uint64_t idle_mask LF_ALIGNED;      /* parked workers */
    int      nspinning LF_ALIGNED;      /* workers looking for work, or woken to */
    int      nactive LF_ALIGNED;        /* workers not parked */
    int      quiesce LF_ALIGNED;        /* workers must park: the frame is over */
    int      quit;
    int      main_idle LF_ALIGNED;      /* the main thread sleeps in its tick, waiting for work */
}s_sync;

/* A home is the tasks only one thread runs, with their event waiters: the
 * main thread's (engine tasks pinned to it) and the script thread's (the
 * script tasks). Only the home's thread touches its heaps and waiters; the
 * inboxes take tasks from any thread.
 */
struct home{
    uint32_t      inbox LF_ALIGNED;     /* tasks made ready for the home's thread */
    uint32_t      reg_inbox LF_ALIGNED; /* tasks registering for an event */
    pq_task_t     heap;
    pq_task_t     rt_heap;
    khash_t(evw) *waiters;
    uint64_t      deadline;
    uint64_t      rt_pass;
};

enum{
    HOME_MAIN,
    HOME_SCRIPT,
    HOME_VIEW,      /* the script thread's tasks it runs alongside the main thread */
    NHOMES
};

static struct home             s_homes[NHOMES];
static uint32_t                s_heap_seq;
static bool                    s_flushing = false;
static bool                    s_clearing = false;

/* The parallel-for batches in flight, one a slot. The claim word packs the batch's
 * generation with the next unclaimed index, so a straggler holding an old
 * generation cannot claim from a newer batch. A new batch first closes the
 * word, then release-stores its fields, then opens the word: a claimer that
 * acquired a new field value has seen the word closed, and its CAS on the old
 * word fails. The submitter's token keeps 'pending' above zero until it has
 * published its join, so only the finisher that takes it to zero touches the
 * batch after its own items, and the submitter does not return before that.
 */
struct batch{
    uint64_t      claim LF_ALIGNED;
    int           pending LF_ALIGNED;   /* items not done, plus the submitter's token */
    struct join  *join;
    void        (*func)(int, int, void*) LF_ALIGNED;
    void         *arg;
    int           nitems;
    int           chunk;
    int           level;
    uint32_t      gen;
    int           busy;                 /* atomic: a submitter holds the slot */
    uint64_t      work_self;
    uint64_t      work_others;
    uint64_t      thread_mask;
};
static struct batch            s_batches[MAX_BATCHES];

static uint64_t                s_tick_task_ticks;
static uint64_t                s_tick_quiesce_ticks;
static uint64_t                s_tick_rt_ticks;
/* End of the last task loop, and the smoothed time from there to the next
 * frame's worker start: the part of the frame the loop's deadline leaves for. */
static uint64_t                s_loop_end_pc;
static double                  s_frame_tail_ema_us = 2000.0;
static uint64_t                s_worker_spin_ticks;
static uint64_t                s_batch_spin_ticks;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/common_interface_defs.h>
#include <pthread.h>
extern int pthread_getattr_np(pthread_t, pthread_attr_t *);

static void sched_asan_capture_self_stack(const void **out_lo, size_t *out_sz)
{
    pthread_attr_t attr;
    void *lo = NULL;
    size_t sz = 0;
    if(pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &lo, &sz);
        pthread_attr_destroy(&attr);
    }
    *out_lo = lo;
    *out_sz = sz;
}
#endif

#if defined(__SANITIZE_THREAD__)
#include <sanitizer/tsan_interface.h>
#endif

/* The thread's record. A fiber may resume on another thread after any switch,
 * so this is read afresh each time and never cached across one.
 */
__attribute__((noinline
#if defined(__GNUC__) && !defined(__clang__)
    , noipa
#endif
))
static struct sched_thread *sched_self(void)
{
    __asm__ volatile("" ::: "memory");
    return t_self;
}

#define SCHED_TASK_STACK_SZ(_t) (((_t)->flags & TASK_BIG_STACK) ? BIG_STACK_SZ : STACK_SZ)

static void task_switch_in(struct sched_thread *self, struct task *task);

static void script_switch_in(void *arg)
{
    task_switch_in(sched_self(), arg);
}

static uint64_t sched_switch(struct context *save, struct context *restore, uint64_t retval,
                             void *arg, void **cur_fake, const void *dst_lo, size_t dst_sz,
                             void *dst_fiber)
{
#if defined(__SANITIZE_THREAD__)
    __tsan_switch_to_fiber(dst_fiber, 0);
#endif
#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_start_switch_fiber(cur_fake, dst_lo, dst_sz);
    uint64_t ret = sched_switch_ctx(save, restore, retval, arg);
    __sanitizer_finish_switch_fiber(cur_fake ? *cur_fake : NULL, NULL, NULL);
    return ret;
#else
    (void)cur_fake; (void)dst_lo; (void)dst_sz; (void)dst_fiber;
    return sched_switch_ctx(save, restore, retval, arg);
#endif
}

static void *thread_fake_slot(struct sched_thread *thr)
{
#if defined(__SANITIZE_ADDRESS__)
    return &thr->fake_stack;
#else
    (void)thr;
    return NULL;
#endif
}

static void *task_fake_slot(struct task *task)
{
#if defined(__SANITIZE_ADDRESS__)
    return &task->fake_stack;
#else
    (void)task;
    return NULL;
#endif
}

static const void *thread_stack_lo(struct sched_thread *thr)
{
#if defined(__SANITIZE_ADDRESS__)
    return thr->stack_lo;
#else
    (void)thr;
    return NULL;
#endif
}

static size_t thread_stack_sz(struct sched_thread *thr)
{
#if defined(__SANITIZE_ADDRESS__)
    return thr->stack_sz;
#else
    (void)thr;
    return 0;
#endif
}

static void *thread_fiber(struct sched_thread *thr)
{
#if defined(__SANITIZE_THREAD__)
    return thr->tsan_fiber;
#else
    (void)thr;
    return NULL;
#endif
}

static void *task_fiber(struct task *task)
{
#if defined(__SANITIZE_THREAD__)
    return task->tsan_fiber;
#else
    (void)task;
    return NULL;
#endif
}

#if defined(__SANITIZE_ADDRESS__)
/* Entry shim for a freshly-started fiber: finalize the switch into it (a new
 * fiber has no prior fake stack, hence NULL) before running the task body. */
static void sched_fiber_asan_entry(void *arg)
{
    __sanitizer_finish_switch_fiber(NULL, NULL, NULL);
    struct task *task = sched_self()->curr;
    sched_task_exit(((struct result (*)(void*))task->entry_code)(arg));
}
#endif

static void sched_init_ctx(struct task *task, void *code)
{
    const size_t stack_size = (task->flags & TASK_BIG_STACK) ? BIG_STACK_SZ : STACK_SZ;
    char *stack_end = task->stackmem;
    char *stack_base = stack_end + stack_size;
    stack_base = (char*)ALIGNED((uintptr_t)stack_base, 32);
    if(stack_base >= stack_end + stack_size)
        stack_base -= 32;

    /* This is the address where we will jump to upon
     * returning from the task function. This routine
     * is responsible for context-switching out of the
     * task context. */
    stack_base -= 8;
    *(uint64_t*)stack_base = (uintptr_t)sched_task_exit_trampoline;

    /* This is where we will jump to the next time we
     * will context switch to this task. */
    stack_base -= 8;
#if defined(__SANITIZE_ADDRESS__)
    task->entry_code = code;
    *(uint64_t*)stack_base = (uintptr_t)sched_fiber_asan_entry;
#else
    *(uint64_t*)stack_base = (uintptr_t)code;
#endif

    memset(&task->ctx, 0, sizeof(task->ctx));
    task->ctx.rsp = (uintptr_t)stack_base;

    /* +-------------------------------------------------------------------+
     * | Register[bits] | Setting                                          |
     * + ---------------+--------------------------------------------------+
     * | FPCSR[0:6]     | Exception masks all 1's (all exceptions masked)  |
     * | FPCSR[7]       | Reserved - 0                                     |
     * | FPCSR[8:9]     | Precision Control - 10B (double precision)       |
     * | FPCSR[10:11]   | Rounding control - 0 (round to nearest)          |
     * | FPCSR[12]      | Infinity control - 0 (not used)                  |
     * +----------------+--------------------------------------------------+
     */
    task->ctx.fpucw = (((uint16_t)(0x0  & 0x1 )) << 12)
                    | (((uint16_t)(0x0  & 0x3 )) << 10)
                    | (((uint16_t)(0x2  & 0x3 )) << 8 )
                    | (((uint16_t)(0x0  & 0x1 )) << 7 )
                    | (((uint16_t)(0x3f & 0x3f)) << 0 );

    /* The volatile portion consists of the six status flags, in MXCSR[0:5].
     * They are caller-saved, so their initial state doesn't matter.
     *
     * +-------------------------------------------------------------------+
     * | Register[bits] | Setting                                          |
     * + ---------------+--------------------------------------------------+
     * | MXCSR[6]       | Denormals are zeros - 0                          |
     * | MXCSR[7:12]    | Exception masks all 1's (all exceptions masked)  |
     * | MXCSR[13:14]   | Rounding control - 0 (round to nearest)          |
     * | MXCSR[15]      | Flush to zero for masked underflow - 0 (off)     |
     * +----------------+--------------------------------------------------+
     */
    task->ctx.mxcsr = (((uint32_t)(0x0  & 0x1 )) << 15)
                    | (((uint32_t)(0x0  & 0x3 )) << 13)
                    | (((uint32_t)(0x3f & 0x3f)) << 7 )
                    | (((uint32_t)(0x0  & 0x1 )) << 6 );
}

static bool stack_pointer_valid(const struct task *task)
{
    assert(task->stackmem != (void*)0xdeadbeef);
    uintptr_t sp = task->ctx.rsp;
    if(((uint64_t)sp) & 0x7)
        return false; /* violated alignment */

    size_t size = (task->flags & TASK_BIG_STACK) ? BIG_STACK_SZ : STACK_SZ;
    if(sp < (uintptr_t)task->stackmem)
        return false; /* overflow */

    if(sp >= ((uintptr_t)task->stackmem) + size)
        return false; /* underflow */

    return true;
}

static int prio_level(int prio)
{
    if(prio <= 0)  return 0;
    if(prio == 1)  return 1;
    if(prio <= 3)  return 2;
    if(prio <= 7)  return 3;
    if(prio <= 15) return 4;
    if(prio <= 23) return 5;
    if(prio <= 30) return 6;
    return 7;
}

static bool seq_before(uint64_t a, uint64_t b)
{
    return (int64_t)((a - b) << 8) < 0;
}

static uint64_t ticks_for_us(double us)
{
    return (uint64_t)(us * SDL_GetPerformanceFrequency() / 1e6);
}

/* Moves an owned task to its next state */
static void task_publish(struct task *task, int kind)
{
    uint64_t st = LF_LOAD(&task->state, LF_RLX);
    LF_STORE(&task->state, ST(kind, ST_SEQ(st) + 1), LF_SC);
}

/* Takes ownership of a task found in exactly the state 'expect' */
static bool task_claim(struct task *task, uint64_t expect)
{
    return LF_CAS(&task->state, &expect, ST(TK_ACTIVE, ST_SEQ(expect) + 1), LF_SC, LF_ACQ);
}

static int task_kind(const struct task *task)
{
    return ST_KIND(LF_LOAD(&task->state, LF_ACQ));
}

static int lat_class(const struct task *task)
{
    if(task->flags & TASK_REALTIME)
        return SL_REALTIME;
    if(task->flags & TASK_SCRIPT)
        return SL_SCRIPT;
    if(task->flags & TASK_MAIN_THREAD_PINNED)
        return SL_MAIN;
    return SL_GENERAL;
}

/*****************************************************************************/
/* Waking and parking                                                        */
/*****************************************************************************/

static bool batch_claimable(int *out_level, int *out_slot);

static bool general_work_visible(void)
{
    for(int i = 0; i < NLEVELS; i++) {
        if(lf_ring_maybe_nonempty(&s_rings[i]))
            return true;
    }
    return batch_claimable(NULL, NULL);
}

/* Clears a parked worker's idle bit, which earns the right to wake it, and
 * counts it as searching before it is up */
static bool wake_worker(void)
{
    for(;;) {
        uint64_t mask = LF_LOAD(&s_sync.idle_mask, LF_RLX);
        if(!mask)
            return false;
        uint64_t bit = mask & (~mask + 1);
        if(!LF_CAS(&s_sync.idle_mask, &mask, mask & ~bit, LF_AR, LF_RLX))
            continue;

        LF_FADD(&s_sync.nspinning, 1, LF_SC);
        if(lf_parker_unpark(&s_threads[__builtin_ctzll(bit) + 1].parker)) {
            SchedStats_Count(SC_SIGNALS, 1);
        }else{
            /* It withdrew from parking on its own, and is up */
            LF_FSUB(&s_sync.nspinning, 1, LF_SC);
        }
        return true;
    }
}

/* Called after making 'nqueued' tasks visible: wakes a parked worker when the
 * searching ones are fewer, else the main thread if it idles in its tick */
static void wake_for_work(uint64_t nqueued)
{
    LF_FENCE(LF_SC);
    if(LF_LOAD(&s_sync.quiesce, LF_RLX))
        return;
    if((uint64_t)LF_LOAD(&s_sync.nspinning, LF_RLX) >= nqueued)
        return;
    if(wake_worker())
        return;
    if(LF_LOAD(&s_sync.main_idle, LF_RLX))
        lf_parker_unpark(MAIN_PARKER);
}

/*****************************************************************************/
/* Task slots                                                                */
/*****************************************************************************/

static struct task *slot_alloc(void)
{
    uint32_t idx = lf_istack_pop(&s_free_tasks);
    if(idx == LF_NIL)
        return NULL;
    struct task *task = &s_tasks[idx];
    assert(task_kind(task) == TK_FREE);
    uint32_t inc = LF_LOAD(&task->inc, LF_RLX) + 1;
    LF_STORE(&task->inc, inc ? inc : 1, LF_RLX);
    task_publish(task, TK_ACTIVE);
    return task;
}

static void slot_free(struct task *task)
{
#if defined(__SANITIZE_THREAD__)
    __tsan_destroy_fiber(task->tsan_fiber);
    task->tsan_fiber = NULL;
#endif
    task_publish(task, TK_FREE);
    lf_istack_push(&s_free_tasks, task->tid - 1);
}

/*****************************************************************************/
/* Making a task ready                                                       */
/*****************************************************************************/

static float heap_prio(const struct task *task)
{
    /* Equal priorities leave in the order they came */
    return (float)task->prio + (float)(s_heap_seq++ & 0xffff) / 65536.0f;
}

/* Where a ready pinned task waits to be picked: the script thread's home for a
 * script task, and its view home for a view task that needs no world */
static struct home *task_home(const struct task *task)
{
    uint32_t flags = LF_LOAD(&task->flags, LF_RLX);
    if((flags & TASK_VIEW) && !task->needs_world)
        return VIEW_HOME;
    return (flags & TASK_SCRIPT) ? SCRIPT_HOME : MAIN_HOME;
}

/* Where its event waits are kept: a view task's with the other script tasks' */
static struct home *task_wait_home(const struct task *task)
{
    return (LF_LOAD(&task->flags, LF_RLX) & TASK_SCRIPT) ? SCRIPT_HOME : MAIN_HOME;
}

/* Set while the script thread runs a view slice, alongside the main thread */
static int s_view_slice;
/* A view task that reached the world mid-slice: no other script code runs
 * until it finishes the slice in a world window
 */
static struct task *s_parked;
/* The same, for the main thread to see */
static int s_has_parked;
/* Set while a view task is ready or runs: the main thread publishes a view of
 * the world only for the frames that have one to read it
 */
static int s_view_backlog;
/* Whether the simulation runs, as the main thread last saw: the script
 * thread picks its view tasks by it, without reading the world */
static int s_sim_running;

/* The home a thread serves, if any */
static struct home *thread_home(const struct sched_thread *self)
{
    if(self->idx == 0)
        return MAIN_HOME;
    if(self->idx == SCRIPT_IDX)
        return SCRIPT_HOME;
    return NULL;
}

static void heap_push(struct home *home, struct task *task)
{
    pq_task_push((task->flags & TASK_REALTIME) ? &home->rt_heap : &home->heap, heap_prio(task), task);
}

/* Moves the tasks made ready for the home's thread into its heaps, in the
 * order they arrived */
static void drain_inbox(struct home *home)
{
    uint32_t tid = LF_XCHG(&home->inbox, 0, LF_ACQ);
    uint32_t prev = 0;
    while(tid) {
        struct task *task = &s_tasks[tid - 1];
        uint32_t next = task->inbox_next;
        task->inbox_next = prev;
        prev = tid;
        tid = next;
    }
    for(tid = prev; tid; ) {
        struct task *task = &s_tasks[tid - 1];
        tid = task->inbox_next;
        assert(task_kind(task) == TK_READY_MAIN);
        heap_push(home, task);
    }
}

static void make_ready_pinned(struct task *task)
{
    struct home *home = task_home(task);
    task->on_main = false;
    task_publish(task, TK_READY_MAIN);

    /* Nothing takes a task in this state before it is out of the inbox, so
     * the pusher still has it to itself */
    uint32_t head = LF_LOAD(&home->inbox, LF_RLX);
    do{
        task->inbox_next = head;
    }while(!LF_CASW(&home->inbox, &head, task->tid, LF_SC, LF_RLX));
    if(home == VIEW_HOME)
        LF_STORE(&s_view_backlog, 1, LF_SC);

    /* The main thread hands the script thread its share of the task loop */
    lf_parker_unpark(MAIN_PARKER);
}

static void make_ready(struct task *task)
{
    task->ready_ts = SchedStats_Now();
    if((task->flags & TASK_MAIN_THREAD_PINNED) || task->on_main) {
        make_ready_pinned(task);
        return;
    }

    /* The task may be taken, run and its slot reused the moment it is
     * published: what the hint needs is read first. A late hint, or a late
     * write to 'queued', carries the old incarnation and is told apart.
     */
    const uint32_t tid = task->tid;
    const uint32_t inc = LF_LOAD(&task->inc, LF_RLX);
    struct lf_ring *ring = &s_rings[task->level];

    task_publish(task, TK_READY);
    if(LF_XCHG(&task->queued, inc, LF_SC) != inc) {
        while(!lf_ring_push(ring, ((uint64_t)inc << 32) | tid)) {
            assert(0);
            lf_relax();
        }
    }
    wake_for_work(lf_ring_size_hint(ring));
}

/* Pops hints off a level's ring until one claims a ready task */
static struct task *ring_take(int level)
{
    struct lf_ring *ring = &s_rings[level];
    uint64_t hint;
    while(lf_ring_maybe_nonempty(ring) && lf_ring_pop(ring, &hint)) {

        struct task *task = &s_tasks[(uint32_t)hint - 1];
        uint32_t inc = (uint32_t)(hint >> 32);
        uint32_t expect = inc;
        LF_CAS(&task->queued, &expect, 0, LF_SC, LF_RLX);

        if(LF_LOAD(&task->inc, LF_RLX) != inc)
            continue;
        uint64_t st = LF_LOAD(&task->state, LF_SC);
        if(ST_KIND(st) == TK_READY && task_claim(task, st))
            return task;
    }
    return NULL;
}

static struct task *take_general(void)
{
    for(int i = 0; i < NLEVELS; i++) {
        struct task *task = ring_take(i);
        if(task)
            return task;
    }
    return NULL;
}

/*****************************************************************************/
/* Futures and joins                                                         */
/*****************************************************************************/

static void join_init(struct join *join, struct sched_thread *self)
{
    join->remaining = 1;
    join->tid = self->curr ? self->curr->tid : NULL_TID;
    join->thread = self->idx;
    join->woken = 0;
}

static void join_arrive(struct join *join)
{
    if(LF_FSUB(&join->remaining, 1, LF_AR) != 1)
        return;

    if(join->tid != NULL_TID) {
        /* The waiter cannot run, so its record lives, until it is claimed */
        struct task *waiter = &s_tasks[join->tid - 1];
        uint64_t st = LF_LOAD(&waiter->state, LF_ACQ);
        assert(ST_KIND(st) == TK_JOIN_BLOCKED);
        bool claimed = task_claim(waiter, st);
        assert(claimed);
        (void)claimed;
        make_ready(waiter);
    }else{
        struct lf_parker *parker = &s_threads[join->thread].parker;
        LF_STORE(&join->woken, 1, LF_REL);
        lf_parker_unpark(parker);
    }
}

/* The future is left alone once its status reads complete: a poller may free
 * it then. Registered waiters are told after that, through their own nodes.
 */
static void future_complete(struct future *future, const struct result *res)
{
    if(res)
        future->res = *res;
    uintptr_t waiters = LF_XCHG(&future->waiter, JOIN_DONE, LF_AR);
    LF_STORE(&future->status.value, FUTURE_COMPLETE, LF_SC);
    /* While the state is cleared, the waiters are discarded with the rest */
    if(s_clearing)
        return;
    struct join_node *node = (struct join_node*)(waiters == JOIN_DONE ? 0 : waiters);
    while(node) {
        struct join_node *next = node->next;
        join_arrive(node->join);
        node = next;
    }
}

/* Returns false when the future completed first */
static bool future_register(struct future *future, struct join_node *node)
{
    uintptr_t head = LF_LOAD(&future->waiter, LF_ACQ);
    do{
        if(head == JOIN_DONE)
            return false;
        node->next = (struct join_node*)head;
    }while(!LF_CASW(&future->waiter, &head, (uintptr_t)node, LF_REL, LF_ACQ));
    return true;
}

static void future_await_status(const struct future *future)
{
    while(LF_LOAD(&future->status.value, LF_ACQ) != FUTURE_COMPLETE)
        lf_relax();
}

/* Blocks a thread that is not running a task until its join completes. Work
 * that only this thread can run, and the listed children not yet taken, are
 * helped along while it waits. */
static void join_wait_thread(struct sched_thread *self, struct join *join,
                             const uint32_t *tids, const struct future *futures, size_t n)
{
    if(LF_FSUB(&join->remaining, 1, LF_AR) == 1)
        return;

    int prev = SchedStats_Set(SS_JOIN);
    while(!LF_LOAD(&join->woken, LF_ACQ)) {
        for(size_t i = 0; i < n; i++) {
            if(!Sched_FutureIsReady(&futures[i]))
                Sched_RunSync(tids[i]);
        }
        lf_parker_prepare(&self->parker);
        if(LF_LOAD(&join->woken, LF_SC)) {
            lf_parker_cancel(&self->parker);
            break;
        }
        lf_parker_wait(&self->parker, 1);
    }
    SchedStats_Set(prev);
}

static void join_wait(struct sched_thread *self, struct join *join,
                      const uint32_t *tids, const struct future *futures, size_t n)
{
    if(!self->curr) {
        join_wait_thread(self, join, tids, futures, n);
        return;
    }
    Sched_Request((struct request){
        .type = _SCHED_REQ_JOIN,
        .argv[0] = (uint64_t)(uintptr_t)join
    });
}

/*****************************************************************************/
/* Parallel-for batch                                                        */
/*****************************************************************************/

/* The most urgent batch with ranges left to claim */
static bool batch_claimable(int *out_level, int *out_slot)
{
    int best = -1, best_level = NLEVELS;
    for(int i = 0; i < MAX_BATCHES; i++) {
        struct batch *b = &s_batches[i];
        uint64_t claim = LF_LOAD(&b->claim, LF_ACQ);
        if((int)(uint32_t)claim >= LF_LOAD(&b->nitems, LF_RLX))
            continue;
        int level = LF_LOAD(&b->level, LF_RLX);
        if(level < best_level) {
            best = i;
            best_level = level;
        }
    }
    if(best < 0)
        return false;
    if(out_level)
        *out_level = best_level;
    if(out_slot)
        *out_slot = best;
    return true;
}

static bool batch_claim_one(struct batch *b, bool submitter)
{
    uint64_t claim = LF_LOAD(&b->claim, LF_ACQ);
    int begin, nitems, chunk;
    void (*func)(int, int, void*);
    void *arg;
    for(;;) {
        begin = (int)(uint32_t)claim;
        nitems = LF_LOAD(&b->nitems, LF_ACQ);
        chunk = LF_LOAD(&b->chunk, LF_ACQ);
        func = LF_LOAD(&b->func, LF_ACQ);
        arg = LF_LOAD(&b->arg, LF_ACQ);
        if(begin >= nitems)
            return false;
        /* A failed claim re-reads all of it: the fields may be a newer batch's */
        if(LF_CASW(&b->claim, &claim, claim + (uint64_t)chunk, LF_ACQ, LF_ACQ))
            break;
    }

    int end = (begin + chunk > nitems) ? (nitems - 1) : (begin + chunk - 1);
    int prev = SchedStats_Set(SS_RANGE);
    uint64_t t0 = SchedStats_Now();
    func(begin, end, arg);
    uint64_t work = SchedStats_Now() - t0;
    SchedStats_Set(prev);
    SchedStats_Count(SC_RANGES, 1);
    if(g_sched_stats_on) {
        int self = SchedStats_Self();
        LF_FADD(submitter ? &b->work_self : &b->work_others, work, LF_RLX);
        if(self >= 0)
            LF_FOR(&b->thread_mask, 1ull << (self & 63), LF_RLX);
    }

    int count = end - begin + 1;
    if(LF_FSUB(&b->pending, count, LF_AR) == count)
        join_arrive(LF_LOAD(&b->join, LF_ACQ));
    return true;
}

/* Claims ranges until the batch has none left */
static void batch_drain(struct batch *b)
{
    while(!LF_LOAD(&s_sync.quiesce, LF_RLX) && batch_claim_one(b, false)) {}
}

/*****************************************************************************/
/* Event waiters                                                             */
/*****************************************************************************/

static void ev_waiters_append(struct home *home, int event, uint32_t tid, uint64_t seq)
{
    int status;
    khiter_t k = kh_get(evw, home->waiters, event);
    if(k == kh_end(home->waiters)) {
        k = kh_put(evw, home->waiters, event, &status);
        assert(status != -1 && status != 0);
        kh_val(home->waiters, k) = (struct ev_waiters){0};
    }
    struct ev_waiters *waiters = &kh_val(home->waiters, k);
    if(waiters->size == waiters->cap) {
        int cap = waiters->cap ? waiters->cap * 2 : 32;
        struct ev_waiter *ents = PF_REALLOC(waiters->ents, cap * sizeof(struct ev_waiter));
        if(!ents)
            return;
        waiters->ents = ents;
        waiters->cap = cap;
    }
    waiters->ents[waiters->size++] = (struct ev_waiter){ .seq = seq, .tid = tid };
}

/* Files the registrations made since the last drain under their events, in
 * the order they were made */
static void drain_reg_inbox(struct home *home)
{
    uint32_t tid = LF_XCHG(&home->reg_inbox, 0, LF_ACQ);
    uint32_t prev = 0;
    while(tid) {
        struct task *task = &s_tasks[tid - 1];
        uint32_t next = LF_LOAD(&task->reg_next, LF_RLX);
        LF_STORE(&task->reg_next, prev, LF_RLX);
        prev = tid;
        tid = next;
    }
    for(tid = prev; tid; ) {
        struct task *task = &s_tasks[tid - 1];
        uint32_t next = LF_LOAD(&task->reg_next, LF_RLX);
        ev_waiters_append(home, task->reg_event, tid, task->reg_seq);
        tid = next;
    }
}

/* The registration goes out before the blocked state, so that once a waker
 * can claim the task its node has already left the host's hands. Until the
 * state shows up, the main thread keeps the entry for the next firing.
 */
static void await_event(struct task *task)
{
    uint64_t st = LF_LOAD(&task->state, LF_RLX);
    task->reg_event = (int)task->req.argv[0];
    task->reg_seq = ST_SEQ(st) + 1;

    struct home *home = task_wait_home(task);
    if(home == SCRIPT_HOME)
        E_NoteScriptAwait(task->reg_event);
    uint32_t head = LF_LOAD(&home->reg_inbox, LF_RLX);
    do{
        LF_STORE(&task->reg_next, head, LF_RLX);
    }while(!LF_CASW(&home->reg_inbox, &head, task->tid, LF_REL, LF_RLX));

    LF_STORE(&task->state, ST(TK_EVENT_BLOCKED, ST_SEQ(st) + 1), LF_SC);
}

static uint64_t sched_retain_arg(struct task *task, int event, int event_source, void *arg)
{
    void *ret = NULL;
    if(event_source != ES_SCRIPT && (task->flags & TASK_SCRIPT)) {
        /* A script task's event comes already encoded as plain data */
        ret = S_EventArgCopy(arg);
        task->erelease = S_EventArgFree;
    }else if(event_source == ES_SCRIPT) {
        /* Scripting references will be released by the event consumer */
        S_Retain(arg);
        ret = arg;
    }else{
        if(event < SDL_LASTEVENT) {
            ret = PF_MALLOC(sizeof(SDL_Event));
            if(arg) {
                memcpy(ret, arg, sizeof(SDL_Event));
            }
            task->erelease = Mem_Free;
        }else if(event == EVENT_ENTERED_REGION || event == EVENT_EXITED_REGION) {
            ret = pf_strdup(arg);
            task->erelease = Mem_Free;
        }else if(event == EVENT_PROJECTILE_HIT) {
            ret = PF_MALLOC(sizeof(struct proj_hit));
            if(arg) {
                memcpy(ret, arg, sizeof(struct proj_hit));
            }
            task->erelease = Mem_Free;
        }else{
            /* We'd better be sure that the pointer won't have been 'free'd or something
             * by the time the waiting task finally runs... */
            ret = arg;
            task->erelease = NULL;
        }
    }
    task->earg = (void*)(uintptr_t)ret;
    return (uint64_t)ret;
}

/*****************************************************************************/
/* Messages                                                                  */
/*****************************************************************************/

static uint32_t mnode_alloc(void)
{
    uint32_t idx = lf_istack_pop(&s_free_mnodes);
    assert(idx != LF_NIL);
    return idx;
}

static void mnode_free(uint32_t idx)
{
    lf_istack_push(&s_free_mnodes, idx);
}

static void mbox_push(struct task *receiver, uint32_t node)
{
    uint32_t head = LF_LOAD(&receiver->mbox_head, LF_RLX);
    do{
        LF_STORE(&s_mnodes[node].next, head, LF_RLX);
    }while(!LF_CASW(&receiver->mbox_head, &head, node + 1, LF_SC, LF_RLX));
}

/* The receiver's owner takes its oldest message whose sender it can claim;
 * messages of senders aborted in the meantime are dropped */
static struct task *mbox_take(struct task *receiver)
{
    for(;;) {
        if(receiver->mbox_first == LF_NIL) {
            uint32_t head = LF_XCHG(&receiver->mbox_head, 0, LF_ACQ);
            if(!head)
                return NULL;
            uint32_t prev = LF_NIL;
            receiver->mbox_last = head - 1;
            for(uint32_t node = head - 1; node != LF_NIL; ) {
                uint32_t next = LF_LOAD(&s_mnodes[node].next, LF_RLX);
                LF_STORE(&s_mnodes[node].next, prev, LF_RLX);
                prev = node;
                node = next ? next - 1 : LF_NIL;
            }
            receiver->mbox_first = prev;
        }

        uint32_t node = receiver->mbox_first;
        receiver->mbox_first = LF_LOAD(&s_mnodes[node].next, LF_RLX);
        struct mnode msg = s_mnodes[node];
        mnode_free(node);

        struct task *sender = &s_tasks[msg.tid - 1];
        uint64_t st = ST(TK_RECV_BLOCKED, msg.seq);
        if(task_claim(sender, st))
            return sender;
    }
}

static void mbox_clear(struct task *task)
{
    while(task->mbox_first != LF_NIL) {
        uint32_t node = task->mbox_first;
        task->mbox_first = LF_LOAD(&s_mnodes[node].next, LF_RLX);
        mnode_free(node);
    }
    uint32_t head = LF_XCHG(&task->mbox_head, 0, LF_ACQ);
    for(uint32_t node = head ? head - 1 : LF_NIL; node != LF_NIL; ) {
        uint32_t next = LF_LOAD(&s_mnodes[node].next, LF_RLX);
        mnode_free(node);
        node = next ? next - 1 : LF_NIL;
    }
}

/* Run by whoever owns the receiver. Returns it when a message was taken, or
 * NULL when it is blocked again, owned by nobody. Re-checking the mailbox
 * after publishing pairs with the sender's push-then-read of the state.
 */
static struct task *receive_or_block(struct task *receiver)
{
    for(;;) {
        struct task *sender = mbox_take(receiver);
        if(sender) {
            uint32_t *out_tid = (uint32_t*)receiver->req.argv[0];
            void *dst = (void*)receiver->req.argv[1];
            size_t dstlen = (size_t)receiver->req.argv[2];
            const void *src = (const void*)sender->req.argv[1];
            size_t srclen = (size_t)sender->req.argv[2];

            assert(srclen == dstlen);
            memcpy(dst, src, srclen < dstlen ? srclen : dstlen);
            *out_tid = sender->tid;
            LF_STORE(&sender->reply_to, receiver->tid, LF_RLX);
            task_publish(sender, TK_REPLY_BLOCKED);
            return receiver;
        }

        task_publish(receiver, TK_SEND_BLOCKED);
        if(!LF_LOAD(&receiver->mbox_head, LF_SC))
            return NULL;
        uint64_t st = LF_LOAD(&receiver->state, LF_ACQ);
        if(ST_KIND(st) != TK_SEND_BLOCKED || !task_claim(receiver, st))
            return NULL;
    }
}

static struct task *do_send(struct task *sender)
{
    struct task *receiver = &s_tasks[(uint32_t)sender->req.argv[0] - 1];
    uint64_t st = LF_LOAD(&sender->state, LF_RLX);
    uint32_t node = mnode_alloc();
    s_mnodes[node].tid = sender->tid;
    s_mnodes[node].seq = ST_SEQ(st) + 1;

    /* From the publish on, the sender is the receiver's to claim */
    task_publish(sender, TK_RECV_BLOCKED);
    mbox_push(receiver, node);

    st = LF_LOAD(&receiver->state, LF_SC);
    if(ST_KIND(st) == TK_SEND_BLOCKED && task_claim(receiver, st)) {
        if(receive_or_block(receiver))
            make_ready(receiver);
    }
    return NULL;
}

static struct task *do_reply(struct task *receiver)
{
    struct task *sender = &s_tasks[(uint32_t)receiver->req.argv[0] - 1];
    uint64_t st = LF_LOAD(&sender->state, LF_ACQ);

    /* A sender aborted while it waited is not replied to */
    if(ST_KIND(st) == TK_REPLY_BLOCKED
    && LF_LOAD(&sender->reply_to, LF_RLX) == receiver->tid
    && task_claim(sender, st)) {

        void *dst = (void*)sender->req.argv[3];
        size_t dstlen = (size_t)sender->req.argv[4];
        size_t replylen = (size_t)receiver->req.argv[2];
        assert(dstlen == replylen);
        memcpy(dst, (void*)receiver->req.argv[1], dstlen < replylen ? dstlen : replylen);
        make_ready(sender);
    }
    return receiver;
}

/*****************************************************************************/
/* Exit and wait                                                             */
/*****************************************************************************/

static void task_cleanup(struct task *task)
{
    if(task->destructor) {
        task->destructor(task->darg);
        task->destructor = NULL;
    }
    if(task->erelease) {
        task->erelease(task->earg);
        task->erelease = NULL;
        task->earg = NULL;
    }
    /* The discarded task will never run to completion. Publish its
     * future so that any code polling it does not wait forever.
     */
    if(task->future) {
        future_complete(task->future, NULL);
    }
}

/* The child publishes its zombie state before it reads the handshake, so a
 * parent that saw the exit may free the slot at once. */
static void task_retire(struct task *task)
{
    PF_FREE(task->name);
    task->name = NULL;
    mbox_clear(task);

    if(task->flags & TASK_DETACHED) {
        slot_free(task);
        return;
    }

    task_publish(task, TK_ZOMBIE);
    uint64_t word = LF_XCHG(&task->exit_word, EXIT_DONE, LF_AR);
    if(word == 0)
        return;

    struct task *parent = &s_tasks[(word >> 1) - 1];
    uint64_t st = LF_LOAD(&parent->state, LF_ACQ);
    assert(ST_KIND(st) == TK_WAIT_BLOCKED);
    bool claimed = task_claim(parent, st);
    assert(claimed);
    (void)claimed;
    make_ready(parent);
    slot_free(task);
}

static struct task *do_wait(struct task *parent)
{
    uint32_t child_tid = (uint32_t)parent->req.argv[0];
    parent->retval = false;
    if(child_tid == NULL_TID || child_tid > MAX_TASKS)
        return parent;

    struct task *child = &s_tasks[child_tid - 1];
    if(LF_LOAD(&child->parent_tid, LF_RLX) != parent->tid
    || (LF_LOAD(&child->flags, LF_RLX) & TASK_DETACHED))
        return parent;

    parent->retval = true;
    task_publish(parent, TK_WAIT_BLOCKED);

    uint64_t expect = 0;
    if(LF_CAS(&child->exit_word, &expect, ((uint64_t)parent->tid) << 1, LF_SC, LF_ACQ))
        return NULL;

    /* The child is a zombie: nobody else wakes the parent, so take it back */
    assert(expect == EXIT_DONE);
    uint64_t st = LF_LOAD(&parent->state, LF_RLX);
    bool claimed = task_claim(parent, st);
    assert(claimed);
    (void)claimed;
    slot_free(child);
    return parent;
}

/*****************************************************************************/
/* Creating                                                                  */
/*****************************************************************************/

static uint32_t sched_create(int prio, task_func_t code, void *arg, const char *name,
                             struct future *future, int flags, uint32_t parent)
{
    struct task *task = slot_alloc();
    if(!task)
        return NULL_TID;

    if(flags & TASK_REALTIME)
        flags |= TASK_MAIN_THREAD_PINNED;

    task->prio = prio;
    task->level = prio_level(prio);
    LF_STORE(&task->parent_tid, parent, LF_RLX);
    LF_STORE(&task->flags, (uint32_t)flags, LF_RLX);
    LF_STORE(&task->exit_word, 0, LF_RLX);
    task->on_main = false;
    task->needs_world = false;
    task->park_request = false;
    task->retval = 0;
    task->name = pf_strdup(name);
    task->arg = arg;
    task->destructor = NULL;
    task->darg = NULL;
    task->future = future;
    task->earg = NULL;
    task->erelease = NULL;
    task->code = (void*)code;
    task->last_thread = -1;
    task->rt_pass = 0;
    task->mbox_first = LF_NIL;
    task->mbox_last = LF_NIL;
    LF_STORE(&task->reply_to, NULL_TID, LF_RLX);
#if SCHED_PERF
    pf_snprintf(task->perf_name, sizeof(task->perf_name), "Task %03u [%s]", task->tid, name);
#endif

    if(future) {
        LF_STORE(&future->waiter, 0, LF_RLX);
        LF_STORE(&future->status.value, FUTURE_INCOMPLETE, LF_RLX);
    }

    if(flags & TASK_BIG_STACK) {
        if(!task->bigstack)
            task->bigstack = PF_MALLOC(BIG_STACK_SZ);
        if(!task->bigstack) {
            PF_FREE(task->name);
            task->name = NULL;
            slot_free(task);
            return NULL_TID;
        }
        task->stackmem = task->bigstack;
    }else{
        task->stackmem = s_stacks[task->tid - 1];
    }

#if defined(__SANITIZE_THREAD__)
    task->tsan_fiber = __tsan_create_fiber(0);
#endif
    sched_init_ctx(task, (void*)code);
    uint32_t tid = task->tid;
    make_ready(task);
    return tid;
}

/*****************************************************************************/
/* Running                                                                   */
/*****************************************************************************/

static void task_switch_in(struct sched_thread *self, struct task *task)
{
    /* Python runs on the script thread only */
    if((task->flags & TASK_SCRIPT) && self->idx != SCRIPT_IDX && S_Thread_Running()) {
        S_Thread_Call(script_switch_in, task);
        return;
    }
    assert(task_kind(task) == TK_ACTIVE);
    assert(!(task->flags & TASK_MAIN_THREAD_PINNED) || thread_home(self) == task_wait_home(task));
    assert(stack_pointer_valid(task));
    /* The host context is one slot: a task is never switched in from inside another */
    assert(!self->curr);
    /* On the script thread, only a view task runs alongside the main thread */
    assert(self->idx != SCRIPT_IDX || !LF_LOAD(&s_view_slice, LF_RLX) || (task->flags & TASK_VIEW));

    self->curr = task;
#if SCHED_PERF
    Perf_Push(task->perf_name);
#endif

    int me = SchedStats_Self();
    SchedStats_Latency(lat_class(task), SchedStats_Now() - task->ready_ts);
    SchedStats_Count(SC_SLICES, 1);
    if(task->last_thread >= 0 && task->last_thread != me)
        SchedStats_Count(SC_SLICES_MIGRATED, 1);
    task->last_thread = me;
    SchedStats_SetRunning(task->code);
    SchedStats_Set(SS_TASK);
    uint64_t t0 = SchedStats_Now();

    sched_switch(&self->ctx, &task->ctx, task->retval, task->arg, thread_fake_slot(self),
        task->stackmem, SCHED_TASK_STACK_SZ(task), task_fiber(task));

    uint64_t slice = SchedStats_Now() - t0;
    SchedStats_Set(SS_SCHED);
    SchedStats_SetRunning(NULL);
    SchedStats_Slice(task->code, task->name, slice);

#if SCHED_PERF
    Perf_Pop(NULL);
#endif
    assert(stack_pointer_valid(task));
    self->curr = NULL;
}

static bool may_continue(struct sched_thread *self)
{
    struct home *home = thread_home(self);
    if(home)
        return SDL_GetPerformanceCounter() < home->deadline;
    return !LF_LOAD(&s_sync.quiesce, LF_RLX);
}

/* Host side of a task that just switched out. Returns the task to run next on
 * this thread without a trip through a queue, if any.
 */
static struct task *service(struct sched_thread *self, struct task *task)
{
    if(task->erelease) {
        task->erelease(task->earg);
        task->erelease = NULL;
        task->earg = NULL;
    }
    SchedStats_Count(SC_REQ_FIRST + (int)task->req.type, 1);

    /* A view task that blocks is done with what it needed the world for */
    if((int)task->req.type != SCHED_REQ_YIELD && (int)task->req.type != _SCHED_REQ_RUN_SYNC)
        task->needs_world = false;

    switch((int)task->req.type) {
    case SCHED_REQ_YIELD:
        if(task->park_request) {
            assert(!s_parked);
            task->park_request = false;
            /* One that reached the world once is likely to again: its next
             * slice too runs in a world window, within the frame's budget
             */
            task->needs_world = true;
            s_parked = task;
            LF_STORE(&s_has_parked, 1, LF_REL);
            task_publish(task, TK_READY_MAIN);
            return NULL;
        }
        make_ready(task);
        return NULL;
    case SCHED_REQ_SEND:
        return do_send(task);
    case SCHED_REQ_RECEIVE:
        return receive_or_block(task);
    case SCHED_REQ_REPLY:
        return do_reply(task);
    case SCHED_REQ_AWAIT_EVENT:
        await_event(task);
        return NULL;
    case SCHED_REQ_WAIT:
        return do_wait(task);
    case SCHED_REQ_RESCHED_ON_MAIN:
        task->on_main = true;
        make_ready(task);
        return NULL;
    case _SCHED_REQ_FREE:
        task_retire(task);
        return NULL;
    case _SCHED_REQ_RUN_SYNC:
        /* The requester claimed the task before asking */
        run_slice(self, &s_tasks[(uint32_t)task->req.argv[0] - 1]);
        /* It goes on with the slice it was in. A view task that had the
         * world keeps it: queued meanwhile, it comes back in a world window,
         * not as a view slice in the middle of what it was doing.
         */
        if((task->flags & TASK_VIEW) && !(self->idx == SCRIPT_IDX && LF_LOAD(&s_view_slice, LF_RLX)))
            task->needs_world = true;
        return task;
    case _SCHED_REQ_JOIN: {
        struct join *join = (struct join*)(uintptr_t)task->req.argv[0];
        task_publish(task, TK_JOIN_BLOCKED);
        if(LF_FSUB(&join->remaining, 1, LF_AR) != 1)
            return NULL;
        /* Everyone arrived first: nobody else will claim the task */
        bool claimed = task_claim(task, LF_LOAD(&task->state, LF_RLX));
        assert(claimed);
        (void)claimed;
        return task;
    }
    default: assert(0);
    }
    return NULL;
}

/* One slice and its request; a task that could go on is queued instead */
static void run_slice(struct sched_thread *self, struct task *task)
{
    task_switch_in(self, task);
    struct task *next = service(self, task);
    if(next)
        make_ready(next);
}

static void run_task(struct sched_thread *self, struct task *task)
{
    while(task) {
        task_switch_in(self, task);
        struct task *next = service(self, task);
        if(next && !may_continue(self)) {
            make_ready(next);
            next = NULL;
        }
        if(next)
            next->ready_ts = SchedStats_Now();
        task = next;
    }
}

/* Runs a task the caller claimed, on the host stack */
static void run_claimed(struct task *task)
{
    struct sched_thread *self = sched_self();
    /* On the script thread, only a view task runs alongside the main thread */
    assert(self->idx != SCRIPT_IDX || !LF_LOAD(&s_view_slice, LF_RLX) || (task->flags & TASK_VIEW));
    if(!self->curr) {
        run_slice(self, task);
        return;
    }
    S_Task_MaybeExit();
    Sched_Request((struct request){
        .type = _SCHED_REQ_RUN_SYNC,
        .argv[0] = (uint64_t)task->tid
    });
    S_Task_MaybeEnter();
}

/* Whether a task asking to yield should: something as urgent waits that this
 * thread could run instead, or the frame wants it back. */
static bool should_yield(struct sched_thread *self, struct task *task)
{
    struct home *home = thread_home(self);
    if(home) {
        if(SDL_GetPerformanceCounter() >= home->deadline)
            return true;
        if(LF_LOAD(&home->inbox, LF_RLX))
            return true;
        if(pq_size(&home->rt_heap) > 0)
            return true;
        float top;
        if(pq_task_top_prio(&home->heap, &top) && top < task->prio + 1)
            return true;
        return false;
    }

    if(LF_LOAD(&s_sync.quiesce, LF_RLX))
        return true;
    int level;
    if(batch_claimable(&level, NULL) && level <= task->level)
        return true;
    for(int i = 0; i <= task->level; i++) {
        if(lf_ring_maybe_nonempty(&s_rings[i]))
            return true;
    }
    return false;
}

/*****************************************************************************/
/* Workers                                                                   */
/*****************************************************************************/

static void worker_go_inactive(void)
{
    if(LF_FSUB(&s_sync.nactive, 1, LF_SC) != 1)
        return;
    if(LF_LOAD(&s_sync.quiesce, LF_SC) || LF_LOAD(&s_sync.main_idle, LF_SC))
        lf_parker_unpark(MAIN_PARKER);
}

/* Returns true when woken by a waker, which counted it as searching */
static bool worker_park(struct sched_thread *self)
{
    const uint64_t bit = 1ull << (self->idx - 1);
    lf_parker_prepare(&self->parker);
    LF_FOR(&s_sync.idle_mask, bit, LF_SC);
    worker_go_inactive();
    LF_FENCE(LF_SC);

    bool woken;
    bool quiesced = LF_LOAD(&s_sync.quiesce, LF_SC);
    if(LF_LOAD(&s_sync.quit, LF_SC) || (!quiesced && general_work_visible())) {
        LF_FAND(&s_sync.idle_mask, ~bit, LF_SC);
        woken = lf_parker_cancel(&self->parker);
    }else{
        if(quiesced)
            mi_stats_merge();
        SchedStats_Set(quiesced ? SS_OUTSIDE : SS_IDLE);
        lf_parker_wait(&self->parker, -1);
        SchedStats_Set(SS_SCHED);
        SchedStats_Count(SC_WAKEUPS, 1);
        woken = true;
    }
    LF_FADD(&s_sync.nactive, 1, LF_SC);
    return woken;
}

/* Looks for work for a while before the worker sleeps */
static bool worker_spin(void)
{
    int prev = SchedStats_Set(SS_SPIN);
    uint64_t end = SDL_GetPerformanceCounter() + s_worker_spin_ticks;
    bool ret = false;
    do{
        for(int i = 0; i < 16; i++)
            lf_relax();
        if(LF_LOAD(&s_sync.quiesce, LF_RLX) || LF_LOAD(&s_sync.quit, LF_RLX))
            break;
        if(general_work_visible()) {
            ret = true;
            break;
        }
    }while(SDL_GetPerformanceCounter() < end);
    SchedStats_Set(prev);
    return ret;
}

/* Takes the most urgent work: a batch range at or above a ring's level comes
 * before that ring's tasks */
static bool worker_find_work(struct task **out)
{
    int blevel, slot = 0;
    if(!batch_claimable(&blevel, &slot))
        blevel = NLEVELS;
    for(int i = 0; i < NLEVELS; i++) {
        if(blevel <= i) {
            batch_drain(&s_batches[slot]);
            return true;
        }
        if((*out = ring_take(i)))
            return true;
    }
    return false;
}

/* The last searcher to find work hands the search on, as the pushers
 * skipped waking while it looked */
static void spinner_found_work(void)
{
    if(LF_FSUB(&s_sync.nspinning, 1, LF_SC) == 1 && general_work_visible())
        wake_for_work(1);
}

static void worker_loop(struct sched_thread *self)
{
    bool spinning = false;
    bool idle_since_wake = false;

    while(!LF_LOAD(&s_sync.quit, LF_ACQ)) {

        if(!LF_LOAD(&s_sync.quiesce, LF_ACQ)) {
            struct task *task = NULL;
            if(worker_find_work(&task)) {
                if(spinning) {
                    spinning = false;
                    spinner_found_work();
                }
                idle_since_wake = false;
                if(task)
                    run_task(self, task);
                continue;
            }
            if(!spinning) {
                spinning = true;
                LF_FADD(&s_sync.nspinning, 1, LF_SC);
            }
            if(worker_spin())
                continue;
        }

        if(spinning) {
            spinning = false;
            LF_FSUB(&s_sync.nspinning, 1, LF_SC);
        }
        if(idle_since_wake)
            SchedStats_Count(SC_FUTILE_WAKES, 1);
        spinning = worker_park(self);
        idle_since_wake = spinning;
    }
    if(spinning)
        LF_FSUB(&s_sync.nspinning, 1, LF_SC);
}

static int worker_threadfn(void *arg)
{
    struct sched_thread *self = &s_threads[(uintptr_t)arg];
    t_self = self;
    SchedStats_RegisterThread(self->idx);
#if defined(__SANITIZE_ADDRESS__)
    sched_asan_capture_self_stack(&self->stack_lo, &self->stack_sz);
#endif
#if defined(__SANITIZE_THREAD__)
    self->tsan_fiber = __tsan_get_current_fiber();
#endif
    SchedStats_Set(SS_SCHED);
    worker_loop(self);
    return 0;
}

/* Parks the workers at their next yield point and waits until all of them are */
static void sched_quiesce_workers(void)
{
    ASSERT_IN_MAIN_THREAD();
    PERF_ENTER();

    LF_STORE(&s_sync.quiesce, 1, LF_SC);
    int prev = SchedStats_Set(SS_BARRIER);
    for(int i = 0; i < 256 && LF_LOAD(&s_sync.nactive, LF_SC); i++)
        lf_relax();

    while(LF_LOAD(&s_sync.nactive, LF_SC) != 0) {
        lf_parker_prepare(MAIN_PARKER);
        if(LF_LOAD(&s_sync.nactive, LF_SC) == 0) {
            lf_parker_cancel(MAIN_PARKER);
            break;
        }
        lf_parker_wait(MAIN_PARKER, -1);
    }
    SchedStats_Set(prev);
    PERF_RETURN_VOID();
}

/*****************************************************************************/
/* The main thread                                                           */
/*****************************************************************************/

static int tasks_compare(void *a, void *b)
{
    struct task *ta = *(struct task**)a;
    struct task *tb = *(struct task**)b;
    return ((uintptr_t)(ta) - (uintptr_t)(tb));
}

static bool can_run_during_pause(void *arg)
{
    struct task *task = *(struct task**)arg;
    return (task->flags & TASK_RUN_DURING_PAUSE);
}

static bool can_run_any(void *arg)
{
    return true;
}

static bool can_run_in_rt_pass(void *arg)
{
    struct task *task = *(struct task**)arg;
    return (task->rt_pass != task_home(task)->rt_pass)
        && (G_GetSimState() == G_RUNNING || (task->flags & TASK_RUN_DURING_PAUSE));
}

/* Pops the most urgent task of the heap that the predicate accepts */
static bool pop_runnable(pq_task_t *heap, struct task **out, bool pred(void*))
{
    if(pred == can_run_any)
        return pq_task_pop(heap, out);

    int best = -1;
    for(int i = 1; i <= heap->size; i++) {
        if(!pred(&heap->nodes[i].data))
            continue;
        if(best < 0 || heap->nodes[i].priority < heap->nodes[best].priority)
            best = i;
    }
    if(best < 0)
        return false;
    *out = heap->nodes[best].data;
    return pq_task_remove(heap, tasks_compare, *out);
}

/* Realtime tasks come first, then the ones only the main thread can run, and
 * the general ones last: the workers serve those too. */
static struct task *home_pick(struct home *home)
{
    drain_inbox(home);
    bool running = (G_GetSimState() == G_RUNNING);
    bool (*pred)(void*) = running ? can_run_any : can_run_during_pause;

    struct task *ret = NULL;
    if(pop_runnable(&home->rt_heap, &ret, pred) || pop_runnable(&home->heap, &ret, pred)) {
        task_publish(ret, TK_ACTIVE);
        return ret;
    }
    if(home == MAIN_HOME && (running || s_nworkers == 0))
        return take_general();
    return NULL;
}

static struct task *next_rt_pass_task(struct home *home)
{
    drain_inbox(home);
    struct task *ret = NULL;
    if(!pop_runnable(&home->rt_heap, &ret, can_run_in_rt_pass))
        return NULL;
    task_publish(ret, TK_ACTIVE);
    return ret;
}

static bool home_has_work(struct home *home)
{
    return LF_LOAD(&home->inbox, LF_SC)
        || pq_size(&home->heap) > 0
        || pq_size(&home->rt_heap) > 0;
}

/* With the perf log on, a realtime slice that is not light says so, by name */
static void log_slow_rt_slice(const char *name, uint64_t ticks)
{
    double us = ticks * 1e6 / SDL_GetPerformanceFrequency();
    if(us < SLOW_RT_SLICE_US)
        return;
    struct sval setting;
    if(Settings_Get("pf.debug.log_perf_csv", &setting) != SS_OKAY || !setting.as_bool)
        return;
    fprintf(stdout, "[slow-rt] %lu,%.0f,%s\n", g_frame_idx, us, name);
}

/* With the perf log on, a task slice that held the frame up says so, by name */
static void log_slow_slice(const char *name, uint64_t ticks)
{
    double us = ticks * 1e6 / SDL_GetPerformanceFrequency();
    if(us < SLOW_SLICE_US)
        return;
    struct sval setting;
    if(Settings_Get("pf.debug.log_perf_csv", &setting) != SS_OKAY || !setting.as_bool)
        return;
    fprintf(stdout, "[slow-task] %lu,%.0f,%s\n", g_frame_idx, us, name);
}

/* Every ready realtime task runs once, whatever the deadline. A task made
 * ready again by its own slice waits for the deadline loop, so the pass ends;
 * one woken by another's slice still runs in it.
 */
static void sched_run_realtime(struct sched_thread *self)
{
    struct home *home = thread_home(self);
    home->rt_pass++;
    struct task *curr;
    while((curr = next_rt_pass_task(home))) {

        curr->rt_pass = home->rt_pass;
        uint64_t t0 = SDL_GetPerformanceCounter();
        char name[64];
        pf_strlcpy(name, curr->name ? curr->name : "?", sizeof(name));
        run_slice(self, curr);
        uint64_t slice = SDL_GetPerformanceCounter() - t0;
        s_tick_rt_ticks += slice;
        log_slow_rt_slice(name, slice);
    }
}

/* Sleeps until work for the main thread shows up, the workers all park, or
 * the deadline comes. The last two milliseconds are spun, so that the frame
 * lands on its budget and not a timer tick past it.
 */
static void main_idle_wait(uint64_t deadline)
{
    struct lf_parker *parker = MAIN_PARKER;
    LF_STORE(&s_sync.main_idle, 1, LF_SC);
    lf_parker_prepare(parker);
    LF_FENCE(LF_SC);

    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t left_us = (now < deadline) ? (deadline - now) * 1000000 / SDL_GetPerformanceFrequency() : 0;
    bool takes_general = (G_GetSimState() == G_RUNNING) || s_nworkers == 0;
    bool wake = LF_LOAD(&MAIN_HOME->inbox, LF_SC)
             || LF_LOAD(&SCRIPT_HOME->inbox, LF_SC)
             || (takes_general && general_work_visible())
             || LF_LOAD(&s_sync.nactive, LF_SC) == 0
             || left_us < 2000;

    int prev = SchedStats_Set(SS_IDLE);
    if(wake) {
        lf_parker_cancel(parker);
        if(left_us < 2000 && left_us > 0)
            SDL_Delay(0);
    }else{
        lf_parker_wait(parker, (int)(left_us / 1000) - 1);
    }
    SchedStats_Set(prev);
    LF_STORE(&s_sync.main_idle, 0, LF_RLX);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

#ifndef _MSC_VER
__attribute__((used))
#endif
void sched_task_exit(struct result ret)
{
    struct sched_thread *self = sched_self();
    struct task *task = self->curr;

    if(task->future) {
        future_complete(task->future, &ret);
    }

    if(task->destructor) {
        task->destructor(task->darg);
        task->destructor = NULL;
    }

    if(task->erelease) {
        task->erelease(task->earg);
        task->erelease = NULL;
        task->earg = NULL;
    }

    task->req.type = _SCHED_REQ_FREE;
    self = sched_self();

#if defined(__SANITIZE_THREAD__)
    __tsan_switch_to_fiber(self->tsan_fiber, 0);
#endif
#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_start_switch_fiber(NULL, self->stack_lo, self->stack_sz);
#endif
    sched_switch_ctx(&task->ctx, &self->ctx, 0, NULL);
    assert(0);
}

static bool home_init(struct home *home)
{
    *home = (struct home){0};
    home->waiters = kh_init(evw);
    if(!home->waiters)
        return false;
    pq_task_init(&home->heap);
    pq_task_init(&home->rt_heap);
    if(!pq_task_reserve(&home->heap, MAX_TASKS) || !pq_task_reserve(&home->rt_heap, MAX_TASKS)) {
        pq_task_destroy(&home->heap);
        pq_task_destroy(&home->rt_heap);
        kh_destroy(evw, home->waiters);
        return false;
    }
    return true;
}

static void home_destroy(struct home *home)
{
    struct ev_waiters waiters;
    kh_foreach_value(home->waiters, waiters, {
        PF_FREE(waiters.ents);
    });
    kh_destroy(evw, home->waiters);
    pq_task_destroy(&home->heap);
    pq_task_destroy(&home->rt_heap);
}

bool Sched_Init(void)
{
    ASSERT_IN_MAIN_THREAD();

    struct sched_thread *main_thr = &s_threads[0];
    main_thr->idx = 0;
    t_self = main_thr;
#if defined(__SANITIZE_ADDRESS__)
    sched_asan_capture_self_stack(&main_thr->stack_lo, &main_thr->stack_sz);
#endif
#if defined(__SANITIZE_THREAD__)
    main_thr->tsan_fiber = __tsan_get_current_fiber();
#endif

    int nhomes = 0;
    for(; nhomes < NHOMES; nhomes++) {
        if(!home_init(&s_homes[nhomes]))
            goto fail_homes;
    }

    for(int i = 0; i < NLEVELS; i++) {
        lf_ring_init(&s_rings[i], s_ring_cells[i], RING_CAP);
    }

    lf_istack_init(&s_free_tasks, s_free_task_links);
    for(int i = MAX_TASKS - 1; i >= 0; i--) {
        s_tasks[i].tid = i + 1;
        s_tasks[i].state = ST(TK_FREE, 0);
        s_tasks[i].mbox_first = LF_NIL;
        s_tasks[i].mbox_last = LF_NIL;
        lf_istack_push(&s_free_tasks, i);
    }

    lf_istack_init(&s_free_mnodes, s_free_mnode_links);
    for(int i = MAX_MNODES - 1; i >= 0; i--) {
        lf_istack_push(&s_free_mnodes, i);
    }

    s_worker_spin_ticks = ticks_for_us(WORKER_SPIN_US);
    s_batch_spin_ticks = ticks_for_us(BATCH_SPIN_US);
    const char *spin_env = getenv("PF_SCHED_SPIN_US");
    if(spin_env)
        s_worker_spin_ticks = ticks_for_us(atof(spin_env));

    /* On a single-core system, all the tasks will just be run on the main thread */
    s_nworkers = SDL_GetCPUCount() - 1;
    const char *nworkers_env = getenv("PF_SCHED_WORKERS");
    if(nworkers_env && atoi(nworkers_env) >= 0)
        s_nworkers = atoi(nworkers_env);
    if(s_nworkers > MAX_WORKER_THREADS)
        s_nworkers = MAX_WORKER_THREADS;
    SchedStats_Init(s_nworkers + 1);
    SchedStats_RegisterThread(0);

    for(int i = 0; i <= s_nworkers; i++) {
        s_threads[i].idx = i;
        s_threads[i].parker.state = 0;
        s_threads[i].parker.sem = SDL_CreateSemaphore(0);
        if(!s_threads[i].parker.sem)
            goto fail_sems;
    }

    /* The workers start out active and park at once: the frame is not open */
    s_sync.quiesce = 1;
    s_sync.quit = 0;
    s_sync.nactive = s_nworkers;
    s_sync.nspinning = 0;
    s_sync.idle_mask = 0;

    for(int i = 1; i <= s_nworkers; i++) {

        char threadname[128];
        pf_snprintf(threadname, sizeof(threadname), "worker-%d", i - 1);
        s_threads[i].thread = SDL_CreateThread(worker_threadfn, threadname, (void*)((uintptr_t)i));
        if(!s_threads[i].thread)
            goto fail_workers;
        Perf_RegisterThread(SDL_GetThreadID(s_threads[i].thread), threadname);
    }
    sched_quiesce_workers();

    Task_CreateServices();
    return true;

fail_workers:
    LF_STORE(&s_sync.quit, 1, LF_SC);
    while(wake_worker()) {}
    for(int i = 1; i <= s_nworkers; i++) {
        if(s_threads[i].thread)
            SDL_WaitThread(s_threads[i].thread, NULL);
    }
fail_sems:
    for(int i = 0; i <= s_nworkers; i++) {
        if(s_threads[i].parker.sem)
            SDL_DestroySemaphore(s_threads[i].parker.sem);
    }
fail_homes:
    while(nhomes-- > 0)
        home_destroy(&s_homes[nhomes]);
    return false;
}

void Sched_RegisterScriptThread(void)
{
    struct sched_thread *thr = &s_threads[SCRIPT_IDX];
    thr->idx = SCRIPT_IDX;
    thr->parker.state = 0;
    thr->parker.sem = SDL_CreateSemaphore(0);
    assert(thr->parker.sem);
    t_self = thr;
#if defined(__SANITIZE_ADDRESS__)
    sched_asan_capture_self_stack(&thr->stack_lo, &thr->stack_sz);
#endif
#if defined(__SANITIZE_THREAD__)
    thr->tsan_fiber = __tsan_get_current_fiber();
#endif
}

void Sched_Shutdown(void)
{
    ASSERT_IN_MAIN_THREAD();
    SchedStats_Shutdown();

    LF_STORE(&s_sync.quit, 1, LF_SC);
    while(wake_worker()) {}
    for(int i = 1; i <= s_nworkers; i++) {
        SDL_WaitThread(s_threads[i].thread, NULL);
    }
    for(int i = 0; i <= s_nworkers; i++) {
        SDL_DestroySemaphore(s_threads[i].parker.sem);
    }
    if(s_threads[SCRIPT_IDX].parker.sem) {
        SDL_DestroySemaphore(s_threads[SCRIPT_IDX].parker.sem);
        s_threads[SCRIPT_IDX].parker.sem = NULL;
    }

    for(int h = 0; h < NHOMES; h++) {
        home_destroy(&s_homes[h]);
    }

    for(int i = 0; i < MAX_TASKS; i++) {
        PF_FREE(s_tasks[i].bigstack);
    }
}

/* Wakes the home's tasks waiting on the event, or runs them at once for an
 * immediate one */
static void handle_event(struct home *home, int event, void *arg, int event_source, bool immediate)
{
    int prev_state = SchedStats_Set(SS_SCHED);
    drain_reg_inbox(home);

    khiter_t k = kh_get(evw, home->waiters, event);
    if(k == kh_end(home->waiters)) {
        SchedStats_Set(prev_state);
        return;
    }

    struct ev_waiters *waiters = &kh_val(home->waiters, k);
    struct task **torun = NULL;
    int nrun = 0, nkept = 0;
    if(immediate && waiters->size > 0)
        torun = PF_MALLOC(waiters->size * sizeof(struct task*));

    for(int i = 0; i < waiters->size; i++) {

        struct ev_waiter ent = waiters->ents[i];
        struct task *task = &s_tasks[ent.tid - 1];
        uint64_t st = LF_LOAD(&task->state, LF_ACQ);

        if(st != ST(TK_EVENT_BLOCKED, ent.seq)) {
            /* Registered, but not yet blocked: it waits for the next firing */
            if(seq_before(ST_SEQ(st), ent.seq))
                waiters->ents[nkept++] = ent;
            continue;
        }
        if(!task_claim(task, st))
            continue;

        int *source = (void*)task->req.argv[1];
        *source = event_source;

        /* We don't know when the task will be scheduled next, so we've
         * got to be careful about making sure that the event arg pointer
         * doesn't become stale between now and when the task is actually
         * run.
         */
        task->retval = sched_retain_arg(task, event, event_source, arg);

        /* An immediate event raised on another home's thread queues a task
         * pinned to this home: it only runs on its own */
        bool here = !(task->flags & TASK_MAIN_THREAD_PINNED) 
                 || thread_home(sched_self()) == home;
        if(torun && here) {
            task->ready_ts = SchedStats_Now();
            torun[nrun++] = task;
        }else{
            make_ready(task);
        }
    }
    waiters->size = nkept;

    for(int i = 0; i < nrun; i++) {
        run_claimed(torun[i]);
    }
    PF_FREE(torun);
    SchedStats_Set(prev_state);
}

static bool can_run_stale_ok(void *arg)
{
    struct task *task = *(struct task**)arg;
    return (task->flags & TASK_VIEW_STALE_OK)
        && (LF_LOAD(&s_sim_running, LF_ACQ) || (task->flags & TASK_RUN_DURING_PAUSE));
}

bool Sched_RunViewSlice(bool fresh)
{
    struct sched_thread *self = sched_self();
    assert(thread_home(self) == SCRIPT_HOME);

    struct home *home = VIEW_HOME;
    if(s_parked)
        return false;
    drain_inbox(home);
    bool (*pred)(void*) = LF_LOAD(&s_sim_running, LF_ACQ) ? can_run_any : can_run_during_pause;
    if(!fresh)
        pred = can_run_stale_ok;
    struct task *curr = NULL;
    if(!pop_runnable(&home->rt_heap, &curr, pred) && !pop_runnable(&home->heap, &curr, pred)) {
        if(!fresh)
            return false;
        /* A task pushed meanwhile is seen by the second look, or its pusher
         * sets the backlog after this store
         */
        if(pq_size(&home->heap) == 0 && pq_size(&home->rt_heap) == 0) {
            LF_STORE(&s_view_backlog, 0, LF_SC);
            if(LF_LOAD(&home->inbox, LF_SC))
                LF_STORE(&s_view_backlog, 1, LF_SC);
        }
        return false;
    }
    task_publish(curr, TK_ACTIVE);

    LF_STORE(&s_view_slice, 1, LF_SC);
    run_slice(self, curr);
    LF_STORE(&s_view_slice, 0, LF_REL);
    return true;
}

bool Sched_HasParked(void)
{
    return LF_LOAD(&s_has_parked, LF_ACQ);
}

bool Sched_ViewBacklog(void)
{
    return LF_LOAD(&s_view_backlog, LF_SC);
}

bool Sched_InViewSlice(void)
{
    return LF_LOAD(&s_view_slice, LF_ACQ);
}

void Sched_ViewPark(void)
{
    struct task *task = sched_self()->curr;
    assert(task && (task->flags & TASK_VIEW) && Sched_InViewSlice());
    task->park_request = true;
    Sched_Request((struct request){ .type = SCHED_REQ_YIELD });
}

void Sched_RunParked(void)
{
    struct sched_thread *self = sched_self();
    assert(thread_home(self) == SCRIPT_HOME);
    struct task *task = s_parked;
    if(!task)
        return;
    s_parked = NULL;
    LF_STORE(&s_has_parked, 0, LF_REL);
    task_publish(task, TK_ACTIVE);
    run_task(self, task);
}

void Sched_HandleEvent(int event, void *arg, int event_source, bool immediate)
{
    ASSERT_IN_MAIN_THREAD();
    handle_event(MAIN_HOME, event, arg, event_source, immediate);
}

void Sched_HandleScriptEvent(int event, void *arg, int event_source, bool immediate)
{
    assert(thread_home(sched_self()) == SCRIPT_HOME);
    handle_event(SCRIPT_HOME, event, arg, event_source, immediate);
}

void Sched_StartBackgroundTasks(void)
{
    ASSERT_IN_MAIN_THREAD();
    SchedStats_FrameReport();

    if(s_loop_end_pc) {
        double tail_us = (SDL_GetPerformanceCounter() - s_loop_end_pc)
                       * 1e6 / SDL_GetPerformanceFrequency();
        s_frame_tail_ema_us = 0.8 * s_frame_tail_ema_us + 0.2 * tail_us;
    }

    LF_STORE(&s_sync.quiesce, 0, LF_SC);

    uint64_t queued = batch_claimable(NULL, NULL) ? s_nworkers : 0;
    for(int i = 0; i < NLEVELS; i++) {
        queued += lf_ring_size_hint(&s_rings[i]);
    }
    for(uint64_t i = 0; i < queued && i < s_nworkers; i++) {
        if(!wake_worker())
            break;
    }
}

struct script_tick{
    uint64_t deadline;
    bool     realtime;
    bool     ran;
};

/* The script thread's share of the frame's task loop, run while the main
 * thread waits on it: its realtime tasks once each first, then the others by
 * priority until none is ready or the deadline comes */
static void script_tick(void *arg)
{
    struct script_tick *tick = arg;
    struct sched_thread *self = sched_self();
    assert(thread_home(self) == SCRIPT_HOME);

    E_ServiceScriptEvents();
    if(tick->realtime) {
        if(E_TakeFrameUpkeep())
            S_FrameUpkeep();
        S_View_PollRefresh();
    }
    /* The tasks keep their least share of the frame, whatever the handlers
     * and a parked slice took before them
     */
    uint64_t deadline = tick->deadline;
    if(tick->realtime) {
        uint64_t least = SDL_GetPerformanceCounter()
            + (uint64_t)(SCHED_MIN_SLICE_US * SDL_GetPerformanceFrequency() / 1e6);
        if(deadline < least)
            deadline = least;
    }
    SCRIPT_HOME->deadline = deadline;
    if(tick->realtime)
        sched_run_realtime(self);

    /* With no view of the world published, as while a map loads, the view
     * tasks run here like the others
     */
    bool view = G_ScriptView_Current();
    struct task *curr;
    while(SDL_GetPerformanceCounter() < deadline
    && ((curr = home_pick(SCRIPT_HOME)) || (!view && (curr = home_pick(VIEW_HOME))))) {
        /* A view task that reached the world runs this slice here; its next
         * one is a view slice again
         */
        curr->needs_world = false;
        uint64_t t0 = SDL_GetPerformanceCounter();
        char name[64];
        pf_strlcpy(name, curr->name ? curr->name : "?", sizeof(name));
        run_task(self, curr);
        uint64_t slice = SDL_GetPerformanceCounter() - t0;
        s_tick_task_ticks += slice;
        log_slow_slice(name, slice);
        tick->ran = true;
    }
    SCRIPT_HOME->deadline = 0;
}

void Sched_Tick(void)
{
    ASSERT_IN_MAIN_THREAD();
    PERF_ENTER();

    struct sched_thread *self = sched_self();
    const uint64_t freq = SDL_GetPerformanceFrequency();
    const double budget_us = 1e6 / CONFIG_SCHED_TARGET_FPS;
    double loop_us = budget_us - s_frame_tail_ema_us;
    if(loop_us < SCHED_MIN_LOOP_US)
        loop_us = SCHED_MIN_LOOP_US;
    uint64_t deadline = Perf_FrameStartTicks() + (uint64_t)(loop_us * freq / 1e6);
    const uint64_t min_deadline = SDL_GetPerformanceCounter() + (uint64_t)(SCHED_MIN_SLICE_US * freq / 1e6);
    if(deadline < min_deadline)
        deadline = min_deadline;

    MAIN_HOME->deadline = deadline;
    LF_STORE(&s_sim_running, G_GetSimState() == G_RUNNING, LF_REL);
    SchedStats_Set(SS_SCHED);
    sched_run_realtime(self);

    struct script_tick tick = {deadline, true, false};
    S_Thread_Call(script_tick, &tick);
    tick.realtime = false;

    /* Use a do-while to ensure we're always making at least _some_ forward progress */
    do{
        struct task *curr = home_pick(MAIN_HOME);
        if(curr) {
            uint64_t t0 = SDL_GetPerformanceCounter();
            char name[64];
            pf_strlcpy(name, curr->name ? curr->name : "?", sizeof(name));
            run_task(self, curr);
            uint64_t slice = SDL_GetPerformanceCounter() - t0;
            s_tick_task_ticks += slice;
            log_slow_slice(name, slice);
            continue;
        }

        /* Script tasks made ready since the script thread's last turn */
        if(LF_LOAD(&SCRIPT_HOME->inbox, LF_SC)) {
            tick.ran = false;
            S_Thread_Call(script_tick, &tick);
            if(tick.ran)
                continue;
        }

        int slot;
        if(batch_claimable(NULL, &slot)) {
            batch_drain(&s_batches[slot]);
            continue;
        }

        /* With every worker parked and nothing queued, nothing more can
         * happen in this frame: let's not waste any more time. */
        if(LF_LOAD(&s_sync.nactive, LF_SC) == 0
        && !LF_LOAD(&MAIN_HOME->inbox, LF_SC)
        && !LF_LOAD(&SCRIPT_HOME->inbox, LF_SC)
        && !general_work_visible())
            break;

        main_idle_wait(deadline);

    }while(SDL_GetPerformanceCounter() < deadline);

    MAIN_HOME->deadline = 0;
    SchedStats_Set(SS_OUTSIDE);
    s_loop_end_pc = SDL_GetPerformanceCounter();
    PERF_RETURN_VOID();
}

void Sched_QuiesceWorkers(void)
{
    ASSERT_IN_MAIN_THREAD();
    uint64_t q0 = SDL_GetPerformanceCounter();
    sched_quiesce_workers();
    s_tick_quiesce_ticks += SDL_GetPerformanceCounter() - q0;
}

void Sched_LastTickTimes(uint64_t *out_task_us, uint64_t *out_quiesce_us, uint64_t *out_rt_us)
{
    double us = 1e6 / SDL_GetPerformanceFrequency();
    *out_task_us = s_tick_task_ticks * us;
    *out_quiesce_us = s_tick_quiesce_ticks * us;
    *out_rt_us = s_tick_rt_ticks * us;
    s_tick_task_ticks = 0;
    s_tick_quiesce_ticks = 0;
    s_tick_rt_ticks = 0;
}

uint32_t Sched_Create(int prio, task_func_t code, void *arg, const char *name,
                      struct future *result, int flags)
{
    return sched_create(prio, code, arg, name, result, flags | TASK_DETACHED, NULL_TID);
}

static bool claim_for_run_sync(struct sched_thread *self, struct task *task)
{
    if(!(LF_LOAD(&task->flags, LF_RLX) & TASK_DETACHED))
        return false;

    uint64_t st = LF_LOAD(&task->state, LF_ACQ);
    switch(ST_KIND(st)) {
    case TK_READY:
        return task_claim(task, st);
    case TK_READY_MAIN: {
        struct home *home = task_home(task);
        struct home *serves = thread_home(self);
        if(serves != home && !(home == VIEW_HOME && serves == SCRIPT_HOME))
            return false;
        drain_inbox(home);
        if(!pq_task_remove(&home->rt_heap, tasks_compare, task)
        && !pq_task_remove(&home->heap, tasks_compare, task))
            return false;
        task_publish(task, TK_ACTIVE);
        return true;
    }
    default:
        return false;
    }
}

bool Sched_RunSync(uint32_t tid)
{
    if(tid == NULL_TID || tid > MAX_TASKS)
        return false;
    struct sched_thread *self = sched_self();
    struct task *task = &s_tasks[tid - 1];

    int prev_state = SchedStats_Set(SS_SCHED);
    bool ret = claim_for_run_sync(self, task);
    SchedStats_Count(ret ? SC_RUNSYNC_HIT : SC_RUNSYNC_MISS, 1);
    if(ret)
        run_claimed(task);
    SchedStats_Set(prev_state);
    return ret;
}

/* Runs the children no thread has taken yet, then blocks until the rest
 * complete: a task gives its thread up meanwhile, a plain thread sleeps. */
void Sched_AwaitAll(const uint32_t *tids, const struct future *futures, size_t n)
{
    if(n == 0)
        return;

    for(size_t i = 0; i < n; i++) {
        while(!Sched_FutureIsReady(&futures[i]) && Sched_RunSync(tids[i])) {}
    }

    struct sched_thread *self = sched_self();
    struct join join;
    join_init(&join, self);
    struct join_node local[16];
    struct join_node *nodes = (n <= 16) ? local : PF_MALLOC(n * sizeof(struct join_node));

    for(size_t i = 0; i < n; i++) {
        struct future *future = (struct future*)&futures[i];
        if(Sched_FutureIsReady(future))
            continue;
        /* Counted before it is published, so no arrival can reach zero early */
        LF_FADD(&join.remaining, 1, LF_RLX);
        nodes[i].join = &join;
        if(future_register(future, &nodes[i]))
            continue;
        LF_FSUB(&join.remaining, 1, LF_RLX);
        future_await_status(future);
    }
    join_wait(self, &join, tids, futures, n);

    if(nodes != local)
        PF_FREE(nodes);
}

void Sched_ParallelFor(void (*fn)(int begin, int end, void *arg), void *arg,
                       size_t nitems, size_t chunk)
{
    if(nitems == 0)
        return;

    assert(chunk > 0);

    /* With every slot taken, the caller runs the ranges itself */
    struct batch *b = NULL;
    for(int i = 0; i < MAX_BATCHES && !b; i++) {
        int expect = 0;
        if(LF_CAS(&s_batches[i].busy, &expect, 1, LF_ACQ, LF_RLX))
            b = &s_batches[i];
    }
    if(!b) {
        for(size_t begin = 0; begin < nitems; begin += chunk)
            fn((int)begin, (int)((begin + chunk < nitems) ? begin + chunk : nitems) - 1, arg);
        return;
    }

    struct sched_thread *self = sched_self();
    int level = self->curr ? self->curr->level : 0;
    uint32_t gen = ++b->gen;

    LF_STORE(&b->claim, (((uint64_t)gen) << 32) | BATCH_CLOSED, LF_SC);
    LF_STORE(&b->func, fn, LF_REL);
    LF_STORE(&b->arg, arg, LF_REL);
    LF_STORE(&b->nitems, (int)nitems, LF_REL);
    LF_STORE(&b->chunk, (int)chunk, LF_REL);
    LF_STORE(&b->level, level, LF_REL);
    LF_STORE(&b->pending, (int)nitems + 1, LF_RLX);
    LF_STORE(&b->join, NULL, LF_RLX);
    b->work_self = 0;
    b->work_others = 0;
    b->thread_mask = 0;
    uint64_t t0 = SchedStats_Now();
    LF_STORE(&b->claim, ((uint64_t)gen) << 32, LF_REL);

    size_t nchunks = (nitems + chunk - 1) / chunk;
    wake_for_work(nchunks > 1 ? nchunks - 1 : 0);
    for(size_t i = 1; i + 1 < nchunks && i < s_nworkers; i++) {
        if(LF_LOAD(&s_sync.quiesce, LF_RLX) || !LF_LOAD(&s_sync.idle_mask, LF_RLX))
            break;
        if((size_t)LF_LOAD(&s_sync.nspinning, LF_RLX) >= nchunks - 1)
            break;
        wake_worker();
    }

    /* A submitting fiber must stay preemptible between ranges: the per-frame
     * worker quiesce blocks the main thread until the fiber's host yields.
     * On a native thread the try-yield is a no-op.
     */
    while(batch_claim_one(b, true)) {
        Sched_TryYield();
    }

    int prev = SchedStats_Set(SS_JOIN);
    uint64_t spin_end = SDL_GetPerformanceCounter() + s_batch_spin_ticks;
    while(LF_LOAD(&b->pending, LF_ACQ) > 1
       && SDL_GetPerformanceCounter() < spin_end) {
        lf_relax();
    }
    self = sched_self();
    struct join join;
    join_init(&join, self);
    join.remaining = 2;
    LF_STORE(&b->join, &join, LF_RLX);
    if(LF_FSUB(&b->pending, 1, LF_AR) != 1)
        join_wait(self, &join, NULL, NULL, 0);
    SchedStats_Set(prev);

    SchedStats_Batch((void*)fn, nitems, SchedStats_Now() - t0,
        LF_LOAD(&b->work_self, LF_RLX),
        LF_LOAD(&b->work_others, LF_RLX),
        __builtin_popcountll(LF_LOAD(&b->thread_mask, LF_RLX)));
    /* Nothing touches the slot once its pending count is spent */
    LF_STORE(&b->busy, 0, LF_REL);
}

void Sched_ClearState(void)
{
    ASSERT_IN_MAIN_THREAD();
    sched_quiesce_workers();

    struct task *keep = sched_self()->curr;
    s_parked = NULL;
    LF_STORE(&s_has_parked, 0, LF_REL);

    for(int i = 0; i < NLEVELS; i++) {
        lf_ring_init(&s_rings[i], s_ring_cells[i], RING_CAP);
    }
    for(int h = 0; h < NHOMES; h++) {
        struct home *home = &s_homes[h];
        pq_task_clear(&home->heap);
        pq_task_clear(&home->rt_heap);
        LF_STORE(&home->inbox, 0, LF_RLX);
        LF_STORE(&home->reg_inbox, 0, LF_RLX);

        int event;
        struct ev_waiters *waiters;
        (void)event;
        kh_foreach_val_ptr(home->waiters, event, waiters, {
            waiters->size = 0;
        });
    }

    s_clearing = true;

    for(int i = 0; i < MAX_TASKS; i++) {

        struct task *task = &s_tasks[i];
        LF_STORE(&task->queued, 0, LF_RLX);
        LF_STORE(&task->mbox_head, 0, LF_RLX);
        task->mbox_first = LF_NIL;
        task->mbox_last = LF_NIL;
        if(task == keep)
            continue;

        switch(task_kind(task)) {
        case TK_FREE:
            continue;
        case TK_READY:
        case TK_READY_MAIN:
        case TK_EVENT_BLOCKED:
        case TK_RECV_BLOCKED:
        case TK_SEND_BLOCKED:
            task_cleanup(task);
            break;
        default:
            break;
        }
        PF_FREE(task->name);
        task->name = NULL;
#if defined(__SANITIZE_THREAD__)
        __tsan_destroy_fiber(task->tsan_fiber);
        task->tsan_fiber = NULL;
#endif
        task_publish(task, TK_FREE);
    }
    s_clearing = false;

    lf_istack_init(&s_free_mnodes, s_free_mnode_links);
    for(int i = MAX_MNODES - 1; i >= 0; i--) {
        lf_istack_push(&s_free_mnodes, i);
    }
    lf_istack_init(&s_free_tasks, s_free_task_links);
    for(int i = MAX_TASKS - 1; i >= 0; i--) {
        if(&s_tasks[i] != keep)
            lf_istack_push(&s_free_tasks, i);
    }

    Task_CreateServices();
}

uint64_t Sched_Request(struct request req)
{
    struct sched_thread *self = sched_self();
    struct task *task = self->curr;

    switch((int)req.type) {
    case SCHED_REQ_MY_TID:
        return task->tid;
    case SCHED_REQ_MY_PARENT_TID:
        return task->parent_tid;
    case SCHED_REQ_SET_DESTRUCTOR:
        task->destructor = (void (*)(void*))req.argv[0];
        task->darg = (void*)req.argv[1];
        return 0;
    case SCHED_REQ_CREATE:
        return sched_create(
            (int)           req.argv[0],
            (task_func_t)   req.argv[1],
            (void*)         req.argv[2],
            (const char*)   req.argv[3],
            (struct future*)req.argv[4],
            (int)           req.argv[5],
            task->tid
        );
    default:
        break;
    }

    task->req = req;

#if SCHED_PERF
    /* Save every entry the task pushed above its `Task NNN` wrapper. The
     * wrapper's matching Perf_Pop runs on the host when we switch out, and
     * it pops the topmost entry: anything left above the wrapper would be
     * popped instead of the wrapper itself, scrambling the stack for both
     * this task's resumption and any other task that runs in the meantime.
     * Names live in the per-thread name_id_table and are stable. */
    const char *saved[64];
    int nsaved = 0;
    while(!Perf_IsRoot() && nsaved < (int)(sizeof(saved)/sizeof(saved[0]))) {
        const char *name = NULL;
        Perf_Pop(&name);
        saved[nsaved++] = name;
    }
#endif

    uint64_t ret = sched_switch(&task->ctx, &self->ctx, 0, NULL, task_fake_slot(task),
        thread_stack_lo(self), thread_stack_sz(self), thread_fiber(self));

#if SCHED_PERF
    for(int i = nsaved - 1; i >= 0; i--) {
        if(saved[i])
            Perf_Push(saved[i]);
    }
#endif
    return ret;
}

uint32_t Sched_ActiveTID(void)
{
    struct sched_thread *self = sched_self();
    struct task *curr = self->curr;
    /* Outside its own tasks, the script thread runs what the main thread's
     * current task called it for, while the main thread waits on it */
    if(!curr && self->idx == SCRIPT_IDX)
        curr = s_threads[0].curr;
    return curr ? curr->tid : NULL_TID;
}

bool Sched_FutureIsReady(const struct future *future)
{
    return (LF_LOAD(&future->status.value, LF_ACQ) == FUTURE_COMPLETE);
}

void Sched_TryYield(void)
{
    struct sched_thread *self = sched_self();
    struct task *task = self->curr;
    if(!task || !should_yield(self, task))
        return;
    Sched_Request((struct request) { .type = SCHED_REQ_YIELD });
}

/* Like Sched_Create, but attempts to run a task to completion
 * when it's not able to allocate a TID
 */
uint32_t Sched_CreateBlocking(int prio, task_func_t code, void *arg, const char *name,
                              struct future *result, int flags)
{
    ASSERT_IN_MAIN_THREAD();
    uint32_t ret;

    do{
        ret = Sched_Create(prio, code, arg, name, result, flags);
        if(ret != NULL_TID)
            break;

        struct home *home = thread_home(sched_self());
        drain_inbox(home);
        struct task *task = NULL;
        if(pq_task_pop(&home->rt_heap, &task) || pq_task_pop(&home->heap, &task)) {
            task_publish(task, TK_ACTIVE);
        }else{
            task = take_general();
        }
        /* All the tasks are blocked. Nothing more we can do until the next event pump */
        if(!task)
            break;
        run_claimed(task);

    }while(ret == NULL_TID);

    return ret;
}

bool Sched_UsingBigStack(void)
{
    struct task *task = sched_self()->curr;
    if(!task)
        return true;
    return (task->flags & TASK_BIG_STACK);
}

/* Runs the home's ready tasks once each, on the home's own thread */
static void flush_home(void *arg)
{
    struct home *home = arg;
    assert(thread_home(sched_self()) == home 
        || (home == VIEW_HOME && thread_home(sched_self()) == SCRIPT_HOME));
    struct task *curr;

    for(;;) {
        drain_inbox(home);
        if(!pq_task_pop(&home->heap, &curr))
            break;
        task_publish(curr, TK_ACTIVE);
        run_claimed(curr);
    }
    for(;;) {
        drain_inbox(home);
        if(!pq_task_pop(&home->rt_heap, &curr))
            break;
        task_publish(curr, TK_ACTIVE);
        run_claimed(curr);
    }
}

void Sched_Flush(void)
{
    ASSERT_IN_MAIN_THREAD();

    s_flushing = true;
    sched_quiesce_workers();
    struct task *curr;

    while((curr = take_general())) {
        run_claimed(curr);
    }
    S_Thread_CallMain(flush_home, MAIN_HOME);
    S_Thread_Call(flush_home, SCRIPT_HOME);
    S_Thread_Call(flush_home, VIEW_HOME);
    s_flushing = false;
}

bool Sched_HasBlocked(void)
{
    ASSERT_IN_MAIN_THREAD();
    for(int i = 0; i < NLEVELS; i++) {
        if(lf_ring_maybe_nonempty(&s_rings[i]))
            return true;
    }
    return home_has_work(MAIN_HOME) || home_has_work(SCRIPT_HOME) || home_has_work(VIEW_HOME);
}

bool Sched_IsReady(uint32_t tid)
{
    int kind = task_kind(&s_tasks[tid - 1]);
    return (kind == TK_READY) || (kind == TK_READY_MAIN);
}

bool Sched_IsEventBlocked(uint32_t tid)
{
    return task_kind(&s_tasks[tid - 1]) == TK_EVENT_BLOCKED;
}

bool Sched_TryCancel(uint32_t tid)
{
    ASSERT_IN_MAIN_THREAD();
    struct task *task = &s_tasks[tid - 1];
    uint64_t st = LF_LOAD(&task->state, LF_ACQ);
    if(ST_KIND(st) != TK_EVENT_BLOCKED || !task_claim(task, st))
        return false;

    /* Its registration reached the inbox before its state did */
    drain_reg_inbox(MAIN_HOME);

    if(task->destructor) {
        task->destructor(task->darg);
        task->destructor = NULL;
    }
    if(task->erelease) {
        task->erelease(task->earg);
        task->erelease = NULL;
        task->earg = NULL;
    }
    task_retire(task);
    return true;
}

/* Wake a blocked script-owned task with a NULL retval so the awaiter resumes
 * and can propagate any pending Python exception. Only safe for TASK_SCRIPT
 * tasks since the wake-up returns garbage from the underlying Task_X call;
 * engine-side C task code is not prepared to handle that.
 */
bool Sched_AbortScriptBlocked(uint32_t tid)
{
    ASSERT_IN_MAIN_THREAD();
    struct task *task = &s_tasks[tid - 1];
    assert(task->flags & TASK_SCRIPT);

    uint64_t st = LF_LOAD(&task->state, LF_ACQ);
    switch(ST_KIND(st)) {
    case TK_EVENT_BLOCKED: {
        if(!task_claim(task, st))
            return false;
        drain_reg_inbox(SCRIPT_HOME);
        int *source = (void*)task->req.argv[1];
        if(source)
            *source = ES_ENGINE;
        task->retval = 0;
        make_ready(task);
        return true;
    }
    case TK_RECV_BLOCKED:
    case TK_SEND_BLOCKED:
    case TK_REPLY_BLOCKED:
        /* A message node left behind no longer matches the task's state */
        if(!task_claim(task, st))
            return false;
        make_ready(task);
        return true;
    default:
        return false;
    }
}

