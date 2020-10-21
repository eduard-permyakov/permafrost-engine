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

#ifndef SCHED_H
#define SCHED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <SDL_atomic.h>


#define NULL_TID (0)
#define NULL_RESULT (struct result){0}

#define ASSERT_IN_CTX(tid) \
    assert(Sched_ActiveTID() == (tid))

enum{
    RESULT_FLOAT,
    RESULT_INT,
    RESULT_BOOL,
    RESULT_PTR,
};

/* Careful changing the size of this. It is assumed to be (64-128] bits 
 * s.t. it's returned via registers.
 */
struct result{
    uint64_t type;
    union{
        float    as_float;
        uint64_t as_int;
        bool     as_bool;
        void    *as_ptr;
    }val;
};

enum comp_status{
    FUTURE_INCOMPLETE = 0,
    FUTURE_COMPLETE   = 1
};

struct future{
    struct result res;
    SDL_atomic_t  status;
};

enum{
    TASK_MAIN_THREAD_PINNED = (1 << 0),
    TASK_DETACHED           = (1 << 1),
    TASK_BIG_STACK          = (1 << 2),
};

typedef struct result (*task_func_t)(void *);

/* The following may only be called from any context */

bool     Sched_FutureIsReady(const struct future *future);

/* The following may only be called from main thread context */

bool     Sched_Init(void);
void     Sched_Shutdown(void);
void     Sched_HandleEvent(int event, void *arg, int event_source, bool immediate);
void     Sched_StartBackgroundTasks(void);
void     Sched_Tick(void);
uint32_t Sched_Create(int prio, task_func_t code, void *arg, struct future *result, int flags);
bool     Sched_RunSync(uint32_t tid);
void     Sched_ClearState(void);

/* The following may only be called from task context 
 * (i.e. from the body of a task function) */

enum reqtype{
    SCHED_REQ_CREATE,
    SCHED_REQ_MY_TID,
    SCHED_REQ_MY_PARENT_TID,
    SCHED_REQ_YIELD,
    SCHED_REQ_SEND,
    SCHED_REQ_RECEIVE,
    SCHED_REQ_REPLY,
    SCHED_REQ_AWAIT_EVENT,
    SCHED_REQ_SET_DESTRUCTOR,
    SCHED_REQ_WAIT,
    _SCHED_REQ_COUNT,
};

struct request{
    enum reqtype type;
    uint64_t     argv[5];
};

uint64_t Sched_Request(struct request req);
uint32_t Sched_ActiveTID(void);

#endif

