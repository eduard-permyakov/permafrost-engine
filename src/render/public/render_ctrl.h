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

#ifndef RENDER_CTRL_H
#define RENDER_CTRL_H

#include "../../lib/public/queue.h"
#include "../../lib/public/stalloc.h"
#include "../../pf_math.h"

#include <stddef.h>

#include <SDL_video.h>
#include <SDL_mutex.h>
#include <SDL_thread.h>


struct frustum;
struct tile_desc;
struct map;
struct camera;

enum render_info{
    RENDER_INFO_VENDOR,
    RENDER_INFO_RENDERER,
    RENDER_INFO_VERSION,
    RENDER_INFO_SL_VERSION,
};

struct render_init_arg{
    SDL_Window *in_window;
    int         in_width; 
    int         in_height;
    bool        out_success;
};

enum render_status{
    RSTAT_NONE,
    RSTAT_DONE,
    RSTAT_YIELD,
};

struct render_sync_state{
    /* The render thread owns the data pointed to by 'arg' until
     * signalling the first 'done'. */
    struct render_init_arg *arg;
    /* The start flag is set by the main thread when the render
     * thread is allowed to start processing commands.
     * The quit flag is set by the main thread when the render 
     * thread should exit. */
    bool       start;
    bool       quit;
    SDL_mutex *sq_lock;
    SDL_cond  *sq_cond;
    /* The status is set by the render thread when it is done 
     * procesing commands for the current frame, or it wants to 
     * yield. */
    enum render_status status;
    SDL_mutex *done_lock;
    SDL_cond  *done_cond;
    /* Flag to specify if the framebuffer should be presented on
     * the screen after all commands are executed */
    bool       swap_buffers;
};

#define MAX_ARGS 10

struct rcmd{
    void (*func)();
    size_t nargs;
    void *args[MAX_ARGS];
};

QUEUE_TYPE(rcmd, struct rcmd)
QUEUE_IMPL(static inline, rcmd, struct rcmd)

struct render_workspace{
    /* Stack allocator for storing all the data/arguments associated
     * with the commands */
    struct memstack   args;
    queue_rcmd_t      commands;
};


bool        R_Init(const char *base_path);
SDL_Thread *R_Run(struct render_sync_state *rstate);
/* Must be set up before creating the window */
void        R_InitAttributes(void);
bool        R_ComputeShaderSupported(void);

void       *R_PushArg(const void *src, size_t size);
void       *R_AllocArg(size_t size);
void        R_PushCmd(struct rcmd cmd);
/* Pushes the command to the render queue directly. May only 
 * be called when the render thread is quiesced */
void        R_PushCmdImmediate(struct rcmd cmd);
void        R_PushCmdImmediateFront(struct rcmd cmd);

bool        R_InitWS(struct render_workspace *ws);
void        R_DestroyWS(struct render_workspace *ws);
void        R_ClearWS(struct render_workspace *ws);

const char *R_GetInfo(enum render_info attr);

void        R_LightFrustum(vec3_t light_pos, vec3_t cam_pos, vec3_t cam_dir, struct frustum *out);
void        R_LightVisibilityFrustum(const struct camera *cam, struct frustum *out);

/* Tile */
int         R_TileGetTriMesh(const struct map *map, struct tile_desc *td, mat4x4_t *model, vec3_t out[]);

/* UI */
int         R_UI_GetFontTexID(void);


#endif

