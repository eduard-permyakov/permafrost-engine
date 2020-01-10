/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019 Eduard Permyakov 
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

#ifndef RENDER_CTRL_H
#define RENDER_CTRL_H

#include "../../lib/public/queue.h"
#include "../../lib/public/stalloc.h"

#include <stddef.h>

#include <SDL_mutex.h>
#include <SDL_thread.h>


struct nk_buffer;

//TODO: put all immediate-mode calls to be made from the sim thread in this header
// and leave

struct render_sync_state{
    /* The start flag is set by the main thread when the render
     * thread is allowed to start processing commands.
     * The quit flag is set by the main thread when the render 
     * thread should exit. */
    bool       start;
    bool       quit;
    SDL_mutex *sq_lock;
    SDL_cond  *sq_cond;
    /* The done flag is set by the render thread when it is done 
     * procesing commands for the current frame. */
    bool       done;
    SDL_mutex *done_lock;
    SDL_cond  *done_cond;
};

struct rcmd_arg_desc{
    enum{
        RARG_PTR,
        RARG_UINT,
        RARG_INT,
        RARG_FLOAT,
        RARG_VEC2,
        RARG_VEC3,
        RARG_VEC4,
        RARG_MAT3,
        RARG_MAT4,
        RARG_TILE_DESC,
        RARG_CHUNK_COORD,
    }type;
    void *data;
};

#define MAX_ARGS 8

struct rcmd{
    void (*func)();
    size_t nargs;
    struct rcmd_arg_desc args[MAX_ARGS];
};

QUEUE_TYPE(rcmd, struct rcmd)
QUEUE_IMPL(static inline, rcmd, struct rcmd)

struct render_workspace{
    /* Stack allocator for storing all the data/arguments associated
     * with the commands */
    struct memstack  *args;
    queue_rcmd_t      commands;
    struct nk_buffer *nukear_cmds;
};


bool        R_Init(const char *base_path);
SDL_Thread *R_Run(struct render_sync_state *rstate);

#endif

