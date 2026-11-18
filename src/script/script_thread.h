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

#ifndef SCRIPT_THREAD_H
#define SCRIPT_THREAD_H

#include "public/script.h"

struct _object;

/* True where Python must not run: on the main thread once the script thread is up */
#define ST_OFF_SCRIPT() (S_Thread_Running() && !S_Thread_OnScript())

/* An event handler that runs Python, moved onto the script thread */
void S_Thread_CallHandler(void (*handler)(void *user, void *event), void *user, void *event);

/* The flag the interpreter checks to let a view slice give the thread up */
volatile int *S_Thread_PreemptFlag(void);

/* View mode: what a view task may do without the world, run on the script
 * thread alongside the main thread */
void S_View_Init(struct _object *module);
/* From a binding or constructor a view task reached: the rest of the slice
 * runs in the main thread's next world window */
int  S_View_RequireWorld(void);
bool S_View_Active(void);
/* A script task's thread state comes in: a view task in a view slice is
 * watched for what it reaches */
void S_View_EnterTask(bool view);
/* Queues a write a view task made for the next world window */
void S_View_DeferRemove(uint32_t uid);
void S_View_ApplyDeferred(void);

#endif
