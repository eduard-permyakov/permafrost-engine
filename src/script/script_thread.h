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

/* View mode: what a view task may do without the world, run on the script
 * thread alongside the main thread */
void S_View_Init(struct _object *module);
bool S_View_Active(void);
/* A script task's thread state comes in: a view task in a view slice is
 * watched for what it reaches */
void S_View_EnterTask(bool view);
/* Runs a view task's slice, alongside the main thread, on the newest view */
bool S_View_RunSlice(void);
/* For a binding: the view to read in a view slice, or NULL for the live world.
 * Returns -1 if the task was ended while it waited for the world. */
struct script_view;
int  S_View_Read(const struct script_view **out);
/* For a binding that only reads: in a view slice, answers from what was polled,
 * waiting for the world the first time. Returns true when '*out' is the
 * binding's result (NULL with an exception set), false when it is to go on and
 * read the world itself.
 */
enum{
    VIEW_POLL_GETSET,
    VIEW_POLL_NOARGS,
    VIEW_POLL_VARARGS,
};
bool S_View_Poll(int kind, void *fn, struct _object *self, struct _object *args, 
                 const char *what, struct _object **out);
#define S_VIEW_POLLED(_fn, _self, _args)                                                        \
    do{                                                                                         \
        struct _object *_polled;                                                                \
        if(S_View_Poll(((_args) ? VIEW_POLL_VARARGS : VIEW_POLL_NOARGS), (void*)(_fn),          \
            (struct _object*)(_self), (struct _object*)(_args), #_fn, &_polled))                \
            return (void*)_polled;                                                              \
    }while(0)
/* A script task's thread state goes out */
void S_View_LeaveTask(void);
void S_View_Shutdown(void);
/* Queues a write a view task made for the next world window */
void S_View_DeferRemove(uint32_t uid);
void S_View_ApplyDeferred(void);

#endif
