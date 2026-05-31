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
 */

/* ==============================================================================
 *  bitmap_grid.h
 * ==============================================================================
 *
 *  A cache-friendly, SIMD-accelerated 2D spatial index for point entities.
 *
 *  A flat 2D grid of fixed-size cells over a structure-of-arrays element pool,
 *  with a two-level occupancy bitmap pyramid for empty-region pruning, three
 *  runtime SIMD tiers, and a deferred-packing strategy.
 *
 *  ----------------------------------------------------------------------------
 *  Data layout
 *  ----------------------------------------------------------------------------
 *
 *  The world is partitioned by a 2D grid of fixed-size cells (16 world units
 *  per side). Each cell holds a small linked-list head + a "packed range" of
 *  same-cell elements in a shared pool. Above the grid sits a two-level
 *  occupancy bitmap pyramid: one bit per cell at the fine level, one bit per
 *  8x8 region at the coarse level. Range queries scan the bitmap rows with
 *  64-bit ANDs and skip empty regions early.
 *
 *  The element pool is stored as four parallel arrays (struct-of-arrays):
 *
 *      xs[]      : scaled int32 x coordinates
 *      ys[]      : scaled int32 y coordinates
 *      nexts[]   : linked-list link (used for the per-cell overflow chain
 *                  and for the free-list; ignored within a cell's "packed"
 *                  range, since packed elements are contiguous)
 *      records[] : caller-supplied payload (typically a uint32 entity UID)
 *
 *  SoA lets the SIMD inner loops load xs and ys directly as 256-bit / 512-bit
 *  vectors with no per-iteration shuffles — that's the single biggest reason
 *  this implementation can keep up with explicit AVX-512 intrinsics.
 *
 *  ----------------------------------------------------------------------------
 *  Insert / delete / cleanup
 *  ----------------------------------------------------------------------------
 *
 *  Per cell, the element population is split into two zones:
 *
 *      - PACKED range: a contiguous run in the pool indexed by
 *        cell.packed_start .. cell.packed_start + cell.packed_size.
 *        Same-cell elements live next to each other in memory; perfect for
 *        the SIMD scan loops.
 *
 *      - OVERFLOW chain: an intrusive linked list anchored at
 *        cell.overflow_head, threaded through nexts[]. New inserts always go
 *        here. Deletes from the chain unlink and push the freed slot onto a
 *        free list (also threaded through nexts[]).
 *
 *  Deletes inside the packed range swap-with-last to keep the run contiguous
 *  and push the displaced tail slot onto the free list.
 *
 *  When the caller invokes bg_<name>_cleanup() (intended to fire once per
 *  frame at a quiet point), the pool is rebuilt: every cell's overflow chain
 *  is merged back into its packed range, and stale coarse-bitmap bits are
 *  cleared (deletes only clear fine bits; the coarse bit is sticky between
 *  cleanups so range queries may have to visit a few extra empty cells, but
 *  correctness is unaffected).
 *
 *  ----------------------------------------------------------------------------
 *  SIMD dispatch
 *  ----------------------------------------------------------------------------
 *
 *  At init time, bg_<name>_init() detects the CPU's wide-SIMD support and
 *  installs one of three implementations of the per-cell scan into a
 *  function-pointer table:
 *
 *      tier        elements per iteration       used when
 *      -----------------------------------------------------------------
 *      scalar      1 (still branchless,         CPU lacks AVX2
 *                  compress-store via stride)
 *      AVX2        8 (rect) / 4 (circle)        CPU has AVX2, no AVX-512
 *      AVX-512     16 / 16                      CPU has AVX-512F+BW+DQ+VL
 *
 *  A second function-pointer table holds the "long-scan" variants that the
 *  whole-pool fast path (used for wide queries) calls. The split exists
 *  because AVX-512's 16-lane batches and clock-throttling overhead favour
 *  long sustained scans but slow down short per-cell scans. On AVX-512
 *  hardware, the cell-loop still uses AVX2 while the whole-pool path uses
 *  AVX-512.
 *
 *  Portability: the binary contains all three implementations regardless of
 *  the build host's CPU. AVX2 and AVX-512 functions carry per-function
 *  target attributes (GCC/Clang) so the wider instructions are emitted only
 *  inside those functions; the rest of the TU stays on the baseline ISA.
 *  See ../public/simd.h for the toolchain abstraction.
 *
 *  ----------------------------------------------------------------------------
 *  Coordinate scaling
 *  ----------------------------------------------------------------------------
 *
 *  Float positions are converted to scaled int32 at the API boundary
 *  (multiply by 256). Internal arithmetic is integer; this kills the
 *  sqrt + pow on the per-element distance test. Map size is bounded by the
 *  int32 range — at BG_LOG2_SCALE = 8 that's roughly +-8 million world units
 *  before squared-distance comparisons start losing accuracy in int64. We
 *  use int64 squared distance throughout.
 *
 *  ----------------------------------------------------------------------------
 *  Macro API
 *  ----------------------------------------------------------------------------
 *
 *  Instantiation works exactly like the engine's other generic containers:
 *
 *      BITMAP_GRID_TYPE(ent, uint32_t)
 *      BITMAP_GRID_PROTOTYPES(static, ent, uint32_t)
 *      BITMAP_GRID_IMPL(static, ent, uint32_t)
 *
 *  ... and then bg_ent_t, bg_ent_init, bg_ent_insert, etc. exist. The
 *  generated code is self-contained and can be repeated for multiple
 *  (name, type) pairs.
 */

#ifndef BITMAP_GRID_H
#define BITMAP_GRID_H

#include "simd.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 *  Compile-time tuning
 * ============================================================================ 
 */

/* Coordinate scale: float -> int multiplier (must be a power of two so that
 * the per-cell halving and integer cell-index arithmetic stay exact). With
 * S = 256 the working range is roughly +-8 million world units before
 * squared-distance comparisons start needing more than int64.
 */
#define BG_LOG2_SCALE       8
#define BG_SCALE            (1 << BG_LOG2_SCALE)

/* Finest cell size, in world units. Power of two so that the int->cell
 * conversion is a single right-shift. With 16 wu cells, a 4x4-chunk map
 * (1024 wu) becomes a 64x64 fine grid; a 16x16-chunk map (4096 wu) becomes
 * a 256x256 fine grid.
 */
#define BG_CELL_LOG2_WU     4

/* Shift count to convert a scaled int32 coordinate (already multiplied by
 * BG_SCALE) into a cell column or row index.
 */
#define BG_CELL_LOG2_INT    (BG_LOG2_SCALE + BG_CELL_LOG2_WU)

/* Coarse bitmap is the fine grid downsampled by 8x in each dimension. So
 * each coarse bit represents an 8x8 block of fine cells = 128 wu on a side.
 */
#define BG_COARSE_LOG2      3

/* Float -> scaled int32 conversion. lrintf compiles to a single
 * cvttss2si-with-rounding on x86_64.
 */
#define BG_SCALE_F(x)       ((int32_t)lrintf((x) * (float)BG_SCALE))

/* Integer ceiling division.
 */
#define _BG_DIV_CEIL(a, b)  (((a) + (b) - 1) / (b))

/* Threshold for the whole-pool "wide query" fast path. When the cell extent
 * a query covers reaches 3/4 of the grid, skip the bitmap iteration and
 * scan the entire pool linearly. The crossover depends on cell-vs-element
 * density; 75% is a conservative pick that wins clearly on r=full-map
 * queries without hurting medium-coverage queries.
 */
#define _BG_WIDE_QUERY_NUM    3
#define _BG_WIDE_QUERY_DEN    4

/* ============================================================================
 *  Generic helpers
 * ============================================================================ 
 */

/* Mask out the bits of `w` below position `bit_lo` (i.e. keep only bits at
 * position >= bit_lo & 63). Used to clip the first uint64 of a bitmap range
 * scan when the range starts mid-word.
 */
static inline uint64_t bg_keep_bits_at_or_above(uint64_t w, int bit_lo)
{
    return w & ~((1ull << (bit_lo & 63)) - 1ull);
}

/* Mask out the bits of `w` at or above position `bit_hi` (i.e. keep only
 * bits below bit_hi & 63). Used to clip the last uint64 of a bitmap range
 * scan when the range ends mid-word. When bit_hi is a multiple of 64, all
 * 64 bits of `w` are below the cap and we keep them all.
 */
static inline uint64_t bg_keep_bits_below(uint64_t w, int bit_hi)
{
    int rem = bit_hi & 63;
    return rem ? (w & ((1ull << rem) - 1ull)) : w;
}

/* ============================================================================
 *  BITMAP_GRID_TYPE - struct definitions for a (name, type) instantiation
 * ============================================================================ 
 */

#define BITMAP_GRID_TYPE(name, type)                                                              \
                                                                                               \
    /* Per-cell metadata. Three int32s = 12 bytes per cell. For a 256x256                  */  \
    /* finest grid (4096 wu map) the cells[] array is 768 KB, sitting in L2.               */  \
    typedef struct bg_##name##_cell_s {                                                        \
        /* Start of this cell's packed elements in the pool, or -1 if no                   */  \
        /* packed elements yet (this cell only has overflow or is empty).                  */  \
        int32_t  packed_start;                                                                 \
        /* Number of live packed elements in this cell. Always == 0 before                 */  \
        /* the first cleanup() since inserts only go to the overflow chain.                */  \
        int32_t  packed_size;                                                                  \
        /* Head of the overflow linked list (slot index), or -1 if no                      */  \
        /* overflow elements. Drained into packed_* by cleanup().                          */  \
        int32_t  overflow_head;                                                                \
    } bg_##name##_cell_t;                                                                      \
                                                                                               \
    /* The bitmap-grid struct. Holds the SoA element pool, the cell metadata               */  \
    /* table, the two-level bitmap pyramid, and the float bounds we were                   */  \
    /* initialised with.                                                                   */  \
    typedef struct bg_##name##_s {                                                             \
        /* Finest-grid dimensions (number of cells). Computed from xmin/xmax               */  \
        /* in bg_<name>_init.                                                              */  \
        int       grid_w, grid_h;                                                              \
        /* Coarse-bitmap dimensions = ceil(grid_w/8) by ceil(grid_h/8).                    */  \
        int       coarse_w, coarse_h;                                                          \
        /* Scaled-int coordinates of the map origin. Cell index for an                     */  \
        /* element at (ix, iy) is ((iy - origin_y) >> BG_CELL_LOG2_INT,                    */  \
        /*                         (ix - origin_x) >> BG_CELL_LOG2_INT).                   */  \
        int32_t   origin_x, origin_y;                                                          \
        /* Cell metadata table: grid_w * grid_h entries.                                   */  \
        bg_##name##_cell_t *cells;                                                             \
        /* SoA element pool: four parallel arrays sharing slot indexing.                   */  \
        /* nexts[] is used by overflow chains and by the free list; the                    */  \
        /* values for slots inside packed runs are ignored.                                */  \
        int32_t  *xs;                                                                          \
        int32_t  *ys;                                                                          \
        int32_t  *nexts;                                                                       \
        type     *records;                                                                     \
        /* Pool size / capacity counters. elts_size is one past the highest                */  \
        /* live slot index (free slots may sit in [0, elts_size)).                         */  \
        int32_t   elts_cap, elts_size;                                                         \
        /* Head of the intrusive free list, or -1 if empty.                                */  \
        int32_t   first_free;                                                                  \
        /* Bitmap pyramid. Each row is bm_*_row_u64 uint64 words. The fine                 */  \
        /* level has one bit per grid cell; a coarse bit is 1 iff any of the               */  \
        /* 64 fine bits below it is 1 (cleared lazily by cleanup()).                       */  \
        uint64_t *bm_fine;                                                                     \
        uint64_t *bm_coarse;                                                                   \
        int       bm_fine_row_u64;                                                             \
        int       bm_coarse_row_u64;                                                           \
        /* Total live record count, exposed for callers.                                   */  \
        size_t    nrecs;                                                                       \
        /* Float bounds retained for inspection and copy semantics.                        */  \
        float     xmin, xmax, ymin, ymax;                                                      \
        /* Caller-supplied equality test on payloads (delete uses this to                  */  \
        /* disambiguate when multiple records share a position).                           */  \
        bool    (*comparator)(const type *a, const type *b);                                   \
        /* True when there are deletes-since-last-cleanup or unmerged                      */  \
        /* overflow. The wide-query fast path requires dirty == false so                   */  \
        /* that [0, nrecs) covers exactly the live elements.                               */  \
        bool      dirty;                                                                       \
    } bg_##name##_t;

#define bg(name)       bg_##name##_t
#define bg_cell(name)  bg_##name##_cell_t

/* ============================================================================
 *  BITMAP_GRID_PROTOTYPES - forward declarations for the (name, type) API
 * ============================================================================ 
 */

#define BITMAP_GRID_PROTOTYPES(scope, name, type)                                                 \
                                                                                               \
    scope bool   bg_##name##_init(bg(name) *bg,                                                \
                                  float xmin, float xmax,                                      \
                                  float ymin, float ymax,                                      \
                                  bool (*comparator)(const type*, const type*));               \
    scope void   bg_##name##_destroy(bg(name) *bg);                                            \
    scope void   bg_##name##_clear(bg(name) *bg);                                              \
    scope bool   bg_##name##_reserve(bg(name) *bg, size_t hint);                               \
    scope bool   bg_##name##_copy(const bg(name) *from, bg(name) *to);                         \
    scope bool   bg_##name##_insert(bg(name) *bg, float x, float y, type record);              \
    scope bool   bg_##name##_delete(bg(name) *bg, float x, float y, type record);              \
    scope bool   bg_##name##_find(bg(name) *bg, float x, float y, type *out, int maxout);      \
    scope bool   bg_##name##_contains(bg(name) *bg, float x, float y);                         \
    scope int    bg_##name##_inrange_circle(bg(name) *bg,                                      \
                                            float x, float y, float range,                     \
                                            type *out, int maxout);                            \
    scope int    bg_##name##_inrange_rect(bg(name) *bg,                                        \
                                          float minx, float maxx,                              \
                                          float miny, float maxy,                              \
                                          type *out, int maxout);                              \
    scope void   bg_##name##_cleanup(bg(name) *bg);                                            \
    scope void   bg_##name##_print(bg(name) *bg);

/* ============================================================================
 *  BITMAP_GRID_IMPL - function definitions for a (name, type) instantiation
 *
 *  The macro emits everything needed for one bitmap-grid type. Sections inside,
 *  in dependency order:
 *
 *      1.  Per-instance helpers: cell-coord conversion, slot allocator,
 *          bitmap bit set/clear, cell-empty test.
 *      2.  Compress-store LUT (used by AVX2 emit) and its lazy initialiser.
 *      3.  Per-cell scan functions:
 *              scalar  -> always-available fallback
 *              AVX2    -> 8 (rect) / 4 (circle) elements per iteration,
 *                         emits hits via shuf-based compress-store
 *              AVX-512 -> 16 (rect) / 16 (circle) elements per iteration,
 *                         emits hits via native VPCOMPRESSD
 *      4.  Runtime SIMD dispatch tables and init.
 *      5.  Public API (init, destroy, insert, delete, find, contains,
 *          inrange_rect, inrange_circle, cleanup, etc.)
 * ============================================================================ 
 */

#define BITMAP_GRID_IMPL(scope, name, type)                                                       \
                                                                                               \
    /* ====================================================================== */               \
    /* Section 1: Per-instance helpers                                        */               \
    /* ====================================================================== */               \
                                                                                               \
    /* Convert a scaled int32 x coordinate into the cell column it belongs                 */  \
    /* to, clamped to [0, grid_w). Out-of-bounds positions get pushed to the               */  \
    /* nearest edge cell -- callers are responsible for not relying on this                */  \
    /* for in-bounds correctness.                                                          */  \
    static inline int _bg_##name##_cell_x_from_int(const bg(name) *bg, int32_t ix)             \
    {                                                                                          \
        int cx = (ix - bg->origin_x) >> BG_CELL_LOG2_INT;                                      \
        if(cx < 0) cx = 0;                                                                     \
        if(cx >= bg->grid_w) cx = bg->grid_w - 1;                                              \
        return cx;                                                                             \
    }                                                                                          \
    static inline int _bg_##name##_cell_y_from_int(const bg(name) *bg, int32_t iy)             \
    {                                                                                          \
        int cy = (iy - bg->origin_y) >> BG_CELL_LOG2_INT;                                      \
        if(cy < 0) cy = 0;                                                                     \
        if(cy >= bg->grid_h) cy = bg->grid_h - 1;                                              \
        return cy;                                                                             \
    }                                                                                          \
                                                                                               \
    /* Grow all four pool arrays to at least want_cap entries. Returns false               */  \
    /* on overflow or alloc failure. Growth factor 1.5x; the partially-grown               */  \
    /* state on alloc failure is correct (each array is independently sized                */  \
    /* by the last successful realloc), but elts_cap won't be updated.                     */  \
    static bool _bg_##name##_grow(bg(name) *bg, int32_t want_cap)                              \
    {                                                                                          \
        if(want_cap <= bg->elts_cap) return true;                                              \
        /* Floor the seed at 16 so the 1.5x step always advances; without this,            */  \
        /* a post-cleanup elts_cap of 0 or 1 would stall (1 + (1>>1) == 1).                */  \
        int32_t new_cap = bg->elts_cap < 16 ? 16 : bg->elts_cap;                               \
        while(new_cap < want_cap) {                                                            \
            int32_t next = new_cap + (new_cap >> 1);                                           \
            if(next <= new_cap) return false; /* int32 overflow */                             \
            new_cap = next;                                                                    \
        }                                                                                      \
        int32_t *nxs = (int32_t*)PF_REALLOC(bg->xs, (size_t)new_cap * sizeof(int32_t));           \
        if(!nxs) return false;                                                                 \
        bg->xs = nxs;                                                                          \
        int32_t *nys = (int32_t*)PF_REALLOC(bg->ys, (size_t)new_cap * sizeof(int32_t));           \
        if(!nys) return false;                                                                 \
        bg->ys = nys;                                                                          \
        int32_t *nn  = (int32_t*)PF_REALLOC(bg->nexts, (size_t)new_cap * sizeof(int32_t));        \
        if(!nn) return false;                                                                  \
        bg->nexts = nn;                                                                        \
        type    *nr  = (type*)   PF_REALLOC(bg->records, (size_t)new_cap * sizeof(type));         \
        if(!nr) return false;                                                                  \
        bg->records = nr;                                                                      \
        bg->elts_cap = new_cap;                                                                \
        return true;                                                                           \
    }                                                                                          \
                                                                                               \
    /* Allocate a pool slot. Takes from the free list when one is available,               */  \
    /* otherwise extends the pool by one and grows if needed. Returns the                  */  \
    /* slot index, or -1 on alloc failure.                                                 */  \
    static int32_t _bg_##name##_alloc_slot(bg(name) *bg)                                       \
    {                                                                                          \
        if(bg->first_free >= 0) {                                                              \
            int32_t s = bg->first_free;                                                        \
            bg->first_free = bg->nexts[s];                                                     \
            return s;                                                                          \
        }                                                                                      \
        if(bg->elts_size >= bg->elts_cap) {                                                    \
            if(!_bg_##name##_grow(bg, bg->elts_size + 1)) return -1;                           \
        }                                                                                      \
        return bg->elts_size++;                                                                \
    }                                                                                          \
                                                                                               \
    /* Push a slot onto the free list. Threaded through nexts[].                           */  \
    static void _bg_##name##_free_slot(bg(name) *bg, int32_t s)                                \
    {                                                                                          \
        bg->nexts[s] = bg->first_free;                                                         \
        bg->first_free = s;                                                                    \
    }                                                                                          \
                                                                                               \
    /* Set the fine bit for (cx, cy) AND the coarse bit covering that 8x8                  */  \
    /* block. Called by insert (the coarse bit is monotone-up: insert sets,                */  \
    /* only cleanup clears).                                                               */  \
    static inline void _bg_##name##_bm_set_fine_and_coarse(bg(name) *bg, int cx, int cy)       \
    {                                                                                          \
        bg->bm_fine[cy * bg->bm_fine_row_u64 + (cx >> 6)] |= (1ull << (cx & 63));              \
        int cxc = cx >> BG_COARSE_LOG2;                                                        \
        int cyc = cy >> BG_COARSE_LOG2;                                                        \
        bg->bm_coarse[cyc * bg->bm_coarse_row_u64 + (cxc >> 6)] |= (1ull << (cxc & 63));       \
    }                                                                                          \
                                                                                               \
    /* Clear the fine bit for (cx, cy). The coarse bit is NOT cleared here;                */  \
    /* it's left as a (possibly stale) "1" until cleanup() decides whether                 */  \
    /* the whole 8x8 block is empty. This lazy approach avoids paying for an               */  \
    /* OR-reduce of 64 fine bits on every delete.                                          */  \
    static inline void _bg_##name##_bm_clear_fine(bg(name) *bg, int cx, int cy)                \
    {                                                                                          \
        bg->bm_fine[cy * bg->bm_fine_row_u64 + (cx >> 6)] &= ~(1ull << (cx & 63));             \
    }                                                                                          \
                                                                                               \
    /* True iff a cell has no packed elements and no overflow elements.                    */  \
    static inline bool _bg_##name##_cell_empty(const bg_cell(name) *c)                         \
    {                                                                                          \
        return c->packed_size == 0 && c->overflow_head < 0;                                    \
    }                                                                                          \
                                                                                               \
    /* ====================================================================== */               \
    /* Section 2: AVX2 compress-store LUT                                     */               \
    /*                                                                        */               \
    /* For the AVX2 rect scanner with 8-lane masks, we need to compact the    */               \
    /* records selected by an 8-bit mask to the front of a __m256i and store  */               \
    /* them. AVX2 has no native compress-store (that's an AVX-512 feature),   */               \
    /* so we precompute a 256-entry LUT of __m256i permutations:              */               \
    /*                                                                        */               \
    /*    perm8_lut[mask][k] = lane index of the k'th passing element         */               \
    /*                                                                        */               \
    /* Example: mask = 0b10010100 (lanes 2, 4, 7 pass).                       */               \
    /*    perm8_lut[0b10010100] = {2, 4, 7, 0, 0, 0, 0, 0}                    */               \
    /*    _mm256_permutevar8x32_epi32(records, perm) places r2, r4, r7 in     */               \
    /*    the first three lanes; the rest are "don't-care" (will be           */               \
    /*    overwritten by the next iteration's store).                         */               \
    /*                                                                        */               \
    /* Total LUT size = 256 entries * 8 ints * 4 bytes = 8 KB. Fits in L1.    */               \
    /* The 16-entry LUT for the AVX2 circle scanner (4-bit mask, 16-byte      */               \
    /* shuffle vector each) is defined inline as a static const inside that   */               \
    /* function since it's small and never modified.                          */               \
    /* ====================================================================== */               \
                                                                                               \
    static int32_t _bg_##name##_perm8_lut[256][8];                                             \
    static bool    _bg_##name##_perm8_lut_inited = false;                                      \
                                                                                               \
    static void _bg_##name##_init_perm8_lut(void)                                              \
    {                                                                                          \
        if(_bg_##name##_perm8_lut_inited) return;                                              \
        for(int mask = 0; mask < 256; mask++) {                                                \
            int next = 0;                                                                      \
            /* Walk the bits of `mask` low-to-high. For each set bit, record                */ \
            /* its lane index in the next slot of this LUT entry.                           */ \
            for(int b = 0; b < 8; b++) {                                                       \
                if((mask >> b) & 1) {                                                          \
                    _bg_##name##_perm8_lut[mask][next++] = b;                                  \
                }                                                                              \
            }                                                                                  \
            /* The remaining slots in the entry are don't-care; we fill with                */ \
            /* zero for deterministic content.                                              */ \
            for(int k = next; k < 8; k++) _bg_##name##_perm8_lut[mask][k] = 0;                 \
        }                                                                                      \
        _bg_##name##_perm8_lut_inited = true;                                                  \
    }                                                                                          \
                                                                                               \
    /* ====================================================================== */               \
    /* Section 3: Per-cell scan functions                                     */               \
    /*                                                                        */               \
    /* The bitmap-driven cell iteration in inrange_rect / inrange_circle      */               \
    /* invokes these for each occupied cell it visits. The function pointer   */               \
    /* table in section 4 picks the widest variant the CPU supports.          */               \
    /*                                                                        */               \
    /* Each scanner has the same contract:                                    */               \
    /*    Inputs : xs, ys, records pointers into the packed run; n            */               \
    /*             elements to test; query parameters; out buffer + offset.   */               \
    /*    Output : updated `written`. Appends matching records to             */               \
    /*             out[written..written+m), increments by m hits.             */               \
    /*    Returns: new `written` value.                                       */               \
    /*                                                                        */               \
    /* Each scanner branches on `maxout - written >= n`:                      */               \
    /*    - Fast path: the output buffer has room for every element being     */               \
    /*      scanned, so no per-iteration bounds check is needed. This is the  */               \
    /*      common case in the engine (callers size the output to n_entities  */               \
    /*      or larger).                                                       */               \
    /*    - Bounded path: scalar branchless emit with a maxout check each     */               \
    /*      iteration. Slower but never overflows.                            */               \
    /* ====================================================================== */               \
                                                                                               \
    /* -- Scalar scanners ---------------------------------------------------- */              \
                                                                                               \
    /* Branchless compress-store: write the record unconditionally, advance                */  \
    /* `written` by 0 or 1 depending on the pass mask. Modern CPUs pipeline                */  \
    /* this very effectively via out-of-order execution -- the "scalar" path               */  \
    /* runs at ~2-3 cycles per element with no actual SIMD instructions.                   */  \
                                                                                               \
    static int _bg_##name##_scan_rect_scalar(                                                  \
        const int32_t *xs, const int32_t *ys, const type *records, int32_t n,                  \
        int32_t imnx, int32_t imxx, int32_t imny, int32_t imxy,                                \
        type *out, int written, int maxout)                                                    \
    {                                                                                          \
        if(maxout - written >= n) {                                                            \
            for(int32_t i = 0; i < n; i++) {                                                   \
                int pass = (xs[i] >= imnx) & (xs[i] <= imxx)                                   \
                         & (ys[i] >= imny) & (ys[i] <= imxy);                                  \
                out[written] = records[i];                                                     \
                written += pass;                                                               \
            }                                                                                  \
        } else {                                                                               \
            for(int32_t i = 0; i < n; i++) {                                                   \
                int pass = (xs[i] >= imnx) & (xs[i] <= imxx)                                   \
                         & (ys[i] >= imny) & (ys[i] <= imxy);                                  \
                out[written] = records[i];                                                     \
                written += pass;                                                               \
                if(written >= maxout) return maxout;                                           \
            }                                                                                  \
        }                                                                                      \
        return written;                                                                        \
    }                                                                                          \
                                                                                               \
    static int _bg_##name##_scan_circle_scalar(                                                \
        const int32_t *xs, const int32_t *ys, const type *records, int32_t n,                  \
        int32_t icx, int32_t icy, int64_t ir2,                                                 \
        type *out, int written, int maxout)                                                    \
    {                                                                                          \
        if(maxout - written >= n) {                                                            \
            for(int32_t i = 0; i < n; i++) {                                                   \
                int64_t dx = (int64_t)xs[i] - (int64_t)icx;                                    \
                int64_t dy = (int64_t)ys[i] - (int64_t)icy;                                    \
                int pass = (dx * dx + dy * dy <= ir2);                                         \
                out[written] = records[i];                                                     \
                written += pass;                                                               \
            }                                                                                  \
        } else {                                                                               \
            for(int32_t i = 0; i < n; i++) {                                                   \
                int64_t dx = (int64_t)xs[i] - (int64_t)icx;                                    \
                int64_t dy = (int64_t)ys[i] - (int64_t)icy;                                    \
                int pass = (dx * dx + dy * dy <= ir2);                                         \
                out[written] = records[i];                                                     \
                written += pass;                                                               \
                if(written >= maxout) return maxout;                                           \
            }                                                                                  \
        }                                                                                      \
        return written;                                                                        \
    }                                                                                          \
                                                                                               \
    /* -- AVX2 scanners ------------------------------------------------------ */              \
                                                                                               \
    /* AVX2 rectangle test, 8 elements per iteration.                                     */   \
    /*                                                                                    */   \
    /* Algorithm:                                                                         */   \
    /*   1. Load 8 xs and 8 ys directly as __m256i vectors (SoA pool -- no                */   \
    /*      shuffles needed).                                                             */   \
    /*   2. Build the 8-bit pass mask by combining four lane-wise int32                   */   \
    /*      comparisons via OR (each comparison gives 0xFFFFFFFF for failing              */   \
    /*      lanes and 0 for passing). We OR the four "fail" conditions and                */   \
    /*      then invert via _mm256_movemask_ps.                                           */   \
    /*   3. Emit hits. For sizeof(type) == 4 (bg_ent), use the AVX2                       */   \
    /*      compress-store LUT to permute up to 8 passing records to the                  */   \
    /*      front of a __m256i and store them in one shot. For larger record              */   \
    /*      types, fall back to a per-bit scalar emit using CTZ.                          */   \
                                                                                               \
    SIMD_TARGET_AVX2                                                                           \
    static int _bg_##name##_scan_rect_avx2(                                                    \
        const int32_t *xs, const int32_t *ys, const type *records, int32_t n,                  \
        int32_t imnx, int32_t imxx, int32_t imny, int32_t imxy,                                \
        type *out, int written, int maxout)                                                    \
    {                                                                                          \
        int32_t i = 0;                                                                         \
        if(maxout - written >= n) {                                                            \
            const __m256i imnxv = _mm256_set1_epi32(imnx);                                     \
            const __m256i imxxv = _mm256_set1_epi32(imxx);                                     \
            const __m256i imnyv = _mm256_set1_epi32(imny);                                     \
            const __m256i imxyv = _mm256_set1_epi32(imxy);                                     \
            for(; i + 8 <= n; i += 8) {                                                        \
                __m256i xv = _mm256_loadu_si256((const __m256i*)&xs[i]);                       \
                __m256i yv = _mm256_loadu_si256((const __m256i*)&ys[i]);                       \
                /* fail = (xs < imnx) | (xs > imxx) | (ys < imny) | (ys > imxy)            */  \
                /* AVX2 only has cmpgt_epi32; we use (a > b) and (b > a) and OR.           */  \
                __m256i fail = _mm256_or_si256(                                                \
                    _mm256_or_si256(_mm256_cmpgt_epi32(imnxv, xv),                             \
                                    _mm256_cmpgt_epi32(xv, imxxv)),                            \
                    _mm256_or_si256(_mm256_cmpgt_epi32(imnyv, yv),                             \
                                    _mm256_cmpgt_epi32(yv, imxyv)));                           \
                unsigned mask = (~(unsigned)_mm256_movemask_ps(                                \
                                _mm256_castsi256_ps(fail))) & 0xFFu;                           \
                if(sizeof(type) == 4) {                                                        \
                    /* LUT compress-store: shuffle records by perm8_lut[mask]              */  \
                    /* so passing records sit in the first popcount(mask) lanes,           */  \
                    /* then store the whole 32 bytes. We advance `written` only            */  \
                    /* by popcount(mask), so the trailing don't-care lanes get             */  \
                    /* overwritten by the next iteration (or are past the end,             */  \
                    /* which is safe because we're inside the room-for-n branch).          */  \
                    __m256i recs = _mm256_loadu_si256((const __m256i*)&records[i]);            \
                    __m256i perm = _mm256_loadu_si256(                                         \
                                    (const __m256i*)_bg_##name##_perm8_lut[mask]);             \
                    __m256i comp = _mm256_permutevar8x32_epi32(recs, perm);                    \
                    _mm256_storeu_si256((__m256i*)&out[written], comp);                        \
                    written += (int)SIMD_POPCOUNT32(mask);                                     \
                } else {                                                                       \
                    while(mask) {                                                              \
                        unsigned b = SIMD_CTZ32(mask);                                         \
                        mask &= mask - 1u;                                                     \
                        out[written++] = records[i + b];                                       \
                    }                                                                          \
                }                                                                              \
            }                                                                                  \
        }                                                                                      \
        /* Scalar tail (and bounded-output path).                                          */  \
        for(; i < n; i++) {                                                                    \
            int pass = (xs[i] >= imnx) & (xs[i] <= imxx)                                       \
                     & (ys[i] >= imny) & (ys[i] <= imxy);                                      \
            out[written] = records[i];                                                         \
            written += pass;                                                                   \
            if(written >= maxout) return maxout;                                               \
        }                                                                                      \
        return written;                                                                        \
    }                                                                                          \
                                                                                               \
    /* AVX2 circle test, 4 elements per iteration.                                        */   \
    /*                                                                                    */   \
    /* Squared distance needs int64 to avoid overflow on the multiply, so                 */   \
    /* one AVX2 256-bit register holds at most 4 int64 lanes -- hence the                 */   \
    /* 4-element batch (vs the rect scanner's 8). For the emit we use a                   */   \
    /* small 16-entry LUT of byte-level shuffles (each entry is 16 bytes, so              */   \
    /* the table is 256 bytes total, declared as static const inside the                  */   \
    /* function).                                                                         */   \
                                                                                               \
    SIMD_TARGET_AVX2                                                                           \
    static int _bg_##name##_scan_circle_avx2(                                                  \
        const int32_t *xs, const int32_t *ys, const type *records, int32_t n,                  \
        int32_t icx, int32_t icy, int64_t ir2,                                                 \
        type *out, int written, int maxout)                                                    \
    {                                                                                          \
        /* shuf4_lut[mask][b] = byte-index source for byte b of the output.               */   \
        /* For a 4-bit mask, the LUT compacts the 4 input int32 records (16               */   \
        /* bytes total) so that selected records appear in the first                      */   \
        /* popcount(mask) int32s. 0xFF entries mean "don't care" (will be                 */   \
        /* overwritten on the next iter or are past the end).                             */   \
                                                                                               \
        static const int8_t shuf4_lut[16][16] = {                                              \
            {-1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1}, /* 0000 */                   \
            { 0, 1, 2, 3, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1}, /* 0001 */                   \
            { 4, 5, 6, 7, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1}, /* 0010 */                   \
            { 0, 1, 2, 3,  4, 5, 6, 7, -1,-1,-1,-1, -1,-1,-1,-1}, /* 0011 */                   \
            { 8, 9,10,11, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1}, /* 0100 */                   \
            { 0, 1, 2, 3,  8, 9,10,11, -1,-1,-1,-1, -1,-1,-1,-1}, /* 0101 */                   \
            { 4, 5, 6, 7,  8, 9,10,11, -1,-1,-1,-1, -1,-1,-1,-1}, /* 0110 */                   \
            { 0, 1, 2, 3,  4, 5, 6, 7,  8, 9,10,11, -1,-1,-1,-1}, /* 0111 */                   \
            {12,13,14,15, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1}, /* 1000 */                   \
            { 0, 1, 2, 3, 12,13,14,15, -1,-1,-1,-1, -1,-1,-1,-1}, /* 1001 */                   \
            { 4, 5, 6, 7, 12,13,14,15, -1,-1,-1,-1, -1,-1,-1,-1}, /* 1010 */                   \
            { 0, 1, 2, 3,  4, 5, 6, 7, 12,13,14,15, -1,-1,-1,-1}, /* 1011 */                   \
            { 8, 9,10,11, 12,13,14,15, -1,-1,-1,-1, -1,-1,-1,-1}, /* 1100 */                   \
            { 0, 1, 2, 3,  8, 9,10,11, 12,13,14,15, -1,-1,-1,-1}, /* 1101 */                   \
            { 4, 5, 6, 7,  8, 9,10,11, 12,13,14,15, -1,-1,-1,-1}, /* 1110 */                   \
            { 0, 1, 2, 3,  4, 5, 6, 7,  8, 9,10,11, 12,13,14,15}, /* 1111 */                   \
        };                                                                                     \
        static const int8_t popcount4[16] = {0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4};                 \
                                                                                               \
        int32_t i = 0;                                                                         \
        if(maxout - written >= n) {                                                            \
            const __m256i icxv = _mm256_set1_epi64x((int64_t)icx);                             \
            const __m256i icyv = _mm256_set1_epi64x((int64_t)icy);                             \
            const __m256i ir2v = _mm256_set1_epi64x(ir2);                                      \
            for(; i + 4 <= n; i += 4) {                                                        \
                /* Load 4 int32 xs and 4 int32 ys (16 bytes each); widen to                */  \
                /* 4 int64 lanes for the squared-distance compute.                         */  \
                __m128i xs32 = _mm_loadu_si128((const __m128i*)&xs[i]);                        \
                __m128i ys32 = _mm_loadu_si128((const __m128i*)&ys[i]);                        \
                __m256i xv = _mm256_cvtepi32_epi64(xs32);                                      \
                __m256i yv = _mm256_cvtepi32_epi64(ys32);                                      \
                __m256i dx = _mm256_sub_epi64(xv, icxv);                                       \
                __m256i dy = _mm256_sub_epi64(yv, icyv);                                       \
                /* _mm256_mul_epi32 quirk: it multiplies the LOW 32 bits of                */  \
                /* each 64-bit lane and stores the signed 64-bit product. The              */  \
                /* low 32 bits of dx are the signed delta (which fits in int32             */  \
                /* given BG_LOG2_SCALE and bounded maps), so this gives the                */  \
                /* exact int64 square of dx in each lane.                                  */  \
                __m256i dx2 = _mm256_mul_epi32(dx, dx);                                        \
                __m256i dy2 = _mm256_mul_epi32(dy, dy);                                        \
                __m256i d2 = _mm256_add_epi64(dx2, dy2);                                       \
                __m256i gt = _mm256_cmpgt_epi64(d2, ir2v);                                     \
                /* movemask_pd gives one bit per 64-bit lane (the high bit).               */  \
                /* gt is set in lanes where d2 > ir2 (i.e. fail); invert to                */  \
                /* get the pass mask.                                                      */  \
                unsigned mask = (~(unsigned)_mm256_movemask_pd(                                \
                                _mm256_castsi256_pd(gt))) & 0xFu;                              \
                if(sizeof(type) == 4) {                                                        \
                    /* 16-byte shuffle compress-store: load 4 records as a                 */  \
                    /* __m128i, shuffle by the LUT entry, store. Advance                  */   \
                    /* `written` by popcount(mask) so the next iteration                  */   \
                    /* overwrites the don't-care tail.                                    */   \
                    __m128i recs = _mm_loadu_si128((const __m128i*)&records[i]);               \
                    __m128i sh   = _mm_loadu_si128((const __m128i*)shuf4_lut[mask]);           \
                    __m128i comp = _mm_shuffle_epi8(recs, sh);                                 \
                    _mm_storeu_si128((__m128i*)&out[written], comp);                           \
                    written += popcount4[mask];                                                \
                } else {                                                                       \
                    while(mask) {                                                              \
                        unsigned b = SIMD_CTZ32(mask);                                         \
                        mask &= mask - 1u;                                                     \
                        out[written++] = records[i + b];                                       \
                    }                                                                          \
                }                                                                              \
            }                                                                                  \
        }                                                                                      \
        /* Scalar tail.                                                                    */  \
        for(; i < n; i++) {                                                                    \
            int64_t dx = (int64_t)xs[i] - (int64_t)icx;                                        \
            int64_t dy = (int64_t)ys[i] - (int64_t)icy;                                        \
            int pass = (dx * dx + dy * dy <= ir2);                                             \
            out[written] = records[i];                                                         \
            written += pass;                                                                   \
            if(written >= maxout) return maxout;                                               \
        }                                                                                      \
        return written;                                                                        \
    }                                                                                          \
                                                                                               \
    /* -- AVX-512 scanners -------------------------------------------------- */               \
                                                                                               \
    /* Used only for "long scan" calls (the whole-pool wide-query fast path),             */   \
    /* not the per-cell hot loop. See the dispatcher commentary in section 4.             */   \
    /*                                                                                    */   \
    /* AVX-512 rect: 16 elements per iter; native VPCOMPRESSD emit.                       */   \
    /*                                                                                    */   \
    /* Mask construction is the cumulative-mask idiom: cmp_epi32_mask                     */   \
    /* produces a 16-bit mask, then mask_cmp_epi32_mask ANDs each subsequent              */   \
    /* result with the running mask. The net effect is to AND four                        */   \
    /* comparisons together using mask registers, which is more efficient                 */   \
    /* than computing each comparison separately and combining at the end.                */   \
                                                                                               \
    SIMD_TARGET_AVX512F                                                                        \
    static int _bg_##name##_scan_rect_avx512(                                                  \
        const int32_t *xs, const int32_t *ys, const type *records, int32_t n,                  \
        int32_t imnx, int32_t imxx, int32_t imny, int32_t imxy,                                \
        type *out, int written, int maxout)                                                    \
    {                                                                                          \
        int32_t i = 0;                                                                         \
        if(maxout - written >= n) {                                                            \
            const __m512i imnxv = _mm512_set1_epi32(imnx);                                     \
            const __m512i imxxv = _mm512_set1_epi32(imxx);                                     \
            const __m512i imnyv = _mm512_set1_epi32(imny);                                     \
            const __m512i imxyv = _mm512_set1_epi32(imxy);                                     \
            for(; i + 16 <= n; i += 16) {                                                      \
                __m512i xv = _mm512_loadu_si512((const __m512i*)&xs[i]);                       \
                __m512i yv = _mm512_loadu_si512((const __m512i*)&ys[i]);                       \
                /* Build pass mask by ANDing four CMP results into a __mmask16.            */  \
                __mmask16 m = _mm512_cmp_epi32_mask(xv, imnxv, _MM_CMPINT_GE);                 \
                m = _mm512_mask_cmp_epi32_mask(m, xv, imxxv, _MM_CMPINT_LE);                   \
                m = _mm512_mask_cmp_epi32_mask(m, yv, imnyv, _MM_CMPINT_GE);                   \
                m = _mm512_mask_cmp_epi32_mask(m, yv, imxyv, _MM_CMPINT_LE);                   \
                if(sizeof(type) == 4) {                                                        \
                    /* VPCOMPRESSD is the AVX-512 compress-store; it writes               */   \
                    /* records[lanes where mask==1] contiguously to memory.               */   \
                    /* No LUT needed.                                                     */   \
                    __m512i recs = _mm512_loadu_si512((const __m512i*)&records[i]);            \
                    _mm512_mask_compressstoreu_epi32(&out[written], m, recs);                  \
                    written += (int)SIMD_POPCOUNT32((unsigned)m);                              \
                } else {                                                                       \
                    unsigned mm = (unsigned)m;                                                 \
                    while(mm) {                                                                \
                        unsigned b = SIMD_CTZ32(mm);                                           \
                        mm &= mm - 1u;                                                         \
                        out[written++] = records[i + b];                                       \
                    }                                                                          \
                }                                                                              \
            }                                                                                  \
        }                                                                                      \
        for(; i < n; i++) {                                                                    \
            int pass = (xs[i] >= imnx) & (xs[i] <= imxx)                                       \
                     & (ys[i] >= imny) & (ys[i] <= imxy);                                      \
            out[written] = records[i];                                                         \
            written += pass;                                                                   \
            if(written >= maxout) return maxout;                                               \
        }                                                                                      \
        return written;                                                                        \
    }                                                                                          \
                                                                                               \
    /* AVX-512 circle: 16 elements per iter, but the int64 squared-distance               */   \
    /* compute needs two batches of 8 int64 lanes (since a __m512i holds 8                */   \
    /* int64 or 16 int32). We load 16 int32 xs and ys, split each into low                */   \
    /* and high halves, widen to int64, compute squared distance for each                 */   \
    /* batch, then combine the two 8-bit masks into a single 16-bit mask                  */   \
    /* and use a single VPCOMPRESSD to emit hits.                                         */   \
                                                                                               \
    SIMD_TARGET_AVX512F                                                                        \
    static int _bg_##name##_scan_circle_avx512(                                                \
        const int32_t *xs, const int32_t *ys, const type *records, int32_t n,                  \
        int32_t icx, int32_t icy, int64_t ir2,                                                 \
        type *out, int written, int maxout)                                                    \
    {                                                                                          \
        int32_t i = 0;                                                                         \
        if(maxout - written >= n) {                                                            \
            const __m512i icxv = _mm512_set1_epi64((int64_t)icx);                              \
            const __m512i icyv = _mm512_set1_epi64((int64_t)icy);                              \
            const __m512i ir2v = _mm512_set1_epi64(ir2);                                       \
            for(; i + 16 <= n; i += 16) {                                                      \
                /* Load 16 int32 xs and 16 int32 ys.                                       */  \
                __m512i xs32 = _mm512_loadu_si512((const __m512i*)&xs[i]);                     \
                __m512i ys32 = _mm512_loadu_si512((const __m512i*)&ys[i]);                     \
                /* Split into low/high halves (8 int32 each).                              */  \
                __m256i xs32_lo = _mm512_castsi512_si256(xs32);                                \
                __m256i ys32_lo = _mm512_castsi512_si256(ys32);                                \
                __m256i xs32_hi = _mm512_extracti64x4_epi64(xs32, 1);                          \
                __m256i ys32_hi = _mm512_extracti64x4_epi64(ys32, 1);                          \
                /* Widen each half to 8 int64 lanes.                                       */  \
                __m512i xv_lo = _mm512_cvtepi32_epi64(xs32_lo);                                \
                __m512i yv_lo = _mm512_cvtepi32_epi64(ys32_lo);                                \
                __m512i xv_hi = _mm512_cvtepi32_epi64(xs32_hi);                                \
                __m512i yv_hi = _mm512_cvtepi32_epi64(ys32_hi);                                \
                __m512i dx_lo = _mm512_sub_epi64(xv_lo, icxv);                                 \
                __m512i dy_lo = _mm512_sub_epi64(yv_lo, icyv);                                 \
                __m512i dx_hi = _mm512_sub_epi64(xv_hi, icxv);                                 \
                __m512i dy_hi = _mm512_sub_epi64(yv_hi, icyv);                                 \
                /* Same mul_epi32 quirk as AVX2: multiplies low 32 bits of                 */  \
                /* each 64-bit lane, gives signed int64 product. Safe since                */  \
                /* dx and dy fit in int32 given BG_LOG2_SCALE.                             */  \
                __m512i d2_lo = _mm512_add_epi64(_mm512_mul_epi32(dx_lo, dx_lo),               \
                                                 _mm512_mul_epi32(dy_lo, dy_lo));              \
                __m512i d2_hi = _mm512_add_epi64(_mm512_mul_epi32(dx_hi, dx_hi),               \
                                                 _mm512_mul_epi32(dy_hi, dy_hi));              \
                /* 8-bit pass masks for each half, combined into a single                  */  \
                /* 16-bit mask (low half = first 8 elements, high half = next 8).          */  \
                __mmask8 m_lo = _mm512_cmp_epi64_mask(d2_lo, ir2v, _MM_CMPINT_LE);             \
                __mmask8 m_hi = _mm512_cmp_epi64_mask(d2_hi, ir2v, _MM_CMPINT_LE);             \
                __mmask16 m = (__mmask16)m_lo | ((__mmask16)m_hi << 8);                        \
                if(sizeof(type) == 4) {                                                        \
                    __m512i recs = _mm512_loadu_si512((const __m512i*)&records[i]);            \
                    _mm512_mask_compressstoreu_epi32(&out[written], m, recs);                  \
                    written += (int)SIMD_POPCOUNT32((unsigned)m);                              \
                } else {                                                                       \
                    unsigned mm = (unsigned)m;                                                 \
                    while(mm) {                                                                \
                        unsigned b = SIMD_CTZ32(mm);                                           \
                        mm &= mm - 1u;                                                         \
                        out[written++] = records[i + b];                                       \
                    }                                                                          \
                }                                                                              \
            }                                                                                  \
        }                                                                                      \
        for(; i < n; i++) {                                                                    \
            int64_t dx = (int64_t)xs[i] - (int64_t)icx;                                        \
            int64_t dy = (int64_t)ys[i] - (int64_t)icy;                                        \
            int pass = (dx * dx + dy * dy <= ir2);                                             \
            out[written] = records[i];                                                         \
            written += pass;                                                                   \
            if(written >= maxout) return maxout;                                               \
        }                                                                                      \
        return written;                                                                        \
    }                                                                                          \
                                                                                               \
    /* ====================================================================== */               \
    /* Section 4: Runtime SIMD dispatch                                       */               \
    /*                                                                        */               \
    /* Two function pointers per scan operation:                              */               \
    /*                                                                        */               \
    /*    _scan_rect      / _scan_circle      : per-cell hot loop. AVX2 wins  */               \
    /*                                          here even on AVX-512 hardware */               \
    /*                                          because the per-iteration     */               \
    /*                                          setup cost and the AVX-512    */               \
    /*                                          clock-throttle penalty hurt   */               \
    /*                                          short scans (cells typically  */               \
    /*                                          hold ~24 elements after       */               \
    /*                                          cleanup).                     */               \
    /*                                                                        */               \
    /*    _scan_rect_long / _scan_circle_long : whole-pool linear scan path.  */               \
    /*                                          AVX-512 wins here -- the long */               \
    /*                                          sustained loop amortises any  */               \
    /*                                          throttle hit, and 16-lane     */               \
    /*                                          batches + native VPCOMPRESSD  */               \
    /*                                          beat AVX2's 8-lane + LUT.     */               \
    /*                                                                        */               \
    /* Initialised once, lazily, on the first call to bg_<name>_init via      */               \
    /* _bg_<name>_init_simd.                                                  */               \
    /* ====================================================================== */               \
                                                                                               \
    typedef int (*_bg_##name##_scan_rect_fn)(                                                  \
        const int32_t*, const int32_t*, const type*, int32_t,                                  \
        int32_t, int32_t, int32_t, int32_t,                                                    \
        type*, int, int);                                                                      \
    typedef int (*_bg_##name##_scan_circle_fn)(                                                \
        const int32_t*, const int32_t*, const type*, int32_t,                                  \
        int32_t, int32_t, int64_t,                                                             \
        type*, int, int);                                                                      \
                                                                                               \
    static _bg_##name##_scan_rect_fn   _bg_##name##_scan_rect        = NULL;                   \
    static _bg_##name##_scan_rect_fn   _bg_##name##_scan_rect_long   = NULL;                   \
    static _bg_##name##_scan_circle_fn _bg_##name##_scan_circle      = NULL;                   \
    static _bg_##name##_scan_circle_fn _bg_##name##_scan_circle_long = NULL;                   \
                                                                                               \
    static void _bg_##name##_init_simd(void)                                                   \
    {                                                                                          \
        if(_bg_##name##_scan_rect != NULL) return;                                             \
        /* Cell-loop dispatch: AVX2 (with compress-store LUT) when available,              */  \
        /* otherwise scalar. AVX-512 is intentionally NOT used here.                       */  \
        if(SIMD_HAS_TARGET_AVX2 && simd_avx2_supported()) {                                    \
            _bg_##name##_init_perm8_lut();                                                     \
            _bg_##name##_scan_rect   = _bg_##name##_scan_rect_avx2;                            \
            _bg_##name##_scan_circle = _bg_##name##_scan_circle_avx2;                          \
        } else {                                                                               \
            _bg_##name##_scan_rect   = _bg_##name##_scan_rect_scalar;                          \
            _bg_##name##_scan_circle = _bg_##name##_scan_circle_scalar;                        \
        }                                                                                      \
        /* Long-scan dispatch: AVX-512 if available, else fall back to the                 */  \
        /* cell-loop variant.                                                              */  \
        if(SIMD_HAS_TARGET_AVX512F && simd_avx512_supported()) {                               \
            _bg_##name##_scan_rect_long   = _bg_##name##_scan_rect_avx512;                     \
            _bg_##name##_scan_circle_long = _bg_##name##_scan_circle_avx512;                   \
        } else {                                                                               \
            _bg_##name##_scan_rect_long   = _bg_##name##_scan_rect;                            \
            _bg_##name##_scan_circle_long = _bg_##name##_scan_circle;                          \
        }                                                                                      \
    }                                                                                          \
                                                                                               \
    /* ====================================================================== */               \
    /* Section 5: Public API                                                  */               \
    /* ====================================================================== */               \
                                                                                               \
    scope bool bg_##name##_init(bg(name) *bg,                                                  \
                                float xmin, float xmax,                                        \
                                float ymin, float ymax,                                        \
                                bool (*comparator)(const type*, const type*))                  \
    {                                                                                          \
        _bg_##name##_init_simd();                                                              \
        memset(bg, 0, sizeof(*bg));                                                            \
        bg->xmin = xmin; bg->xmax = xmax; bg->ymin = ymin; bg->ymax = ymax;                    \
        bg->comparator = comparator;                                                           \
        bg->origin_x = BG_SCALE_F(xmin);                                                       \
        bg->origin_y = BG_SCALE_F(ymin);                                                       \
                                                                                               \
        int32_t span_x = BG_SCALE_F(xmax) - bg->origin_x;                                      \
        int32_t span_y = BG_SCALE_F(ymax) - bg->origin_y;                                      \
        if(span_x <= 0 || span_y <= 0) return false;                                           \
                                                                                               \
        /* Grid dimensions: ceil(span / cell_size).                                        */  \
        bg->grid_w = (int)(((uint32_t)span_x + (1u << BG_CELL_LOG2_INT) - 1u)                  \
                            >> BG_CELL_LOG2_INT);                                              \
        bg->grid_h = (int)(((uint32_t)span_y + (1u << BG_CELL_LOG2_INT) - 1u)                  \
                            >> BG_CELL_LOG2_INT);                                              \
        if(bg->grid_w < 1) bg->grid_w = 1;                                                     \
        if(bg->grid_h < 1) bg->grid_h = 1;                                                     \
                                                                                               \
        bg->coarse_w = _BG_DIV_CEIL(bg->grid_w, 1 << BG_COARSE_LOG2);                          \
        bg->coarse_h = _BG_DIV_CEIL(bg->grid_h, 1 << BG_COARSE_LOG2);                          \
                                                                                               \
        bg->bm_fine_row_u64   = _BG_DIV_CEIL(bg->grid_w,   64);                                \
        bg->bm_coarse_row_u64 = _BG_DIV_CEIL(bg->coarse_w, 64);                                \
                                                                                               \
        /* Cell metadata table. 12 bytes per cell, so even a 256x256 grid                  */  \
        /* fits comfortably (768 KB).                                                      */  \
        size_t ncells = (size_t)bg->grid_w * (size_t)bg->grid_h;                               \
        bg->cells = (bg_cell(name)*)PF_MALLOC(ncells * sizeof(bg_cell(name)));                    \
        if(!bg->cells) return false;                                                           \
        for(size_t i = 0; i < ncells; i++) {                                                   \
            bg->cells[i].packed_start = -1;                                                    \
            bg->cells[i].packed_size  = 0;                                                     \
            bg->cells[i].overflow_head = -1;                                                   \
        }                                                                                      \
                                                                                               \
        bg->first_free = -1;                                                                   \
        bg->xs = NULL; bg->ys = NULL; bg->nexts = NULL; bg->records = NULL;                    \
        bg->elts_cap = 0;                                                                      \
        bg->elts_size = 0;                                                                     \
        bg->nrecs = 0;                                                                         \
        bg->dirty = false;                                                                     \
                                                                                               \
        {                                                                                      \
            size_t nu_fine   = (size_t)bg->bm_fine_row_u64   * (size_t)bg->grid_h;             \
            size_t nu_coarse = (size_t)bg->bm_coarse_row_u64 * (size_t)bg->coarse_h;           \
            bg->bm_fine   = (uint64_t*)PF_CALLOC(nu_fine,   sizeof(uint64_t));                    \
            bg->bm_coarse = (uint64_t*)PF_CALLOC(nu_coarse, sizeof(uint64_t));                    \
            if(!bg->bm_fine || !bg->bm_coarse) {                                               \
                PF_FREE(bg->cells); PF_FREE(bg->bm_fine); PF_FREE(bg->bm_coarse);                       \
                memset(bg, 0, sizeof(*bg));                                                    \
                return false;                                                                  \
            }                                                                                  \
        }                                                                                      \
                                                                                               \
        return true;                                                                           \
    }                                                                                          \
                                                                                               \
    scope void bg_##name##_destroy(bg(name) *bg)                                               \
    {                                                                                          \
        PF_FREE(bg->cells);                                                                       \
        PF_FREE(bg->xs); PF_FREE(bg->ys); PF_FREE(bg->nexts); PF_FREE(bg->records);                        \
        PF_FREE(bg->bm_fine); PF_FREE(bg->bm_coarse);                                                \
        memset(bg, 0, sizeof(*bg));                                                            \
    }                                                                                          \
                                                                                               \
    scope void bg_##name##_clear(bg(name) *bg)                                                 \
    {                                                                                          \
        size_t ncells = (size_t)bg->grid_w * (size_t)bg->grid_h;                               \
        for(size_t i = 0; i < ncells; i++) {                                                   \
            bg->cells[i].packed_start = -1;                                                    \
            bg->cells[i].packed_size  = 0;                                                     \
            bg->cells[i].overflow_head = -1;                                                   \
        }                                                                                      \
        bg->elts_size = 0;                                                                     \
        bg->first_free = -1;                                                                   \
        bg->nrecs = 0;                                                                         \
        bg->dirty = false;                                                                     \
        if(bg->bm_fine) memset(bg->bm_fine, 0,                                                 \
            (size_t)bg->bm_fine_row_u64 * (size_t)bg->grid_h * sizeof(uint64_t));              \
        if(bg->bm_coarse) memset(bg->bm_coarse, 0,                                             \
            (size_t)bg->bm_coarse_row_u64 * (size_t)bg->coarse_h * sizeof(uint64_t));          \
    }                                                                                          \
                                                                                               \
    scope bool bg_##name##_reserve(bg(name) *bg, size_t hint)                                  \
    {                                                                                          \
        if(hint > (size_t)INT32_MAX) hint = (size_t)INT32_MAX;                                 \
        return _bg_##name##_grow(bg, (int32_t)hint);                                           \
    }                                                                                          \
                                                                                               \
    /* Deep copy. The cell array, pool arrays, and bitmaps are all duplicated.             */  \
    /* On any alloc failure the destination is rolled back to empty and false              */  \
    /* is returned.                                                                        */  \
                                                                                               \
    scope bool bg_##name##_copy(const bg(name) *from, bg(name) *to)                            \
    {                                                                                          \
        memcpy(to, from, sizeof(*to));                                                         \
        to->cells = NULL;                                                                      \
        to->xs = NULL; to->ys = NULL; to->nexts = NULL; to->records = NULL;                    \
        to->bm_fine = NULL; to->bm_coarse = NULL;                                              \
                                                                                               \
        size_t ncells = (size_t)from->grid_w * (size_t)from->grid_h;                           \
        to->cells = (bg_cell(name)*)PF_MALLOC(ncells * sizeof(bg_cell(name)));                    \
        if(!to->cells) goto fail;                                                              \
        memcpy(to->cells, from->cells, ncells * sizeof(bg_cell(name)));                        \
                                                                                               \
        if(from->elts_cap > 0) {                                                               \
            to->xs      = (int32_t*)PF_MALLOC((size_t)from->elts_cap * sizeof(int32_t));          \
            to->ys      = (int32_t*)PF_MALLOC((size_t)from->elts_cap * sizeof(int32_t));          \
            to->nexts   = (int32_t*)PF_MALLOC((size_t)from->elts_cap * sizeof(int32_t));          \
            to->records = (type*)   PF_MALLOC((size_t)from->elts_cap * sizeof(type));             \
            if(!to->xs || !to->ys || !to->nexts || !to->records) goto fail;                    \
            memcpy(to->xs,      from->xs,      (size_t)from->elts_size * sizeof(int32_t));     \
            memcpy(to->ys,      from->ys,      (size_t)from->elts_size * sizeof(int32_t));     \
            memcpy(to->nexts,   from->nexts,   (size_t)from->elts_size * sizeof(int32_t));     \
            memcpy(to->records, from->records, (size_t)from->elts_size * sizeof(type));        \
        }                                                                                      \
                                                                                               \
        if(from->bm_fine) {                                                                    \
            size_t nu = (size_t)from->bm_fine_row_u64 * (size_t)from->grid_h;                  \
            to->bm_fine = (uint64_t*)PF_MALLOC(nu * sizeof(uint64_t));                            \
            if(!to->bm_fine) goto fail;                                                        \
            memcpy(to->bm_fine, from->bm_fine, nu * sizeof(uint64_t));                         \
        }                                                                                      \
        if(from->bm_coarse) {                                                                  \
            size_t nu = (size_t)from->bm_coarse_row_u64 * (size_t)from->coarse_h;              \
            to->bm_coarse = (uint64_t*)PF_MALLOC(nu * sizeof(uint64_t));                          \
            if(!to->bm_coarse) goto fail;                                                      \
            memcpy(to->bm_coarse, from->bm_coarse, nu * sizeof(uint64_t));                     \
        }                                                                                      \
        return true;                                                                           \
                                                                                               \
    fail:                                                                                      \
        PF_FREE(to->cells);                                                                       \
        PF_FREE(to->xs); PF_FREE(to->ys); PF_FREE(to->nexts); PF_FREE(to->records);                        \
        PF_FREE(to->bm_fine); PF_FREE(to->bm_coarse);                                                \
        memset(to, 0, sizeof(*to));                                                            \
        return false;                                                                          \
    }                                                                                          \
                                                                                               \
    /* Insert always appends to the cell's overflow chain. This keeps inserts              */  \
    /* O(1) (no shifting of packed elements) and lets the next cleanup() merge             */  \
    /* the overflow back into the packed range. Sets both fine and coarse                  */  \
    /* bitmap bits eagerly so the cell is visible to range queries straight                */  \
    /* away.                                                                               */  \
                                                                                               \
    scope bool bg_##name##_insert(bg(name) *bg, float x, float y, type record)                 \
    {                                                                                          \
        int32_t ix = BG_SCALE_F(x);                                                            \
        int32_t iy = BG_SCALE_F(y);                                                            \
        int cx = _bg_##name##_cell_x_from_int(bg, ix);                                         \
        int cy = _bg_##name##_cell_y_from_int(bg, iy);                                         \
        int ci = cy * bg->grid_w + cx;                                                         \
        bg_cell(name) *c = &bg->cells[ci];                                                     \
                                                                                               \
        int32_t s = _bg_##name##_alloc_slot(bg);                                               \
        if(s < 0) return false;                                                                \
        bg->xs[s] = ix;                                                                        \
        bg->ys[s] = iy;                                                                        \
        bg->records[s] = record;                                                               \
        bg->nexts[s] = c->overflow_head;                                                       \
        c->overflow_head = s;                                                                  \
        bg->nrecs++;                                                                           \
        bg->dirty = true;                                                                      \
        _bg_##name##_bm_set_fine_and_coarse(bg, cx, cy);                                       \
        return true;                                                                           \
    }                                                                                          \
                                                                                               \
    /* Delete: search the packed range first (the common case after cleanup);             */   \
    /* if the target is there, swap with the last element of the cell's range             */   \
    /* and free the now-stale tail slot. Otherwise walk the overflow chain.               */   \
                                                                                               \
    scope bool bg_##name##_delete(bg(name) *bg, float x, float y, type record)                 \
    {                                                                                          \
        int32_t ix = BG_SCALE_F(x);                                                            \
        int32_t iy = BG_SCALE_F(y);                                                            \
        int cx = _bg_##name##_cell_x_from_int(bg, ix);                                         \
        int cy = _bg_##name##_cell_y_from_int(bg, iy);                                         \
        int ci = cy * bg->grid_w + cx;                                                         \
        bg_cell(name) *c = &bg->cells[ci];                                                     \
                                                                                               \
        if(c->packed_size > 0) {                                                               \
            int32_t start = c->packed_start;                                                   \
            for(int32_t i = 0; i < c->packed_size; i++) {                                      \
                int32_t idx = start + i;                                                       \
                if(bg->xs[idx] == ix && bg->ys[idx] == iy                                      \
                && bg->comparator(&record, &bg->records[idx])) {                               \
                    int32_t last = c->packed_size - 1;                                         \
                    int32_t freed_slot = start + last;                                         \
                    if(i != last) {                                                            \
                        /* Swap-with-last: the cell's run stays contiguous;               */   \
                        /* the displaced tail entry goes onto the free list and           */   \
                        /* may be reused by a subsequent insert.                          */   \
                        bg->xs[idx]      = bg->xs[freed_slot];                                 \
                        bg->ys[idx]      = bg->ys[freed_slot];                                 \
                        bg->records[idx] = bg->records[freed_slot];                            \
                    }                                                                          \
                    c->packed_size--;                                                          \
                    bg->nrecs--;                                                               \
                    bg->dirty = true;                                                          \
                    _bg_##name##_free_slot(bg, freed_slot);                                    \
                    if(_bg_##name##_cell_empty(c))                                             \
                        _bg_##name##_bm_clear_fine(bg, cx, cy);                                \
                    return true;                                                               \
                }                                                                              \
            }                                                                                  \
        }                                                                                      \
                                                                                               \
        /* Overflow chain walk.                                                            */  \
        int32_t prev = -1, curr = c->overflow_head;                                            \
        while(curr >= 0) {                                                                     \
            if(bg->xs[curr] == ix && bg->ys[curr] == iy                                        \
            && bg->comparator(&record, &bg->records[curr])) {                                  \
                if(prev >= 0) bg->nexts[prev] = bg->nexts[curr];                               \
                else          c->overflow_head = bg->nexts[curr];                              \
                _bg_##name##_free_slot(bg, curr);                                              \
                bg->nrecs--;                                                                   \
                bg->dirty = true;                                                              \
                if(_bg_##name##_cell_empty(c))                                                 \
                    _bg_##name##_bm_clear_fine(bg, cx, cy);                                    \
                return true;                                                                   \
            }                                                                                  \
            prev = curr;                                                                       \
            curr = bg->nexts[curr];                                                            \
        }                                                                                      \
        return false;                                                                          \
    }                                                                                          \
                                                                                               \
    /* Position lookup. Returns the first record stored at (x, y); if multiple             */  \
    /* records sit on the exact same scaled position, this returns one of them             */  \
    /* (the first in the packed range, else the head of the overflow chain).               */  \
                                                                                               \
    scope bool bg_##name##_find(bg(name) *bg, float x, float y, type *out, int maxout)         \
    {                                                                                          \
        (void)maxout;                                                                          \
        int32_t ix = BG_SCALE_F(x);                                                            \
        int32_t iy = BG_SCALE_F(y);                                                            \
        int cx = _bg_##name##_cell_x_from_int(bg, ix);                                         \
        int cy = _bg_##name##_cell_y_from_int(bg, iy);                                         \
        int ci = cy * bg->grid_w + cx;                                                         \
        const bg_cell(name) *c = &bg->cells[ci];                                               \
                                                                                               \
        if(c->packed_size > 0) {                                                               \
            int32_t start = c->packed_start;                                                   \
            for(int32_t i = 0; i < c->packed_size; i++) {                                      \
                if(bg->xs[start + i] == ix && bg->ys[start + i] == iy) {                       \
                    *out = bg->records[start + i];                                             \
                    return true;                                                               \
                }                                                                              \
            }                                                                                  \
        }                                                                                      \
        int32_t curr = c->overflow_head;                                                       \
        while(curr >= 0) {                                                                     \
            if(bg->xs[curr] == ix && bg->ys[curr] == iy) {                                     \
                *out = bg->records[curr];                                                      \
                return true;                                                                   \
            }                                                                                  \
            curr = bg->nexts[curr];                                                            \
        }                                                                                      \
        return false;                                                                          \
    }                                                                                          \
                                                                                               \
    scope bool bg_##name##_contains(bg(name) *bg, float x, float y)                            \
    {                                                                                          \
        type tmp;                                                                              \
        return bg_##name##_find(bg, x, y, &tmp, 1);                                            \
    }                                                                                          \
                                                                                               \
    /* Resolve a (possibly out-of-bounds) integer query bbox into the inclusive            */  \
    /* fine-cell extent [cx_lo, cx_hi] x [cy_lo, cy_hi] that it overlaps. Returns          */  \
    /* false if the bbox doesn't intersect the grid at all.                                */  \
                                                                                               \
    static inline bool _bg_##name##_cell_extent(const bg(name) *bg,                            \
        int32_t imnx, int32_t imxx, int32_t imny, int32_t imxy,                                \
        int *cx_lo, int *cx_hi, int *cy_lo, int *cy_hi)                                        \
    {                                                                                          \
        if(imxx < bg->origin_x) return false;                                                  \
        if(imxy < bg->origin_y) return false;                                                  \
        int32_t span_x = (int32_t)((uint32_t)bg->grid_w << BG_CELL_LOG2_INT);                  \
        int32_t span_y = (int32_t)((uint32_t)bg->grid_h << BG_CELL_LOG2_INT);                  \
        if(imnx >= bg->origin_x + span_x) return false;                                        \
        if(imny >= bg->origin_y + span_y) return false;                                        \
        int xl = (imnx - bg->origin_x) >> BG_CELL_LOG2_INT;                                    \
        int xh = (imxx - bg->origin_x) >> BG_CELL_LOG2_INT;                                    \
        int yl = (imny - bg->origin_y) >> BG_CELL_LOG2_INT;                                    \
        int yh = (imxy - bg->origin_y) >> BG_CELL_LOG2_INT;                                    \
        if(xl < 0) xl = 0;                                                                     \
        if(yl < 0) yl = 0;                                                                     \
        if(xh >= bg->grid_w) xh = bg->grid_w - 1;                                              \
        if(yh >= bg->grid_h) yh = bg->grid_h - 1;                                              \
        *cx_lo = xl; *cx_hi = xh; *cy_lo = yl; *cy_hi = yh;                                    \
        return true;                                                                           \
    }                                                                                          \
                                                                                               \
    /* Range queries (rect and circle) share the same skeleton:                            */  \
    /*                                                                                     */  \
    /*   1.  Compute the query bbox in scaled-int and its fine-cell extent.                */  \
    /*   2.  Wide-query fast path: when the tree is clean (no overflow,                    */  \
    /*       no holes) and the bbox covers >= 75% of the grid, skip the                    */  \
    /*       bitmap iteration entirely and call the long-scan variant on the               */  \
    /*       whole pool. Avoids ~4096 per-cell metadata loads for a query                  */  \
    /*       that's going to visit them all anyway.                                        */  \
    /*   3.  Outer bitmap iteration: walk the coarse bitmap row-by-row, and                */  \
    /*       for each set coarse bit, descend into its 8x8 fine block. Inside              */  \
    /*       each fine block, walk the fine bitmap and call the per-cell scan              */  \
    /*       on every set bit (clipped to the query extent).                               */  \
    /*   4.  For each visited cell, scan the packed range via the function-                */  \
    /*       pointer scanner (AVX-512 / AVX2 / scalar), then walk the                      */  \
    /*       overflow chain scalar-style.                                                  */  \
                                                                                               \
    scope int bg_##name##_inrange_rect(bg(name) *bg,                                           \
                                       float minx, float maxx,                                 \
                                       float miny, float maxy,                                 \
                                       type *out, int maxout)                                  \
    {                                                                                          \
        if(maxout <= 0) return 0;                                                              \
        int32_t imnx = BG_SCALE_F(minx), imxx = BG_SCALE_F(maxx);                              \
        int32_t imny = BG_SCALE_F(miny), imxy = BG_SCALE_F(maxy);                              \
        int cx_lo, cx_hi, cy_lo, cy_hi;                                                        \
        if(!_bg_##name##_cell_extent(bg, imnx, imxx, imny, imxy,                               \
                                     &cx_lo, &cx_hi, &cy_lo, &cy_hi)) return 0;                \
                                                                                               \
        /* Wide-query fast path -- see comment above.                                      */  \
        if(!bg->dirty) {                                                                       \
            int64_t extent = (int64_t)(cx_hi - cx_lo + 1) * (int64_t)(cy_hi - cy_lo + 1);      \
            int64_t total  = (int64_t)bg->grid_w * (int64_t)bg->grid_h;                        \
            if(extent * _BG_WIDE_QUERY_DEN >= total * _BG_WIDE_QUERY_NUM) {                    \
                return _bg_##name##_scan_rect_long(                                            \
                    bg->xs, bg->ys, bg->records, (int32_t)bg->nrecs,                           \
                    imnx, imxx, imny, imxy, out, 0, maxout);                                   \
            }                                                                                  \
        }                                                                                      \
                                                                                               \
        int written = 0;                                                                       \
        int cxc_lo = cx_lo >> BG_COARSE_LOG2;                                                  \
        int cxc_hi = cx_hi >> BG_COARSE_LOG2;                                                  \
        int cyc_lo = cy_lo >> BG_COARSE_LOG2;                                                  \
        int cyc_hi = cy_hi >> BG_COARSE_LOG2;                                                  \
        const int cstep = 1 << BG_COARSE_LOG2; /* fine cells per coarse cell side */           \
                                                                                               \
        for(int cyc = cyc_lo; cyc <= cyc_hi; cyc++) {                                          \
            const uint64_t *crow = &bg->bm_coarse[cyc * bg->bm_coarse_row_u64];                \
            for(int cxc = cxc_lo; cxc <= cxc_hi; cxc++) {                                      \
                /* Skip coarse cells whose 8x8 fine block is entirely empty.               */  \
                if(!((crow[cxc >> 6] >> (cxc & 63)) & 1ull)) continue;                         \
                /* Compute the fine extent of this 8x8 block, clipped to the               */  \
                /* query's fine extent.                                                    */  \
                int fy0 = cyc * cstep, fy1 = fy0 + cstep;                                      \
                int fx0 = cxc * cstep, fx1 = fx0 + cstep;                                      \
                if(fy0 < cy_lo)     fy0 = cy_lo;                                               \
                if(fy1 > cy_hi + 1) fy1 = cy_hi + 1;                                           \
                if(fx0 < cx_lo)     fx0 = cx_lo;                                               \
                if(fx1 > cx_hi + 1) fx1 = cx_hi + 1;                                           \
                for(int fy = fy0; fy < fy1; fy++) {                                            \
                    const uint64_t *frow = &bg->bm_fine[fy * bg->bm_fine_row_u64];             \
                    /* Walk the fine bitmap row from word u0 to word u1, masking            */ \
                    /* out partial-word bits at the range boundaries.                      */  \
                    int u0 = fx0 >> 6, u1 = (fx1 - 1) >> 6;                                    \
                    for(int fu = u0; fu <= u1; fu++) {                                         \
                        uint64_t w = frow[fu];                                                 \
                        if(fu == u0) w = bg_keep_bits_at_or_above(w, fx0);                     \
                        if(fu == u1) w = bg_keep_bits_below(w, fx1);                           \
                        while(w) {                                                             \
                            int b = (int)SIMD_CTZ64(w);                                        \
                            uint64_t next_w = w & (w - 1ull);                                  \
                            /* Software prefetch: the next cell's xs/ys are                */  \
                            /* almost certainly cold in cache (cell-major hops              */ \
                            /* defeat the hardware prefetcher). Issue prefetch              */ \
                            /* hints while the current cell is processed.                  */  \
                            if(next_w) {                                                       \
                                int nb = (int)SIMD_CTZ64(next_w);                              \
                                const bg_cell(name) *nc =                                      \
                                    &bg->cells[fy * bg->grid_w + (fu * 64 + nb)];              \
                                if(nc->packed_size > 0) {                                      \
                                    _mm_prefetch((const char*)&bg->xs[nc->packed_start],       \
                                                 _MM_HINT_T0);                                 \
                                    _mm_prefetch((const char*)&bg->ys[nc->packed_start],       \
                                                 _MM_HINT_T0);                                 \
                                }                                                              \
                            }                                                                  \
                            w = next_w;                                                        \
                            int cx = fu * 64 + b;                                              \
                            const bg_cell(name) *c = &bg->cells[fy * bg->grid_w + cx];         \
                            /* Packed range first (the warm, contiguous data).             */  \
                            if(c->packed_size > 0) {                                           \
                                int32_t st = c->packed_start;                                  \
                                written = _bg_##name##_scan_rect(                              \
                                    &bg->xs[st], &bg->ys[st], &bg->records[st],                \
                                    c->packed_size,                                            \
                                    imnx, imxx, imny, imxy,                                    \
                                    out, written, maxout);                                     \
                                if(written >= maxout) return maxout;                           \
                            }                                                                  \
                            /* Then the overflow chain, scalar-style. After a               */ \
                            /* cleanup() this is empty for nearly every cell.               */ \
                            int32_t curr = c->overflow_head;                                   \
                            while(curr >= 0) {                                                 \
                                if(bg->xs[curr] >= imnx && bg->xs[curr] <= imxx                \
                                && bg->ys[curr] >= imny && bg->ys[curr] <= imxy) {             \
                                    out[written++] = bg->records[curr];                        \
                                    if(written >= maxout) return written;                      \
                                }                                                              \
                                curr = bg->nexts[curr];                                        \
                            }                                                                  \
                        }                                                                      \
                    }                                                                          \
                }                                                                              \
            }                                                                                  \
        }                                                                                      \
        return written;                                                                        \
    }                                                                                          \
                                                                                               \
    scope int bg_##name##_inrange_circle(bg(name) *bg,                                         \
                                         float x, float y, float range,                        \
                                         type *out, int maxout)                                \
    {                                                                                          \
        if(maxout <= 0 || range < 0.0f) return 0;                                              \
        int32_t icx = BG_SCALE_F(x);                                                           \
        int32_t icy = BG_SCALE_F(y);                                                           \
        int32_t ir  = BG_SCALE_F(range);                                                       \
        int64_t ir2 = (int64_t)ir * (int64_t)ir;                                               \
        int32_t imnx = icx - ir, imxx = icx + ir;                                              \
        int32_t imny = icy - ir, imxy = icy + ir;                                              \
        int cx_lo, cx_hi, cy_lo, cy_hi;                                                        \
        if(!_bg_##name##_cell_extent(bg, imnx, imxx, imny, imxy,                               \
                                     &cx_lo, &cx_hi, &cy_lo, &cy_hi)) return 0;                \
                                                                                               \
        if(!bg->dirty) {                                                                       \
            int64_t extent = (int64_t)(cx_hi - cx_lo + 1) * (int64_t)(cy_hi - cy_lo + 1);      \
            int64_t total  = (int64_t)bg->grid_w * (int64_t)bg->grid_h;                        \
            if(extent * _BG_WIDE_QUERY_DEN >= total * _BG_WIDE_QUERY_NUM) {                    \
                return _bg_##name##_scan_circle_long(                                          \
                    bg->xs, bg->ys, bg->records, (int32_t)bg->nrecs,                           \
                    icx, icy, ir2, out, 0, maxout);                                            \
            }                                                                                  \
        }                                                                                      \
                                                                                               \
        int written = 0;                                                                       \
        int cxc_lo = cx_lo >> BG_COARSE_LOG2;                                                  \
        int cxc_hi = cx_hi >> BG_COARSE_LOG2;                                                  \
        int cyc_lo = cy_lo >> BG_COARSE_LOG2;                                                  \
        int cyc_hi = cy_hi >> BG_COARSE_LOG2;                                                  \
        const int cstep = 1 << BG_COARSE_LOG2;                                                 \
                                                                                               \
        for(int cyc = cyc_lo; cyc <= cyc_hi; cyc++) {                                          \
            const uint64_t *crow = &bg->bm_coarse[cyc * bg->bm_coarse_row_u64];                \
            for(int cxc = cxc_lo; cxc <= cxc_hi; cxc++) {                                      \
                if(!((crow[cxc >> 6] >> (cxc & 63)) & 1ull)) continue;                         \
                int fy0 = cyc * cstep, fy1 = fy0 + cstep;                                      \
                int fx0 = cxc * cstep, fx1 = fx0 + cstep;                                      \
                if(fy0 < cy_lo)     fy0 = cy_lo;                                               \
                if(fy1 > cy_hi + 1) fy1 = cy_hi + 1;                                           \
                if(fx0 < cx_lo)     fx0 = cx_lo;                                               \
                if(fx1 > cx_hi + 1) fx1 = cx_hi + 1;                                           \
                for(int fy = fy0; fy < fy1; fy++) {                                            \
                    const uint64_t *frow = &bg->bm_fine[fy * bg->bm_fine_row_u64];             \
                    int u0 = fx0 >> 6, u1 = (fx1 - 1) >> 6;                                    \
                    for(int fu = u0; fu <= u1; fu++) {                                         \
                        uint64_t w = frow[fu];                                                 \
                        if(fu == u0) w = bg_keep_bits_at_or_above(w, fx0);                     \
                        if(fu == u1) w = bg_keep_bits_below(w, fx1);                           \
                        while(w) {                                                             \
                            int b = (int)SIMD_CTZ64(w);                                        \
                            uint64_t next_w = w & (w - 1ull);                                  \
                            if(next_w) {                                                       \
                                int nb = (int)SIMD_CTZ64(next_w);                              \
                                const bg_cell(name) *nc =                                      \
                                    &bg->cells[fy * bg->grid_w + (fu * 64 + nb)];              \
                                if(nc->packed_size > 0) {                                      \
                                    _mm_prefetch((const char*)&bg->xs[nc->packed_start],       \
                                                 _MM_HINT_T0);                                 \
                                    _mm_prefetch((const char*)&bg->ys[nc->packed_start],       \
                                                 _MM_HINT_T0);                                 \
                                }                                                              \
                            }                                                                  \
                            w = next_w;                                                        \
                            int cx = fu * 64 + b;                                              \
                            const bg_cell(name) *c = &bg->cells[fy * bg->grid_w + cx];         \
                            if(c->packed_size > 0) {                                           \
                                int32_t st = c->packed_start;                                  \
                                written = _bg_##name##_scan_circle(                            \
                                    &bg->xs[st], &bg->ys[st], &bg->records[st],                \
                                    c->packed_size,                                            \
                                    icx, icy, ir2,                                             \
                                    out, written, maxout);                                     \
                                if(written >= maxout) return maxout;                           \
                            }                                                                  \
                            int32_t curr = c->overflow_head;                                   \
                            while(curr >= 0) {                                                 \
                                int64_t dx = (int64_t)bg->xs[curr] - (int64_t)icx;             \
                                int64_t dy = (int64_t)bg->ys[curr] - (int64_t)icy;             \
                                if(dx * dx + dy * dy <= ir2) {                                 \
                                    out[written++] = bg->records[curr];                        \
                                    if(written >= maxout) return written;                      \
                                }                                                              \
                                curr = bg->nexts[curr];                                        \
                            }                                                                  \
                        }                                                                      \
                    }                                                                          \
                }                                                                              \
            }                                                                                  \
        }                                                                                      \
        return written;                                                                        \
    }                                                                                          \
                                                                                               \
    /* Cleanup: rebuilds the element pool so each cell's elements are physically           */  \
    /* contiguous, drains all overflow chains into packed ranges, resets the free          */  \
    /* list to empty, and prunes stale coarse-bitmap bits (cleared if and only             */  \
    /* if the entire 8x8 fine region they cover is empty). Called periodically             */  \
    /* by the engine (e.g. once per frame at a quiet moment); skipping a frame             */  \
    /* is fine -- correctness is unaffected, only the wide-query fast path                 */  \
    /* requires a clean tree.                                                              */  \
                                                                                               \
    scope void bg_##name##_cleanup(bg(name) *bg)                                               \
    {                                                                                          \
        if(bg->dirty) {                                                                        \
            int32_t live = (int32_t)bg->nrecs;                                                 \
            int32_t *nxs = NULL, *nys = NULL, *nn = NULL;                                      \
            type    *nrec = NULL;                                                              \
            if(live > 0) {                                                                     \
                nxs  = (int32_t*)PF_MALLOC((size_t)live * sizeof(int32_t));                       \
                nys  = (int32_t*)PF_MALLOC((size_t)live * sizeof(int32_t));                       \
                nn   = (int32_t*)PF_MALLOC((size_t)live * sizeof(int32_t));                       \
                nrec = (type*)   PF_MALLOC((size_t)live * sizeof(type));                          \
                if(!nxs || !nys || !nn || !nrec) {                                             \
                    PF_FREE(nxs); PF_FREE(nys); PF_FREE(nn); PF_FREE(nrec);                                \
                    /* OOM -- leave the pool dirty. Queries still work; the                */  \
                    /* wide-query fast path will just stay disabled until next             */  \
                    /* successful cleanup.                                                 */  \
                    return;                                                                    \
                }                                                                              \
            }                                                                                  \
                                                                                               \
            /* Walk cells in row-major order, copying packed elements then                 */  \
            /* overflow elements into the new contiguous pool. Update each                 */  \
            /* cell's metadata to point at its new packed range.                           */  \
            int32_t cursor = 0;                                                                \
            size_t ncells = (size_t)bg->grid_w * (size_t)bg->grid_h;                           \
            for(size_t i = 0; i < ncells; i++) {                                               \
                bg_cell(name) *c = &bg->cells[i];                                              \
                int32_t cell_start = cursor;                                                   \
                if(c->packed_size > 0) {                                                       \
                    int32_t st = c->packed_start;                                              \
                    memcpy(&nxs[cursor],  &bg->xs[st],                                         \
                           (size_t)c->packed_size * sizeof(int32_t));                          \
                    memcpy(&nys[cursor],  &bg->ys[st],                                         \
                           (size_t)c->packed_size * sizeof(int32_t));                          \
                    memcpy(&nrec[cursor], &bg->records[st],                                    \
                           (size_t)c->packed_size * sizeof(type));                             \
                    cursor += c->packed_size;                                                  \
                }                                                                              \
                int32_t curr = c->overflow_head;                                               \
                while(curr >= 0) {                                                             \
                    nxs[cursor]  = bg->xs[curr];                                               \
                    nys[cursor]  = bg->ys[curr];                                               \
                    nrec[cursor] = bg->records[curr];                                          \
                    cursor++;                                                                  \
                    curr = bg->nexts[curr];                                                    \
                }                                                                              \
                int32_t cell_size = cursor - cell_start;                                       \
                if(cell_size > 0) {                                                            \
                    c->packed_start = cell_start;                                              \
                    c->packed_size  = cell_size;                                               \
                } else {                                                                       \
                    c->packed_start = -1;                                                      \
                    c->packed_size  = 0;                                                       \
                }                                                                              \
                c->overflow_head = -1;                                                         \
            }                                                                                  \
            assert(cursor == live);                                                            \
            PF_FREE(bg->xs); PF_FREE(bg->ys); PF_FREE(bg->nexts); PF_FREE(bg->records);                    \
            bg->xs = nxs; bg->ys = nys; bg->nexts = nn; bg->records = nrec;                    \
            bg->elts_cap = live;                                                               \
            bg->elts_size = live;                                                              \
            bg->first_free = -1;                                                               \
            bg->dirty = false;                                                                 \
        }                                                                                      \
                                                                                               \
        /* Prune stale coarse-bitmap bits. For each set coarse bit, OR-reduce              */  \
        /* the 8x8 fine bits it covers; if they're all zero, clear the coarse              */  \
        /* bit. This is the only place coarse bits are ever cleared.                       */  \
        const int cstep = 1 << BG_COARSE_LOG2;                                                 \
        for(int cyc = 0; cyc < bg->coarse_h; cyc++) {                                          \
            uint64_t *cr = &bg->bm_coarse[cyc * bg->bm_coarse_row_u64];                        \
            for(int u = 0; u < bg->bm_coarse_row_u64; u++) {                                   \
                uint64_t bits = cr[u];                                                         \
                while(bits) {                                                                  \
                    int b = (int)SIMD_CTZ64(bits);                                             \
                    bits &= bits - 1ull;                                                       \
                    int cxc = u * 64 + b;                                                      \
                    if(cxc >= bg->coarse_w) break;                                             \
                    int fy0 = cyc * cstep, fy1 = fy0 + cstep;                                  \
                    if(fy1 > bg->grid_h) fy1 = bg->grid_h;                                     \
                    int fx0 = cxc * cstep, fx1 = fx0 + cstep;                                  \
                    if(fx1 > bg->grid_w) fx1 = bg->grid_w;                                     \
                    uint64_t any = 0;                                                          \
                    for(int fy = fy0; fy < fy1 && !any; fy++) {                                \
                        uint64_t *fr = &bg->bm_fine[fy * bg->bm_fine_row_u64];                 \
                        int u0 = fx0 >> 6, u1 = (fx1 - 1) >> 6;                                \
                        for(int fu = u0; fu <= u1; fu++) {                                     \
                            uint64_t fw = fr[fu];                                              \
                            if(fu == u0) fw = bg_keep_bits_at_or_above(fw, fx0);               \
                            if(fu == u1) fw = bg_keep_bits_below(fw, fx1);                     \
                            any |= fw;                                                         \
                            if(any) break;                                                     \
                        }                                                                      \
                    }                                                                          \
                    if(!any) cr[u] &= ~(1ull << b);                                            \
                }                                                                              \
            }                                                                                  \
        }                                                                                      \
    }                                                                                          \
                                                                                               \
    scope void bg_##name##_print(bg(name) *bg)                                                 \
    {                                                                                          \
        printf("bg: grid %dx%d (coarse %dx%d), nrecs=%zu, pool size=%d cap=%d, dirty=%d\n",    \
            bg->grid_w, bg->grid_h, bg->coarse_w, bg->coarse_h,                                \
            bg->nrecs, bg->elts_size, bg->elts_cap, (int)bg->dirty);                           \
    }

#endif /* BITMAP_GRID_H */

