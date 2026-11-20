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
#include "../../lib/public/pf_math.h"

#include <stddef.h>

#include <SDL_video.h>
#include <SDL_mutex.h>
#include <SDL_thread.h>


struct frustum;
struct tile_desc;
struct map;
struct camera;
struct render_input;
struct splatmap;
struct map_resolution;
struct obb;

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
    /* Set by the main thread to have the render thread time its frames;
     * the render thread fills the rest before signalling 'done'. The GPU
     * time is that of the frame FRAME_GPU_QUERY_LAG frames ago, or zero.
     */
    bool       timing;
    uint64_t   t_cmds_us;
    uint64_t   t_present_us;
    uint64_t   t_swap_us;
    uint64_t   t_gpu_us;
};

#define MAX_ARGS 10

struct rcmd{
    void (*func)();
    size_t nargs;
    void *args[MAX_ARGS];
};

QUEUE_TYPE(rcmd, struct rcmd)
QUEUE_IMPL(static inline, rcmd, struct rcmd)

/* A run of commands taken out of the queue while it was being captured */
struct rcmd_list{
    size_t       ncmds;
    struct rcmd *cmds;
};

struct scene_chunk{
    void     *render_private;
    mat4x4_t  model;
    /* The layer holding the baked top faces, or -1 to draw the chunk whole */
    int       bake_layer;
    vec4_t    bake_xform;
    bool      near_water;
};

struct scene_terrain{
    vec2_t                 pos;
    size_t                 num_splats;
    const struct splatmap *splatmap;
    size_t                 nchunks;
    struct scene_chunk    *chunks;
};

struct scene_decals{
    const struct map_resolution *res;
    vec2_t                       map_pos;
    size_t                       nchunks;
    size_t                      *chunks;
};

/* A ring on the ground around a unit, or a rectangle around a building */
struct scene_marker{
    const struct obb *box;
    vec2_t            xz;
    float             radius;
    float             width;
    vec3_t            color;
};

struct scene_healthbars{
    bool    shown;
    size_t  nents;
    float  *health_pc;
    vec3_t *top_pos_ws;
    int    *yoffsets;
};

struct scene_minimap{
    bool    shown;
    vec2_t  center;
    int     side_len;
    vec4_t  border_clr;
    size_t  nunits;
    vec2_t *unit_pos;
    vec3_t *unit_clr;
};

/* One frame's worth of drawing. Everything pointed to lives in the render 
 * workspace the scene was pushed to, the map and the render data of the 
 * entities and chunks excepted.
 */
struct render_scene{
    bool                       shadows;
    bool                       batching;
    bool                       water_refraction;
    bool                       water_reflection;
    /* The directory of the look the world is graded to, or NULL for none.
     * The world is then drawn 'world_scale' times smaller along each side.
     */
    const char                *look;
    bool                       antialias;
    float                      world_scale;
    float                      dither;

    const struct camera       *cam;
    /* NULL when no map is loaded */
    const struct map          *map;
    struct render_input       *world;
    /* The entities by the water, or NULL when no water can be in view */
    struct render_input       *water;

    struct scene_terrain       terrain;
    struct scene_decals        decals;
    size_t                     nmarkers;
    struct scene_marker       *markers;
    struct scene_healthbars    healthbars;
    struct scene_minimap       minimap;

    /* What the listeners of the render events pushed: world geometry ahead
     * of and after the engine's own, then the screen-space drawing.
     */
    struct rcmd_list           pre_world;
    struct rcmd_list           post_world;
    struct rcmd_list           ui;
    struct rcmd_list           finish;

    const char                *dump_path;
};

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

/* Between the two calls, the commands the main thread pushes are set aside
 * instead of queued. The list is theirs to run, in the pushed order.
 */
void        R_BeginCapture(void);
struct rcmd_list R_EndCapture(void);

/* The next frame is written to the PPM file once it is complete */
void        R_RequestFramebufferDump(const char *path);
/* The pending request, copied into the render workspace, or NULL */
const char *R_TakeFramebufferDump(void);

bool        R_InitWS(struct render_workspace *ws);
void        R_DestroyWS(struct render_workspace *ws);
void        R_ClearWS(struct render_workspace *ws);

const char *R_GetInfo(enum render_info attr);

void        R_LightFrustum(vec3_t light_pos, vec3_t cam_pos, vec3_t cam_dir, struct frustum *out);
void        R_LightVisibilityFrustum(const struct camera *cam, vec3_t light_pos, struct frustum *out);

/* Tile */
int         R_TileGetTriMesh(const struct map *map, struct tile_desc *td, mat4x4_t *model, vec3_t out[]);
/* The top face triangles with the (smoothed) normals the terrain is lit with */
int         R_TileGetTopFace(const struct map *map, struct tile_desc *td, mat4x4_t *model, 
                             vec3_t out_pos[], vec3_t out_normals[]);

/* UI */
int         R_UI_GetFontTexID(void);


#endif

