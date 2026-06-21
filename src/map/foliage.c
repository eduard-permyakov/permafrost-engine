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
#define MEM_FILE_SUB MEM_SUB_MAP_FOLIAGE

#include "public/map.h"
#include "public/tile.h"
#include "map_private.h"
#include "pfchunk.h"
#include "../game/public/game.h"
#include "../settings.h"
#include "../event.h"
#include "../camera.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../asset_load.h"
#include "../lib/public/noise.h"
#include "../mem.h"
#include "../phys/public/collision.h"
#include "../pf_math.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_MAP, MEM_SUB_MAP_FOLIAGE)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_MAP, MEM_SUB_MAP_FOLIAGE)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_MAP, MEM_SUB_MAP_FOLIAGE)


#define GRASS_MODEL_DIR          "assets/models/foliage"
#define GRASS_MODEL_OBJ          "grass-01-lowpoly.pfobj"

/* Uniform scale applied to every grass mesh instance */
#define COVER_SCALE              3.5f

/* Maximum instances per tile when noise value is 1.0 */
#define MAX_INST_FULL            6
#define MAX_INST_SPARSE          3

#define NOISE_FREQUENCY          0.05f
#define NOISE_OCTAVES            4
#define NOISE_PERSISTENCE        0.5f

struct chunk_foliage {
    vec3_t *positions;
    float  *rotations;
    size_t  count;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map    *s_map         = NULL;
static struct chunk_foliage *s_chunks     = NULL;
static bool                *s_dirty       = NULL;
static size_t               s_num_chunks  = 0;
static float               *s_noise       = NULL;
static size_t               s_noise_w     = 0;
static size_t               s_noise_h     = 0;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static enum tile_cover tile_cover_at(const struct map *map,
                                     struct map_resolution res,
                                     int gr, int gc)
{
    int total_h = (int)(res.chunk_h * res.tile_h);
    int total_w = (int)(res.chunk_w * res.tile_w);
    if(gr < 0 || gr >= total_h || gc < 0 || gc >= total_w)
        return TILE_COVER_NONE;
    int cr = gr / (int)res.tile_h;
    int tr = gr % (int)res.tile_h;
    int cc = gc / (int)res.tile_w;
    int tc = gc % (int)res.tile_w;
    const struct pfchunk *chunk = &map->chunks[cr * res.chunk_w + cc];
    return chunk->tiles[tr * res.tile_w + tc].cover;
}

static bool any_neighbor_uncovered(const struct map *map,
                                   struct map_resolution res,
                                   int gr, int gc)
{
    for(int dr = -1; dr <= 1; dr++) {
    for(int dc = -1; dc <= 1; dc++) {
        if(dr == 0 && dc == 0)
            continue;
        if(tile_cover_at(map, res, gr + dr, gc + dc) == TILE_COVER_NONE)
            return true;
    }}
    return false;
}

static uint32_t lcg_rand(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

/* Returns a float in [0, 1) */
static float lcg_frand(uint32_t *state)
{
    return (float)(lcg_rand(state) & 0xffffffu) / (float)0x1000000u;
}

static void generate_chunk_positions(const struct map *map, int cr, int cc,
                                     struct chunk_foliage *out)
{
    struct map_resolution res;
    M_GetResolution(map, &res);

    const struct pfchunk *chunk = &map->chunks[cr * res.chunk_w + cc];

    /* Count pass */
    size_t total = 0;
    for(int tr = 0; tr < res.tile_h; tr++) {
    for(int tc = 0; tc < res.tile_w; tc++) {
        const struct tile *tile = &chunk->tiles[tr * res.tile_w + tc];
        if(tile->cover == TILE_COVER_NONE)
            continue;
        int gr = cr * res.tile_h + tr;
        int gc = cc * res.tile_w + tc;
        if(any_neighbor_uncovered(map, res, gr, gc))
            continue;
        float nv = s_noise[(size_t)gr * s_noise_w + (size_t)gc];
        int max_n = (tile->cover == TILE_COVER_GRASS_FULL) ? MAX_INST_FULL : MAX_INST_SPARSE;
        total += (size_t)(nv * max_n + 0.5f);
    }}

    out->count     = 0;
    out->positions = NULL;
    out->rotations = NULL;

    if(total == 0)
        return;

    vec3_t *positions = PF_MALLOC(total * sizeof(vec3_t));
    float  *rotations = PF_MALLOC(total * sizeof(float));
    if(!positions || !rotations) {
        PF_FREE(positions);
        PF_FREE(rotations);
        return;
    }

    /* Generate pass */
    size_t idx = 0;
    for(int tr = 0; tr < res.tile_h; tr++) {
    for(int tc = 0; tc < res.tile_w; tc++) {
        const struct tile *tile = &chunk->tiles[tr * res.tile_w + tc];
        if(tile->cover == TILE_COVER_NONE)
            continue;
        int gr = cr * res.tile_h + tr;
        int gc = cc * res.tile_w + tc;
        if(any_neighbor_uncovered(map, res, gr, gc))
            continue;
        float nv = s_noise[(size_t)gr * s_noise_w + (size_t)gc];
        int max_n = (tile->cover == TILE_COVER_GRASS_FULL) ? MAX_INST_FULL : MAX_INST_SPARSE;
        int n = (int)(nv * max_n + 0.5f);
        if(n == 0)
            continue;

        struct tile_desc td = {cr, cc, tr, tc};
        struct box bounds = M_Tile_Bounds(res, map->pos, td);
        uint32_t seed = (uint32_t)((size_t)gr * s_noise_w + (size_t)gc);

        for(int i = 0; i < n; i++) {
            float rx = bounds.x - bounds.width  * lcg_frand(&seed);
            float rz = bounds.z + bounds.height * lcg_frand(&seed);
            float y  = M_HeightAtPoint(map, (vec2_t){rx, rz});
            positions[idx] = (vec3_t){rx, y, rz};
            rotations[idx] = lcg_frand(&seed) * 2.0f * (float)M_PI;
            idx++;
        }
    }}
    assert(idx == total);

    out->positions = positions;
    out->rotations = rotations;
    out->count     = total;
}

static void push_chunk_to_render(size_t chunk_idx)
{
    const struct chunk_foliage *chunk = &s_chunks[chunk_idx];
    size_t count = chunk->count;
    R_PushCmd((struct rcmd){
        .func  = R_GL_MapFoliageSetChunk,
        .nargs = 4,
        .args  = {
            R_PushArg(&chunk_idx, sizeof(chunk_idx)),
            count > 0 ? R_PushArg(chunk->positions, count * sizeof(vec3_t)) : NULL,
            count > 0 ? R_PushArg(chunk->rotations, count * sizeof(float))  : NULL,
            R_PushArg(&count, sizeof(count)),
        },
    });
}

static void flush_dirty_chunks(void)
{
    struct map_resolution res;
    M_GetResolution(s_map, &res);

    for(int cr = 0; cr < res.chunk_h; cr++) {
    for(int cc = 0; cc < res.chunk_w; cc++) {
        size_t chunk_idx = (size_t)cr * res.chunk_w + cc;
        if(!s_dirty[chunk_idx])
            continue;

        PF_FREE(s_chunks[chunk_idx].positions);
        PF_FREE(s_chunks[chunk_idx].rotations);
        s_chunks[chunk_idx] = (struct chunk_foliage){0};

        generate_chunk_positions(s_map, cr, cc, &s_chunks[chunk_idx]);
        push_chunk_to_render(chunk_idx);
        s_dirty[chunk_idx] = false;
    }}
}

static void aabb_for_chunk(int cr, int cc, struct aabb *out)
{
    size_t chunk_x_dim = TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE;
    size_t chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    out->x_max = s_map->pos.x - (float)(cc * (int)chunk_x_dim);
    out->x_min = out->x_max   - (float)chunk_x_dim;
    out->z_min = s_map->pos.z + (float)(cr * (int)chunk_z_dim);
    out->z_max = out->z_min   + (float)chunk_z_dim;
    out->y_min = 0.0f;
    out->y_max = (float)(MAX_HEIGHT_LEVEL * Y_COORDS_PER_TILE);
}

static void on_render_3d(void *user, void *event)
{
    const struct camera *cam = G_GetActiveCamera();

    struct sval setting;
    ss_e status;
    (void)status;

    status = Settings_Get("pf.game.show_map_foliage", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {
        M_NavRenderMapCover(s_map, cam);
    }

    flush_dirty_chunks();

    static const float s_scale = COVER_SCALE;
    struct map_resolution res;
    M_GetResolution(s_map, &res);

    vec3_t map_world_pos = M_GetPos(s_map);
    vec2_t map_pos = (vec2_t){map_world_pos.x, map_world_pos.z};

    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    bool any_visible = false;
    for(int cr = 0; cr < res.chunk_h; cr++) {
    for(int cc = 0; cc < res.chunk_w; cc++) {
        size_t chunk_idx = (size_t)cr * res.chunk_w + cc;
        if(s_chunks[chunk_idx].count == 0)
            continue;

        struct aabb chunk_aabb;
        aabb_for_chunk(cr, cc, &chunk_aabb);
        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        if(!any_visible) {
            R_PushCmd((struct rcmd){
                .func  = R_GL_MapFoliageBeginDraw,
                .nargs = 4,
                .args  = {
                    R_PushArg(cam, g_sizeof_camera),
                    R_PushArg(&s_scale, sizeof(s_scale)),
                    R_PushArg(&res, sizeof(res)),
                    R_PushArg(&map_pos, sizeof(map_pos)),
                },
            });
            any_visible = true;
        }

        R_PushCmd((struct rcmd){
            .func  = R_GL_MapFoliageDrawChunk,
            .nargs = 1,
            .args  = { R_PushArg(&chunk_idx, sizeof(chunk_idx)) },
        });
    }}

    if(any_visible) {
        R_PushCmd((struct rcmd){ .func = R_GL_MapFoliageEndDraw, .nargs = 0 });
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool M_FoliageInit(const struct map *map)
{
    s_map = map;

    struct map_resolution res;
    M_GetResolution(map, &res);

    s_noise_w = (size_t)(res.chunk_w * res.tile_w);
    s_noise_h = (size_t)(res.chunk_h * res.tile_h);

    s_noise = PF_MALLOC(s_noise_w * s_noise_h * sizeof(float));
    if(!s_noise)
        return false;

    Noise_GenerateOctavePerlin2D(s_noise_w, s_noise_h,
        NOISE_FREQUENCY, NOISE_OCTAVES, NOISE_PERSISTENCE, s_noise);
    Noise_Normalize2D(s_noise_w, s_noise_h, s_noise);

    s_num_chunks = (size_t)(res.chunk_h * res.chunk_w);
    s_chunks = PF_CALLOC(s_num_chunks, sizeof(struct chunk_foliage));
    s_dirty  = PF_CALLOC(s_num_chunks, sizeof(bool));
    if(!s_chunks || !s_dirty)
        goto fail_alloc;

    AL_PreloadPFObj(GRASS_MODEL_DIR, GRASS_MODEL_OBJ);
    void *priv = AL_RenderPrivateForName(GRASS_MODEL_DIR, GRASS_MODEL_OBJ);
    if(!priv)
        goto fail_alloc;

    R_PushCmd((struct rcmd){
        .func  = R_GL_MapFoliageInit,
        .nargs = 2,
        .args  = {
            priv,
            R_PushArg(&s_num_chunks, sizeof(s_num_chunks)),
        },
    });

    for(int cr = 0; cr < res.chunk_h; cr++) {
    for(int cc = 0; cc < res.chunk_w; cc++) {
        size_t chunk_idx = (size_t)cr * res.chunk_w + cc;
        generate_chunk_positions(map, cr, cc, &s_chunks[chunk_idx]);
        push_chunk_to_render(chunk_idx);
    }}

    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL,
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    return true;

fail_alloc:
    PF_FREE(s_chunks);
    PF_FREE(s_dirty);
    PF_FREE(s_noise);
    s_chunks     = NULL;
    s_dirty      = NULL;
    s_noise      = NULL;
    s_num_chunks = 0;
    return false;
}

void M_FoliageShutdown(void)
{
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    R_PushCmd((struct rcmd){ .func = R_GL_MapFoliageShutdown, .nargs = 0 });

    for(size_t i = 0; i < s_num_chunks; i++) {
        PF_FREE(s_chunks[i].positions);
        PF_FREE(s_chunks[i].rotations);
    }
    PF_FREE(s_chunks);
    PF_FREE(s_dirty);
    PF_FREE(s_noise);

    s_map        = NULL;
    s_chunks     = NULL;
    s_dirty      = NULL;
    s_noise      = NULL;
    s_num_chunks = 0;
    s_noise_w    = 0;
    s_noise_h    = 0;
}

static void mark_neighbour_chunks_dirty(struct map_resolution res, int gr, int gc)
{
    int total_h = (int)(res.chunk_h * res.tile_h);
    int total_w = (int)(res.chunk_w * res.tile_w);

    for(int dr = -1; dr <= 1; dr++) {
    for(int dc = -1; dc <= 1; dc++) {
        int ngr = gr + dr;
        int ngc = gc + dc;
        if(ngr < 0 || ngr >= total_h || ngc < 0 || ngc >= total_w)
            continue;
        int cr = ngr / (int)res.tile_h;
        int cc = ngc / (int)res.tile_w;
        s_dirty[(size_t)cr * res.chunk_w + (size_t)cc] = true;
    }}
}

void M_FoliageUpdateTile(struct tile_desc td)
{
    struct map_resolution res;
    M_GetResolution(s_map, &res);

    int gr = td.chunk_r * (int)res.tile_h + td.tile_r;
    int gc = td.chunk_c * (int)res.tile_w + td.tile_c;
    mark_neighbour_chunks_dirty(res, gr, gc);
}

