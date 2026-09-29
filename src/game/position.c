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

#define MEM_FILE_SYS MEM_SYS_GAME
#define MEM_FILE_SUB MEM_SUB_GAME_POSITION

#include "position.h"
#include "game_private.h"
#include "movement.h"
#include "building.h"
#include "resource.h"
#include "fog_of_war.h"
#include "combat.h"
#include "region.h"
#include "public/game.h"
#include "../main.h"
#include "../event.h"
#include "../perf.h"
#include "../sched.h"
#include "../mem.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"

#include <assert.h>
#include <float.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_POSITION)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_POSITION)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_POSITION)


BITMAP_GRID_IMPL(extern, ent, uint32_t)
__KHASH_IMPL(pos,  extern, khint32_t, vec3_t, 1, kh_int_hash_func, kh_int_hash_equal)
struct reach_entry{
    float reach;
    int   band;
};

KHASH_MAP_INIT_INT(reach, struct reach_entry)

#define POSBUF_INIT_SIZE (16384)
/* Entities whose box can reach further than this from their position are
 * also kept in a grid of their own, so that a search that must find every box
 * near a point pads the main grid by this much only.
 */
#define SMALL_REACH      (32.0f)
/* Wider entities are banded by reach, each band doubling the last, so that one
 * huge prop does not pad the search for every wide unit.
 */
#define NREACH_BANDS     (6)
#define MAX_SEARCH_ENTS  (8192)
#define MAX(a, b)        ((a) > (b) ? (a) : (b))
#define MIN(a, b)        ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)      (sizeof(a)/sizeof(a[0]))

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(pos) *s_postable;
/* The bitmap_grid is always synchronized with the postable, at function call boundaries */
static bg_ent_t      s_postree;
/* The reach and band of every entity beyond SMALL_REACH, and per band a grid
 * of the positioned ones, their count and an upper bound of their reach.
 */
static khash_t(reach) *s_wide;
static bg_ent_t        s_widetree[NREACH_BANDS];
static size_t          s_wide_count[NREACH_BANDS];
static float           s_wide_max[NREACH_BANDS];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool any_ent(uint32_t uid, void *arg)
{
    return true;
}

static bool uids_equal(const uint32_t *a, const uint32_t *b)
{
    return (*a == *b);
}

static void wide_remove(uint32_t uid, bool positioned, vec2_t xz);

static int filter_garrisoned(khash_t(id) *flags, uint32_t *candidates, int count)
{
    int ret = count;
    for(int i = count-1; i >=0; i--) {
        uint32_t curr = candidates[i];
        uint32_t f;
        if(flags) {
            f = G_FlagsGetFrom(flags, curr);
        }else{
            if(!G_EntityExists(curr))
                continue;
            f = G_FlagsGet(curr);
        }
        if(f & ENTITY_FLAG_GARRISONED) {
            candidates[i] = candidates[ret - 1];
            ret--;
        }
    }
    return ret;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

static bool pos_set(uint32_t uid, vec3_t pos, bool notify_move)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(pos, s_postable, uid);
    bool overwrite = (k != kh_end(s_postable));
    int faction_id = G_GetFactionID(uid);
    uint32_t flags = G_FlagsGet(uid);
    float vrange = G_Fog_Enabled() ? G_GetVisionRange(uid) : 0.0f;

    vec2_t old_xz = {0};
    if(overwrite) {
        vec3_t old_pos = kh_val(s_postable, k);
        old_xz = (vec2_t){old_pos.x, old_pos.z};
        if(!bg_ent_update(&s_postree, old_pos.x, old_pos.z, pos.x, pos.z, uid))
            return false;
        kh_val(s_postable, k) = pos;
        Entity_DirtyModelMatrix(uid);

        G_Combat_MoveRef(faction_id, (vec2_t){old_pos.x, old_pos.z},
            (vec2_t){pos.x, pos.z});
        G_Region_MoveRef(uid, (vec2_t){old_pos.x, old_pos.z}, (vec2_t){pos.x, pos.z});
        G_Fog_UpdateVision((vec2_t){old_pos.x, old_pos.z}, (vec2_t){pos.x, pos.z},
            faction_id, vrange);
    }else{
        if(!bg_ent_insert(&s_postree, pos.x, pos.z, uid))
            return false;

        int ret;
        kh_put(pos, s_postable, uid, &ret);
        if(ret == -1) {
            bg_ent_delete(&s_postree, pos.x, pos.z, uid);
            return false;
        }
        k = kh_get(pos, s_postable, uid);
        kh_val(s_postable, k) = pos;
        Entity_DirtyModelMatrix(uid);

        G_Combat_AddRef(faction_id, (vec2_t){pos.x, pos.z});
        G_Region_AddRef(uid, (vec2_t){pos.x, pos.z});
        G_Fog_AddVision((vec2_t){pos.x, pos.z}, faction_id, vrange);
    }
    assert(kh_size(s_postable) == s_postree.nrecs);

    khiter_t w;
    if(kh_size(s_wide) > 0 && (w = kh_get(reach, s_wide, uid)) != kh_end(s_wide)) {
        bg_ent_t *grid = &s_widetree[kh_val(s_wide, w).band];
        if(overwrite)
            bg_ent_update(grid, old_xz.x, old_xz.z, pos.x, pos.z, uid);
        else
            bg_ent_insert(grid, pos.x, pos.z, uid);
    }

    if(notify_move)
        G_Move_UpdatePos(uid, (vec2_t){pos.x, pos.z});
    if(flags & ENTITY_FLAG_BUILDING)
        G_Building_UpdateBounds(uid);
    if(flags & ENTITY_FLAG_RESOURCE)
        G_Resource_UpdateBounds(uid);

    return true;
}

bool G_Pos_Set(uint32_t uid, vec3_t pos)
{
    return pos_set(uid, pos, true);
}

bool G_Pos_SetFromMovement(uint32_t uid, vec3_t pos)
{
    return pos_set(uid, pos, false);
}

vec3_t G_Pos_Get(uint32_t uid)
{
    khiter_t k = kh_get(pos, s_postable, uid);
    assert(k != kh_end(s_postable));
    return kh_val(s_postable, k);
}

vec2_t G_Pos_GetXZ(uint32_t uid)
{
    khiter_t k = kh_get(pos, s_postable, uid);
    assert(k != kh_end(s_postable));
    vec3_t pos = kh_val(s_postable, k);
    return (vec2_t){pos.x, pos.z};
}

khash_t(pos) *G_Pos_CopyTable(void)
{
    return kh_copy_pos(s_postable);
}

khash_t(pos) *G_Pos_CopyTableInto(khash_t(pos) *dst)
{
    return kh_copy_into_pos(s_postable, dst);
}

vec3_t G_Pos_GetFrom(khash_t(pos) *table, uint32_t uid)
{
    khiter_t k = kh_get(pos, table, uid);
    assert(k != kh_end(table));
    return kh_val(table, k);
}

vec2_t G_Pos_GetXZFrom(khash_t(pos) *table, uint32_t uid)
{
    khiter_t k = kh_get(pos, table, uid);
    assert(k != kh_end(table));
    vec3_t pos = kh_val(table, k);
    return (vec2_t){pos.x, pos.z};
}

bool G_Pos_HasFrom(khash_t(pos) *table, uint32_t uid)
{
    return (kh_get(pos, table, uid) != kh_end(table));
}

void G_Pos_Delete(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(pos, s_postable, uid);
    assert(k != kh_end(s_postable));

    vec3_t pos = kh_val(s_postable, k);
    kh_del(pos, s_postable, k);
    Entity_DirtyModelMatrix(uid);

    bool ret = bg_ent_delete(&s_postree, pos.x, pos.z, uid);
    assert(ret);
    assert(kh_size(s_postable) == s_postree.nrecs);

    wide_remove(uid, true, (vec2_t){pos.x, pos.z});
}

static int reach_band(float reach)
{
    int band = 0;
    float bound = SMALL_REACH * 2.0f;
    while(reach > bound && band < NREACH_BANDS - 1) {
        bound *= 2.0f;
        band++;
    }
    return band;
}

static void wide_remove(uint32_t uid, bool positioned, vec2_t xz)
{
    khiter_t w = kh_get(reach, s_wide, uid);
    if(w == kh_end(s_wide))
        return;
    int band = kh_val(s_wide, w).band;
    if(positioned)
        bg_ent_delete(&s_widetree[band], xz.x, xz.z, uid);
    s_wide_count[band]--;
    kh_del(reach, s_wide, w);
}

void G_Pos_SetReach(uint32_t uid, float reach)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(pos, s_postable, uid);
    bool positioned = (k != kh_end(s_postable));
    vec3_t pos = positioned ? kh_val(s_postable, k) : (vec3_t){0};
    vec2_t xz = (vec2_t){pos.x, pos.z};

    khiter_t w = kh_get(reach, s_wide, uid);
    int band = reach_band(reach);
    if(w != kh_end(s_wide) && (reach <= SMALL_REACH || kh_val(s_wide, w).band != band)) {
        wide_remove(uid, positioned, xz);
        w = kh_end(s_wide);
    }
    if(reach <= SMALL_REACH)
        return;

    if(w == kh_end(s_wide)) {
        int status;
        w = kh_put(reach, s_wide, uid, &status);
        assert(status != -1);
        if(positioned)
            bg_ent_insert(&s_widetree[band], xz.x, xz.z, uid);
        s_wide_count[band]++;
    }
    kh_val(s_wide, w) = (struct reach_entry){reach, band};
    s_wide_max[band] = MAX(s_wide_max[band], reach);
}

void G_Pos_Garrison(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(pos, s_postable, uid);
    assert(k != kh_end(s_postable));
    vec3_t old_pos = kh_val(s_postable, k);
    float vrange = G_GetVisionRange(uid);

    G_Combat_RemoveRef(G_GetFactionID(uid), (vec2_t){old_pos.x, old_pos.z});
    G_Region_RemoveRef(uid, (vec2_t){old_pos.x, old_pos.z});
    G_Fog_RemoveVision((vec2_t){old_pos.x, old_pos.z}, G_GetFactionID(uid), vrange);
}

void G_Pos_Ungarrison(uint32_t uid, vec3_t pos)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(pos, s_postable, uid);
    assert(k != kh_end(s_postable));

    vec3_t old_pos = kh_val(s_postable, k);
    bg_ent_delete(&s_postree, old_pos.x, old_pos.z, uid);
    bg_ent_insert(&s_postree, pos.x, pos.z, uid);
    khiter_t w = kh_get(reach, s_wide, uid);
    if(w != kh_end(s_wide))
        bg_ent_update(&s_widetree[kh_val(s_wide, w).band], old_pos.x, old_pos.z, pos.x, pos.z, uid);

    kh_val(s_postable, k) = pos;
    float vrange = G_GetVisionRange(uid);

    G_Combat_AddRef(G_GetFactionID(uid), (vec2_t){pos.x, pos.z});
    G_Region_AddRef(uid, (vec2_t){pos.x, pos.z});
    G_Fog_AddVision((vec2_t){pos.x, pos.z}, G_GetFactionID(uid), vrange);
}

/* Once-per-frame hook: pack the pool back into a contiguous cell-major layout
 * and prune stale coarse-bitmap bits. Lets the wide-query fast path stay
 * armed on the steady state and keeps the per-cell scans hitting the warm
 * packed runs instead of overflow chains.
 */
static void on_update_start(void *user, void *event)
{
    PERF_PUSH("position::on_update_start");
    bg_ent_cleanup(&s_postree);
    for(int i = 0; i < NREACH_BANDS; i++)
        bg_ent_cleanup(&s_widetree[i]);
    PERF_POP();
}

bool G_Pos_Init(const struct map *map)
{
    ASSERT_IN_MAIN_THREAD();

    if(NULL == (s_postable = kh_init(pos)))
        return false;
    if(kh_resize(pos, s_postable, POSBUF_INIT_SIZE) < 0)
        return false;

    struct map_resolution res;
    M_GetResolution(map, &res);

    vec3_t center = M_GetCenterPos(map);

    float xmin = center.x - (res.tile_w * res.chunk_w * X_COORDS_PER_TILE) / 2.0f;
    float xmax = center.x + (res.tile_w * res.chunk_w * X_COORDS_PER_TILE) / 2.0f;
    float zmin = center.z - (res.tile_h * res.chunk_h * Z_COORDS_PER_TILE) / 2.0f;
    float zmax = center.z + (res.tile_h * res.chunk_h * Z_COORDS_PER_TILE) / 2.0f;

    bg_ent_init(&s_postree, xmin, xmax, zmin, zmax, uids_equal);
    if(!bg_ent_reserve(&s_postree, POSBUF_INIT_SIZE)) {
        kh_destroy(pos, s_postable);
        return false;
    }

    if(NULL == (s_wide = kh_init(reach))) {
        bg_ent_destroy(&s_postree);
        kh_destroy(pos, s_postable);
        return false;
    }
    for(int i = 0; i < NREACH_BANDS; i++) {
        bg_ent_init(&s_widetree[i], xmin, xmax, zmin, zmax, uids_equal);
        s_wide_count[i] = 0;
        s_wide_max[i] = 0.0f;
    }

    E_Global_Register(EVENT_UPDATE_START, on_update_start, NULL, G_ALL);
    return true;
}

void G_Pos_Shutdown(void)
{
    ASSERT_IN_MAIN_THREAD();

    E_Global_Unregister(EVENT_UPDATE_START, on_update_start);
    kh_destroy(pos, s_postable);
    bg_ent_destroy(&s_postree);
    kh_destroy(reach, s_wide);
    for(int i = 0; i < NREACH_BANDS; i++)
        bg_ent_destroy(&s_widetree[i]);
}

int G_Pos_EntsInRect(vec2_t xz_min, vec2_t xz_max, uint32_t *out, size_t maxout)
{
    PERF_ENTER();
    int ret = bg_ent_inrange_rect(&s_postree, 
        xz_min.x, xz_max.x, xz_min.z, xz_max.z, out, maxout);
    ret = filter_garrisoned(NULL, out, ret);
    PERF_RETURN(ret);
}

int G_Pos_EntsInRectFrom(bg_ent_t *tree, khash_t(id) *flags,
                         vec2_t xz_min, vec2_t xz_max, uint32_t *out, size_t maxout)
{
    return bg_ent_inrange_rect(tree, xz_min.x, xz_max.x, xz_min.z, xz_max.z, out, maxout);
}

int G_Pos_EntsInRectWithPred(vec2_t xz_min, vec2_t xz_max, uint32_t *out, size_t maxout,
                             bool (*predicate)(uint32_t ent, void *arg), void *arg)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();

    STALLOC(uint32_t, ent_ids, maxout);

    int ntotal = bg_ent_inrange_rect(&s_postree, 
        xz_min.x, xz_max.x, xz_min.z, xz_max.z, ent_ids, maxout);
    ntotal = filter_garrisoned(NULL, ent_ids, ntotal);
    int ret = 0;

    for(int i = 0; i < ntotal; i++) {

        uint32_t curr = ent_ids[i];
        if(!predicate(curr, arg))
            continue;

        out[ret++] = curr;
    }
    STFREE(ent_ids);
    PERF_RETURN(ret);
}

int G_Pos_EntsInCircle(vec2_t xz_point, float range, uint32_t *out, size_t maxout)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();
    int ret = bg_ent_inrange_circle(&s_postree, 
        xz_point.x, xz_point.z, range, out, maxout);
    ret = filter_garrisoned(NULL, out, ret);
    PERF_RETURN(ret);
}

/* Appends every entity of the grid within the circle: the grid can only
 * report a full buffer by filling it, so a full result is queried again with
 * room for every entity the grid holds.
 */
static bool grid_circle_append(bg_ent_t *grid, size_t nrecs, vec2_t center, float range,
                               vec_entity_t *out)
{
    size_t base = out->size;
    size_t cap = MAX(out->capacity - base, 256);
    int ret = 0;
    for(;;) {
        if(!vec_entity_resize(out, base + cap))
            return false;
        ret = bg_ent_inrange_circle(grid, center.x, center.z, range, out->array + base, cap);
        if(ret < (int)cap || cap >= nrecs)
            break;
        cap = nrecs;
    }
    out->size = base + ret;
    return true;
}

/* Every entity whose box, in any pose and rotation, can touch the segment:
 * such an entity's position lies within its reach of some point of the
 * segment, so within half the segment's length plus its reach of the
 * segment's middle.
 */
int G_Pos_EntsNearSegment(vec2_t a, vec2_t b, vec_entity_t *out)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();

    vec2_t mid = (vec2_t){(a.x + b.x) / 2.0f, (a.z + b.z) / 2.0f};
    vec2_t delta;
    PFM_Vec2_Sub(&b, &a, &delta);
    float half = PFM_Vec2_Len(&delta) / 2.0f;

    out->size = 0;
    if(!grid_circle_append(&s_postree, kh_size(s_postable), mid, half + SMALL_REACH, out)) {
        out->size = 0;
        PERF_RETURN(0);
    }
    /* The wide ones come from their own grid */
    if(kh_size(s_wide) > 0) {
        size_t n = 0;
        for(size_t i = 0; i < out->size; i++) {
            uint32_t uid = out->array[i];
            if(kh_get(reach, s_wide, uid) == kh_end(s_wide))
                out->array[n++] = uid;
        }
        out->size = n;
        for(int i = 0; i < NREACH_BANDS; i++) {
            if(s_wide_count[i] == 0)
                continue;
            if(!grid_circle_append(&s_widetree[i], s_wide_count[i], mid, half + s_wide_max[i], out)) {
                out->size = 0;
                PERF_RETURN(0);
            }
        }
    }
    out->size = filter_garrisoned(NULL, out->array, out->size);
    PERF_RETURN(out->size);
}

int G_Pos_EntsInCircleWithPred(vec2_t xz_point, float range, uint32_t *out, size_t maxout,
                               bool (*predicate)(uint32_t ent, void *arg), void *arg)
{
    ASSERT_IN_MAIN_THREAD();
    return G_Pos_EntsInCircleWithPredFrom(&s_postree, NULL, xz_point, range, out, maxout, predicate, arg);
}

bg_ent_t *G_Pos_CopyBitmapGrid(void)
{
    ASSERT_IN_MAIN_THREAD();
    /* Pack before snapshotting so the worker's copy keeps the wide-query
     * fast path armed and avoids per-cell overflow walks.
     */
    bg_ent_cleanup(&s_postree);
    bg_ent_t *ret = PF_MALLOC(sizeof(bg_ent_t));
    if(!ret)
        return NULL;
    bg_ent_copy(&s_postree, ret);
    return ret;
}

bg_ent_t *G_Pos_CopyBitmapGridInto(bg_ent_t *dst)
{
    ASSERT_IN_MAIN_THREAD();
    if(!dst)
        return G_Pos_CopyBitmapGrid();
    bg_ent_cleanup(&s_postree);
    bg_ent_copy_into(&s_postree, dst);
    return dst;
}

void G_Pos_DestroyBitmapGrid(bg_ent_t *tree)
{
    bg_ent_destroy(tree);
    PF_FREE(tree);
}

int G_Pos_EntsInCircleFrom(bg_ent_t *tree, khash_t(id) *flags, vec2_t xz_point, float range, 
                           uint32_t *out, size_t maxout)
{
    PERF_ENTER();
    int ret = bg_ent_inrange_circle(tree, xz_point.x, xz_point.z, range, out, maxout);
    ret = filter_garrisoned(flags, out, ret);
    PERF_RETURN(ret);
}

int G_Pos_EntsInCircleWithPredFrom(bg_ent_t *tree, khash_t(id) *flags, vec2_t xz_point, float range, 
                                   uint32_t *out, size_t maxout,
                                   bool (*predicate)(uint32_t ent, void *arg), void *arg)
{
    PERF_ENTER();
    assert(Sched_UsingBigStack());

    STALLOC(uint32_t, ent_ids, maxout);

    int ntotal = bg_ent_inrange_circle(tree, 
        xz_point.x, xz_point.z, range, ent_ids, maxout);
    ntotal = filter_garrisoned(flags, ent_ids, ntotal);
    int ret = 0;

    for(int i = 0; i < ntotal; i++) {

        uint32_t curr = ent_ids[i];
        if(!predicate(curr, arg))
            continue;

        out[ret++] = curr;
    }

    STFREE(ent_ids);
    PERF_RETURN(ret);
}

uint32_t G_Pos_NearestWithPred(vec2_t xz_point, 
                               bool (*predicate)(uint32_t ent, void *arg), 
                               void *arg, float max_range)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();
    assert(Sched_UsingBigStack());

    uint32_t ent_ids[MAX_SEARCH_ENTS];
    const float bg_len = MAX(s_postree.xmax - s_postree.xmin, s_postree.ymax - s_postree.ymin);
    float len = (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE) / 8.0f;

    if(max_range == 0.0) {
        max_range = bg_len;
    }
    max_range = MIN(bg_len, max_range);

    while(len <= max_range) {

        float min_dist = FLT_MAX;
        uint32_t ret = NULL_UID;

        int num_cands = bg_ent_inrange_circle(&s_postree, xz_point.x, xz_point.z,
            len, ent_ids, ARR_SIZE(ent_ids));
        num_cands = filter_garrisoned(NULL, ent_ids, num_cands);

        for(int i = 0; i < num_cands; i++) {
        
            uint32_t curr = ent_ids[i];
            vec2_t delta, can_pos_xz = G_Pos_GetXZ(curr);
            PFM_Vec2_Sub(&xz_point, &can_pos_xz, &delta);

            if(PFM_Vec2_Len(&delta) < min_dist && predicate(curr, arg)) {
                min_dist = PFM_Vec2_Len(&delta);
                ret = curr;
            }
        }

        if(ret != NULL_UID)
            PERF_RETURN(ret);

        if(len == max_range)
            break;

        len *= 2.0f; 
        len = MIN(max_range, len);
    }
    PERF_RETURN(NULL_UID);
}

uint32_t G_Pos_NearestWithPredFrom(bg_ent_t *tree, khash_t(pos) *positions,
                                   khash_t(id) *flags, vec2_t xz_point,
                                   bool (*predicate)(uint32_t ent, void *arg), void *arg,
                                   float max_range)
{
    PERF_ENTER();
    assert(Sched_UsingBigStack());

    uint32_t ent_ids[MAX_SEARCH_ENTS];
    const float bg_len = MAX(tree->xmax - tree->xmin, tree->ymax - tree->ymin);
    float len = (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE) / 8.0f;

    if(max_range == 0.0) {
        max_range = bg_len;
    }
    max_range = MIN(bg_len, max_range);

    while(len <= max_range) {

        float min_dist = FLT_MAX;
        uint32_t ret = NULL_UID;

        int num_cands = bg_ent_inrange_circle(tree, xz_point.x, xz_point.z,
            len, ent_ids, ARR_SIZE(ent_ids));
        num_cands = filter_garrisoned(flags, ent_ids, num_cands);

        for(int i = 0; i < num_cands; i++) {

            uint32_t curr = ent_ids[i];
            vec2_t delta, can_pos_xz = G_Pos_GetXZFrom(positions, curr);
            PFM_Vec2_Sub(&xz_point, &can_pos_xz, &delta);

            if(PFM_Vec2_Len(&delta) < min_dist && predicate(curr, arg)) {
                min_dist = PFM_Vec2_Len(&delta);
                ret = curr;
            }
        }

        if(ret != NULL_UID)
            PERF_RETURN(ret);

        if(len == max_range)
            break;

        len *= 2.0f;
        len = MIN(max_range, len);
    }
    PERF_RETURN(NULL_UID);
}

uint32_t G_Pos_Nearest(vec2_t xz_point)
{
    ASSERT_IN_MAIN_THREAD();

    return G_Pos_NearestWithPred(xz_point, any_ent, NULL, 0.0);
}
