/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2023 Eduard Permyakov 
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

#define MEM_FILE_SYS MEM_SYS_GAME
#define MEM_FILE_SUB MEM_SUB_GAME_FOG_OF_WAR

#include "fog_of_war.h"
#include "public/game.h"
#include "position.h"
#include "game_private.h"
#include "../event.h"
#include "../settings.h"
#include "../sched.h"
#include "../perf.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"
#include "../lib/public/mem.h"
#include "../lib/public/simd.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"

#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <SDL.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_FOG_OF_WAR)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_FOG_OF_WAR)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_FOG_OF_WAR)


#define MIN(a, b)               ((a) < (b) ? (a) : (b))
#define MAX(a, b)               ((a) > (b) ? (a) : (b))
#define CLAMP(a, min, max)      (MIN(MAX((a), (min)), (max)))
#define ARR_SIZE(a)             (sizeof(a)/sizeof(a[0]))
#define FAC_STATE(val, fac_id)  (((val) >> ((fac_id) * 2)) & 0x3)
#define IDX(r, width, c)        ((r) * (width) + (c))

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

enum fog_state{
    STATE_UNEXPLORED = 0,
    STATE_IN_FOG,
    STATE_VISIBLE,
};

KHASH_SET_INIT_INT(uid)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map *s_map;
/* Map resolution and origin are constant for the map's lifetime; cache them at
 * init so the hot vision path never re-fetches them per tile. */
static struct map_resolution s_res;
static vec3_t                s_map_pos;
/* Holds a 32-bit value for every tile of the map. The chunks are stored in row-major
 * order. Within a chunk, the tiles are in row-major order. Each 32-bit value encodes
 * a 2-bit faction state for up to 16 factions. */
static uint32_t         *s_fog_state;
/* How many units of a faction currently 'see' every tile. */
static uint8_t          *s_vision_refcnts[MAX_FACTIONS];
/* Cache all the entities that have been explored by the player, for faster queries */
static khash_t(uid)     *s_explored_cache;
static bool              s_enabled = true;

/* Vision updates are batched: G_Fog_AddVision/RemoveVision enqueue a descriptor
 * and return immediately; the queue is drained once per frame (or on the first
 * state read) by fog_flush_pending. Main-thread only -- no locking. */
struct vis_update{
    int    faction_id;
    vec2_t pos;
    float  radius;
    int    delta;
};
static struct vis_update *s_updates;
static size_t             s_nupdates;
static size_t             s_updates_cap;

/* Precomputed vision disc: per-row contiguous [dcmin,dcmax] tile-offset runs
 * whose center is within radius -- the full unoccluded visible set on open
 * terrain, applied as a cheap forward scan. Cached per distinct radius. */
#define STAMP_MAX_ROWS  256
#define MAX_STAMP_CACHE  16
struct disc_stamp{
    float radius;
    int   nrows;
    int   dr[STAMP_MAX_ROWS];
    int   dcmin[STAMP_MAX_ROWS];
    int   dcmax[STAMP_MAX_ROWS];
};
static struct disc_stamp s_stamps[MAX_STAMP_CACHE];
static int               s_nstamps;

/* Per-chunk maximum tile base height. Lets fog_flush_pending decide cheaply
 * whether a unit's vision box is fully open (disc stamp) or may contain an LOS
 * blocker (the LOS-aware stamp). Computed at init from static terrain. */
static int *s_chunk_maxh;

/* LOS-aware stamp (recursive shadowcasting): per-box visible mask, reused. */
static uint8_t *s_los_vis;
static size_t   s_los_cap;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool fog_any_matches(uint32_t val, enum fog_state state)
{
    for(;val; val >>= 2) {
        if((val & 0x3) == state)
            return true;
    }
    return false;
}

static void fog_set_state(uint32_t *inout_tileval, int faction_id, enum fog_state state)
{
    assert(state >= 0 && state < 4);
    uint32_t val = *inout_tileval & ~(0x3 << faction_id * 2);
    val |= (state << (faction_id * 2));
    *inout_tileval = val;
}

static int td_index(struct tile_desc td)
{
    struct map_resolution res = s_res;

    size_t tiles_per_chunk = res.tile_w * res.tile_h;
    return td.chunk_r * (res.chunk_w * tiles_per_chunk) + td.chunk_c * tiles_per_chunk
        + (td.tile_r * res.tile_w + td.tile_c);
}

static void update_tile(int faction_id, struct tile_desc td, int delta)
{
    uint8_t old = s_vision_refcnts[faction_id][td_index(td)];
    uint8_t new = old + delta;

    if(new) {
        fog_set_state(s_fog_state + td_index(td), faction_id, STATE_VISIBLE);
    }else{
        fog_set_state(s_fog_state + td_index(td), faction_id, STATE_IN_FOG);
    }

    s_vision_refcnts[faction_id][td_index(td)] = new;
}

static void enqueue_update(int faction_id, vec2_t pos, float radius, int delta)
{
    if(s_nupdates == s_updates_cap) {
        s_updates_cap = s_updates_cap ? s_updates_cap * 2 : 4096;
        s_updates = PF_REALLOC(s_updates, s_updates_cap * sizeof(*s_updates));
        assert(s_updates);
    }
    s_updates[s_nupdates++] = (struct vis_update){faction_id, pos, radius, delta};
}

/* Build per-row disc runs from the tile-center distance test (origin_delta =
 * (-dc*X, dr*Z), len <= radius): every tile within radius, as contiguous runs. */
static void build_disc_stamp(struct disc_stamp *st, float radius)
{
    int zrad = (int)ceil(radius / Z_COORDS_PER_TILE) + 1;
    int xrad = (int)ceil(radius / X_COORDS_PER_TILE) + 1;

    st->radius = radius;
    st->nrows  = 0;

    for(int dr = -zrad; dr <= zrad && st->nrows < STAMP_MAX_ROWS; dr++) {
        int found = 0, dcmin = 0, dcmax = 0;
        for(int dc = -xrad; dc <= xrad; dc++) {
            float dx = (float)(-dc * X_COORDS_PER_TILE);
            float dz = (float)( dr * Z_COORDS_PER_TILE);
            if(sqrtf(dx * dx + dz * dz) <= radius) {
                if(!found) { dcmin = dc; found = 1; }
                dcmax = dc;
            }
        }
        if(found) {
            st->dr[st->nrows]    = dr;
            st->dcmin[st->nrows] = dcmin;
            st->dcmax[st->nrows] = dcmax;
            st->nrows++;
        }
    }
}

/* Look up (or build + cache) the disc stamp for a radius. Distinct radii are few
 * (one per unit type), so a small linear cache suffices. */
static const struct disc_stamp *get_stamp(float radius)
{
    for(int i = 0; i < s_nstamps; i++) {
        if(s_stamps[i].radius == radius)
            return &s_stamps[i];
    }
    int slot = (s_nstamps < MAX_STAMP_CACHE) ? s_nstamps++ : 0;
    build_disc_stamp(&s_stamps[slot], radius);
    return &s_stamps[slot];
}

/* Update a contiguous run of n tiles: refcnt[i] += delta (uint8 wrap), then set
 * faction 'shift'/'clearmask' bits of fog_state[i] to VISIBLE if the new refcnt
 * != 0 else IN_FOG. Bit-identical to update_tile. The concrete variant is bound
 * once in G_Fog_Init via simd_*_supported(). */
typedef void (*stamp_row_fn)(uint8_t *rc, uint32_t *fs, int n, int shift,
                             uint32_t clearmask, int delta);

static void stamp_row_scalar(uint8_t *rc, uint32_t *fs, int n, int shift,
                             uint32_t clearmask, int delta)
{
    for(int i = 0; i < n; i++) {
        uint8_t nv = (uint8_t)(rc[i] + delta);
        rc[i] = nv;
        fs[i] = (fs[i] & clearmask) | ((uint32_t)(nv ? STATE_VISIBLE : STATE_IN_FOG) << shift);
    }
}

SIMD_TARGET_AVX2
static void stamp_row_avx2(uint8_t *rc, uint32_t *fs, int n, int shift,
                           uint32_t clearmask, int delta)
{
    const __m256i vclear = _mm256_set1_epi32((int)clearmask);
    const __m128i vdelta = _mm_set1_epi8((char)delta);
    const __m128i vone   = _mm_set1_epi8(1);
    const __m128i vshift = _mm_cvtsi32_si128(shift);
    int i = 0;
    for(; i + 8 <= n; i += 8) {
        __m128i r8  = _mm_loadl_epi64((const __m128i*)(rc + i));
        __m128i nv8 = _mm_add_epi8(r8, vdelta);
        _mm_storel_epi64((__m128i*)(rc + i), nv8);
        __m128i isz   = _mm_cmpeq_epi8(nv8, _mm_setzero_si128());
        __m128i state = _mm_add_epi8(vone, _mm_andnot_si128(isz, vone));
        __m256i sbits = _mm256_sll_epi32(_mm256_cvtepu8_epi32(state), vshift);
        __m256i f     = _mm256_loadu_si256((const __m256i*)(fs + i));
        f = _mm256_or_si256(_mm256_and_si256(f, vclear), sbits);
        _mm256_storeu_si256((__m256i*)(fs + i), f);
    }
    stamp_row_scalar(rc + i, fs + i, n - i, shift, clearmask, delta);
}

SIMD_TARGET_AVX512F
static void stamp_row_avx512(uint8_t *rc, uint32_t *fs, int n, int shift,
                             uint32_t clearmask, int delta)
{
    const __m512i vclear  = _mm512_set1_epi32((int)clearmask);
    const __m256i vclear2 = _mm256_set1_epi32((int)clearmask);
    const __m128i vdelta  = _mm_set1_epi8((char)delta);
    const __m128i vone    = _mm_set1_epi8(1);
    const __m128i vshift  = _mm_cvtsi32_si128(shift);
    int i = 0;
    for(; i + 16 <= n; i += 16) {
        __m128i r16  = _mm_loadu_si128((const __m128i*)(rc + i));
        __m128i nv16 = _mm_add_epi8(r16, vdelta);
        _mm_storeu_si128((__m128i*)(rc + i), nv16);
        __m128i isz   = _mm_cmpeq_epi8(nv16, _mm_setzero_si128());
        __m128i state = _mm_add_epi8(vone, _mm_andnot_si128(isz, vone));
        __m512i sbits = _mm512_sll_epi32(_mm512_cvtepu8_epi32(state), vshift);
        __m512i f     = _mm512_loadu_si512((const void*)(fs + i));
        f = _mm512_or_si512(_mm512_and_si512(f, vclear), sbits);
        _mm512_storeu_si512((void*)(fs + i), f);
    }
    for(; i + 8 <= n; i += 8) {
        __m128i r8  = _mm_loadl_epi64((const __m128i*)(rc + i));
        __m128i nv8 = _mm_add_epi8(r8, vdelta);
        _mm_storel_epi64((__m128i*)(rc + i), nv8);
        __m128i isz   = _mm_cmpeq_epi8(nv8, _mm_setzero_si128());
        __m128i state = _mm_add_epi8(vone, _mm_andnot_si128(isz, vone));
        __m256i sbits = _mm256_sll_epi32(_mm256_cvtepu8_epi32(state), vshift);
        __m256i f     = _mm256_loadu_si256((const __m256i*)(fs + i));
        f = _mm256_or_si256(_mm256_and_si256(f, vclear2), sbits);
        _mm256_storeu_si256((__m256i*)(fs + i), f);
    }
    stamp_row_scalar(rc + i, fs + i, n - i, shift, clearmask, delta);
}

/* Bound once in G_Fog_Init from the runtime CPU query. s_stamp_row covers
 * short/medium runs; s_stamp_row_long the >=16-tile runs where AVX-512 wins. */
static stamp_row_fn s_stamp_row      = stamp_row_scalar;
static stamp_row_fn s_stamp_row_long = stamp_row_scalar;

/* Apply a run of n tiles starting at (abs_r, abs_c). The fog arrays are
 * chunk-blocked, so a run is contiguous only within a chunk -- split it into
 * per-chunk segments and SIMD each. */
static void stamp_run(uint8_t *refcnt, int abs_r, int abs_c, int n, int shift,
                      uint32_t clearmask, int delta)
{
    struct map_resolution res = s_res;
    size_t tiles_per_chunk = res.tile_w * res.tile_h;
    int chunk_r  = abs_r / res.tile_h;
    int tile_r   = abs_r % res.tile_h;
    int row_base = chunk_r * (res.chunk_w * tiles_per_chunk) + tile_r * res.tile_w;

    int c = abs_c, end = abs_c + n;
    while(c < end) {
        int chunk_c = c / res.tile_w;
        int seg_end = (chunk_c + 1) * res.tile_w;   /* first column of next chunk */
        if(seg_end > end) seg_end = end;
        int idx0 = row_base + chunk_c * tiles_per_chunk + (c % res.tile_w);
        int seg_n = seg_end - c;
        (seg_n >= 16 ? s_stamp_row_long : s_stamp_row)
            (refcnt + idx0, s_fog_state + idx0, seg_n, shift, clearmask, delta);
        c = seg_end;
    }
}

static void fog_stamp_disc(int faction_id, struct tile_desc origin,
                           const struct disc_stamp *stamp, int delta)
{
    struct map_resolution res = s_res;
    int total_rows = res.chunk_h * res.tile_h;
    int total_cols = res.chunk_w * res.tile_w;
    int abs_r0 = origin.chunk_r * res.tile_h + origin.tile_r;
    int abs_c0 = origin.chunk_c * res.tile_w + origin.tile_c;

    const int      shift = faction_id * 2;
    const uint32_t clearmask = ~(0x3u << shift);
    uint8_t *refcnt = s_vision_refcnts[faction_id];

    for(int k = 0; k < stamp->nrows; k++) {
        int abs_r = abs_r0 + stamp->dr[k];
        if(abs_r < 0 || abs_r >= total_rows)
            continue;
        int cmin = abs_c0 + stamp->dcmin[k];
        int cmax = abs_c0 + stamp->dcmax[k];
        if(cmin < 0) cmin = 0;
        if(cmax >= total_cols) cmax = total_cols - 1;
        if(cmax >= cmin)
            stamp_run(refcnt, abs_r, cmin, cmax - cmin + 1, shift, clearmask, delta);
    }
}

/* True iff no chunk spanned by the vision box can hold a tile that blocks LOS
 * from origin_height (conservative: chunk max <= origin_height+1 => no blocker).
 * When true the disc stamp needs no occlusion and is exactly correct. */
static bool box_is_open(struct tile_desc origin, float radius, int origin_height)
{
    struct map_resolution res = s_res;
    int zrad = (int)ceil(radius / Z_COORDS_PER_TILE) + 1;
    int xrad = (int)ceil(radius / X_COORDS_PER_TILE) + 1;
    int abs_r0 = origin.chunk_r * res.tile_h + origin.tile_r;
    int abs_c0 = origin.chunk_c * res.tile_w + origin.tile_c;

    int rmin = abs_r0 - zrad, rmax = abs_r0 + zrad;
    int cmin = abs_c0 - xrad, cmax = abs_c0 + xrad;
    if(rmin < 0) rmin = 0;
    if(cmin < 0) cmin = 0;
    if(rmax >= res.chunk_h * res.tile_h) rmax = res.chunk_h * res.tile_h - 1;
    if(cmax >= res.chunk_w * res.tile_w) cmax = res.chunk_w * res.tile_w - 1;

    int cr0 = rmin / res.tile_h, cr1 = rmax / res.tile_h;
    int cc0 = cmin / res.tile_w, cc1 = cmax / res.tile_w;
    for(int cr = cr0; cr <= cr1; cr++) {
    for(int cc = cc0; cc <= cc1; cc++) {
        if(s_chunk_maxh[cr * res.chunk_w + cc] > origin_height + 1)
            return false;
    }}
    return true;
}

/* Octant transforms for recursive shadowcasting (xx, xy, yx, yy per octant). */
static const int s_oct_mult[4][8] = {
    {1, 0, 0, -1, -1,  0,  0,  1},
    {0, 1, -1, 0,  0, -1,  1,  0},
    {0, 1,  1, 0,  0, -1, -1,  0},
    {1, 0,  0, 1, -1,  0,  0, -1},
};

static bool los_blocked_abs(int abs_r, int abs_c, int origin_height)
{
    struct map_resolution res = s_res;
    if(abs_r < 0 || abs_c < 0 || abs_r >= res.chunk_h * res.tile_h
                              || abs_c >= res.chunk_w * res.tile_w)
        return false;
    struct tile_desc td = {abs_r / res.tile_h, abs_c / res.tile_w,
                           abs_r % res.tile_h, abs_c % res.tile_w};
    struct tile *t;
    M_TileForDesc(s_map, td, &t);
    return (M_Tile_BaseHeight(t) - origin_height > 1);
}

static void los_mark(uint8_t *mask, int abs_r, int abs_c, int abs_r0, int abs_c0,
                     int rad, int box_w)
{
    struct map_resolution res = s_res;
    if(abs_r < 0 || abs_c < 0 || abs_r >= res.chunk_h * res.tile_h
                              || abs_c >= res.chunk_w * res.tile_w)
        return;
    mask[(abs_r - abs_r0 + rad) * box_w + (abs_c - abs_c0 + rad)] = 1;
}

/* Recursive shadowcasting (Bjorn Bergstrom's algorithm): light one octant,
 * recursing past each blocker to carve its shadow. O(visible tiles), no PQ. */
static void cast_light(uint8_t *mask, int abs_r0, int abs_c0, int maxdist, float radius2,
                       int origin_height, int rad, int box_w, int row,
                       float start, float end, int xx, int xy, int yx, int yy)
{
    if(start < end)
        return;
    for(int j = row; j <= maxdist; j++) {
        int dx = -j - 1, dy = -j;
        bool blocked = false;
        float new_start = 0.0f;
        while(dx <= 0) {
            dx += 1;
            int mc = abs_c0 + dx * xx + dy * xy;
            int mr = abs_r0 + dx * yx + dy * yy;
            float l_slope = (dx - 0.5f) / (dy + 0.5f);
            float r_slope = (dx + 0.5f) / (dy - 0.5f);
            if(start < r_slope)
                continue;
            else if(end > l_slope)
                break;

            bool blk = los_blocked_abs(mr, mc, origin_height);
            /* A blocking tile occludes but is not itself revealed (a unit
             * doesn't see one tile up onto higher ground). Only light
             * non-blockers within radius. */
            if(!blk && (float)(dx * dx + dy * dy) <= radius2)
                los_mark(mask, mr, mc, abs_r0, abs_c0, rad, box_w);

            if(blocked) {
                if(blk) {
                    new_start = r_slope;
                    continue;
                }else{
                    blocked = false;
                    start = new_start;
                }
            }else{
                if(blk && j < maxdist) {
                    blocked = true;
                    cast_light(mask, abs_r0, abs_c0, maxdist, radius2, origin_height,
                               rad, box_w, j + 1, start, l_slope, xx, xy, yx, yy);
                    new_start = r_slope;
                }
            }
        }
        if(blocked)
            break;
    }
}

/* LOS fast path for blocked boxes: shadowcast a visible mask, then apply it as
 * forward-scan runs through stamp_run. */
static void fog_los_stamp(int faction_id, struct tile_desc origin, int origin_height,
                          float radius, int delta)
{
    struct map_resolution res = s_res;
    int total_rows = res.chunk_h * res.tile_h;
    int total_cols = res.chunk_w * res.tile_w;
    int abs_r0 = origin.chunk_r * res.tile_h + origin.tile_r;
    int abs_c0 = origin.chunk_c * res.tile_w + origin.tile_c;

    int   rad     = (int)ceil(radius / X_COORDS_PER_TILE) + 1;
    float radius2 = (radius / X_COORDS_PER_TILE) * (radius / X_COORDS_PER_TILE);
    int   box_w   = 2 * rad + 1;
    size_t box_n  = (size_t)box_w * box_w;

    if(box_n > s_los_cap) {
        s_los_cap = box_n;
        s_los_vis = PF_REALLOC(s_los_vis, box_n);
        assert(s_los_vis);
    }
    memset(s_los_vis, 0, box_n);
    s_los_vis[rad * box_w + rad] = 1;   /* origin always visible */

    for(int oct = 0; oct < 8; oct++) {
        cast_light(s_los_vis, abs_r0, abs_c0, rad, radius2, origin_height, rad, box_w,
                   1, 1.0f, 0.0f,
                   s_oct_mult[0][oct], s_oct_mult[1][oct],
                   s_oct_mult[2][oct], s_oct_mult[3][oct]);
    }

    const int      shift = faction_id * 2;
    const uint32_t clearmask = ~(0x3u << shift);
    uint8_t *refcnt = s_vision_refcnts[faction_id];

    for(int rr = 0; rr < box_w; rr++) {
        int abs_r = abs_r0 - rad + rr;
        if(abs_r < 0 || abs_r >= total_rows)
            continue;
        uint8_t *mrow = s_los_vis + (size_t)rr * box_w;
        int cc = 0;
        while(cc < box_w) {
            if(!mrow[cc]) { cc++; continue; }
            int run0 = cc;
            while(cc < box_w && mrow[cc]) cc++;
            int start = abs_c0 - rad + run0;
            int cnt   = cc - run0;
            if(start < 0) { cnt += start; start = 0; }
            if(start + cnt > total_cols) cnt = total_cols - start;
            if(cnt > 0)
                stamp_run(refcnt, abs_r, start, cnt, shift, clearmask, delta);
        }
    }
}

/* Drain the batched vision-update queue in one pipelined pass: resolve each
 * origin once, then the disc stamp (open box) or the LOS-aware stamp. */
static void fog_flush_pending(void)
{
    if(s_nupdates == 0)
        return;
    PERF_ENTER();

    for(size_t i = 0; i < s_nupdates; i++) {
        struct vis_update *u = &s_updates[i];
        if(u->radius == 0.0f)
            continue;

        struct tile_desc origin;
        if(!M_Tile_DescForPoint2D(s_res, s_map_pos, u->pos, &origin))
            continue;

        struct tile *tile;
        M_TileForDesc(s_map, origin, &tile);
        int origin_height = M_Tile_BaseHeight(tile);

        if(box_is_open(origin, u->radius, origin_height))
            fog_stamp_disc(u->faction_id, origin, get_stamp(u->radius), u->delta);
        else
            fog_los_stamp(u->faction_id, origin, origin_height, u->radius, u->delta);
    }
    s_nupdates = 0;
    PERF_RETURN_VOID();
}

static bool fog_obj_matches(uint32_t *state, uint16_t fac_mask, const struct obb *obj, 
                            enum fog_state *states, size_t nstates)
{
    assert(Sched_UsingBigStack());

    vec3_t pos = s_map_pos;
    struct map_resolution res = s_res;

    uint32_t facstate_mask = 0;
    for(int i = 0; fac_mask; fac_mask >>= 1, i++) {
        if(fac_mask & 0x1) {
            facstate_mask |= (0x3 << (i * 2));
        }
    }

    struct tile_desc tds[4096];
    size_t ntiles = M_Tile_AllUnderObj(pos, res, obj, tds, ARR_SIZE(tds));

    for(int i = 0; i < ntiles; i++) {

        int idx = td_index(tds[i]);
        uint32_t fac_state = state[idx] & facstate_mask;

        for(int j = 0; j < nstates; j++) {
            if(fog_any_matches(fac_state, states[j]))
                return true;
        }
    }
    return false;
}

static bool fog_circle_matches(uint16_t fac_mask, vec2_t xz_center, float radius, 
                               enum fog_state *states, size_t nstates)
{
    assert(Sched_UsingBigStack());

    vec3_t pos = s_map_pos;
    struct map_resolution res = s_res;

    uint32_t facstate_mask = 0;
    for(int i = 0; fac_mask; fac_mask >>= 1, i++) {
        if(fac_mask & 0x1) {
            facstate_mask |= (0x3 << (i * 2));
        }
    }

    struct tile_desc tds[4096];
    size_t ntiles = M_Tile_AllUnderCircle(res, xz_center, radius, s_map_pos, tds, ARR_SIZE(tds));

    for(int i = 0; i < ntiles; i++) {

        int idx = td_index(tds[i]);
        uint32_t fac_state = s_fog_state[idx] & facstate_mask;

        for(int j = 0; j < nstates; j++) {
            if(fog_any_matches(fac_state, states[j]))
                return true;
        }
    }
    return false;
}

static bool fog_rect_matches(uint16_t fac_mask, vec2_t xz_center, float halfx, float halfz, 
                             enum fog_state *states, size_t nstates)
{
    assert(Sched_UsingBigStack());

    vec3_t pos = s_map_pos;
    struct map_resolution res = s_res;

    uint32_t facstate_mask = 0;
    for(int i = 0; fac_mask; fac_mask >>= 1, i++) {
        if(fac_mask & 0x1) {
            facstate_mask |= (0x3 << (i * 2));
        }
    }

    struct tile_desc tds[4096];
    size_t ntiles = M_Tile_AllUnderAABB(res, xz_center, halfx, halfz, 
        s_map_pos, tds, ARR_SIZE(tds));

    for(int i = 0; i < ntiles; i++) {

        int idx = td_index(tds[i]);
        uint32_t fac_state = s_fog_state[idx] & facstate_mask;

        for(int j = 0; j < nstates; j++) {
            if(fog_any_matches(fac_state, states[j]))
                return true;
        }
    }
    return false;
}

static void on_render_3d(void *user, void *event)
{
    const struct camera *cam = G_GetActiveCamera();

    struct sval setting;
    ss_e status;
    (void)status;

    status = Settings_Get("pf.debug.show_faction_vision", &setting);
    assert(status == SS_OKAY);

    if(setting.as_int == -1)
        return;

    M_RenderChunkVisibility(s_map, G_GetActiveCamera(), setting.as_int);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Fog_Init(const struct map *map)
{
    struct map_resolution res;
    M_GetResolution(map, &res);
    const size_t ntiles = res.chunk_w * res.chunk_h * res.tile_w * res.tile_h;

    s_fog_state = PF_CALLOC(sizeof(s_fog_state[0]), ntiles);
    if(!s_fog_state)
        goto fail;

    for(int i = 0; i < MAX_FACTIONS; i++) {
        s_vision_refcnts[i] = PF_CALLOC(sizeof(s_vision_refcnts[0]), ntiles);
        if(!s_vision_refcnts[i])
            goto fail;
    }

    s_explored_cache = kh_init(uid);
    if(!s_explored_cache)
        goto fail;

    s_map = map;
    s_res = res;
    s_map_pos = M_GetPos(map);

    s_stamp_row      = simd_avx2_supported()   ? stamp_row_avx2   : stamp_row_scalar;
    s_stamp_row_long = simd_avx512_supported() ? stamp_row_avx512 : s_stamp_row;

    /* Per-chunk max base height for the open-box fast-path test. Computed from
     * the (static) terrain; kept current by G_Fog_OnTileUpdated on edits. */
    s_chunk_maxh = PF_CALLOC(sizeof(s_chunk_maxh[0]), res.chunk_w * res.chunk_h);
    if(!s_chunk_maxh)
        goto fail;
    for(int cr = 0; cr < res.chunk_h; cr++) {
    for(int cc = 0; cc < res.chunk_w; cc++) {
        int maxh = INT_MIN;
        for(int tr = 0; tr < res.tile_h; tr++) {
        for(int tc = 0; tc < res.tile_w; tc++) {
            struct tile *tile;
            M_TileForDesc(map, (struct tile_desc){cr, cc, tr, tc}, &tile);
            int h = M_Tile_BaseHeight(tile);
            if(h > maxh) maxh = h;
        }}
        s_chunk_maxh[cr * res.chunk_w + cc] = maxh;
    }}

    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);
    return true;

fail:
    kh_destroy(uid, s_explored_cache);
    PF_FREE(s_fog_state);
    PF_FREE(s_chunk_maxh);
    for(int i = 0; i < MAX_FACTIONS; i++) {
        PF_FREE(s_vision_refcnts[i]);
    }
    return false;
}

void G_Fog_Shutdown(void)
{
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);

    kh_destroy(uid, s_explored_cache);
    PF_FREE(s_fog_state);

    for(int i = 0; i < MAX_FACTIONS; i++) {
        PF_FREE(s_vision_refcnts[i]);
    }
    memset(s_vision_refcnts, 0, sizeof(s_vision_refcnts));

    PF_FREE(s_updates);
    PF_FREE(s_chunk_maxh);
    PF_FREE(s_los_vis);
    s_updates = NULL;
    s_chunk_maxh = NULL;
    s_los_vis = NULL;
    s_nupdates = s_updates_cap = s_los_cap = 0;
    s_nstamps = 0;
    s_map = NULL;
}

void G_Fog_AddVision(vec2_t xz_pos, int faction_id, float radius)
{
    enqueue_update(faction_id, xz_pos, radius, +1);
}

void G_Fog_RemoveVision(vec2_t xz_pos, int faction_id, float radius)
{
    enqueue_update(faction_id, xz_pos, radius, -1);
}

/* Flush all pending vision updates immediately. Must be called on the main
 * thread (mutates shared fog state). Called at the movement-apply boundary and
 * before the per-frame vision-state upload. */
void G_Fog_FlushUpdates(void)
{
    fog_flush_pending();
}

/* Keep the per-chunk max-height cache current after a runtime terrain edit so
 * box_is_open stays correct (an under-estimate would wrongly skip LOS). */
void G_Fog_OnTileUpdated(int chunk_r, int chunk_c)
{
    if(!s_chunk_maxh)
        return;
    int maxh = INT_MIN;
    for(int tr = 0; tr < s_res.tile_h; tr++) {
    for(int tc = 0; tc < s_res.tile_w; tc++) {
        struct tile *tile;
        M_TileForDesc(s_map, (struct tile_desc){chunk_r, chunk_c, tr, tc}, &tile);
        int h = M_Tile_BaseHeight(tile);
        if(h > maxh) maxh = h;
    }}
    s_chunk_maxh[chunk_r * s_res.chunk_w + chunk_c] = maxh;
}

void G_Fog_ExploreCircle(vec2_t xz_pos, int faction_id, float radius)
{
    assert(Sched_UsingBigStack());

    struct map_resolution res = s_res;

    struct tile_desc tds[4096];
    size_t ntiles = M_Tile_AllUnderCircle(res, xz_pos, radius, s_map_pos, tds, ARR_SIZE(tds));

    for(int i = 0; i < ntiles; i++) {
        update_tile(faction_id, tds[i], +1);
        update_tile(faction_id, tds[i], -1);
    }
}

void G_Fog_ExploreRectangle(vec2_t xz_pos, int faction_id, float halfx, float halfz)
{
    assert(Sched_UsingBigStack());

    struct map_resolution res = s_res;

    struct tile_desc tds[4096];
    size_t ntiles = M_Tile_AllUnderAABB(res, xz_pos, halfx, halfz, s_map_pos, tds, ARR_SIZE(tds));

    for(int i = 0; i < ntiles; i++) {
        update_tile(faction_id, tds[i], +1);
        update_tile(faction_id, tds[i], -1);
    }
}

bool G_Fog_Visible(int faction_id, vec2_t xz_pos)
{
    struct map_resolution res = s_res;

    struct tile_desc td;
    if(!M_Tile_DescForPoint2D(res, s_map_pos, xz_pos, &td))
        return false;

    return (FAC_STATE(s_fog_state[td_index(td)], faction_id) == STATE_VISIBLE);
}

bool G_Fog_PlayerVisible(vec2_t xz_pos)
{
    if(!s_enabled)
        return true;

    struct map_resolution res = s_res;

    struct tile_desc td;
    if(!M_Tile_DescForPoint2D(res, s_map_pos, xz_pos, &td))
        return false;

    bool controllable[MAX_FACTIONS];
    uint16_t facs = G_GetFactions(NULL, NULL, controllable);
    uint32_t fs = s_fog_state[td_index(td)];

    for(int i = 0; facs; facs >>= 1, i++) {
        if(!(facs & 0x1) || !controllable[i])
            continue;
        if(FAC_STATE(fs, i) == STATE_VISIBLE)
            return true;
    }
    return false;
}

bool G_Fog_Explored(int faction_id, vec2_t xz_pos)
{
    struct map_resolution res = s_res;

    struct tile_desc td;
    if(!M_Tile_DescForPoint2D(res, s_map_pos, xz_pos, &td))
        return false;

    return (FAC_STATE(s_fog_state[td_index(td)], faction_id) != STATE_UNEXPLORED);
}

bool G_Fog_PlayerExplored(vec2_t xz_pos)
{
    if(!s_enabled)
        return true;

    struct map_resolution res = s_res;

    struct tile_desc td;
    if(!M_Tile_DescForPoint2D(res, s_map_pos, xz_pos, &td))
        return false;

    bool controllable[MAX_FACTIONS];
    uint16_t facs = G_GetFactions(NULL, NULL, controllable);
    uint32_t fs = s_fog_state[td_index(td)];

    for(int i = 0; facs; facs >>= 1, i++) {
        if(!(facs & 0x1) || !controllable[i])
            continue;
        if(FAC_STATE(fs, i) != STATE_UNEXPLORED)
            return true;
    }
    return false;
}

void G_Fog_RenderChunkVisibility(int faction_id, int chunk_r, int chunk_c, mat4x4_t *model)
{
    struct map_resolution res = s_res;

    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    STALLOC(vec2_t, corners_buff, 4 * res.tile_w * res.tile_h);
    STALLOC(vec3_t, colors_buff, res.tile_w * res.tile_h);

    vec2_t *corners_base = corners_buff;
    vec3_t *colors_base = colors_buff; 

    for(int r = 0; r < res.tile_h; r++) {
    for(int c = 0; c < res.tile_w; c++) {

        float square_x_len = (1.0f / res.tile_w) * chunk_x_dim;
        float square_z_len = (1.0f / res.tile_h) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / res.tile_w) * chunk_x_dim, -chunk_x_dim, chunk_x_dim);
        float square_z = CLAMP( (((float)r) / res.tile_h) * chunk_z_dim, -chunk_z_dim, chunk_z_dim);

        *corners_base++ = (vec2_t){square_x, square_z};
        *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
        *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

        struct tile_desc curr = (struct tile_desc){chunk_r, chunk_c, r, c};
        enum fog_state state = FAC_STATE(s_fog_state[td_index(curr)], faction_id);
        *colors_base++ = state == STATE_UNEXPLORED ? (vec3_t){0.0f, 0.0f, 0.0f}
                       : state == STATE_IN_FOG     ? (vec3_t){1.0f, 1.0f, 0.0f}
                       : state == STATE_VISIBLE    ? (vec3_t){0.0f, 1.0f, 0.0f}
                                                   : (assert(0), (vec3_t){0});
    }}

    assert(colors_base == colors_buff + ARR_SIZE(colors_buff));
    assert(corners_base == corners_buff + ARR_SIZE(corners_buff));

    size_t count = res.tile_w * res.tile_h;
    bool on_water_surface = true;
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawMapOverlayQuads,
        .nargs = 6,
        .args = {
            R_PushArg(corners_buff, sizeof(corners_buff)),
            R_PushArg(colors_buff, sizeof(colors_buff)),
            R_PushArg(&count, sizeof(count)),
            R_PushArg(model, sizeof(*model)),
            R_PushArg(&on_water_surface, sizeof(bool)),
            (void*)G_GetPrevTickMap(),
        },
    });

    STFREE(corners_buff);
    STFREE(colors_buff);
}

void G_Fog_UpdateVisionState(void)
{
    PERF_ENTER();
    fog_flush_pending();

    bool controllable[MAX_FACTIONS];
    uint16_t facs = G_GetFactions(NULL, NULL, controllable);

    uint32_t player_mask = 0;
    for(int i = 0; facs; facs >>= 1, i++) {
        if((facs & 0x1) && controllable[i])
            player_mask |= (0x3 << (i * 2));
    }

    struct map_resolution res = s_res;

    size_t size = res.chunk_h * res.tile_h * res.chunk_w * res.tile_w;
    unsigned char *visbuff = stalloc(&G_GetSimWS()->args, size);

    if(!s_enabled) {
        memset(visbuff, STATE_VISIBLE, size);
        goto submit;
    }

    for(int cr = 0; cr < res.chunk_h; cr++) {
    for(int cc = 0; cc < res.chunk_w; cc++) {

        for(int tr = 0; tr < res.tile_h; tr++) {
        for(int tc = 0; tc < res.tile_w; tc++) {

            struct tile_desc td = (struct tile_desc){cr, cc, tr, tc};
            int idx = td_index(td);
            uint32_t player_state = s_fog_state[idx] & player_mask;

            if(!player_state)
                visbuff[idx] = STATE_UNEXPLORED;
            else if(fog_any_matches(player_state, STATE_VISIBLE))
                visbuff[idx] = STATE_VISIBLE;
            else
                visbuff[idx] = STATE_IN_FOG;
        }}
    }}

submit:
    R_PushCmd((struct rcmd){
        .func = R_GL_MapUpdateFog,
        .nargs = 2,
        .args = {
            visbuff,
            R_PushArg(&size, sizeof(size)),
        },
    });
    PERF_RETURN_VOID();
}

bool G_Fog_ObjExplored(uint16_t fac_mask, uint32_t uid, const struct obb *obb)
{
    if(!s_enabled)
        return true;

    khiter_t k = kh_get(uid, s_explored_cache, uid);
    if(k != kh_end(s_explored_cache))
        return true;

    enum fog_state states[] = {STATE_IN_FOG, STATE_VISIBLE};
    bool result = fog_obj_matches(s_fog_state, fac_mask, obb, states, ARR_SIZE(states));

    if(result) {
        int status;
        k = kh_put(uid, s_explored_cache, uid, &status);
        assert(status != -1 && status != 0);
    }
    return result;
}

bool G_Fog_ObjVisible(uint16_t fac_mask, const struct obb *obb)
{
    if(!s_enabled)
        return true;

    enum fog_state states[] = {STATE_VISIBLE};
    return fog_obj_matches(s_fog_state, fac_mask, obb, states, ARR_SIZE(states));
}

bool G_Fog_CircleExplored(uint16_t fac_mask, vec2_t xz_pos, float radius)
{
    if(!s_enabled)
        return true;

    enum fog_state states[] = {STATE_IN_FOG, STATE_VISIBLE};
    return fog_circle_matches(fac_mask, xz_pos, radius, states, ARR_SIZE(states));
}

bool G_Fog_RectExplored(uint16_t fac_mask, vec2_t xz_pos, float halfx, float halfz)
{
    if(!s_enabled)
        return true;

    enum fog_state states[] = {STATE_IN_FOG, STATE_VISIBLE};
    return fog_rect_matches(fac_mask, xz_pos, halfx, halfz, states, ARR_SIZE(states));
}

bool G_Fog_NearVisibleWater(uint16_t fac_mask, vec2_t xz_pos, float radius)
{
    vec3_t pos = s_map_pos;
    struct map_resolution res = s_res;

    uint32_t facstate_mask = 0;
    for(int i = 0; fac_mask; fac_mask >>= 1, i++) {
        if(fac_mask & 0x1) {
            facstate_mask |= (0x3 << (i * 2));
        }
    }

    struct tile_desc tds[4096];
    size_t ntiles = M_Tile_AllUnderCircle(res, xz_pos, radius, s_map_pos, tds, ARR_SIZE(tds));

    for(int i = 0; i < ntiles; i++) {

        struct tile *tile = NULL;
        M_TileForDesc(s_map, tds[i], &tile);
        assert(tile);

        if(M_Tile_BaseHeight(tile) >= 0)
            continue;

        if(!s_enabled)
            return true;

        int idx = td_index(tds[i]);
        uint32_t fac_state = s_fog_state[idx] & facstate_mask;

        enum fog_state states[] = {
            STATE_IN_FOG,
            STATE_VISIBLE
        };
        for(int j = 0; j < ARR_SIZE(states); j++) {
            if(fog_any_matches(fac_state, states[j]))
                return true;
        }
    }
    return false;
}

void G_Fog_UpdateVisionRange(vec2_t xz_pos, int faction_id, float oldr, float newr)
{
    G_Fog_RemoveVision(xz_pos, faction_id, oldr);
    G_Fog_AddVision(xz_pos, faction_id, newr);
}

void G_Fog_ClearExploredCache(void)
{
    kh_clear(uid, s_explored_cache);
}

bool G_Fog_SaveState(struct SDL_RWops *stream)
{
    fog_flush_pending();

    struct map_resolution res = s_res;
    const size_t ntiles = res.chunk_w * res.chunk_h * res.tile_w * res.tile_h;

    struct attr enabled = (struct attr){
        .type = TYPE_BOOL, 
        .val.as_bool = s_enabled
    };
    CHK_TRUE_RET(Attr_Write(stream, &enabled, "enabled"));

    struct attr ntiles_attr = (struct attr){
        .type = TYPE_INT,
        .val.as_int = ntiles
    };
    CHK_TRUE_RET(Attr_Write(stream, &ntiles_attr, "num_tiles"));

    for(int i = 0; i < ntiles; i++) {

        uint32_t fs = s_fog_state[i];
        for(int j = 0; j < MAX_FACTIONS; j++) {
            enum fog_state curr = (fs >> (j * 2)) & 0x3;
            if(curr == STATE_VISIBLE) {
                curr = STATE_IN_FOG;
            }
            fs = fs & ~(0x3 << (j * 2));
            fs = fs | (curr << (j * 2));
        }

        struct attr tilestate = (struct attr){
            .type = TYPE_INT,
            .val.as_int = fs
        };
        CHK_TRUE_RET(Attr_Write(stream, &tilestate, "tilestate"));
    }

    return true;
}

bool G_Fog_LoadState(struct SDL_RWops *stream)
{
    /* Discard any updates enqueued before the load; the stream fully defines
     * the fog state and must not be overwritten by stale pending floods. */
    s_nupdates = 0;

    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_BOOL);
    s_enabled = attr.val.as_bool;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t ntiles = attr.val.as_int;

    for(int i = 0; i < ntiles; i++) {
    
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        s_fog_state[i] = attr.val.as_int;
    }

    return true;
}

void G_Fog_ExploreMap(int faction_id)
{
    struct map_resolution res = s_res;

    for(int chunk_c = 0; chunk_c < res.chunk_w; chunk_c++) {
    for(int chunk_r = 0; chunk_r < res.chunk_h; chunk_r++) {

        for(int tile_c = 0; tile_c < res.tile_w; tile_c++) {
        for(int tile_r = 0; tile_r < res.tile_h; tile_r++) {

            struct tile_desc td = (struct tile_desc) {
                chunk_r, chunk_c,
                tile_r, tile_c
            };
            uint32_t ts = s_fog_state[td_index(td)];
            if(((ts >> (faction_id * 2)) & 0x3) == STATE_UNEXPLORED) {
                ts &= ~(0x3 << (faction_id * 2));
                ts |= (STATE_IN_FOG << (faction_id * 2));
            }
            s_fog_state[td_index(td)] = ts;
        }}
    }}
}

uint32_t *G_Fog_CopyState(void)
{
    struct map_resolution res = s_res;
    const size_t ntiles = res.chunk_w * res.chunk_h * res.tile_w * res.tile_h;

    uint32_t *ret = PF_MALLOC(sizeof(s_fog_state[0]) * ntiles);
    if(!ret)
        return NULL;

    memcpy(ret, s_fog_state, sizeof(s_fog_state[0]) * ntiles);
    return ret;
}

bool G_Fog_ObjVisibleFrom(uint32_t *state, bool enabled, uint16_t fac_mask, const struct obb *obb)
{
    if(!enabled)
        return true;

    enum fog_state states[] = {STATE_VISIBLE};
    return fog_obj_matches(state, fac_mask, obb, states, ARR_SIZE(states));
}

void G_Fog_Enable(void)
{
    s_enabled = true;
}

void G_Fog_Disable(void)
{
    s_enabled = false;
}

bool G_Fog_Enabled(void)
{
    return s_enabled;
}

