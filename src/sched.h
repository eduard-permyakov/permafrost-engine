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

#include "lib/public/attr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <SDL_atomic.h>


#define NULL_TID (0)

enum comp_status{
    COMP_INCOMPLETE = 0,
    COMP_COMPLETE   = 1
};

struct future{
    struct attr  retval;
    SDL_atomic_t status;
};

/* The following may only be called from main thread context */

bool     Sched_Init(void);
void     Sched_Shutdown(void);
void     Sched_HandleEvent(int event, void *arg);
void     Sched_StartBackgroundTasks(void);
void     Sched_Tick(void);
uint32_t Sched_Create(int prio, void (*code)(void *), void *arg, struct future *result);
/* Same as Sched_Create, except the task may be scheduled on
 * one of the worker threads, instead of just the main thread. */
uint32_t Sched_CreateJob(int prio, void (*code)(void *), void *arg, struct future *result);

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
    _SCHED_REQ_COUNT,
};

struct request{
    enum reqtype type;
    uint64_t     argv[5];
};

/* The following may only be called from task context 
 * (i.e. from the body of a task function) */

uint64_t Sched_Request(struct request req);

#endif

