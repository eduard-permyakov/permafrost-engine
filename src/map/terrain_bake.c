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


#define MEM_FILE_SYS MEM_SYS_MAP
#define MEM_FILE_SUB MEM_SUB_MAP_TERRAIN_BAKE

#include "public/map.h"
#include "map_private.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../game/public/game.h"
#include "../settings.h"
#include "../camera.h"
#include "../config.h"
#include "../main.h"

#include <assert.h>
#include <string.h>
#include <math.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_MAP, MEM_SUB_MAP_TERRAIN_BAKE)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_MAP, MEM_SUB_MAP_TERRAIN_BAKE)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_MAP, MEM_SUB_MAP_TERRAIN_BAKE)

#define CHUNK_WIDTH         (TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE)
#define CHUNK_HEIGHT        (TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE)
#define BAKE_HALF_EXTENT    (CHUNK_WIDTH / 2.0f + CONFIG_TERRAIN_BAKE_GUTTER)
#define BAKE_EYE_HEIGHT     (256.0f)
#define SLICE_GRID          (CONFIG_TERRAIN_BAKE_SLICE_GRID)
#define BAKE_SLICES         (SLICE_GRID * SLICE_GRID)
/* Slices baked per frame while this many visible chunks await a bake: one when
 * the view is covered, a whole chunk after a load or a camera jump
 */
#define BURST_PENDING       (2)
#define BURST_SLICES        (4)
#define JUMP_PENDING        (8)
/* An edited chunk keeps showing its previous bake until the new one is done,
 * so this is the delay before an edit (an editor brush stroke) appears
 */
#define EDIT_SLICES         (8)
/* Frames the view direction must hold before chunks are baked for it */
#define DIR_SETTLE_FRAMES   (10)
#define DIR_REBAKE_DEG      (1.0f)

struct bake_chunk{
    /* The layer holding the chunk's latest complete bake */
    int  layer;
    bool stale;
    /* The lighting, splats or view changed: the layer must not be drawn. A chunk
     * that is only stale because its tiles changed keeps drawing it.
     */
    bool outdated;
    /* Outdated this frame: baking waits for a frame without such changes */
    bool dirtied;
};

struct bake_layer{
    int      chunk;
    bool     ready;
    uint64_t last_used;
};

struct bake_inflight{
    int  chunk;
    /* Either the chunk's own layer, or a spare one while its own stays drawn */
    int  layer;
    int  next_slice;
    /* The chunk's tiles changed after the bake started */
    bool restale;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map    *s_map;
static struct bake_chunk   *s_chunks;
static struct bake_layer    s_layers[CONFIG_TERRAIN_BAKE_LAYERS];
static struct bake_inflight s_inflight = {-1, -1, 0, false};
static uint64_t             s_frame;
/* The view direction the resident layers are baked for */
static vec3_t               s_view_dir;
static uint64_t             s_dir_frame;
static vec3_t               s_light_pos;
static vec3_t               s_ambient;
static vec3_t               s_emit;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static size_t num_chunks(void)
{
    return s_map->width * s_map->height;
}

static void reset_residency(void)
{
    for(int i = 0; i < num_chunks(); i++) {
        s_chunks[i] = (struct bake_chunk){-1, true, false, false};
    }
    for(int i = 0; i < CONFIG_TERRAIN_BAKE_LAYERS; i++) {
        s_layers[i] = (struct bake_layer){-1, false, 0};
    }
    s_inflight = (struct bake_inflight){-1, -1, 0, false};
}

static bool chunk_drawable(const struct bake_chunk *chunk)
{
    return (chunk->layer >= 0 && s_layers[chunk->layer].ready && !chunk->outdated);
}

static void abort_inflight(void)
{
    if(s_inflight.chunk < 0)
        return;

    if(s_inflight.layer != s_chunks[s_inflight.chunk].layer) {
        s_layers[s_inflight.layer] = (struct bake_layer){-1, false, 0};
    }
    s_inflight.chunk = -1;
}

static void mark_all_outdated(void)
{
    if(!s_chunks)
        return;
    for(int i = 0; i < num_chunks(); i++) {
        s_chunks[i].stale = true;
        s_chunks[i].outdated = true;
        s_chunks[i].dirtied = true;
    }
    abort_inflight();
}

static bool bake_enabled(void)
{
    struct sval setting;
    ss_e status = Settings_Get("pf.video.terrain_bake", &setting);
    assert(status == SS_OKAY);
    return setting.as_bool;
}

static void push_configure(int res, bool compress)
{
    R_PushCmd((struct rcmd){
        .func = R_GL_TerrainBakeConfigure,
        .nargs = 2,
        .args = {
            R_PushArg(&res, sizeof(res)),
            R_PushArg(&compress, sizeof(compress)),
        },
    });
}

static vec2_t chunk_centre(int r, int c)
{
    return (vec2_t){
        s_map->pos.x - c * CHUNK_WIDTH - CHUNK_WIDTH / 2.0f,
        s_map->pos.z + r * CHUNK_HEIGHT + CHUNK_HEIGHT / 2.0f
    };
}

/* The ground point the camera looks at: chunks nearest it are baked first */
static vec2_t camera_focus(const struct camera *cam)
{
    vec3_t pos = Camera_GetPos(cam);
    vec3_t dir = Camera_GetDir(cam);
    float t = (dir.y < -1e-3f) ? -pos.y / dir.y : 0.0f;
    return (vec2_t){pos.x + dir.x * t, pos.z + dir.z * t};
}

/* The chunks next to the visible ones, baked ahead of the camera reaching them */
static void near_chunks(const bool *vis, bool *out)
{
    int w = s_map->width, h = s_map->height;
    for(int r = 0; r < h; r++) {
    for(int c = 0; c < w; c++) {

        bool near = false;
        for(int dr = -1; dr <= 1 && !near; dr++) {
        for(int dc = -1; dc <= 1 && !near; dc++) {
            int nr = r + dr, nc = c + dc;
            if(nr < 0 || nr >= h || nc < 0 || nc >= w)
                continue;
            near = vis[nr * w + nc];
        }}
        out[r * w + c] = near && !vis[r * w + c];
    }}
}

static int pick_candidate(const bool *set, vec2_t focus)
{
    int best = -1;
    float best_dist = INFINITY;

    for(int i = 0; i < num_chunks(); i++) {

        const struct bake_chunk *chunk = &s_chunks[i];
        if(!set[i] || chunk->dirtied)
            continue;
        if(chunk->layer >= 0 && !chunk->stale)
            continue;

        vec2_t centre = chunk_centre(i / s_map->width, i % s_map->width);
        float dx = centre.x - focus.x, dz = centre.z - focus.z;
        float dist = dx * dx + dz * dz;
        if(dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

/* A free layer, else the least recently used one whose chunk is not 'kept' */
static int acquire_layer(const bool *kept)
{
    int lru = -1;
    for(int i = 0; i < CONFIG_TERRAIN_BAKE_LAYERS; i++) {

        const struct bake_layer *layer = &s_layers[i];
        if(layer->chunk < 0)
            return i;
        if(kept[layer->chunk])
            continue;
        if(lru < 0 || layer->last_used < s_layers[lru].last_used)
            lru = i;
    }
    if(lru >= 0) {
        s_chunks[s_layers[lru].chunk].layer = -1;
    }
    return lru;
}

/* Whether the chunk covers any of the slice's texels, in the bake's texture space */
static bool chunk_in_slice(const struct terrain_bake_frame *frame, int r, int c, int slice)
{
    float x_max = s_map->pos.x - c * CHUNK_WIDTH, x_min = x_max - CHUNK_WIDTH;
    float z_min = s_map->pos.z + r * CHUNK_HEIGHT, z_max = z_min + CHUNK_HEIGHT;

    float u0 = x_min * frame->xform.x + frame->xform.z;
    float u1 = x_max * frame->xform.x + frame->xform.z;
    float v0 = z_min * frame->xform.y + frame->xform.w;
    float v1 = z_max * frame->xform.y + frame->xform.w;

    float cell = 1.0f / SLICE_GRID;
    float su = (slice % SLICE_GRID) * cell, sv = (slice / SLICE_GRID) * cell;

    return (fminf(u0, u1) < su + cell && fmaxf(u0, u1) > su)
        && (fminf(v0, v1) < sv + cell && fmaxf(v0, v1) > sv);
}

static void push_slice(int chunk, int slice)
{
    int r = chunk / s_map->width, c = chunk % s_map->width;
    struct terrain_bake_slice args;

    M_TerrainBake_ChunkFrame(s_map, r, c, &args.frame);
    args.slice = slice;
    args.view_dir = s_view_dir;
    args.map_pos = (vec2_t){s_map->pos.x, s_map->pos.z};
    args.num_splats = s_map->num_splats;
    args.splatmap = s_map->splatmap;
    args.num_chunks = 0;

    /* The neighbours fill the gutter around the chunk */
    for(int dr = -1; dr <= 1; dr++) {
    for(int dc = -1; dc <= 1; dc++) {

        int nr = r + dr, nc = c + dc;
        if(nr < 0 || nr >= s_map->height || nc < 0 || nc >= s_map->width)
            continue;
        if(!chunk_in_slice(&args.frame, nr, nc, slice))
            continue;

        size_t n = args.num_chunks++;
        args.chunk_rprivates[n] = s_map->chunks[nr * s_map->width + nc].render_private;
        M_ModelMatrixForChunk(s_map, (struct chunkpos){nr, nc}, &args.chunk_models[n]);
    }}

    R_PushCmd((struct rcmd){
        .func = R_GL_TerrainBakeSlice,
        .nargs = 1,
        .args = { R_PushArg(&args, sizeof(args)) },
    });
}

static void push_finish(int layer)
{
    R_PushCmd((struct rcmd){
        .func = R_GL_TerrainBakeFinish,
        .nargs = 1,
        .args = { R_PushArg(&layer, sizeof(layer)) },
    });
}

static void update_view_dir(const struct camera *cam)
{
    vec3_t dir = Camera_GetDir(cam);
    if(PFM_Vec3_Dot(&dir, &s_view_dir) >= cosf(DEG_TO_RAD(DIR_REBAKE_DEG)))
        return;

    s_view_dir = dir;
    s_dir_frame = s_frame;
    for(int i = 0; i < num_chunks(); i++) {
        s_chunks[i].stale = true;
        s_chunks[i].outdated = true;
    }
    abort_inflight();
}

/* Visible chunks are baked first, evicting any layer not showing a visible chunk;
 * the chunks next to the view follow, only into layers not needed by either set.
 */
static bool start_bake(const bool *vis, const bool *near, vec2_t focus)
{
    const bool *kept = vis;
    int next = pick_candidate(vis, focus);
    if(next < 0) {
        next = pick_candidate(near, focus);
        kept = NULL;
    }
    if(next < 0)
        return false;

    /* A chunk still showing its previous bake is baked into a spare layer, so the
     * picture only changes once, when the new bake replaces it
     */
    struct bake_chunk *chunk = &s_chunks[next];
    bool keep_own = chunk_drawable(chunk);
    int layer = keep_own ? -1 : chunk->layer;

    if(layer < 0) {

        STALLOC(bool, both, num_chunks());
        for(int i = 0; i < num_chunks(); i++) {
            both[i] = (kept ? kept[i] : (vis[i] || near[i])) || (i == next);
        }
        layer = acquire_layer(both);
        STFREE(both);

        if(layer < 0 && keep_own) {
            layer = chunk->layer;
        }
        if(layer < 0)
            return false;

        s_layers[layer].chunk = next;
        if(chunk->layer < 0) {
            chunk->layer = layer;
        }
    }
    s_layers[layer].ready = false;
    s_inflight = (struct bake_inflight){next, layer, 0, false};
    return true;
}

/* Start, continue and complete bakes, at most 'slices' of a chunk this frame */
static void advance_bakes(const bool *vis, const bool *near, vec2_t focus, int slices)
{
    while(slices > 0) {

        if(s_inflight.chunk < 0 && !start_bake(vis, near, focus))
            return;

        push_slice(s_inflight.chunk, s_inflight.next_slice++);
        slices--;

        if(s_inflight.next_slice == BAKE_SLICES) {

            struct bake_chunk *chunk = &s_chunks[s_inflight.chunk];
            push_finish(s_inflight.layer);

            if(chunk->layer != s_inflight.layer) {
                if(chunk->layer >= 0) {
                    s_layers[chunk->layer] = (struct bake_layer){-1, false, 0};
                }
                chunk->layer = s_inflight.layer;
            }
            s_layers[s_inflight.layer].ready = true;
            s_layers[s_inflight.layer].last_used = s_frame;
            chunk->stale = s_inflight.restale;
            chunk->outdated = false;
            s_inflight.chunk = -1;
        }
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool M_TerrainBake_Init(const struct map *map)
{
    ASSERT_IN_MAIN_THREAD();
    assert(!s_chunks);

    s_map = map;
    s_chunks = PF_MALLOC(num_chunks() * sizeof(struct bake_chunk));
    if(!s_chunks) {
        s_map = NULL;
        return false;
    }
    reset_residency();
    s_frame = 0;
    s_dir_frame = 0;
    s_view_dir = (vec3_t){0.0f, 0.0f, 0.0f};

    struct sval res, compress;
    ss_e status;
    (void)status;
    status = Settings_Get("pf.video.terrain_bake_res", &res);
    assert(status == SS_OKAY);
    status = Settings_Get("pf.video.terrain_bake_compress", &compress);
    assert(status == SS_OKAY);
    push_configure(res.as_int, compress.as_bool);
    return true;
}

void M_TerrainBake_Shutdown(void)
{
    ASSERT_IN_MAIN_THREAD();
    PF_FREE(s_chunks);
    s_chunks = NULL;
    s_map = NULL;
}

void M_TerrainBake_Reconfigure(int res, bool compress)
{
    ASSERT_IN_MAIN_THREAD();
    push_configure(res, compress);
    if(s_chunks) {
        reset_residency();
    }
}

void M_TerrainBake_ChunkFrame(const struct map *map, int r, int c, struct terrain_bake_frame *out)
{
    float cx = map->pos.x - c * CHUNK_WIDTH - CHUNK_WIDTH / 2.0f;
    float cz = map->pos.z + r * CHUNK_HEIGHT + CHUNK_HEIGHT / 2.0f;
    float half = BAKE_HALF_EXTENT;

    vec3_t eye = (vec3_t){cx, BAKE_EYE_HEIGHT, cz};
    vec3_t target = (vec3_t){cx, 0.0f, cz};
    vec3_t up = (vec3_t){0.0f, 0.0f, -1.0f};
    PFM_Mat4x4_MakeLookAt(&eye, &target, &up, &out->view);
    PFM_Mat4x4_MakeOrthographic(-half, half, -half, half, 1.0f, 2.0f * BAKE_EYE_HEIGHT, &out->proj);

    /* Derive the world XZ to texture coordinate affine from the projection itself,
     * so the two can never disagree about the view's orientation.
     */
    mat4x4_t vp;
    PFM_Mat4x4_Mult4x4(&out->proj, &out->view, &vp);

    vec4_t origin, step_x, step_z;
    PFM_Mat4x4_Mult4x1(&vp, &(vec4_t){cx, 0.0f, cz, 1.0f}, &origin);
    PFM_Mat4x4_Mult4x1(&vp, &(vec4_t){cx + 1.0f, 0.0f, cz, 1.0f}, &step_x);
    PFM_Mat4x4_Mult4x1(&vp, &(vec4_t){cx, 0.0f, cz + 1.0f, 1.0f}, &step_z);
    assert(fabs(step_x.y - origin.y) < 1e-6f && fabs(step_z.x - origin.x) < 1e-6f);

    float scale_x = (step_x.x - origin.x) * 0.5f;
    float scale_z = (step_z.y - origin.y) * 0.5f;
    out->xform = (vec4_t){
        scale_x, scale_z,
        origin.x * 0.5f + 0.5f - scale_x * cx,
        origin.y * 0.5f + 0.5f - scale_z * cz
    };
}

void M_TerrainBake_MarkChunkStale(int r, int c)
{
    ASSERT_IN_MAIN_THREAD();
    if(!s_chunks)
        return;
    int idx = r * s_map->width + c;
    s_chunks[idx].stale = true;
    if(s_inflight.chunk == idx) {
        s_inflight.restale = true;
    }
}

void M_TerrainBake_MarkAllStale(void)
{
    ASSERT_IN_MAIN_THREAD();
    mark_all_outdated();
}

void M_TerrainBake_OnLightChanged(vec3_t light_pos, vec3_t ambient, vec3_t emit)
{
    ASSERT_IN_MAIN_THREAD();
    if(!memcmp(&light_pos, &s_light_pos, sizeof(vec3_t))
    && !memcmp(&ambient, &s_ambient, sizeof(vec3_t))
    && !memcmp(&emit, &s_emit, sizeof(vec3_t)))
        return;

    s_light_pos = light_pos;
    s_ambient = ambient;
    s_emit = emit;
    mark_all_outdated();
}

const int *M_TerrainBake_Tick(const struct camera *cam)
{
    ASSERT_IN_MAIN_THREAD();
    if(!s_chunks)
        return NULL;

    size_t nchunks = num_chunks();
    if(!bake_enabled()) {
        abort_inflight();
        for(int i = 0; i < nchunks; i++) {
            s_chunks[i].dirtied = false;
        }
        return NULL;
    }
    s_frame++;

    STALLOC(bool, vis, nchunks);
    STALLOC(bool, near, nchunks);
    M_VisibleChunks(s_map, cam, vis);
    near_chunks(vis, near);
    update_view_dir(cam);

    size_t pending = 0;
    bool edited = false;
    for(int i = 0; i < nchunks; i++) {
        if(!vis[i])
            continue;
        if(s_chunks[i].layer >= 0) {
            s_layers[s_chunks[i].layer].last_used = s_frame;
        }
        if(s_chunks[i].stale && chunk_drawable(&s_chunks[i])) {
            edited = true;
        }else if(s_chunks[i].layer < 0 || s_chunks[i].stale) {
            pending++;
        }
    }

    if(s_frame - s_dir_frame >= DIR_SETTLE_FRAMES) {
        int slices = (pending > JUMP_PENDING) ? BAKE_SLICES
                   : (pending > BURST_PENDING) ? BURST_SLICES : 1;
        if(edited && slices < EDIT_SLICES) {
            slices = EDIT_SLICES;
        }
        advance_bakes(vis, near, camera_focus(cam), slices);
    }

    int *out = R_AllocArg(nchunks * sizeof(int));
    for(int i = 0; i < nchunks; i++) {

        struct bake_chunk *chunk = &s_chunks[i];
        if(out) {
            out[i] = chunk_drawable(chunk) ? chunk->layer : -1;
        }
        chunk->dirtied = false;
    }

    STFREE(near);
    STFREE(vis);
    return out;
}
