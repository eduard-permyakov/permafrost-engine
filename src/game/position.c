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
#include "../perf.h"
#include "../sched.h"
#include "../lib/public/mem.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"

#include <assert.h>
#include <float.h>


QUADTREE_IMPL(extern, ent, uint32_t)
__KHASH_IMPL(pos,  extern, khint32_t, vec3_t, 1, kh_int_hash_func, kh_int_hash_equal)

#define POSBUF_INIT_SIZE (16384)
#define MAX_SEARCH_ENTS  (8192)
#define MAX(a, b)        ((a) > (b) ? (a) : (b))
#define MIN(a, b)        ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)      (sizeof(a)/sizeof(a[0]))

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(pos) *s_postable;
/* The quadtree is always synchronized with the postable, at function call boundaries */
static qt_ent_t      s_postree;

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

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Pos_Set(uint32_t uid, vec3_t pos)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(pos, s_postable, uid);
    bool overwrite = (k != kh_end(s_postable));
    float vrange = G_GetVisionRange(uid);

    if(overwrite) {
        vec3_t old_pos = kh_val(s_postable, k);
        bool ret = qt_ent_delete(&s_postree, old_pos.x, old_pos.z, uid);
        assert(ret);

        G_Combat_RemoveRef(G_GetFactionID(uid), (vec2_t){old_pos.x, old_pos.z});
        G_Region_RemoveRef(uid, (vec2_t){old_pos.x, old_pos.z});
        G_Fog_RemoveVision((vec2_t){old_pos.x, old_pos.z}, G_GetFactionID(uid), vrange);
    }

    if(!qt_ent_insert(&s_postree, pos.x, pos.z, uid))
        return false;

    if(!overwrite) {
        int ret;
        kh_put(pos, s_postable, uid, &ret); 
        if(ret == -1) {
            qt_ent_delete(&s_postree, pos.x, pos.z, uid);
            return false;
        }
        k = kh_get(pos, s_postable, uid);
    }

    kh_val(s_postable, k) = pos;
    assert(kh_size(s_postable) == s_postree.nrecs);

    G_Move_UpdatePos(uid, (vec2_t){pos.x, pos.z});
    G_Combat_AddRef(G_GetFactionID(uid), (vec2_t){pos.x, pos.z});
    G_Region_AddRef(uid, (vec2_t){pos.x, pos.z});
    G_Building_UpdateBounds(uid);
    G_Resource_UpdateBounds(uid);
    G_Fog_AddVision((vec2_t){pos.x, pos.z}, G_GetFactionID(uid), vrange);

    return true; 
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

void G_Pos_Delete(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(pos, s_postable, uid);
    assert(k != kh_end(s_postable));

    vec3_t pos = kh_val(s_postable, k);
    kh_del(pos, s_postable, k);

    bool ret = qt_ent_delete(&s_postree, pos.x, pos.z, uid);
    assert(ret);
    assert(kh_size(s_postable) == s_postree.nrecs);
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

    qt_ent_init(&s_postree, xmin, xmax, zmin, zmax, uids_equal);
    if(!qt_ent_reserve(&s_postree, POSBUF_INIT_SIZE)) {
        kh_destroy(pos, s_postable);
        return false;
    }

    return true;
}

void G_Pos_Shutdown(void)
{
    ASSERT_IN_MAIN_THREAD();

    kh_destroy(pos, s_postable);
    qt_ent_destroy(&s_postree);
}

int G_Pos_EntsInRect(vec2_t xz_min, vec2_t xz_max, uint32_t *out, size_t maxout)
{
    PERF_ENTER();
    int ret = qt_ent_inrange_rect(&s_postree, 
        xz_min.x, xz_max.x, xz_min.z, xz_max.z, out, maxout);
    PERF_RETURN(ret);
}

int G_Pos_EntsInRectWithPred(vec2_t xz_min, vec2_t xz_max, uint32_t *out, size_t maxout,
                             bool (*predicate)(uint32_t ent, void *arg), void *arg)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();

    STALLOC(uint32_t, ent_ids, maxout);

    int ntotal = qt_ent_inrange_rect(&s_postree, 
        xz_min.x, xz_max.x, xz_min.z, xz_max.z, ent_ids, maxout);
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
    int ret = qt_ent_inrange_circle(&s_postree, 
        xz_point.x, xz_point.z, range, out, maxout);
    PERF_RETURN(ret);
}

int G_Pos_EntsInCircleWithPred(vec2_t xz_point, float range, uint32_t *out, size_t maxout,
                               bool (*predicate)(uint32_t ent, void *arg), void *arg)
{
    ASSERT_IN_MAIN_THREAD();
    return G_Pos_EntsInCircleWithPredFrom(&s_postree, xz_point, range, out, maxout, predicate, arg);
}

qt_ent_t *G_Pos_CopyQuadTree(void)
{
    qt_ent_t *ret = malloc(sizeof(qt_ent_t));
    if(!ret)
        return NULL;
    qt_ent_copy(&s_postree, ret);
    return ret;
}

void G_Pos_DestroyQuadTree(qt_ent_t *tree)
{
    qt_ent_destroy(tree);
    free(tree);
}

int G_Pos_EntsInCircleFrom(qt_ent_t *tree, vec2_t xz_point, float range, 
                           uint32_t *out, size_t maxout)
{
    PERF_ENTER();
    int ret = qt_ent_inrange_circle(tree, xz_point.x, xz_point.z, range, out, maxout);
    PERF_RETURN(ret);
}

int G_Pos_EntsInCircleWithPredFrom(qt_ent_t *tree, vec2_t xz_point, float range, 
                                   uint32_t *out, size_t maxout,
                                   bool (*predicate)(uint32_t ent, void *arg), void *arg)
{
    PERF_ENTER();
    assert(Sched_UsingBigStack());

    STALLOC(uint32_t, ent_ids, maxout);

    int ntotal = qt_ent_inrange_circle(tree, 
        xz_point.x, xz_point.z, range, ent_ids, maxout);
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
    const float qt_len = MAX(s_postree.xmax - s_postree.xmin, s_postree.ymax - s_postree.ymin);
    float len = (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE) / 8.0f;

    if(max_range == 0.0) {
        max_range = qt_len;
    }
    max_range = MIN(qt_len, max_range);

    while(len <= max_range) {

        float min_dist = FLT_MAX;
        uint32_t ret = NULL_UID;

        int num_cands = qt_ent_inrange_circle(&s_postree, xz_point.x, xz_point.z,
            len, ent_ids, ARR_SIZE(ent_ids));

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

uint32_t G_Pos_Nearest(vec2_t xz_point)
{
    ASSERT_IN_MAIN_THREAD();

    return G_Pos_NearestWithPred(xz_point, any_ent, NULL, 0.0);
}

void G_Pos_Upload(void)
{
    PERF_ENTER();

    const size_t max_ents = kh_size(s_postable);
    struct render_workspace *ws = G_GetSimWS();
    vec3_t *buff = stalloc(&ws->args, max_ents * sizeof(vec3_t));
    uint32_t *gpu_idbuff = stalloc(&ws->args, max_ents * sizeof(uint32_t));

    uint32_t uid;
    vec3_t curr;
    size_t nents = 0;

    kh_foreach(s_postable, uid, curr, {

        uint32_t gpu_id = G_GPUIDForEnt(uid);
        if(gpu_id == 0)
            continue;

        buff[nents] = curr;
        gpu_idbuff[nents] = gpu_id;
        nents++;
    });
    assert(nents == kh_size(G_GetDynamicEntsSet()));

    R_PushCmd((struct rcmd){
        .func = R_GL_PositionsUploadData,
        .nargs = 4,
        .args = {
            buff,
            gpu_idbuff,
            R_PushArg(&nents, sizeof(nents)),
            (void*)G_GetPrevTickMap()
        },
    });

    PERF_RETURN_VOID();
}

