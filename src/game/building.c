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
#include "public/game.h"
#include "../event.h"
#include "../collision.h"
#include "../entity.h"
#include "../asset_load.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/khash.h"
#include "../lib/public/vec.h"

#include <assert.h>
#include <stdint.h>


#define ARR_SIZE(a)         (sizeof(a)/sizeof(a[0]))
#define MARKER_DIR          "assets/models/build_site_marker"
#define MARKER_OBJ          "build-site-marker.pfobj"
#define CENTER_MARKER_DIR   "assets/models/build_site"
#define CENTER_MARKER_OBJ   "build-site.pfobj"
#define EPSILON             (1.0 / 1024)
#define UID_NONE            (~((uint32_t)0))

VEC_TYPE(uid, uint32_t)
VEC_IMPL(static inline, uid, uint32_t)

struct buildstate{
    enum{
        BUILDING_STATE_PLACEMENT,
        BUILDING_STATE_MARKED,
        BUILDING_STATE_FOUNDED,
        BUILDING_STATE_COMPLETED,
    }state;
    vec_uid_t markers;
    uint32_t  progress_model;
};

KHASH_MAP_INIT_INT(state, struct buildstate)
KHASH_SET_INIT_INT64(td)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map     *s_map;
static khash_t(state)       *s_entity_state_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

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

        M_NavRenderBuildableTiles(s_map, cam, &obb);
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
        ent->rotation = (quat_t){ 0, 1.0 / sqrt(2.0), 0, 1.0 / sqrt(2.0) };
    }
    ent->scale = (vec3_t){1.0, 1.5f, 1.0f};
    ent->flags |= ENTITY_FLAG_STATIC;

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

    marker->scale = (vec3_t){2.5, 2.5f, 2.5f};
    marker->flags |= ENTITY_FLAG_STATIC;

    G_AddEntity(marker, pos);
    vec_uid_push(&bs->markers, marker->uid);
}

static void building_place_markers(struct buildstate *bs, const struct entity *ent)
{
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

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Building_Init(const struct map *map)
{
    if(NULL == (s_entity_state_table = kh_init(state)))
        return false;

    E_Global_Register(EVENT_RENDER_3D_PRE, on_render_3d, NULL, G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    s_map = map;
    return true;
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

    kh_destroy(state, s_entity_state_table);
}

void G_Building_AddEntity(struct entity *ent)
{
    assert(buildstate_get(ent->uid) == NULL);
    assert(ent->flags & ENTITY_FLAG_BUILDING);
    assert(ent->flags & ENTITY_FLAG_STATIC);

    struct buildstate new_bs = (struct buildstate) {
        .state = BUILDING_STATE_PLACEMENT,
        .markers = {0},
        .progress_model = UID_NONE
    };
    buildstate_set(ent, new_bs);

    ent->flags |= ENTITY_FLAG_TRANSLUCENT;
    ent->flags &= ~ENTITY_FLAG_SELECTABLE;
}

void G_Building_RemoveEntity(const struct entity *ent)
{
    if(!(ent->flags & ENTITY_FLAG_BUILDING))
        return;

    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);

    if(bs->state >= BUILDING_STATE_FOUNDED) {
        struct obb obb;
        Entity_CurrentOBB(ent, &obb, true);
        M_NavBlockersDecrefOBB(s_map, &obb);
    }

    struct entity *progress = G_EntityForUID(bs->progress_model);
    if(progress) {
        G_RemoveEntity(progress);
        G_SafeFree(progress);
    }
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

bool G_Building_Found(struct entity *ent)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);

    if(bs->state != BUILDING_STATE_MARKED)
        return false;

    if(!G_Building_Unobstructed(ent))
        return false;

    struct obb obb;
    Entity_CurrentOBB(ent, &obb, true);
    float height = obb.half_lengths[1] * 2;

    struct entity *progress = AL_EntityFromPFObj(ent->basedir, ent->filename, ent->name, Entity_NewUID());
    if(progress) {
        progress->flags |= ENTITY_FLAG_TRANSLUCENT;
        progress->scale = ent->scale;
        progress->rotation = ent->rotation;

        G_AddEntity(progress, G_Pos_Get(ent->uid));
        bs->progress_model = progress->uid;
    }

    if(ent->flags & ENTITY_FLAG_COMBATABLE) {
        G_Combat_SetHP(ent, ent->max_hp * 0.1f);
        G_Building_UpdateProgress(ent, 0.1f);
    }

    ent->flags &= ~ENTITY_FLAG_TRANSLUCENT;
    ent->flags |= ENTITY_FLAG_SELECTABLE;
    ent->flags |= ENTITY_FLAG_INVISIBLE;

    building_place_markers(bs, ent);
    bs->state = BUILDING_STATE_FOUNDED;

    M_NavBlockersIncrefOBB(s_map, &obb);
    return true;
}

bool G_Building_Complete(struct entity *ent)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);

    if(bs->state != BUILDING_STATE_FOUNDED)
        return false;

    struct entity *progress = G_EntityForUID(bs->progress_model);
    if(progress) {
        G_RemoveEntity(progress);
        G_SafeFree(progress);
    }
    building_clear_markers(bs);

    bs->state = BUILDING_STATE_COMPLETED;
    bs->progress_model = UID_NONE;
    ent->flags &= ~ENTITY_FLAG_INVISIBLE;

    return true;
}

bool G_Building_Unobstructed(const struct entity *ent)
{
    struct obb obb;
    Entity_CurrentOBB(ent, &obb, true);

    return M_NavObjectBuildable(s_map, &obb);
}

bool G_Building_IsFounded(struct entity *ent)
{
    struct buildstate *bs = buildstate_get(ent->uid);
    assert(bs);
    return (bs->state >= BUILDING_STATE_FOUNDED);
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
}

