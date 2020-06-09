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
#include "task.h"
#include "lib/public/pqueue.h"

#include <setjmp.h>

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

struct task{
    enum taskstate state;
    struct context ctx;
    int            prio;
    uint32_t       tid;
    uint32_t       parent_tid;
    uint32_t       flags;
};

#define MAX_TASKS   (512)
#define STACK_SZ    (64 * 1024)
#define NPRIORITIES (32)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static uint64_t    s_freelist[MAX_TASKS / 64];
static struct task s_tasks[MAX_TASKS];
static char        s_stacks[MAX_TASKS][STACK_SZ];

//TODO: create threadpool for running the tasks on different threads...

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

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Sched_Init(void)
{
    return true;
}

void Sched_Shutdown(void)
{

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

