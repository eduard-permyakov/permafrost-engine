/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2023 Eduard Permyakov 
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

#ifndef MAIN_H
#define MAIN_H

#include <SDL.h>
#include <assert.h>
#include <stdbool.h>

extern const char    *g_basepath;      /* readonly */
extern unsigned       g_last_frame_ms; /* readonly */
extern unsigned long  g_frame_idx;     /* readonly */
extern SDL_threadID   g_main_thread_id;   /* readonly */
extern SDL_threadID   g_render_thread_id; /* readonly */
extern const char     g_version[]; /* readonly */


#define ASSERT_IN_RENDER_THREAD() \
    assert(SDL_ThreadID() == g_render_thread_id)

#define ASSERT_IN_MAIN_THREAD() \
    assert(SDL_ThreadID() == g_main_thread_id)


enum pf_window_flags {

    PF_WF_FULLSCREEN     = SDL_WINDOW_FULLSCREEN 
                         | SDL_WINDOW_INPUT_GRABBED,
    PF_WF_BORDERLESS_WIN = SDL_WINDOW_BORDERLESS 
                         | SDL_WINDOW_INPUT_GRABBED,
    PF_WF_WINDOW         = SDL_WINDOW_INPUT_GRABBED,
};


int  Engine_SetRes(int w, int h);
void Engine_SetDispMode(enum pf_window_flags wf);
void Engine_WinDrawableSize(int *out_w, int *out_h);
void Engine_SetRenderThreadID(SDL_threadID id);

/* Execute all the currently queued render commands on the render thread. 
 * Block until it completes. This is used during initialization only to 
 * execute rendering code serially.
 */
void Engine_FlushRenderWorkQueue(void);
/* Wait for the current batch of render command to finish */
void Engine_WaitRenderWorkDone(void);
void Engine_ClearPendingEvents(void);
bool Engine_GetArg(const char *name, size_t maxout, char out[]);
bool Engine_InRunningState(void);

/* Present the window (from render thread only).
 */
void Engine_SwapWindow(void);

#endif

