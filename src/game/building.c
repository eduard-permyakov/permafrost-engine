/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#include "building.h"
#include "game_private.h"
#include "storage_site.h"
#include "fog_of_war.h"
#include "public/game.h"
#include "../event.h"
#include "../collision.h"
#include "../entity.h"
#include "../asset_load.h"
#include "../sched.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/khash.h"
#include "../lib/public/vec.h"
#include "../lib/public/attr.h"
#include "../lib/public/mpool.h"
#include "../lib/public/string_intern.h"
#include "../lib/public/pf_string.h"

#include <assert.h>
#include <stdint.h>


static void *pmalloc(size_t size);
static void *pcalloc(size_t n, size_t size);
static void *prealloc(void *ptr, size_t size);
static void  pfree(void *ptr);

#undef kmalloc
#undef kcalloc
#undef krealloc
#undef kfree

#define kmalloc  pmalloc
#define kcalloc  pcalloc
#define krealloc prealloc
#define kfree    pfree

#define ARR_SIZE(a)         (sizeof(a)/sizeof(a[0]))
#define MARKER_DIR          "assets/models/build_site_marker"
#define MARKER_OBJ          "build-site-marker.pfobj"
#define CENTER_MARKER_DIR   "assets/models/build_site"
#define CENTER_MARKER_OBJ   "build-site.pfobj"
#define EPSILON             (1.0 / 1024)
#define UID_NONE            (~((uint32_t)0))
#define MAX_BUILDINGS       (8192)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

KHASH_MAP_INIT_STR(int, int)

VEC_TYPE(uid, uint32_t)
VEC_IMPL(static inline, uid, uint32_t)

struct buildstate{
    enum{
        BUILDING_STATE_PLACEMENT,
        BUILDING_STATE_MARKED,
        BUILDING_STATE_FOUNDED,
        BUILDING_STATE_SUPPLIED,
        BUILDING_STATE_COMPLETED,
    }state;
    float      frac_done;
    vec_uid_t  markers;
    uint32_t   progress_model;
    float      vision_range;
    bool       blocking;
    bool       is_storage_site;
    struct obb obb;
    kh_int_t  *required;
};

typedef char buff_t[512];

MPOOL_TYPE(buff, buff_t)
MPOOL_PROTOTYPES(static, buff, buff_t)
MPOOL_IMPL(static, buff, buff_t)

#undef kmalloc
#undef kcalloc
#undef krealloc
#undef kfree

#define kmalloc  malloc
#define kcalloc  calloc
#define krealloc realloc
#define kfree    free

KHASH_MAP_INIT_INT(state, struct buildstate)
KHASH_SET_INIT_INT64(td)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map     *s_map;
static khash_t(state)       *s_entity_state_table;

static mp_buff_t             s_mpool;
static khash_t(stridx)      *s_stridx;
static mp_strbuff_t          s_stringpool;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void *pmalloc(size_t size)
{
    if(size > sizeof(buff_t))
        return NULL;
    mp_ref_t ref = mp_buff_alloc(&s_mpool);
    if(ref == 0)
        return NULL;
    return mp_buff_entry(&s_mpool, ref);
}

static void *pcalloc(size_t n, size_t size)
{
    void *ret = pmalloc(n * size);
    if(!ret)
        return NULL;
    memset(ret, 0, n * size);
    return ret;
}

static void *prealloc(void *ptr, size_t size)
{
    if(!ptr)
        return pmalloc(size);
    if(size <= sizeof(buff_t))
        return ptr;
    return NULL;
}

static void pfree(void *ptr)
{
    if(!ptr)
        return;
    mp_ref_t ref = mp_buff_ref(&s_mpool, ptr);
    mp_buff_free(&s_mpool, ref);
}

static struct buildstate *buildstate_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;

    return &kh_value(s_entity_state_table, k);
}

static void buildstate_set(const struct entity *ent, struct buildstate bs)
{
    int ret;
    assert(ent->flags & ENTITY_FLAG_BUILDING);

    khiter_t k = kh_put(state, s_entity_state_table, ent->uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_state_table, k) = bs;
}

static void buildstate_remove(const struct entity *ent)
{
    assert(ent->flags & ENTITY_FLAG_BUILDING);

    khiter_t k = kh_get(state, s_entity_state_table, ent->uid);
    if(k != kh_end(s_entity_state_table)) {

        struct buildstate *bs = &kh_value(s_entity_state_table, k);
        kh_destroy(int, bs->required);
        vec_uid_destroy(&bs->markers);
        kh_del(state, s_entity_state_table, k);
    }
}

static void on_render_3d(void *user, void *event)
{
    const struct camera *cam = G_GetActiveCamera();

    uint32_t key;
    struct buildstate curr;

    kh_foreach(s_entity_state_table, key, curr, {

        if(curr.state != BUILDING_STATE_PLACEMENT)
            continue;

        const struct entity *ent = G_EntityForUID(key);
        struct obb obb;
        Entity_CurrentOBB(ent, &obb, true);

        M_NavRenderBuildableTiles(s_map, cam, &obb, NAV_LAYER_GROUND_1X1);
    });
}

static uint64_t td_key(const struct tile_desc *td)
{
    return (((uint64_t)td->chunk_r << 48)
          | ((uint64_t)td->chunk_c << 32)
          | ((uint64_t)td->tile_r  << 16)
          | ((uint64_t)td->tile_c  <<  0));
}

static void building_mark_border(struct buildstate *bs, struct tile_desc *a, struct tile_desc *b)
{
    vec3_t map_pos = M_GetPos(s_map);

    struct map_resolution res;
    M_NavGetResolution(s_map, &res);

    struct box abox = M_Tile_Bounds(res, map_pos, *a);
    struct box bbox = M_Tile_Bounds(res, map_pos, *b);

    vec2_t acenter = (vec2_t){
        abox.x - abox.width / 2.0f,
        abox.z + abox.height / 2.0f
    };
    vec2_t bcenter = (vec2_t){
        bbox.x - bbox.width / 2.0f,
        bbox.z + bbox.height / 2.0f
    };

    vec2_t center;
    PFM_Vec2_Add(&acenter, &bcenter, &center);
    PFM_Vec2_Scale(&center, 0.5f, &center);

    vec3_t marker_pos = (vec3_t) {
        center.x,
        M_HeightAtPoint(s_map, center),
        center.z
    };

    struct entity *ent = AL_EntityFromPFObj(MARKER_DIR, MARKER_OBJ, 
        "__build_site_marker__", Entity_NewUID());
    if(!ent)
        return;

    if(fabs(acenter.z - bcenter.z) > EPSILON) {
        Entity_SetRot(ent->uid, (quat_t){ 0, 1.0 / sqrt(2.0), 0, 1.0 / sqrt(2.0) });
    }
    Entity_SetScale(ent->uid, (vec3_t){1.0, 1.5f, 1.0f});

    G_AddEntity(ent, marker_pos);
    vec_uid_push(&bs->markers, ent->uid);
}

static void building_mark_center(struct buildstate *bs, const struct entity *ent)
{
    vec3_t pos = G_Pos_Get(ent->uid);

    struct entity *marker = AL_EntityFromPFObj(CENTER_MARKER_DIR, CENTER_MARKER_OBJ, 
        "__build_site_marker__", Entity_NewUID());
    if(!marker)
        return;

    Entity_SetScale(marker->uid, (vec3_t){2.5, 2.5f, 2.5f});
    G_AddEntity(marker, pos);
    vec_uid_push(&bs->markers, marker->uid);
}

static void building_place_markers(struct buildstate *bs, const struct entity *ent)
{
    assert(Sched_UsingBigStack());

    struct obb obb;
    Entity_CurrentOBB(ent, &obb, true);

    struct map_resolution res;
    M_NavGetResolution(s_map, &res);

    struct tile_desc tds[2048];
    size_t ntiles = M_Tile_AllUnderObj(M_GetPos(s_map), res, &obb, tds, ARR_SIZE(tds));

    /* Build a set of which tiles are under the building */
    khash_t(td) *set = kh_init(td);
    for(int i = 0; i < ntiles; i++) {
        int status;
        kh_put(td, set, td_key(tds + i), &status);
    }

    for(int i = 0; i < ntiles; i++) {

        struct tile_desc curr = tds[i];
        int deltas[][2] = {
            {-1,  0},
            {+1,  0},
            { 0, -1},
            { 0, +1},
        };

        for(int j = 0; j < ARR_SIZE(deltas); j++) {
        
            struct tile_desc adj = curr;

            if(!M_Tile_RelativeDesc(res, &adj, deltas[j][0], deltas[j][1])
            || (kh_get(td, set, td_key(&adj)) == kh_end(set))) {

                building_mark_border(bs, &adj, &curr);
            }
        }
    }
    kh_destroy(td, set);
    building_mark_center(bs, ent);
}

static void building_clear_markers(struct buildstate *bs)
{
    for(int i = 0; i < vec_size(&bs->markers); i++) {
        struct entity *tofree = G_EntityForUID(vec_AT(&bs->markers, i));
        if(!tofree)
            continue; /* May have already been deleted during shutdown */
        G_RemoveEntity(tofree);
        G_SafeFree(tofree);
    }
}

static bool bstate_set_key(khash_t(int) *table, const char *name, int val)
{
    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(int, table, key);
    if(k != kh_end(table)) {
        kh_value(table, k) = val;
        return true;
    }

    int status;
    k = kh_put(int, table, key, &status);
    if(status == -1)
        return false;

    assert(status == 1);
    kh_value(table, k) = val;
    return true;
}

static bool bstate_get_key(khash_t(int) *table, const char *name, int *out)
{
    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(int, table, key);
    if(k == kh_end(table))
        return false;
    *out = kh_value(table, k);
    return true;
}

static void on_amount_changed(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct entity *ent = G_EntityForUID(uid);
    assert(ent);

    if(!G_StorageSite_IsSaturated(uid))
        return;

    G_Building_Supply(ent);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Building_Init(const struct map *map)
{
    mp_buff_init(&s_mpool);

    if(!mp_buff_reserve(&s_mpool, MAX_BUILDINGS * 3))
        goto fail_mpool; 
    if(NULL == (s_entity_state_table = kh_init(state)))
        goto fail_table;
    if(0 != kh_resize(state, s_entity_state_table, 2048))
        goto fail_res;
    if(!si_init(&s_stringpool, &s_stridx, 512))
        goto fail_res;

    E_Global_Register(EVENT_RENDER_3D_PRE, on_render_3d, NULL, G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    s_map = map;
    return true;

fail_res:
    kh_destroy(state, s_entity_state_table);
fail_table:
    mp_buff_destroy(&s_mpool);
fail_mpool:
    return false;
}

void G_Building_Shutdown(void)
{
    s_map = NULL;
    E_Global_Unregister(EVENT_RENDER_3D_PRE, on_render_3d);

    uint32_t key;
    struct buildstate curr;
    (void)key;

    kh_foreach(s_entity_state_table, key, curr, {
        vec_uid_destroy(&curr.markers);
    });

    si_shutdown(&s_stringpool, s_stridx);
    kh_destroy(state, s_entity_state_table);
    mp_buff_destroy(&s_mpool);
}

bool G_Building_AddEntity(struct entity *ent)
{
    if(kh_size(s_entity_state_table) == MAX_BUILDINGS)
        return false;

    assert(buildstate_get(ent->uid) == NULL);
    assert(ent->flags & ENTITY_FLAG_BUILDING);
    assert(!(ent->flags & ENTITY_FLAG_MOVABLE));

    struct buildstate new_bs = (struct buildstate) {
        .state = BUILDING_STATE_PLACEMENT,
        .frac_done = 0.0,
        .markers = {0},
        .progress_model = UID_NONE,
        .obb = {0},
        .is_storage_site = !!(ent->flags & ENTITY_FLAG_STORAGE_SITE)
    };

    new_bs.required = kh_init(int);
    if(!new_bs.required)
        return false;

    vec_uid_init(&new_bs.markers);
    buildstate_set(ent, new_bs);

    ent->flags |= ENTITY_FLAG_TRANSLUCENT;
    ent->flags &= ~ENTITY_FLAG_SELECTABLE;

    if(!new_bs.is_storage_site) {
        ent->flags |= ENTITY_FLAG_STORAGE_SITE;
        G_StorageSite_AddEntity(ent);
    }
    G_StorageSite_SetUseAlt(ent, true);

    return true;
}

void G_Building_RemoveEntity(const struct entity *ent)
{
    if(!(ent->flags & ENTITY_FLAG_BUILDING))
        return;

    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);

    if(bs->state >= BUILDING_STATE_FOUNDED && bs->blocking) {
        M_NavBlockersDecrefOBB(s_map, G_GetFactionID(ent->uid), &bs->obb);
    }

    struct entity *progress = G_EntityForUID(bs->progress_model);
    if(progress) {
        G_RemoveEntity(progress);
        G_SafeFree(progress);
    }

    E_Entity_Unregister(EVENT_STORAGE_SITE_AMOUNT_CHANGED, ent->uid, on_amount_changed);
    building_clear_markers(bs);
    buildstate_remove(ent);
}

bool G_Building_Mark(const struct entity *ent)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);

    if(bs->state != BUILDING_STATE_PLACEMENT)
        return false;

    bs->state = BUILDING_STATE_MARKED;
    return true;
}

bool G_Building_Found(struct entity *ent, bool blocking)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);

    if(bs->state != BUILDING_STATE_MARKED)
        return false;

    struct obb obb;
    Entity_CurrentOBB(ent, &obb, true);
    float height = obb.half_lengths[1] * 2;

    struct entity *progress = AL_EntityFromPFObj(ent->basedir, ent->filename, ent->name, Entity_NewUID());
    if(progress) {

        progress->flags |= ENTITY_FLAG_TRANSLUCENT;
        Entity_SetScale(progress->uid, Entity_GetScale(ent->uid));
        Entity_SetRot(progress->uid, Entity_GetRot(ent->uid));

        G_AddEntity(progress, G_Pos_Get(ent->uid));
        bs->progress_model = progress->uid;
    }

    if(ent->flags & ENTITY_FLAG_COMBATABLE) {
        int max_hp = G_Combat_GetMaxHP(ent);
        G_Combat_SetCurrentHP(ent, max_hp * 0.1f);
        G_Building_UpdateProgress(ent, 0.1f);
    }

    ent->flags &= ~ENTITY_FLAG_TRANSLUCENT;
    ent->flags |= ENTITY_FLAG_SELECTABLE;
    ent->flags |= ENTITY_FLAG_INVISIBLE;

    building_place_markers(bs, ent);
    bs->state = BUILDING_STATE_FOUNDED;

    bs->blocking = blocking;
    if(bs->blocking) {
        M_NavBlockersIncrefOBB(s_map, G_GetFactionID(ent->uid), &obb);
        bs->obb = obb;
    }

    const char *key;
    int amount;
    kh_foreach(bs->required, key, amount, {
        G_StorageSite_SetAltCapacity(ent, key, amount);
        G_StorageSite_SetAltDesired(ent->uid, key, amount);
    });

    E_Entity_Register(EVENT_STORAGE_SITE_AMOUNT_CHANGED, ent->uid, on_amount_changed, 
        (void*)((uintptr_t)ent->uid), G_RUNNING);
    return true;
}

bool G_Building_Supply(struct entity *ent)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);

    if(bs->state != BUILDING_STATE_FOUNDED)
        return false;

    bs->state = BUILDING_STATE_SUPPLIED;
    E_Entity_Unregister(EVENT_STORAGE_SITE_AMOUNT_CHANGED, ent->uid, on_amount_changed);
    G_StorageSite_ClearAlt(ent);
    G_StorageSite_ClearCurr(ent);
    return true;
}

bool G_Building_Complete(struct entity *ent)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);

    if(bs->state != BUILDING_STATE_SUPPLIED)
        return false;

    G_StorageSite_SetUseAlt(ent, false);
    if(!bs->is_storage_site) {
        G_StorageSite_RemoveEntity(ent);
        ent->flags &= ~ENTITY_FLAG_STORAGE_SITE;
    }

    struct entity *progress = G_EntityForUID(bs->progress_model);
    if(progress) {
        G_RemoveEntity(progress);
        G_SafeFree(progress);
    }
    building_clear_markers(bs);

    bs->state = BUILDING_STATE_COMPLETED;
    bs->progress_model = UID_NONE;
    ent->flags &= ~ENTITY_FLAG_INVISIBLE;

    float old = G_GetVisionRange(ent->uid);
    vec2_t xz_pos = G_Pos_GetXZ(ent->uid);

    G_SetVisionRange(ent->uid, bs->vision_range);
    E_Entity_Notify(EVENT_BUILDING_COMPLETED, ent->uid, NULL, ES_ENGINE);
    E_Global_Notify(EVENT_BUILDING_CONSTRUCTED, ent, ES_ENGINE);

    return true;
}

bool G_Building_Unobstructed(const struct entity *ent)
{
    struct obb obb;
    Entity_CurrentOBB(ent, &obb, true);

    return M_NavObjectBuildable(s_map, NAV_LAYER_GROUND_1X1, &obb);
}

bool G_Building_IsFounded(const struct entity *ent)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);
    return (bs->state >= BUILDING_STATE_FOUNDED);
}

bool G_Building_IsSupplied(const struct entity *ent)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);
    return (bs->state >= BUILDING_STATE_SUPPLIED);
}

bool G_Building_IsCompleted(const struct entity *ent)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);
    return (bs->state >= BUILDING_STATE_COMPLETED);
}

void G_Building_SetVisionRange(struct entity *ent, float vision_range)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);
    bs->vision_range = vision_range;

    /* Buildings have no vision until they are completed */
    if(bs->state < BUILDING_STATE_COMPLETED)
        return;

    G_SetVisionRange(ent->uid, bs->vision_range);
}

float G_Building_GetVisionRange(const struct entity *ent)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);
    return bs->vision_range;
}

void G_Building_UpdateProgress(struct entity *ent, float frac_done)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    if(!bs)
        return;

    struct entity *pent = G_EntityForUID(bs->progress_model);
    if(!pent)
        return;

    struct obb obb;
    Entity_CurrentOBB(pent, &obb, true);
    float height = obb.half_lengths[1] * 2;

    vec3_t pos = G_Pos_Get(pent->uid);
    float map_height = M_HeightAtPoint(s_map, (vec2_t){pos.x, pos.z});

    pos.y = map_height - (height * (1.0f - frac_done));
    G_Pos_Set(pent, pos);
    bs->frac_done = frac_done;
}

void G_Building_UpdateBounds(const struct entity *ent)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    if(!bs)
        return;

    if(!G_Building_IsFounded(ent))
        return;

    if(!bs->blocking)
        return;

    M_NavBlockersDecrefOBB(s_map, G_GetFactionID(ent->uid), &bs->obb);
    Entity_CurrentOBB(ent, &bs->obb, true);
    M_NavBlockersIncrefOBB(s_map, G_GetFactionID(ent->uid), &bs->obb);
}

void G_Building_UpdateFactionID(const struct entity *ent, int oldfac, int newfac)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    if(!bs)
        return;

    if(!G_Building_IsFounded(ent))
        return;

    if(!bs->blocking)
        return;

    M_NavBlockersDecrefOBB(s_map, oldfac, &bs->obb);
    M_NavBlockersIncrefOBB(s_map, newfac, &bs->obb);
}

bool G_Building_NeedsRepair(const struct entity *ent)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);

    if(bs->state < BUILDING_STATE_FOUNDED)
        return false;
    if(bs->state < BUILDING_STATE_COMPLETED)
        return true;

    if(!(ent->flags & ENTITY_FLAG_COMBATABLE))
        return false;

    int hp = G_Combat_GetCurrentHP(ent);
    int max_hp = G_Combat_GetMaxHP(ent);

    if(max_hp == 0)
        return false;

    return (hp < max_hp);
}

int G_Building_GetRequired(uint32_t uid, const char *rname)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);

    int ret = 0;
    bstate_get_key(bs->required, rname, &ret);
    return ret;
}

bool G_Building_SetRequired(uint32_t uid, const char *rname, int req)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);
    return bstate_set_key(bs->required, rname, req);
}

bool G_Building_SaveState(struct SDL_RWops *stream)
{
    struct attr num_buildings = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_entity_state_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_buildings, "num_buildings"));

    uint32_t uid;
    struct buildstate curr;

    kh_foreach(s_entity_state_table, uid, curr, {

        struct entity *ent = G_EntityForUID(uid);
        assert(ent);

        struct attr buid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &buid, "building_uid"));
    
        struct attr state = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.state
        };
        CHK_TRUE_RET(Attr_Write(stream, &state, "building_state"));

        struct attr frac_done = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = curr.frac_done
        };
        CHK_TRUE_RET(Attr_Write(stream, &frac_done, "building_frac_done"));

        struct attr building_hp = (struct attr){
            .type = TYPE_INT,
            .val.as_int = (ent->flags & ENTITY_FLAG_COMBATABLE) ? G_Combat_GetCurrentHP(ent) : 0
        };
        CHK_TRUE_RET(Attr_Write(stream, &building_hp, "building_hp"));

        struct attr building_vis_range = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = curr.vision_range
        };
        CHK_TRUE_RET(Attr_Write(stream, &building_vis_range, "building_vis_range"));

        struct attr building_blocking = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.blocking
        };
        CHK_TRUE_RET(Attr_Write(stream, &building_blocking, "building_blocking"));

        struct attr building_ss = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.is_storage_site
        };
        CHK_TRUE_RET(Attr_Write(stream, &building_ss, "building_ss"));

        struct attr num_required = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr.required)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_required, "num_required"));

        const char *required_key;
        int required_amount;
        kh_foreach(curr.required, required_key, required_amount, {
        
            struct attr required_key_attr = (struct attr){ .type = TYPE_STRING, };
            pf_strlcpy(required_key_attr.val.as_string, required_key, sizeof(required_key_attr.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &required_key_attr, "required_key"));

            struct attr required_amount_attr = (struct attr){
                .type = TYPE_INT,
                .val.as_int = required_amount
            };
            CHK_TRUE_RET(Attr_Write(stream, &required_amount_attr, "required_amount"));
        });

        CHK_TRUE_RET(AL_SaveOBB(stream, &curr.obb));
    });

    return true;
}

bool G_Building_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_buildings = attr.val.as_int;

    for(int i = 0; i < num_buildings; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int state = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        float frac_done = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int hp = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        float vis_range = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        bool blocking = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        bool is_storage_site = attr.val.as_bool;

        struct entity *ent = G_EntityForUID(uid);
        CHK_TRUE_RET(ent);
        CHK_TRUE_RET(ent->flags & ENTITY_FLAG_BUILDING);

        struct buildstate *bs = buildstate_get(ent->uid);
        assert(bs);
        bs->vision_range = vis_range;
        bs->is_storage_site = is_storage_site;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int num_required  = attr.val.as_int;

        for(int j = 0; j < num_required; j++) {
        
            struct attr keyattr;
            CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
            CHK_TRUE_RET(keyattr.type == TYPE_STRING);
            const char *key = keyattr.val.as_string;

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            int val = attr.val.as_int;

            G_Building_SetRequired(uid, key, val);
        }

        CHK_TRUE_RET(AL_LoadOBB(stream, &bs->obb));

        switch(state) {
        case BUILDING_STATE_PLACEMENT:
            break;
        case BUILDING_STATE_MARKED:
            G_Building_Mark(ent);
            break;
        case BUILDING_STATE_FOUNDED:
            G_Building_Mark(ent);
            G_Building_Found(ent, blocking);
            break;
        case BUILDING_STATE_SUPPLIED:
            G_Building_Mark(ent);
            G_Building_Found(ent, blocking);
            G_Building_Supply(ent);
            G_Building_UpdateProgress(ent, frac_done);
            break;
        case BUILDING_STATE_COMPLETED:
            G_Building_Mark(ent);
            G_Building_Found(ent, blocking);
            G_Building_Supply(ent);
            G_Building_Complete(ent);
            break;
        default:
            return false;
        }

        if(ent->flags & ENTITY_FLAG_COMBATABLE) {
            G_Combat_SetCurrentHP(ent, hp);
        }
    }
    return true;
}

