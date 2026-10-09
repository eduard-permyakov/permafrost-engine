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
#define MEM_FILE_SUB MEM_SUB_MAP_DECAL

#include "public/map.h"
#include "public/tile.h"
#include "map_private.h"
#include "pfchunk.h"
#include "../main.h"
#include "../settings.h"
#include "../camera.h"
#include "../event.h"
#include "../game/public/game.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../phys/public/collision.h"
#include "../lib/public/attr.h"
#include "../lib/public/vec.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/stb_image.h"
#include "../lib/public/stb_image_resize.h"
#include "../mem.h"
#include "../pf_math.h"

#include <SDL.h>
#include <assert.h>
#include <string.h>
#include <math.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_MAP, MEM_SUB_MAP_DECAL)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_MAP, MEM_SUB_MAP_DECAL)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_MAP, MEM_SUB_MAP_DECAL)

#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))

/* Side of the alpha mask sampled to hide foliage */
#define FOLIAGE_MASK_RES (128)
/* Foliage planted just outside a decal still spills over it */
#define FOLIAGE_BUFFER   ((float)X_COORDS_PER_TILE)
/* Every clipping plane can add at most one vertex to the polygon */
#define MAX_POLY_VERTS  (16)
#define CHUNK_X_DIM     (TILES_PER_CHUNK_WIDTH  * X_COORDS_PER_TILE)
#define CHUNK_Z_DIM     (TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

struct decal{
    uint32_t id;
    /* Index into s_textures */
    int      tex;
    vec2_t   pos;
    vec2_t   dims;
    float    rotation;
    float    alpha;
    bool     highlight;
    bool     hide_foliage;
    bool     internal;
};

/* Images much longer than they are wide are split along their long side into
 * square segments, each in its own layer, to keep their detail. */
struct decal_tex{
    char     path[MAX_DECAL_PATH_LEN];
    int      refcount;
    int      nsegs;
    bool     vertical;
    int      layers[MAX_DECAL_SEGMENTS];
    /* Lazily loaded; FOLIAGE_MASK_RES^2 alpha values */
    uint8_t *mask;
};

/* A half-plane in the XZ plane: a*x + b*z + c >= 0 */
struct plane2d{
    float a, b, c;
};

/* The affine mapping from world XZ to a decal's texture coordinates */
struct uv_map{
    struct plane2d u, v;
};

struct rect{
    float x_min, x_max;
    float z_min, z_max;
};

struct poly_vert{
    vec3_t pos;
    vec3_t normal;
};

struct foliage_masker{
    struct uv_map  map;
    const uint8_t *mask;
    float          alpha;
    /* FOLIAGE_BUFFER in texture coordinates */
    vec2_t         buffer;
};

VEC_TYPE(decal, struct decal)
VEC_IMPL(static inline, decal, struct decal)

VEC_TYPE(dtex, struct decal_tex)
VEC_IMPL(static inline, dtex, struct decal_tex)

VEC_TYPE(dvert, struct decal_vert)
VEC_IMPL(static inline, dvert, struct decal_vert)

VEC_TYPE(masker, struct foliage_masker)
VEC_IMPL(static inline, masker, struct foliage_masker)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map *s_map         = NULL;
/* In draw order, the internal decals first */
static vec_decal_t       s_decals;
static size_t            s_num_internal = 0;
static vec_dtex_t        s_textures;
static bool              s_layer_used[MAX_DECAL_LAYERS];
/* Side of a texture array layer */
static int               s_res         = 0;
static vec_dvert_t       s_scratch;
/* The foliage-hiding decals over the chunk of the last foliage query */
static vec_masker_t      s_maskers;
static bool             *s_dirty       = NULL;
static size_t           *s_counts      = NULL;
static size_t            s_num_chunks  = 0;
static uint32_t          s_next_id     = 1;
/* The screen position a selection box is dragged from, if any */
static bool              s_drag_box_on = false;
static vec2_t            s_drag_box_start;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static float plane_eval(struct plane2d p, vec3_t pos)
{
    return p.a * pos.x + p.b * pos.z + p.c;
}

static struct uv_map decal_uv_map(const struct decal *decal)
{
    float rad = DEG_TO_RAD(decal->rotation);
    float c = cosf(rad), s = sinf(rad);

    struct plane2d u = (struct plane2d){ -c / decal->dims.x, -s / decal->dims.x, 0.0f };
    struct plane2d v = (struct plane2d){  s / decal->dims.z, -c / decal->dims.z, 0.0f };
    u.c = 0.5f - (u.a * decal->pos.x + u.b * decal->pos.z);
    v.c = 0.5f - (v.a * decal->pos.x + v.b * decal->pos.z);
    return (struct uv_map){ u, v };
}

static void decal_corners(const struct decal *decal, vec2_t out[4])
{
    float rad = DEG_TO_RAD(decal->rotation);
    float c = cosf(rad), s = sinf(rad);
    float hw = decal->dims.x / 2.0f;
    float hh = decal->dims.z / 2.0f;

    const vec2_t local[4] = {
        { -hw, -hh },
        {  hw, -hh },
        {  hw,  hh },
        { -hw,  hh },
    };
    for(int i = 0; i < 4; i++) {
        out[i] = (vec2_t){
            decal->pos.x + local[i].x * c - local[i].z * s,
            decal->pos.z + local[i].x * s + local[i].z * c,
        };
    }
}

static struct rect decal_bounds(const struct decal *decal)
{
    float rad = DEG_TO_RAD(decal->rotation);
    float c = fabsf(cosf(rad)), s = fabsf(sinf(rad));
    float hx = (decal->dims.x * c + decal->dims.z * s) / 2.0f;
    float hz = (decal->dims.x * s + decal->dims.z * c) / 2.0f;

    return (struct rect){
        decal->pos.x - hx, decal->pos.x + hx,
        decal->pos.z - hz, decal->pos.z + hz,
    };
}

static struct rect chunk_bounds(int cr, int cc)
{
    float x_max = s_map->pos.x - (float)(cc * CHUNK_X_DIM);
    float z_min = s_map->pos.z + (float)(cr * CHUNK_Z_DIM);
    return (struct rect){ x_max - CHUNK_X_DIM, x_max, z_min, z_min + CHUNK_Z_DIM };
}

static bool rects_overlap(struct rect a, struct rect b)
{
    return (a.x_min < b.x_max) && (a.x_max > b.x_min)
        && (a.z_min < b.z_max) && (a.z_max > b.z_min);
}

static void rect_planes(struct rect r, struct plane2d out[4])
{
    out[0] = (struct plane2d){  1.0f,  0.0f, -r.x_min };
    out[1] = (struct plane2d){ -1.0f,  0.0f,  r.x_max };
    out[2] = (struct plane2d){  0.0f,  1.0f, -r.z_min };
    out[3] = (struct plane2d){  0.0f, -1.0f,  r.z_max };
}

static void uv_planes(struct uv_map map, struct plane2d out[4])
{
    out[0] = map.u;
    out[1] = (struct plane2d){ -map.u.a, -map.u.b, 1.0f - map.u.c };
    out[2] = map.v;
    out[3] = (struct plane2d){ -map.v.a, -map.v.b, 1.0f - map.v.c };
}

/* Sutherland-Hodgman against a single half-plane. Height is interpolated
 * linearly, which is exact since the input polygons are planar. */
static size_t clip_poly(const struct poly_vert *in, size_t nin, struct plane2d plane, 
                        struct poly_vert *out)
{
    size_t nout = 0;
    for(size_t i = 0; i < nin; i++) {

        struct poly_vert curr = in[i];
        struct poly_vert next = in[(i + 1) % nin];
        float dc = plane_eval(plane, curr.pos);
        float dn = plane_eval(plane, next.pos);

        if(dc >= 0.0f)
            out[nout++] = curr;
        if((dc >= 0.0f) != (dn >= 0.0f)) {
            float t = dc / (dc - dn);
            out[nout++] = (struct poly_vert){
                .pos = {
                    curr.pos.x + (next.pos.x - curr.pos.x) * t,
                    curr.pos.y + (next.pos.y - curr.pos.y) * t,
                    curr.pos.z + (next.pos.z - curr.pos.z) * t,
                },
                .normal = {
                    curr.normal.x + (next.normal.x - curr.normal.x) * t,
                    curr.normal.y + (next.normal.y - curr.normal.y) * t,
                    curr.normal.z + (next.normal.z - curr.normal.z) * t,
                },
            };
        }
    }
    assert(nout <= MAX_POLY_VERTS);
    return nout;
}

static size_t clip_poly_all(struct poly_vert poly[MAX_POLY_VERTS], size_t n,
                            const struct plane2d *planes, size_t nplanes)
{
    struct poly_vert tmp[MAX_POLY_VERTS];
    for(size_t i = 0; i < nplanes && n >= 3; i++) {
        n = clip_poly(poly, n, planes[i], tmp);
        memcpy(poly, tmp, n * sizeof(struct poly_vert));
    }
    return n;
}

static void emit_poly(const struct decal *decal, struct uv_map map, int layer,
                      const struct poly_vert *poly, size_t n)
{
    for(size_t i = 1; i + 1 < n; i++) {

        const struct poly_vert *tri[3] = { &poly[0], &poly[i], &poly[i + 1] };
        for(int j = 0; j < 3; j++) {

            vec3_t normal = tri[j]->normal;
            PFM_Vec3_Normal(&normal, &normal);

            vec_dvert_push(&s_scratch, (struct decal_vert){
                .pos       = tri[j]->pos,
                .normal    = normal,
                .uv        = (vec2_t){ plane_eval(map.u, tri[j]->pos), plane_eval(map.v, tri[j]->pos) },
                .layer     = (float)layer,
                .alpha     = decal->alpha,
                .highlight = decal->highlight ? 1.0f : 0.0f,
            });
        }
    }
}

/* Takes the tile coordinates over the whole map, not a single chunk */
static const struct tile *map_tile(int gr, int gc)
{
    int cr = gr / TILES_PER_CHUNK_HEIGHT, tr = gr % TILES_PER_CHUNK_HEIGHT;
    int cc = gc / TILES_PER_CHUNK_WIDTH,  tc = gc % TILES_PER_CHUNK_WIDTH;
    const struct pfchunk *chunk = &s_map->chunks[cr * s_map->width + cc];
    return &chunk->tiles[tr * TILES_PER_CHUNK_WIDTH + tc];
}

/* The terrain's smoothed normals tilt at the edges of a flat tile beside a
 * slope, so the neighbouring ring of tiles must be level too. */
static bool tiles_flat_and_level(int gr_min, int gr_max, int gc_min, int gc_max, 
                                 int *out_height)
{
    gr_min = MAX(gr_min - 1, 0);
    gc_min = MAX(gc_min - 1, 0);
    gr_max = MIN(gr_max + 1, s_map->height * TILES_PER_CHUNK_HEIGHT - 1);
    gc_max = MIN(gc_max + 1, s_map->width  * TILES_PER_CHUNK_WIDTH  - 1);

    int height = map_tile(gr_min, gc_min)->base_height;
    for(int gr = gr_min; gr <= gr_max; gr++) {
    for(int gc = gc_min; gc <= gc_max; gc++) {
        const struct tile *tile = map_tile(gr, gc);
        if(tile->type != TILETYPE_FLAT || tile->base_height != height)
            return false;
    }}
    *out_height = height;
    return true;
}

static void emit_tile(const struct decal *decal, struct uv_map map, int layer,
                      int cr, int cc, int tr, int tc)
{
    struct plane2d planes[4];
    uv_planes(map, planes);

    mat4x4_t model;
    M_ModelMatrixForChunk(s_map, (struct chunkpos){cr, cc}, &model);

    vec3_t pos[VERTS_PER_TOP_FACE], normals[VERTS_PER_TOP_FACE];
    struct tile_desc td = (struct tile_desc){cr, cc, tr, tc};
    int nverts = R_TileGetTopFace(s_map, &td, &model, pos, normals);

    for(int i = 0; i + 2 < nverts; i += 3) {

        struct poly_vert poly[MAX_POLY_VERTS] = {
            { pos[i + 0], normals[i + 0] },
            { pos[i + 1], normals[i + 1] },
            { pos[i + 2], normals[i + 2] },
        };
        size_t n = clip_poly_all(poly, 3, planes, ARR_SIZE(planes));
        emit_poly(decal, map, layer, poly, n);
    }
}

/* Maps the segment's band of the decal to [0, 1] */
static struct uv_map segment_uv_map(struct uv_map map, const struct decal_tex *tex, int seg)
{
    struct plane2d *axis = tex->vertical ? &map.v : &map.u;
    axis->a *= tex->nsegs;
    axis->b *= tex->nsegs;
    axis->c = axis->c * tex->nsegs - seg;
    return map;
}

/* The whole segment over level ground is a single polygon */
static void emit_level(const struct decal *decal, struct uv_map map, int layer,
                       struct rect chunk, int height)
{
    vec2_t corners[4];
    decal_corners(decal, corners);

    float y = height * Y_COORDS_PER_TILE;
    struct poly_vert poly[MAX_POLY_VERTS];
    for(int i = 0; i < 4; i++)
        poly[i] = (struct poly_vert){ { corners[i].x, y, corners[i].z }, { 0.0f, 1.0f, 0.0f } };

    struct plane2d planes[8];
    rect_planes(chunk, planes);
    uv_planes(map, planes + 4);
    size_t n = clip_poly_all(poly, 4, planes, ARR_SIZE(planes));
    emit_poly(decal, map, layer, poly, n);
}

static void emit_decal_in_chunk(const struct decal *decal, int cr, int cc)
{
    struct rect chunk = chunk_bounds(cr, cc);
    struct rect bounds = decal_bounds(decal);
    if(!rects_overlap(chunk, bounds))
        return;

    int tc_min = (int)floorf((chunk.x_max - bounds.x_max) / X_COORDS_PER_TILE);
    int tc_max = (int)floorf((chunk.x_max - bounds.x_min) / X_COORDS_PER_TILE);
    int tr_min = (int)floorf((bounds.z_min - chunk.z_min) / Z_COORDS_PER_TILE);
    int tr_max = (int)floorf((bounds.z_max - chunk.z_min) / Z_COORDS_PER_TILE);

    tc_min = MAX(tc_min, 0); tc_max = MIN(tc_max, TILES_PER_CHUNK_WIDTH - 1);
    tr_min = MAX(tr_min, 0); tr_max = MIN(tr_max, TILES_PER_CHUNK_HEIGHT - 1);

    const struct decal_tex *tex = &vec_AT(&s_textures, decal->tex);
    struct uv_map map = decal_uv_map(decal);

    int height;
    bool level = tiles_flat_and_level(
        cr * TILES_PER_CHUNK_HEIGHT + tr_min, cr * TILES_PER_CHUNK_HEIGHT + tr_max,
        cc * TILES_PER_CHUNK_WIDTH  + tc_min, cc * TILES_PER_CHUNK_WIDTH  + tc_max, &height);

    for(int i = 0; i < tex->nsegs; i++) {

        struct uv_map seg_map = segment_uv_map(map, tex, i);
        if(level) {
            emit_level(decal, seg_map, tex->layers[i], chunk, height);
            continue;
        }

        for(int tr = tr_min; tr <= tr_max; tr++) {
        for(int tc = tc_min; tc <= tc_max; tc++) {
            emit_tile(decal, seg_map, tex->layers[i], cr, cc, tr, tc);
        }}
    }
}

static void push_chunk_to_render(size_t chunk_idx)
{
    size_t count = vec_size(&s_scratch);
    R_PushCmd((struct rcmd){
        .func  = R_GL_DecalSetChunk,
        .nargs = 3,
        .args  = {
            R_PushArg(&chunk_idx, sizeof(chunk_idx)),
            count > 0 ? R_PushArg(s_scratch.array, count * sizeof(struct decal_vert)) : NULL,
            R_PushArg(&count, sizeof(count)),
        },
    });
}

static void flush_dirty_chunks(void)
{
    for(int cr = 0; cr < s_map->height; cr++) {
    for(int cc = 0; cc < s_map->width; cc++) {

        size_t chunk_idx = (size_t)cr * s_map->width + cc;
        if(!s_dirty[chunk_idx])
            continue;

        vec_dvert_reset(&s_scratch);
        for(int i = 0; i < vec_size(&s_decals); i++) {
            emit_decal_in_chunk(&vec_AT(&s_decals, i), cr, cc);
        }
        s_counts[chunk_idx] = vec_size(&s_scratch);
        push_chunk_to_render(chunk_idx);
        s_dirty[chunk_idx] = false;
    }}
}

static void mark_dirty(const struct decal *decal)
{
    struct rect bounds = decal_bounds(decal);
    for(int cr = 0; cr < s_map->height; cr++) {
    for(int cc = 0; cc < s_map->width; cc++) {
        if(rects_overlap(chunk_bounds(cr, cc), bounds))
            s_dirty[(size_t)cr * s_map->width + cc] = true;
    }}

    if(decal->hide_foliage) {
        M_FoliageInvalidateArea(
            (vec2_t){ bounds.x_min - FOLIAGE_BUFFER, bounds.z_min - FOLIAGE_BUFFER }, 
            (vec2_t){ bounds.x_max + FOLIAGE_BUFFER, bounds.z_max + FOLIAGE_BUFFER });
    }
}

static void aabb_for_chunk(int cr, int cc, struct aabb *out)
{
    struct rect bounds = chunk_bounds(cr, cc);
    out->x_min = bounds.x_min;
    out->x_max = bounds.x_max;
    out->z_min = bounds.z_min;
    out->z_max = bounds.z_max;
    out->y_min = 0.0f;
    out->y_max = (float)(MAX_HEIGHT_LEVEL * Y_COORDS_PER_TILE);
}

static int decal_index(uint32_t id, bool internal)
{
    int begin = internal ? 0 : s_num_internal;
    int end = internal ? s_num_internal : vec_size(&s_decals);
    for(int i = begin; i < end; i++) {
        if(vec_AT(&s_decals, i).id == id)
            return i;
    }
    return -1;
}

static void abs_path(const char *path, char *out, size_t size)
{
    pf_snprintf(out, size, "%s/%s", g_basepath, path);
}

static bool texture_valid(const char *path)
{
    char abspath[512];
    abs_path(path, abspath, sizeof(abspath));

    int w, h, n;
    return (strlen(path) < MAX_DECAL_PATH_LEN)
        && stbi_info(abspath, &w, &h, &n);
}

static int layer_alloc(void)
{
    for(int i = 0; i < MAX_DECAL_LAYERS; i++) {
        if(!s_layer_used[i]) {
            s_layer_used[i] = true;
            return i;
        }
    }
    return -1;
}

static void texture_free_layers(struct decal_tex *tex)
{
    for(int i = 0; i < tex->nsegs; i++)
        s_layer_used[tex->layers[i]] = false;
    tex->nsegs = 0;
}

static int segment_count(int long_side, int short_side)
{
    int by_aspect = (int)floorf((float)long_side / short_side + 0.5f);
    int by_res = (long_side + s_res - 1) / s_res;
    return MAX(1, MIN(MIN(by_aspect, by_res), MAX_DECAL_SEGMENTS));
}

static int texture_acquire(const char *path)
{
    int free_slot = -1;
    for(int i = 0; i < vec_size(&s_textures); i++) {
        struct decal_tex *tex = &vec_AT(&s_textures, i);
        if(tex->refcount > 0 && 0 == strcmp(tex->path, path)) {
            tex->refcount++;
            return i;
        }
        if(tex->refcount == 0 && free_slot < 0)
            free_slot = i;
    }

    char abspath[512];
    abs_path(path, abspath, sizeof(abspath));

    int w, h, n;
    if(!stbi_info(abspath, &w, &h, &n))
        return -1;

    if(free_slot < 0) {
        if(!vec_dtex_push(&s_textures, (struct decal_tex){0}))
            return -1;
        free_slot = vec_size(&s_textures) - 1;
    }

    struct decal_tex *tex = &vec_AT(&s_textures, free_slot);
    pf_strlcpy(tex->path, path, sizeof(tex->path));
    tex->vertical = (h > w);
    tex->nsegs = 0;

    int nsegs = tex->vertical ? segment_count(h, w) : segment_count(w, h);
    for(int i = 0; i < nsegs; i++) {
        int layer = layer_alloc();
        if(layer < 0) {
            texture_free_layers(tex);
            return -1;
        }
        tex->layers[tex->nsegs++] = layer;
    }
    tex->refcount = 1;

    for(int i = 0; i < nsegs; i++) {

        struct decal_segment seg = (struct decal_segment){
            .index    = i,
            .count    = nsegs,
            .vertical = tex->vertical,
        };
        R_PushCmd((struct rcmd){
            .func  = R_GL_DecalSetTexture,
            .nargs = 4,
            .args  = {
                R_PushArg(&tex->layers[i], sizeof(tex->layers[i])),
                R_PushArg(&seg, sizeof(seg)),
                R_PushArg(abspath, strlen(abspath) + 1),
                R_PushArg(path, strlen(path) + 1),
            },
        });
    }
    return free_slot;
}

static void texture_release(int slot)
{
    struct decal_tex *tex = &vec_AT(&s_textures, slot);
    assert(tex->refcount > 0);
    if(--tex->refcount == 0) {
        texture_free_layers(tex);
        PF_FREE(tex->mask);
        tex->mask = NULL;
    }
}

static const uint8_t *texture_mask(int slot)
{
    struct decal_tex *tex = &vec_AT(&s_textures, slot);
    if(tex->mask)
        return tex->mask;

    char abspath[512];
    abs_path(tex->path, abspath, sizeof(abspath));

    int w, h, n;
    unsigned char *data = stbi_load(abspath, &w, &h, &n, 4);
    if(!data)
        return NULL;

    const size_t npx = FOLIAGE_MASK_RES * FOLIAGE_MASK_RES;
    unsigned char *rgba = PF_MALLOC(npx * 4);
    tex->mask = PF_MALLOC(npx);
    if(!rgba || !tex->mask
    || !stbir_resize_uint8(data, w, h, 0, rgba, FOLIAGE_MASK_RES, FOLIAGE_MASK_RES, 0, 4)) {
        PF_FREE(tex->mask);
        tex->mask = NULL;
    }else{
        for(size_t i = 0; i < npx; i++)
            tex->mask[i] = rgba[i * 4 + 3];
    }

    PF_FREE(rgba);
    stbi_image_free(data);
    return tex->mask;
}

static bool desc_valid(const struct decal_desc *desc)
{
    return (desc->dims.x > 0.0f) && (desc->dims.z > 0.0f)
        && isfinite(desc->pos.x) && isfinite(desc->pos.z)
        && isfinite(desc->rotation)
        && (desc->alpha >= 0.0f) && (desc->alpha <= 1.0f);
}

static bool decal_add(uint32_t id, const struct decal_desc *desc, bool internal)
{
    if(!desc_valid(desc) || !texture_valid(desc->path))
        return false;

    int tex = texture_acquire(desc->path);
    if(tex < 0)
        return false;

    struct decal decal = (struct decal){
        .id        = id,
        .tex       = tex,
        .pos       = desc->pos,
        .dims      = desc->dims,
        .rotation  = desc->rotation,
        .alpha     = desc->alpha,
        .highlight = false,
        .hide_foliage = desc->hide_foliage,
        .internal  = internal,
    };
    if(!vec_decal_push(&s_decals, decal)) {
        texture_release(tex);
        return false;
    }

    if(internal) {
        size_t last = vec_size(&s_decals) - 1;
        memmove(&vec_AT(&s_decals, s_num_internal + 1), &vec_AT(&s_decals, s_num_internal),
            (last - s_num_internal) * sizeof(struct decal));
        vec_AT(&s_decals, s_num_internal) = decal;
        s_num_internal++;
    }
    mark_dirty(&decal);
    return true;
}

static void decal_remove(int idx)
{
    struct decal *decal = &vec_AT(&s_decals, idx);
    if(decal->internal)
        s_num_internal--;

    mark_dirty(decal);
    texture_release(decal->tex);
    vec_decal_del(&s_decals, idx);
}

static bool decal_update(int idx, const struct decal_desc *desc)
{
    if(!desc_valid(desc))
        return false;

    struct decal *decal = &vec_AT(&s_decals, idx);
    if(0 != strcmp(vec_AT(&s_textures, decal->tex).path, desc->path)) {

        if(!texture_valid(desc->path))
            return false;
        int tex = texture_acquire(desc->path);
        if(tex < 0)
            return false;
        texture_release(decal->tex);
        decal->tex = tex;
    }

    mark_dirty(decal);
    decal->pos = desc->pos;
    decal->dims = desc->dims;
    decal->rotation = desc->rotation;
    decal->alpha = desc->alpha;
    decal->hide_foliage = desc->hide_foliage;
    mark_dirty(decal);
    return true;
}

static void desc_for_decal(const struct decal *decal, struct decal_desc *out)
{
    pf_strlcpy(out->path, vec_AT(&s_textures, decal->tex).path, sizeof(out->path));
    out->pos = decal->pos;
    out->dims = decal->dims;
    out->rotation = decal->rotation;
    out->alpha = decal->alpha;
    out->hide_foliage = decal->hide_foliage;
}

static void on_render_ui(void *user, void *event)
{
    if(!s_drag_box_on)
        return;

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    vec2_t signed_size = (vec2_t){ mouse_x - s_drag_box_start.x, mouse_y - s_drag_box_start.z };
    const float width = 2.0f;
    const vec3_t color = (vec3_t){ 1.0f, 0.85f, 0.2f };

    R_PushCmd((struct rcmd){
        .func  = R_GL_DrawBox2D,
        .nargs = 4,
        .args  = {
            R_PushArg(&s_drag_box_start, sizeof(s_drag_box_start)),
            R_PushArg(&signed_size, sizeof(signed_size)),
            R_PushArg(&color, sizeof(color)),
            R_PushArg(&width, sizeof(width)),
        },
    });
}

static bool screen_pos(const mat4x4_t *view_proj, vec3_t ws_pos, vec2_t *out)
{
    vec4_t homo = (vec4_t){ ws_pos.x, ws_pos.y, ws_pos.z, 1.0f };
    vec4_t clip;
    PFM_Mat4x4_Mult4x1((mat4x4_t*)view_proj, &homo, &clip);
    if(clip.w <= 0.0f)
        return false;

    int w, h;
    Engine_WinDrawableSize(&w, &h);
    *out = (vec2_t){
        (clip.x / clip.w + 1.0f) * 0.5f * w,
        (1.0f - clip.y / clip.w) * 0.5f * h,
    };
    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool M_Decal_Init(const struct map *map)
{
    s_map = map;
    s_num_chunks = (size_t)(map->width * map->height);
    s_next_id = 1;
    s_num_internal = 0;

    s_dirty  = PF_CALLOC(s_num_chunks, sizeof(bool));
    s_counts = PF_CALLOC(s_num_chunks, sizeof(size_t));
    if(!s_dirty || !s_counts)
        goto fail_alloc;

    vec_decal_init(&s_decals);
    vec_dtex_init(&s_textures);
    vec_dvert_init(&s_scratch);
    vec_masker_init(&s_maskers);

    struct sval res;
    ss_e status = Settings_Get("pf.video.decal_res", &res);
    assert(status == SS_OKAY);
    (void)status;

    s_res = res.as_int;
    memset(s_layer_used, 0, sizeof(s_layer_used));
    s_drag_box_on = false;
    E_Global_Register(EVENT_RENDER_UI, on_render_ui, NULL, G_ALL);

    R_PushCmd((struct rcmd){
        .func  = R_GL_DecalMapInit,
        .nargs = 2,
        .args  = {
            R_PushArg(&s_num_chunks, sizeof(s_num_chunks)),
            R_PushArg(&res.as_int, sizeof(res.as_int)),
        },
    });
    return true;

fail_alloc:
    PF_FREE(s_dirty);
    PF_FREE(s_counts);
    s_dirty = NULL;
    s_counts = NULL;
    s_num_chunks = 0;
    s_map = NULL;
    return false;
}

void M_Decal_Shutdown(void)
{
    if(!s_map)
        return;

    R_PushCmd((struct rcmd){ .func = R_GL_DecalMapShutdown, .nargs = 0 });
    E_Global_Unregister(EVENT_RENDER_UI, on_render_ui);

    for(int i = 0; i < vec_size(&s_textures); i++) {
        PF_FREE(vec_AT(&s_textures, i).mask);
    }
    vec_decal_destroy(&s_decals);
    vec_dtex_destroy(&s_textures);
    vec_dvert_destroy(&s_scratch);
    vec_masker_destroy(&s_maskers);
    PF_FREE(s_dirty);
    PF_FREE(s_counts);

    s_map        = NULL;
    s_dirty      = NULL;
    s_counts     = NULL;
    s_num_chunks = 0;
    s_next_id    = 1;
    s_num_internal = 0;
}

uint32_t M_Decal_Add(const struct decal_desc *desc)
{
    if(!s_map)
        return NULL_DECAL_ID;
    if(!decal_add(s_next_id, desc, false))
        return NULL_DECAL_ID;
    return s_next_id++;
}

bool M_Decal_Remove(uint32_t id)
{
    if(!s_map)
        return false;

    int idx = decal_index(id, false);
    if(idx < 0)
        return false;

    decal_remove(idx);
    return true;
}

bool M_Decal_Update(uint32_t id, const struct decal_desc *desc)
{
    if(!s_map)
        return false;

    int idx = decal_index(id, false);
    if(idx < 0)
        return false;

    return decal_update(idx, desc);
}

uint32_t M_Decal_AddInternal(const struct decal_desc *desc)
{
    if(!s_map)
        return NULL_DECAL_ID;
    if(!decal_add(s_next_id, desc, true))
        return NULL_DECAL_ID;
    return s_next_id++;
}

bool M_Decal_RemoveInternal(uint32_t id)
{
    if(!s_map)
        return false;

    int idx = decal_index(id, true);
    if(idx < 0)
        return false;

    decal_remove(idx);
    return true;
}

bool M_Decal_UpdateInternal(uint32_t id, const struct decal_desc *desc)
{
    if(!s_map)
        return false;

    int idx = decal_index(id, true);
    if(idx < 0)
        return false;

    return decal_update(idx, desc);
}

bool M_Decal_Get(uint32_t id, struct decal_desc *out)
{
    if(!s_map)
        return false;

    int idx = decal_index(id, false);
    if(idx < 0)
        return false;

    desc_for_decal(&vec_AT(&s_decals, idx), out);
    return true;
}

size_t M_Decal_At(vec2_t xz, size_t maxout, uint32_t out[])
{
    if(!s_map)
        return 0;

    size_t ret = 0;
    vec3_t pos = (vec3_t){ xz.x, 0.0f, xz.z };

    for(int i = vec_size(&s_decals) - 1; i >= (int)s_num_internal && ret < maxout; i--) {

        const struct decal *decal = &vec_AT(&s_decals, i);
        struct uv_map map = decal_uv_map(decal);
        float u = plane_eval(map.u, pos);
        float v = plane_eval(map.v, pos);
        if(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)
            out[ret++] = decal->id;
    }
    return ret;
}

size_t M_Decal_Count(void)
{
    if(!s_map)
        return 0;
    return vec_size(&s_decals) - s_num_internal;
}

size_t M_Decal_GetAll(size_t maxout, uint32_t out[])
{
    if(!s_map)
        return 0;

    size_t ret = 0;
    for(int i = s_num_internal; i < vec_size(&s_decals) && ret < maxout; i++) {
        out[ret++] = vec_AT(&s_decals, i).id;
    }
    return ret;
}

bool M_Decal_SetOrder(uint32_t id, int order)
{
    if(!s_map)
        return false;

    int idx = decal_index(id, false);
    if(idx < 0)
        return false;

    order = MAX(order, 0) + s_num_internal;
    order = MIN(order, vec_size(&s_decals) - 1);

    struct decal decal = vec_AT(&s_decals, idx);
    if(idx < order) {
        memmove(&vec_AT(&s_decals, idx), &vec_AT(&s_decals, idx + 1),
            (order - idx) * sizeof(struct decal));
    }else{
        memmove(&vec_AT(&s_decals, order + 1), &vec_AT(&s_decals, order),
            (idx - order) * sizeof(struct decal));
    }
    vec_AT(&s_decals, order) = decal;
    mark_dirty(&decal);
    return true;
}

bool M_Decal_SetHighlight(uint32_t id, bool on)
{
    if(!s_map)
        return false;

    int idx = decal_index(id, false);
    if(idx < 0)
        return false;

    struct decal *decal = &vec_AT(&s_decals, idx);
    if(decal->highlight != on) {
        decal->highlight = on;
        mark_dirty(decal);
    }
    return true;
}

void M_Decal_Clear(void)
{
    if(!s_map)
        return;

    for(int i = s_num_internal; i < vec_size(&s_decals); i++) {
        mark_dirty(&vec_AT(&s_decals, i));
        texture_release(vec_AT(&s_decals, i).tex);
    }
    s_decals.size = s_num_internal;
}

bool M_Decal_RetainImage(const char *path)
{
    if(!s_map || !texture_valid(path))
        return false;

    int slot = texture_acquire(path);
    if(slot < 0)
        return false;
    texture_mask(slot);
    return true;
}

void M_Decal_ReleaseImage(const char *path)
{
    if(!s_map)
        return;

    for(int i = 0; i < vec_size(&s_textures); i++) {
        struct decal_tex *tex = &vec_AT(&s_textures, i);
        if(tex->refcount > 0 && 0 == strcmp(tex->path, path)) {
            texture_release(i);
            return;
        }
    }
}

bool M_Decal_ImageSize(const char *path, int *out_w, int *out_h)
{
    char abspath[512];
    abs_path(path, abspath, sizeof(abspath));

    int n;
    return stbi_info(abspath, out_w, out_h, &n);
}

size_t M_Decal_InScreenRect(const struct camera *cam, vec2_t corner_a, vec2_t corner_b,
                            size_t maxout, uint32_t out[])
{
    if(!s_map)
        return 0;

    mat4x4_t view, proj, view_proj;
    Camera_MakeViewMat(cam, &view);
    Camera_MakeProjMat(cam, &proj);
    PFM_Mat4x4_Mult4x4(&proj, &view, &view_proj);

    float x_min = MIN(corner_a.x, corner_b.x), x_max = MAX(corner_a.x, corner_b.x);
    float y_min = MIN(corner_a.z, corner_b.z), y_max = MAX(corner_a.z, corner_b.z);

    size_t ret = 0;
    for(int i = s_num_internal; i < vec_size(&s_decals) && ret < maxout; i++) {

        const struct decal *decal = &vec_AT(&s_decals, i);
        float y = M_PointInsideMap(s_map, decal->pos) ? M_HeightAtPoint(s_map, decal->pos) : 0.0f;

        vec2_t ss;
        if(!screen_pos(&view_proj, (vec3_t){ decal->pos.x, y, decal->pos.z }, &ss))
            continue;
        if(ss.x >= x_min && ss.x <= x_max && ss.z >= y_min && ss.z <= y_max)
            out[ret++] = decal->id;
    }
    return ret;
}

void M_Decal_SetDragBox(const vec2_t *start)
{
    s_drag_box_on = (start != NULL);
    if(start)
        s_drag_box_start = *start;
}

void M_Decal_FoliageQueryBegin(int chunk_r, int chunk_c)
{
    vec_masker_reset(&s_maskers);
    if(!s_map)
        return;

    struct rect chunk = chunk_bounds(chunk_r, chunk_c);
    for(int i = 0; i < vec_size(&s_decals); i++) {

        const struct decal *decal = &vec_AT(&s_decals, i);
        if(!decal->hide_foliage)
            continue;

        struct rect bounds = decal_bounds(decal);
        bounds.x_min -= FOLIAGE_BUFFER; bounds.x_max += FOLIAGE_BUFFER;
        bounds.z_min -= FOLIAGE_BUFFER; bounds.z_max += FOLIAGE_BUFFER;
        if(!rects_overlap(chunk, bounds))
            continue;

        vec_masker_push(&s_maskers, (struct foliage_masker){
            .map   = decal_uv_map(decal),
            .mask  = texture_mask(decal->tex),
            .alpha = decal->alpha,
            .buffer = (vec2_t){ FOLIAGE_BUFFER / decal->dims.x, FOLIAGE_BUFFER / decal->dims.z },
        });
    }
}

static bool mask_opaque(const struct foliage_masker *masker, float u, float v)
{
    if(u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f)
        return false;

    /* Without a readable mask, the whole rectangle is opaque */
    if(!masker->mask)
        return true;

    int x = (int)(u * FOLIAGE_MASK_RES);
    int y = (int)(v * FOLIAGE_MASK_RES);
    return (masker->mask[y * FOLIAGE_MASK_RES + x] * masker->alpha >= 127.5f);
}

/* Samples a disc of FOLIAGE_BUFFER radius about the point */
static bool near_opaque(const struct foliage_masker *masker, float u, float v)
{
    static const float s_rings[] = { 0.5f, 1.0f };
    enum{ NUM_DIRS = 8 };

    if(mask_opaque(masker, u, v))
        return true;

    for(int r = 0; r < ARR_SIZE(s_rings); r++) {
    for(int d = 0; d < NUM_DIRS; d++) {
        float angle = d * (2.0f * (float)M_PI / NUM_DIRS);
        float du = cosf(angle) * masker->buffer.x * s_rings[r];
        float dv = sinf(angle) * masker->buffer.z * s_rings[r];
        if(mask_opaque(masker, u + du, v + dv))
            return true;
    }}
    return false;
}

bool M_Decal_HidesFoliage(vec2_t xz)
{
    vec3_t pos = (vec3_t){ xz.x, 0.0f, xz.z };
    for(int i = 0; i < vec_size(&s_maskers); i++) {

        const struct foliage_masker *curr = &vec_AT(&s_maskers, i);
        float u = plane_eval(curr->map.u, pos);
        float v = plane_eval(curr->map.v, pos);
        if(u < -curr->buffer.x || u >= 1.0f + curr->buffer.x 
        || v < -curr->buffer.z || v >= 1.0f + curr->buffer.z)
            continue;

        if(near_opaque(curr, u, v))
            return true;
    }
    return false;
}

void M_Decal_UpdateTile(struct tile_desc td)
{
    if(!s_map)
        return;

    struct map_resolution res;
    M_GetResolution(s_map, &res);

    /* The tile's height also shapes the bordering faces of its neighbours */
    for(int dr = -1; dr <= 1; dr++) {
    for(int dc = -1; dc <= 1; dc++) {

        struct tile_desc curr = td;
        if(!M_Tile_RelativeDesc(res, &curr, dc, dr))
            continue;
        s_dirty[(size_t)curr.chunk_r * s_map->width + curr.chunk_c] = true;
    }}
}

void M_Decal_Render(const struct camera *cam)
{
    if(!s_map)
        return;

    flush_dirty_chunks();

    struct map_resolution res;
    M_GetResolution(s_map, &res);

    vec2_t map_pos = (vec2_t){ s_map->pos.x, s_map->pos.z };

    struct frustum frustum;
    Camera_MakeFrustum(cam, &frustum);

    bool any_visible = false;
    for(int cr = 0; cr < s_map->height; cr++) {
    for(int cc = 0; cc < s_map->width; cc++) {

        size_t chunk_idx = (size_t)cr * s_map->width + cc;
        if(s_counts[chunk_idx] == 0)
            continue;

        struct aabb chunk_aabb;
        aabb_for_chunk(cr, cc, &chunk_aabb);
        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        if(!any_visible) {
            R_PushCmd((struct rcmd){
                .func  = R_GL_DecalBeginDraw,
                .nargs = 3,
                .args  = {
                    R_PushArg(cam, g_sizeof_camera),
                    R_PushArg(&res, sizeof(res)),
                    R_PushArg(&map_pos, sizeof(map_pos)),
                },
            });
            any_visible = true;
        }

        R_PushCmd((struct rcmd){
            .func  = R_GL_DecalDrawChunk,
            .nargs = 1,
            .args  = { R_PushArg(&chunk_idx, sizeof(chunk_idx)) },
        });
    }}

    if(any_visible) {
        R_PushCmd((struct rcmd){ .func = R_GL_DecalEndDraw, .nargs = 0 });
    }
}

bool M_Decal_SaveState(SDL_RWops *stream)
{
    struct attr num_decals = (struct attr){
        .type = TYPE_INT,
        .val.as_int = M_Decal_Count()
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_decals, "num_decals"));

    struct attr next_id = (struct attr){
        .type = TYPE_INT,
        .val.as_int = s_next_id
    };
    CHK_TRUE_RET(Attr_Write(stream, &next_id, "next_id"));

    for(int i = s_num_internal; i < vec_size(&s_decals); i++) {

        const struct decal *decal = &vec_AT(&s_decals, i);
        struct decal_desc desc;
        desc_for_decal(decal, &desc);

        struct attr id = (struct attr){
            .type = TYPE_INT,
            .val.as_int = decal->id
        };
        CHK_TRUE_RET(Attr_Write(stream, &id, "id"));

        struct attr path = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(path.val.as_string, desc.path, sizeof(path.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &path, "path"));

        struct attr pos = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = desc.pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &pos, "pos"));

        struct attr dims = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = desc.dims
        };
        CHK_TRUE_RET(Attr_Write(stream, &dims, "dims"));

        struct attr rotation = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = desc.rotation
        };
        CHK_TRUE_RET(Attr_Write(stream, &rotation, "rotation"));

        struct attr alpha = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = desc.alpha
        };
        CHK_TRUE_RET(Attr_Write(stream, &alpha, "alpha"));

        struct attr hide_foliage = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = desc.hide_foliage
        };
        CHK_TRUE_RET(Attr_Write(stream, &hide_foliage, "hide_foliage"));
    }
    return true;
}

bool M_Decal_LoadState(SDL_RWops *stream)
{
    struct attr attr;
    M_Decal_Clear();

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_decals = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const uint32_t next_id = attr.val.as_int;

    for(int i = 0; i < num_decals; i++) {

        struct decal_desc desc;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t id = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        pf_strlcpy(desc.path, attr.val.as_string, sizeof(desc.path));

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        desc.pos = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        desc.dims = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        desc.rotation = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        desc.alpha = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        desc.hide_foliage = attr.val.as_bool;

        /* A texture missing since the save drops just that decal */
        decal_add(id, &desc, false);
    }

    s_next_id = MAX(s_next_id, next_id);
    return true;
}
