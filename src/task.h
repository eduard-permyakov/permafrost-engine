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

#ifndef TASK_H
#define TASK_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

struct taskret;
struct future;

typedef struct result (*task_t)(void *);

/* The following may only be called from task context 
 * (i.e. from the body of a task function) */

uint32_t Task_Create(int prio, task_t code, void *arg, struct future *result, int flags);
bool     Task_Wait(uint32_t child_tid);
uint32_t Task_MyTid(void);
uint32_t Task_ParentTid(void);
void     Task_Yield(void);
void     Task_Send(uint32_t tid, void *msg, size_t msglen, void *reply, size_t replylen);
void     Task_Receive(uint32_t *tid, void *msg, size_t msglen);
void     Task_Reply(uint32_t tid, void *reply, size_t replylen);
void    *Task_AwaitEvent(int event);
void     Task_Sleep(int ms);
void     Task_SetDestructor(void (*destructor)(void*), void *darg);

#endif

