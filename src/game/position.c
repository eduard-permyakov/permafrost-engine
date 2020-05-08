/* 
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2020 Eduard Permyakov 
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

#include "game_private.h"
#include "movement.h"
#include "fog_of_war.h"
#include "public/game.h"
#include "../main.h"
#include "../pf_math.h"
#include "../perf.h"
#include "../lib/public/quadtree.h"
#include "../lib/public/khash.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"

#include <assert.h>
#include <float.h>


QUADTREE_TYPE(ent, uint32_t)
QUADTREE_PROTOTYPES(static, ent, uint32_t)
QUADTREE_IMPL(static, ent, uint32_t)

KHASH_MAP_INIT_INT(pos, vec3_t)

#define POSBUF_INIT_SIZE (16384)
#define MAX_SEARCH_ENTS  (8192)
#define MAX(a, b)        ((a) < (b) ? (a) : (b))
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

static bool any_ent(const struct entity *ent, void *arg)
{
    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Pos_Set(const struct entity *ent, vec3_t pos)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(pos, s_postable, ent->uid);
    bool overwrite = (k != kh_end(s_postable));

    if(overwrite) {
        vec3_t old_pos = kh_val(s_postable, k);
        bool ret = qt_ent_delete(&s_postree, old_pos.x, old_pos.z, ent->uid);
        assert(ret);

        G_Fog_RemoveVision((vec2_t){old_pos.x, old_pos.z}, ent->faction_id, ent->vision_range);
    }

    if(!qt_ent_insert(&s_postree, pos.x, pos.z, ent->uid))
        return false;

    if(!overwrite) {
        int ret;
        kh_put(pos, s_postable, ent->uid, &ret); 
        if(ret == -1) {
            qt_ent_delete(&s_postree, pos.x, pos.z, ent->uid);
            return false;
        }
        k = kh_get(pos, s_postable, ent->uid);
    }

    kh_val(s_postable, k) = pos;
    assert(kh_size(s_postable) == s_postree.nrecs);

    G_Move_UpdatePos(ent, (vec2_t){pos.x, pos.z});
    G_Fog_AddVision((vec2_t){pos.x, pos.z}, ent->faction_id, ent->vision_range);
    return true; 
}

vec3_t G_Pos_Get(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(pos, s_postable, uid);
    assert(k != kh_end(s_postable));
    return kh_val(s_postable, k);
}

vec2_t G_Pos_GetXZ(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(pos, s_postable, uid);
    assert(k != kh_end(s_postable));
    vec3_t pos = kh_val(s_postable, k);
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

    qt_ent_init(&s_postree, xmin, xmax, zmin, zmax);
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

int G_Pos_EntsInRect(vec2_t xz_min, vec2_t xz_max, struct entity **out, size_t maxout)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();

    int ret = G_Pos_EntsInRectWithPred(xz_min, xz_max, out, maxout, any_ent, NULL);
    PERF_RETURN(ret);
}

int G_Pos_EntsInRectWithPred(vec2_t xz_min, vec2_t xz_max, struct entity **out, size_t maxout,
                             bool (*predicate)(const struct entity *ent, void *arg), void *arg)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();

    uint32_t ent_ids[maxout];
    const khash_t(entity) *ents = G_GetAllEntsSet();

    int ntotal = qt_ent_inrange_rect(&s_postree, 
        xz_min.x, xz_max.x, xz_min.z, xz_max.z, ent_ids, maxout);
    int ret = 0;

    for(int i = 0; i < ntotal; i++) {

        khiter_t k = kh_get(entity, ents, ent_ids[i]);
        assert(k != kh_end(s_postable));
        struct entity *curr = kh_val(ents, k);

        if(!predicate(curr, arg))
            continue;

        out[ret++] = curr;
    }
    PERF_RETURN(ret);
}

int G_Pos_EntsInCircle(vec2_t xz_point, float range, struct entity **out, size_t maxout)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();

    uint32_t ent_ids[maxout];
    const khash_t(entity) *ents = G_GetAllEntsSet();
    for(int i = 0; i < maxout; i++)
        ent_ids[i] = (uint32_t)-1;

    int ret = qt_ent_inrange_circle(&s_postree, 
        xz_point.x, xz_point.z, range, ent_ids, maxout);

    for(int i = 0; i < ret; i++) {
        assert(ent_ids[i] != (uint32_t)-1);
        khiter_t k = kh_get(entity, ents, ent_ids[i]);
        assert(k != kh_end(s_postable));
        out[i] = kh_val(ents, k);
    }
    PERF_RETURN(ret);
}

struct entity *G_Pos_NearestWithPred(vec2_t xz_point, 
                                     bool (*predicate)(const struct entity *ent, void *arg), 
                                     void *arg)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();

    uint32_t ent_ids[MAX_SEARCH_ENTS];
    const khash_t(entity) *ents = G_GetAllEntsSet();

    const float qt_len = MAX(s_postree.xmax - s_postree.xmin, s_postree.ymax - s_postree.ymin);
    float len = (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE) / 8.0f;

    while(len < qt_len) {
        float min_dist = FLT_MAX;
        struct entity *ret = NULL;

        int num_cands = qt_ent_inrange_circle(&s_postree, xz_point.x, xz_point.z,
            len, ent_ids, ARR_SIZE(ent_ids));

        for(int i = 0; i < num_cands; i++) {
        
            khiter_t k = kh_get(entity, ents, ent_ids[i]);
            assert(k != kh_end(s_postable));
            struct entity *curr = kh_val(ents, k);

            vec2_t delta, can_pos_xz = G_Pos_GetXZ(curr->uid);
            PFM_Vec2_Sub(&xz_point, &can_pos_xz, &delta);

            if(PFM_Vec2_Len(&delta) < min_dist && predicate(curr, arg)) {
                min_dist = PFM_Vec2_Len(&delta);
                ret = curr;
            }
        }

        if(ret)
            PERF_RETURN(ret);

        len *= 2.0f; 
    }
    PERF_RETURN(NULL);
}

struct entity *G_Pos_Nearest(vec2_t xz_point)
{
    ASSERT_IN_MAIN_THREAD();

    return G_Pos_NearestWithPred(xz_point, any_ent, NULL);
}

