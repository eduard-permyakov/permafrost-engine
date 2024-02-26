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

#include "building.h"
#include "game_private.h"
#include "storage_site.h"
#include "fog_of_war.h"
#include "public/game.h"
#include "../event.h"
#include "../entity.h"
#include "../asset_load.h"
#include "../sched.h"
#include "../phys/public/collision.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/khash.h"
#include "../lib/public/vec.h"
#include "../lib/public/attr.h"
#include "../lib/public/mpool_allocator.h"
#include "../lib/public/string_intern.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/stalloc.h"

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
    bool       pathable;
    bool       blocking;
    bool       is_storage_site;
    vec2_t     rally_point;
    struct obb obb;
    kh_int_t  *required;
};

typedef char buff_t[512];

MPOOL_ALLOCATOR_TYPE(buff, buff_t)
MPOOL_ALLOCATOR_PROTOTYPES(static, buff, buff_t)
MPOOL_ALLOCATOR_IMPL(static, buff, buff_t)

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

static mpa_buff_t            s_mpool;
static khash_t(stridx)      *s_stridx;
static mp_strbuff_t          s_stringpool;
static struct memstack       s_eventargs;
static bool                  s_set_rally_on_lclick = false;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void *pmalloc(size_t size)
{
    if(size > sizeof(buff_t))
        return NULL;
    return mpa_buff_alloc(&s_mpool);
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
    mpa_buff_free(&s_mpool, ptr);
}

static struct buildstate *buildstate_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;

    return &kh_value(s_entity_state_table, k);
}

static void buildstate_set(uint32_t uid, struct buildstate bs)
{
    int ret;
    assert(G_FlagsGet(uid) & ENTITY_FLAG_BUILDING);

    khiter_t k = kh_put(state, s_entity_state_table, uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_state_table, k) = bs;
}

static void buildstate_remove(uint32_t uid)
{
    assert(G_FlagsGet(uid) & ENTITY_FLAG_BUILDING);

    khiter_t k = kh_get(state, s_entity_state_table, uid);
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

        struct obb obb;
        Entity_CurrentOBB(key, &obb, true);
        uint32_t flags = G_FlagsGet(key);

        if(flags & ENTITY_FLAG_WATER) {
            M_NavRenderBuildableTiles(s_map, cam, &obb, NAV_LAYER_WATER_1X1,
                !(M_ObjectAdjacentToLand(s_map, &obb) && M_ObjectAdjacentToWater(s_map, &obb)), 
                true);
        }else{
            M_NavRenderBuildableTiles(s_map, cam, &obb, NAV_LAYER_GROUND_1X1, false, false);
        }
    });
}

static uint64_t td_key(const struct tile_desc *td)
{
    return (((uint64_t)td->chunk_r << 48)
          | ((uint64_t)td->chunk_c << 32)
          | ((uint64_t)td->tile_r  << 16)
          | ((uint64_t)td->tile_c  <<  0));
}

static float building_height(uint32_t uid, vec2_t pos)
{
    uint32_t flags = G_FlagsGet(uid);
    if(flags & ENTITY_FLAG_WATER)
        return 0.0f;
    return M_HeightAtPoint(s_map, pos);
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

    uint32_t flags;
    uint32_t uid = Entity_NewUID();
    bool result = AL_EntityFromPFObj(MARKER_DIR, MARKER_OBJ, 
        "__build_site_marker__", uid, &flags);
    if(!result)
        return;

    if(fabs(acenter.z - bcenter.z) > EPSILON) {
        Entity_SetRot(uid, (quat_t){ 0, 1.0 / sqrt(2.0), 0, 1.0 / sqrt(2.0) });
    }
    Entity_SetScale(uid, (vec3_t){1.0, 1.5f, 1.0f});

    G_AddEntity(uid, flags, marker_pos);
    vec_uid_push(&bs->markers, uid);
}

static void building_mark_center(struct buildstate *bs, uint32_t uid)
{
    vec3_t pos = G_Pos_Get(uid);
    uint32_t marker_uid = Entity_NewUID();
    uint32_t flags;

    bool result = AL_EntityFromPFObj(CENTER_MARKER_DIR, CENTER_MARKER_OBJ, 
        "__build_site_marker__", marker_uid, &flags);
    if(!result)
        return;

    Entity_SetScale(marker_uid, (vec3_t){2.5, 2.5f, 2.5f});
    G_AddEntity(marker_uid, flags, pos);
    vec_uid_push(&bs->markers, marker_uid);
}

static void building_place_markers(struct buildstate *bs, uint32_t uid)
{
    assert(Sched_UsingBigStack());

    struct obb obb;
    Entity_CurrentOBB(uid, &obb, true);

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
    building_mark_center(bs, uid);
}

static void building_clear_markers(struct buildstate *bs)
{
    for(int i = 0; i < vec_size(&bs->markers); i++) {
        uint32_t tofree = vec_AT(&bs->markers, i);
        if(!G_EntityExists(tofree))
            continue; /* May have already been deleted during shutdown */
        G_RemoveEntity(tofree);
        G_FreeEntity(tofree);
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

    if(!G_StorageSite_IsSaturated(uid))
        return;

    G_Building_Supply(uid);
}

static void on_update(void *user, void *event)
{
    stalloc_clear(&s_eventargs);
}

static void try_place_rally_points(void)
{
    vec3_t map_pos;
    if(!M_Raycast_MouseIntersecCoord(&map_pos))
        return;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);
    if(vec_size(sel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return;

    size_t nset = 0;
    for(int i = 0; i < vec_size(sel); i++) {
        uint32_t curr = vec_AT(sel, i);
        uint32_t flags = G_FlagsGet(curr);
        if(!(flags & ENTITY_FLAG_BUILDING))
            continue;
        G_Building_SetRallyPoint(curr, (vec2_t){map_pos.x, map_pos.z});
        nset++;
    }

    if(nset > 0) {
        E_Global_Notify(EVENT_RALLY_POINT_SET, NULL, ES_ENGINE);
    }
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    bool targeting = G_Building_InTargetMode();
    bool right = (mouse_event->button == SDL_BUTTON_RIGHT);
    bool left = (mouse_event->button == SDL_BUTTON_LEFT);

    bool set = s_set_rally_on_lclick;
    s_set_rally_on_lclick = false;

    if(G_MouseOverMinimap())
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    if(right && targeting)
        return;

    if(left && !targeting)
        return;

    if(G_CurrContextualAction() != CTX_ACTION_NONE)
        return;

    if(right || (left && set)) {
        try_place_rally_points();
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Building_Init(const struct map *map)
{
    mpa_buff_init(&s_mpool, 1024, 0);

    if(!mpa_buff_reserve(&s_mpool, 1024))
        goto fail_mpool; 
    if(NULL == (s_entity_state_table = kh_init(state)))
        goto fail_table;
    if(0 != kh_resize(state, s_entity_state_table, 2048))
        goto fail_res;
    if(!si_init(&s_stringpool, &s_stridx, 512))
        goto fail_res;
    if(!stalloc_init(&s_eventargs))
        goto fail_eventargs;

    E_Global_Register(EVENT_RENDER_3D_PRE, on_render_3d, NULL, 
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    E_Global_Register(EVENT_UPDATE_START, on_update, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    s_map = map;
    return true;

fail_eventargs:
    si_shutdown(&s_stringpool, s_stridx);
fail_res:
    kh_destroy(state, s_entity_state_table);
fail_table:
    mpa_buff_destroy(&s_mpool);
fail_mpool:
    return false;
}

void G_Building_Shutdown(void)
{
    s_map = NULL;
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    E_Global_Unregister(EVENT_UPDATE_START, on_update);
    E_Global_Unregister(EVENT_RENDER_3D_PRE, on_render_3d);

    uint32_t key;
    struct buildstate curr;
    (void)key;

    kh_foreach(s_entity_state_table, key, curr, {
        vec_uid_destroy(&curr.markers);
    });

    stalloc_destroy(&s_eventargs);
    si_shutdown(&s_stringpool, s_stridx);
    kh_destroy(state, s_entity_state_table);
    mpa_buff_destroy(&s_mpool);
}

bool G_Building_AddEntity(uint32_t uid)
{
    assert(buildstate_get(uid) == NULL);
    assert(G_FlagsGet(uid) & ENTITY_FLAG_BUILDING);
    assert(!(G_FlagsGet(uid) & ENTITY_FLAG_MOVABLE));

    struct buildstate new_bs = (struct buildstate) {
        .state = BUILDING_STATE_PLACEMENT,
        .frac_done = 0.0,
        .markers = {0},
        .progress_model = UID_NONE,
        .obb = {0},
        .pathable = false,
        .is_storage_site = !!(G_FlagsGet(uid) & ENTITY_FLAG_STORAGE_SITE),
        .rally_point = G_Pos_GetXZ(uid)
    };

    new_bs.required = kh_init(int);
    if(!new_bs.required)
        return false;

    vec_uid_init(&new_bs.markers);
    buildstate_set(uid, new_bs);

    uint32_t newflags = G_FlagsGet(uid);
    newflags |= ENTITY_FLAG_TRANSLUCENT;
    newflags &= ~ENTITY_FLAG_SELECTABLE;

    if(!new_bs.is_storage_site) {
        newflags |= ENTITY_FLAG_STORAGE_SITE;
        G_StorageSite_AddEntity(uid);
    }
    G_FlagsSet(uid, newflags);
    G_StorageSite_SetUseAlt(uid, true);

    return true;
}

void G_Building_RemoveEntity(uint32_t uid)
{
    if(!(G_FlagsGet(uid) & ENTITY_FLAG_BUILDING))
        return;

    struct buildstate *bs = buildstate_get(uid);
    assert(bs);

    if(bs->state >= BUILDING_STATE_FOUNDED && bs->blocking) {
        M_NavBlockersDecrefOBB(s_map, G_GetFactionID(uid), G_FlagsGet(uid), &bs->obb);
        struct entity_block_desc *desc = stalloc(&s_eventargs, sizeof(struct entity_block_desc));
        *desc = (struct entity_block_desc){
            .uid = uid,
            .radius = G_GetSelectionRadius(uid),
            .pos = G_Pos_GetXZ(uid)
        };
        E_Global_Notify(EVENT_BUILDING_REMOVED, desc, ES_ENGINE);
    }

    if(G_EntityExists(bs->progress_model)) {
        G_RemoveEntity(bs->progress_model);
        G_FreeEntity(bs->progress_model);
    }

    E_Entity_Unregister(EVENT_STORAGE_SITE_AMOUNT_CHANGED, uid, on_amount_changed);
    building_clear_markers(bs);
    buildstate_remove(uid);
}

void *G_Building_CopyState(void)
{
    return kh_copy_state(s_entity_state_table);
}

bool G_Building_IsFoundedFrom(void *state, uint32_t uid)
{
    khash_t(state) *table = (khash_t(state)*)state;
    khiter_t k = kh_get(state, table, uid);
    assert(k != kh_end(table));
    struct buildstate *bs = &kh_value(table, k);
    return (bs->state >= BUILDING_STATE_FOUNDED);
}

bool G_Building_IsCompletedFrom(void *state, uint32_t uid)
{
    khash_t(state) *table = (khash_t(state)*)state;
    khiter_t k = kh_get(state, table, uid);
    assert(k != kh_end(table));
    struct buildstate *bs = &kh_value(table, k);
    return (bs->state >= BUILDING_STATE_COMPLETED);
}

bool G_Building_Mark(uint32_t uid)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);

    if(bs->state != BUILDING_STATE_PLACEMENT)
        return false;

    bs->state = BUILDING_STATE_MARKED;
    return true;
}

bool G_Building_Found(uint32_t uid, bool blocking)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);

    if(bs->state != BUILDING_STATE_MARKED)
        return false;

    struct obb obb;
    Entity_CurrentOBB(uid, &obb, true);
    float height = obb.half_lengths[1] * 2;

    const struct entity *ent = AL_EntityGet(uid);
    if(!ent)
        return false;

    uint32_t progress_flags;
    uint32_t progress_uid = Entity_NewUID();
    bool result = AL_EntityFromPFObj(ent->basedir, ent->filename, ent->name, 
        progress_uid, &progress_flags);

    if(result) {

        progress_flags |= ENTITY_FLAG_TRANSLUCENT;
        Entity_SetScale(progress_uid, Entity_GetScale(uid));
        Entity_SetRot(progress_uid, Entity_GetRot(uid));

        G_AddEntity(progress_uid, progress_flags, G_Pos_Get(uid));
        bs->progress_model = progress_uid;
    }

    uint32_t ent_flags = G_FlagsGet(uid);
    if(ent_flags & ENTITY_FLAG_COMBATABLE) {
        int max_hp = G_Combat_GetMaxHP(uid);
        G_Combat_SetCurrentHP(uid, max_hp * 0.1f);
        G_Building_UpdateProgress(uid, 0.1f);
    }

    ent_flags &= ~ENTITY_FLAG_TRANSLUCENT;
    ent_flags |= ENTITY_FLAG_SELECTABLE;
    ent_flags |= ENTITY_FLAG_INVISIBLE;
    G_FlagsSet(uid, ent_flags);

    building_place_markers(bs, uid);
    bs->state = BUILDING_STATE_FOUNDED;

    bs->blocking = blocking;
    if(bs->blocking) {
        M_NavBlockersIncrefOBB(s_map, G_GetFactionID(uid), ent_flags, &obb);
        bs->obb = obb;
    }

    const char *key;
    int amount;
    kh_foreach(bs->required, key, amount, {
        G_StorageSite_SetAltCapacity(uid, key, amount);
        G_StorageSite_SetAltDesired(uid, key, amount);
    });

    E_Entity_Register(EVENT_STORAGE_SITE_AMOUNT_CHANGED, uid, on_amount_changed, 
        (void*)((uintptr_t)uid), G_RUNNING);

    struct entity_block_desc *desc = malloc(sizeof(struct entity_block_desc));
    *desc = (struct entity_block_desc){
        .uid = uid,
        .radius = G_GetSelectionRadius(uid),
        .pos = G_Pos_GetXZ(uid)
    };
    E_Global_Notify(EVENT_BUILDING_PLACED, desc, ES_ENGINE);
    return true;
}

bool G_Building_Supply(uint32_t uid)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);

    if(bs->state != BUILDING_STATE_FOUNDED)
        return false;

    bs->state = BUILDING_STATE_SUPPLIED;
    E_Entity_Unregister(EVENT_STORAGE_SITE_AMOUNT_CHANGED, uid, on_amount_changed);
    G_StorageSite_ClearAlt(uid);
    G_StorageSite_ClearCurr(uid);
    return true;
}

bool G_Building_Complete(uint32_t uid)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);

    if(bs->state != BUILDING_STATE_SUPPLIED)
        return false;

    G_StorageSite_SetUseAlt(uid, false);
    if(!bs->is_storage_site) {
        G_StorageSite_RemoveEntity(uid);
        uint32_t flags = G_FlagsGet(uid);
        flags &= ~ENTITY_FLAG_STORAGE_SITE;
        G_FlagsSet(uid, flags);
    }

    if(G_EntityExists(bs->progress_model)) {
        G_RemoveEntity(bs->progress_model);
        G_FreeEntity(bs->progress_model);
    }
    building_clear_markers(bs);

    bs->state = BUILDING_STATE_COMPLETED;
    bs->progress_model = UID_NONE;

    uint32_t flags = G_FlagsGet(uid);
    flags &= ~ENTITY_FLAG_INVISIBLE;
    G_FlagsSet(uid, flags);

    if(bs->blocking && (!(flags & ENTITY_FLAG_COLLISION) || bs->pathable)) {
        bs->blocking = false;
        M_NavBlockersDecrefOBB(s_map, G_GetFactionID(uid), G_FlagsGet(uid), &bs->obb);
    }

    float old = G_GetVisionRange(uid);
    vec2_t xz_pos = G_Pos_GetXZ(uid);

    G_SetVisionRange(uid, bs->vision_range);
    E_Entity_Notify(EVENT_BUILDING_COMPLETED, uid, NULL, ES_ENGINE);
    E_Global_Notify(EVENT_BUILDING_CONSTRUCTED, (void*)((uintptr_t)uid), ES_ENGINE);

    return true;
}

bool G_Building_Unobstructed(uint32_t uid)
{
    struct obb obb;
    Entity_CurrentOBB(uid, &obb, true);
    uint32_t flags = G_FlagsGet(uid);

    if(flags & ENTITY_FLAG_WATER) {
        return M_NavObjectBuildable(s_map, NAV_LAYER_WATER_1X1, true, &obb)
            && M_ObjectAdjacentToWater(s_map, &obb)
            && M_ObjectAdjacentToLand(s_map, &obb);
    }else{
        return M_NavObjectBuildable(s_map, NAV_LAYER_GROUND_1X1, false, &obb);
    }
}

bool G_Building_IsFounded(uint32_t uid)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);
    return (bs->state >= BUILDING_STATE_FOUNDED);
}

bool G_Building_IsSupplied(uint32_t uid)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);
    return (bs->state >= BUILDING_STATE_SUPPLIED);
}

bool G_Building_IsCompleted(uint32_t uid)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);
    return (bs->state >= BUILDING_STATE_COMPLETED);
}

void G_Building_SetVisionRange(uint32_t uid, float vision_range)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);
    bs->vision_range = vision_range;

    /* Buildings have no vision until they are completed */
    if(bs->state < BUILDING_STATE_COMPLETED)
        return;

    G_SetVisionRange(uid, bs->vision_range);
}

float G_Building_GetVisionRange(uint32_t uid)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);
    return bs->vision_range;
}

void G_Building_UpdateProgress(uint32_t uid, float frac_done)
{
    struct buildstate *bs = buildstate_get(uid);
    if(!bs)
        return;

    if(!G_EntityExists(bs->progress_model))
        return;

    struct obb obb;
    Entity_CurrentOBB(bs->progress_model, &obb, true);
    float height = obb.half_lengths[1] * 2;

    vec3_t pos = G_Pos_Get(bs->progress_model);
    float map_height = building_height(uid, (vec2_t){pos.x, pos.z});

    pos.y = map_height - (height * (1.0f - frac_done));
    G_Pos_Set(bs->progress_model, pos);
    bs->frac_done = frac_done;
}

void G_Building_UpdateBounds(uint32_t uid)
{
    struct buildstate *bs = buildstate_get(uid);
    if(!bs)
        return;

    bs->rally_point = G_Pos_GetXZ(uid);
    if(!G_Building_IsFounded(uid))
        return;

    if(!bs->blocking)
        return;

    uint32_t flags = G_FlagsGet(uid);
    M_NavBlockersDecrefOBB(s_map, G_GetFactionID(uid), flags, &bs->obb);
    Entity_CurrentOBB(uid, &bs->obb, true);
    M_NavBlockersIncrefOBB(s_map, G_GetFactionID(uid), flags, &bs->obb);
}

void G_Building_UpdateFactionID(uint32_t uid, int oldfac, int newfac)
{
    struct buildstate *bs = buildstate_get(uid);
    if(!bs)
        return;

    if(!G_Building_IsFounded(uid))
        return;

    if(!bs->blocking)
        return;

    uint32_t flags = G_FlagsGet(uid);
    M_NavBlockersDecrefOBB(s_map, oldfac, flags, &bs->obb);
    M_NavBlockersIncrefOBB(s_map, newfac, flags, &bs->obb);
}

bool G_Building_NeedsRepair(uint32_t uid)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);

    if(bs->state < BUILDING_STATE_FOUNDED)
        return false;
    if(bs->state < BUILDING_STATE_COMPLETED)
        return true;

    uint32_t flags = G_FlagsGet(uid);
    if(!(flags & ENTITY_FLAG_COMBATABLE))
        return false;

    int hp = G_Combat_GetCurrentHP(uid);
    int max_hp = G_Combat_GetMaxHP(uid);

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

size_t G_Building_GetAllRequired(uint32_t uid, size_t maxout, 
                                 const char *names[], int amounts[])
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);

    size_t ret = 0;
    const char *key;
    int amount;
    kh_foreach(bs->required, key, amount, {

        if(ret == maxout)
            return ret;
        names[ret] = key;
        amounts[ret] = amount;
        ret++;
    });
    return ret;
}

void G_Building_SetPathable(uint32_t uid, bool pathable)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);
    bs->pathable = pathable;
}

bool G_Building_GetPathable(uint32_t uid)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);
    return bs->pathable;
}

bool G_Building_SetRequired(uint32_t uid, const char *rname, int req)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);
    return bstate_set_key(bs->required, rname, req);
}

void G_Building_SetRallyPoint(uint32_t uid, vec2_t pos)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);
    bs->rally_point = pos;
}

vec2_t G_Building_GetRallyPoint(uint32_t uid)
{
    struct buildstate *bs = buildstate_get(uid);
    assert(bs);
    return bs->rally_point;
}

void G_Building_SetPositionRallyPointOnLeftClick(void)
{
    s_set_rally_on_lclick = true;
}

bool G_Building_InTargetMode(void)
{
    return s_set_rally_on_lclick;
}

bool G_Building_SaveState(struct SDL_RWops *stream)
{
    struct attr num_buildings = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_entity_state_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_buildings, "num_buildings"));
    Sched_TryYield();

    uint32_t uid;
    struct buildstate curr;

    kh_foreach(s_entity_state_table, uid, curr, {

        uint32_t flags = G_FlagsGet(uid);

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
            .val.as_int = (flags & ENTITY_FLAG_COMBATABLE) ? G_Combat_GetCurrentHP(uid) : 0
        };
        CHK_TRUE_RET(Attr_Write(stream, &building_hp, "building_hp"));

        struct attr building_vis_range = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = curr.vision_range
        };
        CHK_TRUE_RET(Attr_Write(stream, &building_vis_range, "building_vis_range"));

        struct attr building_pathable = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.pathable
        };
        CHK_TRUE_RET(Attr_Write(stream, &building_pathable, "building_pathable"));

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

        struct attr rally_point = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.rally_point
        };
        CHK_TRUE_RET(Attr_Write(stream, &rally_point, "rally_point"));

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
        Sched_TryYield();
    });

    return true;
}

bool G_Building_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_buildings = attr.val.as_int;
    Sched_TryYield();

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
        bool pathable = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        bool blocking = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        bool is_storage_site = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        vec2_t rally_point = attr.val.as_vec2;

        CHK_TRUE_RET(G_EntityExists(uid));
        CHK_TRUE_RET(G_FlagsGet(uid) & ENTITY_FLAG_BUILDING);

        struct buildstate *bs = buildstate_get(uid);
        assert(bs);
        bs->vision_range = vis_range;
        bs->pathable = pathable;
        bs->is_storage_site = is_storage_site;
        bs->rally_point = rally_point;

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
            G_Building_Mark(uid);
            break;
        case BUILDING_STATE_FOUNDED:
            G_Building_Mark(uid);
            G_Building_Found(uid, blocking);
            break;
        case BUILDING_STATE_SUPPLIED:
            G_Building_Mark(uid);
            G_Building_Found(uid, blocking);
            G_Building_Supply(uid);
            G_Building_UpdateProgress(uid, frac_done);
            break;
        case BUILDING_STATE_COMPLETED:
            G_Building_Mark(uid);
            G_Building_Found(uid, blocking);
            G_Building_Supply(uid);
            G_Building_Complete(uid);
            break;
        default:
            return false;
        }

        if(G_FlagsGet(uid) & ENTITY_FLAG_COMBATABLE) {
            G_Combat_SetCurrentHP(uid, hp);
        }
        Sched_TryYield();
    }
    return true;
}

