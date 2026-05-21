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
#include "../pf_math.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>


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

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map *s_map;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

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

static void generate_positions(const struct map *map,
                                vec3_t **out_pos, float **out_rot,
                                size_t *out_count)
{
    struct map_resolution res;
    M_GetResolution(map, &res);

    size_t noise_w = (size_t)(res.chunk_w * res.tile_w);
    size_t noise_h = (size_t)(res.chunk_h * res.tile_h);

    float *noise = malloc(noise_w * noise_h * sizeof(float));
    if(!noise)
        goto fail_noise;

    Noise_GenerateOctavePerlin2D(noise_w, noise_h,
        NOISE_FREQUENCY, NOISE_OCTAVES, NOISE_PERSISTENCE, noise);
    Noise_Normalize2D(noise_w, noise_h, noise);

    /* First pass: count total instances so we can allocate exactly once */
    size_t total = 0;
    for(int cr = 0; cr < res.chunk_h; cr++) {
    for(int cc = 0; cc < res.chunk_w; cc++) {
        const struct pfchunk *chunk = &map->chunks[cr * res.chunk_w + cc];
        for(int tr = 0; tr < res.tile_h; tr++) {
        for(int tc = 0; tc < res.tile_w; tc++) {
            const struct tile *tile = &chunk->tiles[tr * res.tile_w + tc];
            if(tile->cover == TILE_COVER_NONE)
                continue;

            int gr = cr * res.tile_h + tr;
            int gc = cc * res.tile_w + tc;
            float nv = noise[(size_t)gr * noise_w + (size_t)gc];

            int max_n = (tile->cover == TILE_COVER_GRASS_FULL)
                ? MAX_INST_FULL : MAX_INST_SPARSE;
            total += (size_t)(nv * max_n + 0.5f);
        }}
    }}

    if(total == 0) {
        free(noise);
        *out_pos   = NULL;
        *out_rot   = NULL;
        *out_count = 0;
        return;
    }

    vec3_t *positions = malloc(total * sizeof(vec3_t));
    float  *rotations = malloc(total * sizeof(float));
    if(!positions || !rotations)
        goto fail_arrays;

    /* Second pass: generate world-space positions and rotations */
    size_t idx = 0;
    for(int cr = 0; cr < res.chunk_h; cr++) {
    for(int cc = 0; cc < res.chunk_w; cc++) {
        const struct pfchunk *chunk = &map->chunks[cr * res.chunk_w + cc];
        for(int tr = 0; tr < res.tile_h; tr++) {
        for(int tc = 0; tc < res.tile_w; tc++) {
            const struct tile *tile = &chunk->tiles[tr * res.tile_w + tc];
            if(tile->cover == TILE_COVER_NONE)
                continue;

            int gr = cr * res.tile_h + tr;
            int gc = cc * res.tile_w + tc;
            float nv = noise[(size_t)gr * noise_w + (size_t)gc];

            int max_n = (tile->cover == TILE_COVER_GRASS_FULL)
                ? MAX_INST_FULL : MAX_INST_SPARSE;
            int n = (int)(nv * max_n + 0.5f);
            if(n == 0)
                continue;

            struct tile_desc td = {cr, cc, tr, tc};
            struct box bounds = M_Tile_Bounds(res, map->pos, td);

            /* Seed per-tile RNG from the tile's global index */
            uint32_t seed = (uint32_t)((size_t)gr * noise_w + (size_t)gc);

            for(int i = 0; i < n; i++) {
                /* Random XZ within tile bounds.
                 * Tile X spans [bounds.x - bounds.width, bounds.x],
                 * tile Z spans [bounds.z, bounds.z + bounds.height]. */
                float rx = bounds.x - bounds.width  * lcg_frand(&seed);
                float rz = bounds.z + bounds.height * lcg_frand(&seed);
                float y  = M_HeightAtPoint(map, (vec2_t){rx, rz});

                positions[idx] = (vec3_t){rx, y, rz};
                rotations[idx] = lcg_frand(&seed) * 2.0f * (float)M_PI;
                idx++;
            }
        }}
    }}
    assert(idx == total);

    free(noise);
    *out_pos   = positions;
    *out_rot   = rotations;
    *out_count = total;
    return;

fail_arrays:
    free(positions);
    free(rotations);
    free(noise);
fail_noise:
    *out_pos   = NULL;
    *out_rot   = NULL;
    *out_count = 0;
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

    static const float s_scale = COVER_SCALE;
    R_PushCmd((struct rcmd){
        .func  = R_GL_MapFoliageDraw,
        .nargs = 2,
        .args  = {
            R_PushArg(cam, g_sizeof_camera),
            R_PushArg(&s_scale, sizeof(s_scale)),
        },
    });
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool M_FoliageInit(const struct map *map)
{
    s_map = map;

    AL_PreloadPFObj(GRASS_MODEL_DIR, GRASS_MODEL_OBJ);
    void *priv = AL_RenderPrivateForName(GRASS_MODEL_DIR, GRASS_MODEL_OBJ);
    if(!priv)
        return false;

    vec3_t *positions = NULL;
    float  *rotations = NULL;
    size_t  count     = 0;
    generate_positions(map, &positions, &rotations, &count);

    R_PushCmd((struct rcmd){
        .func  = R_GL_MapFoliageInit,
        .nargs = 4,
        .args  = {
            priv,
            count > 0 ? R_PushArg(positions, count * sizeof(vec3_t)) : NULL,
            count > 0 ? R_PushArg(rotations, count * sizeof(float))  : NULL,
            R_PushArg(&count, sizeof(count)),
        },
    });

    free(positions);
    free(rotations);

    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL,
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    return true;
}

void M_FoliageShutdown(void)
{
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    R_PushCmd((struct rcmd){ .func = R_GL_MapFoliageShutdown, .nargs = 0 });
    s_map = NULL;
}

