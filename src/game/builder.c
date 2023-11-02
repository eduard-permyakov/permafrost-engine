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

#include "builder.h"
#include "building.h"
#include "harvester.h"
#include "storage_site.h"
#include "game_private.h"
#include "movement.h"
#include "position.h"
#include "public/game.h"
#include "../sched.h"
#include "../entity.h"
#include "../perf.h"
#include "../event.h"
#include "../cursor.h"
#include "../phys/public/collision.h"
#include "../map/public/map.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"

#include <stdint.h>
#include <assert.h>

#define UID_NONE  (~((uint32_t)0))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

struct builderstate{
    enum{
        STATE_NOT_BUILDING,
        STATE_MOVING_TO_TARGET,
        STATE_BUILDING,
    }state;
    int build_speed;
    uint32_t target_uid;
};

KHASH_MAP_INIT_INT(state, struct builderstate)

static void on_build_anim_finished(void *user, void *event);
static void on_motion_end(void *user, void *event);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(state) *s_entity_state_table;
static struct map     *s_map;
static bool            s_build_on_lclick = false;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct builderstate *builderstate_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;

    return &kh_value(s_entity_state_table, k);
}

static void builderstate_set(uint32_t uid, struct builderstate bs)
{
    int ret;
    khiter_t k = kh_put(state, s_entity_state_table, uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_state_table, k) = bs;
}

static void builderstate_remove(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
}

static void on_motion_begin(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct builderstate *bs = builderstate_get(uid);
    assert(bs);
    assert(bs->state == STATE_BUILDING);

    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_build_anim_finished);

    bs->state = STATE_NOT_BUILDING;
    E_Entity_Notify(EVENT_BUILD_END, uid, NULL, ES_ENGINE);
}

static void finish_building(struct builderstate *bs, uint32_t uid)
{
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_build_anim_finished);
    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_motion_end);

    if(bs->state == STATE_BUILDING) {
        E_Entity_Notify(EVENT_BUILD_END, uid, NULL, ES_ENGINE);
    }

    bs->state = STATE_NOT_BUILDING;
    bs->target_uid = UID_NONE;
}

static void on_build_anim_finished(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct builderstate *bs = builderstate_get(uid);
    assert(bs);

    if(!G_EntityExists(bs->target_uid)|| (G_FlagsGet(bs->target_uid) & ENTITY_FLAG_ZOMBIE)) {
        finish_building(bs, uid);
        return;
    }

    if(!(G_FlagsGet(bs->target_uid) & ENTITY_FLAG_COMBATABLE)) {
        G_Building_Complete(bs->target_uid);
        finish_building(bs, uid);
        return;
    }

    int max_hp = G_Combat_GetMaxHP(bs->target_uid);
    int hp = MIN(G_Combat_GetCurrentHP(bs->target_uid) + bs->build_speed, max_hp);

    G_Combat_SetCurrentHP(bs->target_uid, hp);
    G_Building_UpdateProgress(bs->target_uid, hp / (float)max_hp);

    if(hp == max_hp) {
        G_Building_Complete(bs->target_uid);
        finish_building(bs, uid);
        return;
    }
}

static void on_motion_end(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct builderstate *bs = builderstate_get(uid);
    assert(bs);

    if(!G_Move_Still(uid))
        return;

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_motion_end);
    assert(bs->target_uid != UID_NONE);

    if(!G_EntityExists(bs->target_uid)
    || !M_NavObjAdjacent(s_map, uid, bs->target_uid)) {
        bs->state = STATE_NOT_BUILDING;
        bs->target_uid = UID_NONE;
        return; /* builder could not reach the building */
    }

    if(!G_Building_IsFounded(bs->target_uid)) {

        if(G_Building_Unobstructed(bs->target_uid) && G_Building_Found(bs->target_uid, true)) {
            E_Entity_Notify(EVENT_BUILDING_FOUNDED, bs->target_uid, NULL, ES_ENGINE);
        }else{
            bs->state = STATE_NOT_BUILDING;
            bs->target_uid = UID_NONE;
            E_Entity_Notify(EVENT_BUILD_FAIL_FOUND, uid, NULL, ES_ENGINE);
            return; 
        }
    }

    if(!G_Building_IsSupplied(bs->target_uid)
    && G_StorageSite_IsSaturated(bs->target_uid)) {
        G_Building_Supply(bs->target_uid);
    }

    if(!G_Building_IsSupplied(bs->target_uid)) {
        uint32_t flags = G_FlagsGet(uid);
        if(flags & ENTITY_FLAG_HARVESTER) {
            G_Harvester_Stop(uid);
            G_Harvester_SupplyBuilding(uid, bs->target_uid);
        }
        bs->state = STATE_NOT_BUILDING;
        return;
    }

    bs->state = STATE_BUILDING; 
    E_Entity_Notify(EVENT_BUILD_BEGIN, uid, NULL, ES_ENGINE);
    E_Entity_Register(EVENT_MOTION_START, uid, on_motion_begin, (void*)((uintptr_t)uid), G_RUNNING);
    E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_build_anim_finished, 
        (void*)((uintptr_t)uid), G_RUNNING);
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    bool targeting = G_Builder_InTargetMode();
    bool right = (mouse_event->button == SDL_BUTTON_RIGHT);
    bool left = (mouse_event->button == SDL_BUTTON_LEFT);

    s_build_on_lclick = false;

    if(G_MouseOverMinimap())
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    if(right && targeting)
        return;

    if(left && !targeting)
        return;

    if(right && G_CurrContextualAction() != CTX_ACTION_BUILD)
        return;

    uint32_t target = G_Sel_GetHovered();
    if(!G_EntityExists(target)|| !(G_FlagsGet(target) & ENTITY_FLAG_BUILDING) || !G_Building_NeedsRepair(target))
        return;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);
    size_t nbuilding = 0;

    if(sel_type != SELECTION_TYPE_PLAYER)
        return;

    for(int i = 0; i < vec_size(sel); i++) {

        uint32_t curr = vec_AT(sel, i);
        uint32_t flags = G_FlagsGet(curr);

        if(!(flags & ENTITY_FLAG_BUILDER))
            continue;

        struct builderstate *bs = builderstate_get(curr);
        assert(bs);

        finish_building(bs, curr);
        G_StopEntity(curr, true);
        G_Builder_Build(curr, target);
        G_NotifyOrderIssued(curr);
        nbuilding++;
    }

    if(nbuilding) {
        Entity_Ping(target);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Builder_Init(struct map *map)
{
    if(NULL == (s_entity_state_table = kh_init(state)))
        return false;

    s_map = map;
    s_build_on_lclick = false;
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    return true;
}

void G_Builder_Shutdown(void)
{
    s_map = NULL;
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    kh_destroy(state, s_entity_state_table);
}

bool G_Builder_Build(uint32_t uid, uint32_t building)
{
    struct builderstate *bs = builderstate_get(uid);
    assert(bs);

    if(!(G_FlagsGet(building) & ENTITY_FLAG_BUILDING))
        return false;

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_motion_end);
    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_build_anim_finished);

    bs->state = STATE_MOVING_TO_TARGET;
    bs->target_uid = building;
    E_Entity_Notify(EVENT_BUILD_TARGET_ACQUIRED, uid, (void*)((uintptr_t)building), ES_ENGINE);

    if(M_NavObjAdjacent(s_map, uid, building)) {
        on_motion_end((void*)((uintptr_t)uid), NULL);
    }else{
        G_Move_SetSurroundEntity(uid, building);
        E_Entity_Register(EVENT_MOTION_END, uid, on_motion_end, (void*)((uintptr_t)uid), G_RUNNING);
    }

    return true;
}

void G_Builder_AddEntity(uint32_t uid)
{
    assert(builderstate_get(uid) == NULL);

    builderstate_set(uid, (struct builderstate){
        .state = STATE_NOT_BUILDING,
        .build_speed = 0.0,
        .target_uid = UID_NONE,
    });
}

void G_Builder_RemoveEntity(uint32_t uid)
{
    uint32_t flags = G_FlagsGet(uid);
    if(!(flags & ENTITY_FLAG_BUILDER))
        return;
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_motion_end);
    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_build_anim_finished);
    builderstate_remove(uid);
}

void G_Builder_SetBuildSpeed(uint32_t uid, int speed)
{
    struct builderstate *bs = builderstate_get(uid);
    assert(bs);
    bs->build_speed = speed;
}

int G_Builder_GetBuildSpeed(uint32_t uid)
{
    struct builderstate *bs = builderstate_get(uid);
    assert(bs);
    return bs->build_speed;
}

void G_Builder_SetBuildOnLeftClick(void)
{
    s_build_on_lclick = true;
}

bool G_Builder_InTargetMode(void)
{
    return s_build_on_lclick;
}

bool G_Builder_HasRightClickAction(void)
{
    uint32_t hovered = G_Sel_GetHovered();
    if(!G_EntityExists(hovered))
        return false;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);

    if(vec_size(sel) == 0)
        return false;

    uint32_t first = vec_AT(sel, 0);
    uint32_t flags = G_FlagsGet(first);

    if(flags & ENTITY_FLAG_BUILDER 
    && G_FlagsGet(hovered) & ENTITY_FLAG_BUILDING
    && G_Building_IsFounded(hovered))
        return true;

    return false;
}

int G_Builder_CurrContextualAction(void)
{
    uint32_t hovered = G_Sel_GetHovered();
    if(!G_EntityExists(hovered))
        return CTX_ACTION_NONE;

    if(M_MouseOverMinimap(s_map))
        return CTX_ACTION_NONE;

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    if(S_UI_MouseOverWindow(mouse_x, mouse_y))
        return CTX_ACTION_NONE;

    if(G_Builder_InTargetMode())
        return CTX_ACTION_NONE;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);

    if(vec_size(sel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return CTX_ACTION_NONE;

    uint32_t first = vec_AT(sel, 0);
    if(!(G_FlagsGet(first) & ENTITY_FLAG_BUILDER))
        return CTX_ACTION_NONE;

    if(G_GetFactionID(hovered) != G_GetFactionID(first))
        return false;

    if(G_FlagsGet(hovered) & ENTITY_FLAG_BUILDING
    && G_Building_NeedsRepair(hovered))
        return CTX_ACTION_BUILD;

    return CTX_ACTION_NONE;
}

void G_Builder_Stop(uint32_t uid)
{
    struct builderstate *bs = builderstate_get(uid);
    assert(bs);
    finish_building(bs, uid);
}

bool G_Builder_SaveState(struct SDL_RWops *stream)
{
    struct attr num_builders = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_entity_state_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_builders, "num_builders"));
    Sched_TryYield();

    uint32_t uid;
    struct builderstate curr;

    kh_foreach(s_entity_state_table, uid, curr, {

        struct attr buid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &buid, "builder_uid"));

        struct attr state = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.state
        };
        CHK_TRUE_RET(Attr_Write(stream, &state, "builder_state"));

        struct attr speed = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.build_speed
        };
        CHK_TRUE_RET(Attr_Write(stream, &speed, "builder_speed"));

        struct attr target = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.target_uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &target, "builder_target"));
        Sched_TryYield();
    });

    return true;
}

bool G_Builder_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_builders = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_builders; i++) {
    
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int state = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int speed = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int target = attr.val.as_int;

        struct builderstate *bs = builderstate_get(uid);
        CHK_TRUE_RET(bs);

        bs->state = state;
        bs->build_speed = speed;
        bs->target_uid = target;

        switch(state) {
        case STATE_NOT_BUILDING:
            break;
        case STATE_MOVING_TO_TARGET:
            E_Entity_Register(EVENT_MOTION_END, uid, 
                on_motion_end, (void*)((uintptr_t)uid), G_RUNNING);
            break;
        case STATE_BUILDING:
            E_Entity_Register(EVENT_MOTION_START, uid, 
                on_motion_begin, (void*)((uintptr_t)uid), G_RUNNING);
            E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, 
                on_build_anim_finished, (void*)((uintptr_t)uid), G_RUNNING);
            break;
        default:
            return false;
        }
        Sched_TryYield();
    }
    return true;
}

