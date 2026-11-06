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
#define MEM_FILE_SUB MEM_SUB_GAME_BUILDER

#include "builder.h"
#include "duty.h"
#include "building.h"
#include "harvester.h"
#include "command.h"
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
#include "../lib/public/pf_string.h"

#include <stdint.h>
#include <limits.h>
#include <assert.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_BUILDER)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_BUILDER)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_BUILDER)

#define UID_NONE  (~((uint32_t)0))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define FOUND_RETRY_TICKS (10)
#define SUPPLY_RETRY_TICKS (20)
/* How long a builder by order waits at its site for carriers before it fetches the rest itself */
#define SUPPLY_WAIT_TICKS  (160)
#define FOUND_CLEAR_TICKS (40)
#define FOUND_WAIT_TICKS  (600) /* half a minute for a busy footprint to empty before giving up */
#define STILL_ARRIVAL_TICKS (40)

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
        STATE_WAIT_FOUND,
        STATE_WAIT_SUPPLY,
    }state;
    int build_speed;
    uint32_t target_uid;
    int wait_ticks;
    /* How many were last found standing on the site to be founded */
    int clear_left;
    /* Sent by the player, it sees the site through its supply to the end */
    bool ordered;
};

KHASH_MAP_INIT_INT(state, struct builderstate)

static void on_build_anim_finished(void *user, void *event);
static void on_motion_end(void *user, void *event);
static void proceed_at_site(uint32_t uid, struct builderstate *bs);
static void complete(struct builderstate *bs, uint32_t uid);
static bool impatient_now(const struct builderstate *bs);
static void approach(uint32_t uid, struct builderstate *bs, uint32_t building);
static void next_command(uint32_t uid);
static bool wants_builder(uint32_t building);
static bool start_build(uint32_t uid, uint32_t building, bool ordered);

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

static void finish_building(struct builderstate *bs, uint32_t uid, bool work_field)
{
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_build_anim_finished);
    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_motion_end);

    if(bs->state == STATE_BUILDING) {
        E_Entity_Notify(EVENT_BUILD_END, uid, NULL, ES_ENGINE);
    }

    uint32_t building = bs->target_uid;
    bs->state = STATE_NOT_BUILDING;
    bs->target_uid = UID_NONE;

    if(building == NULL_UID)
        return;

    if(!G_EntityExists(building) || G_EntityIsZombie(building))
        return;

    /* With more orders queued, it goes on to them rather than working the field */
    size_t ncmds;
    G_Command_Get(uid, &ncmds);

    if(work_field
    && G_Building_IsCompleted(building)
    && (G_FlagsGet(building) & ENTITY_FLAG_RESOURCE)
    && (G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER)
    && ncmds == 0
    && G_Harvester_Gather(uid, building)) {
        G_Automation_TraceFollowOn(uid, "gather", "field", G_Resource_GetName(building), building);
    }
}

/* The builder's duty ends with the site; one that works the field it raised
 * gathers for whoever had it build */
static void complete(struct builderstate *bs, uint32_t uid)
{
    struct duty duty;
    bool had = G_Duty_Get(uid, &duty);
    G_Building_Complete(bs->target_uid);
    if(had) {
        G_Duty_TagNext(uid, duty.by);
    }
    finish_building(bs, uid, true);
    /* Not the field it went on to work, whose duty it has just taken */
    if(G_Duty_Kind(uid) != DUTY_GATHER) {
        G_Duty_End(uid, NULL);
    }
    /* What it fetched beyond the site's need goes to a store */
    if((G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER) && G_Harvester_Idle(uid) && G_Harvester_GetCurrTotalCarry(uid)) {
        G_Harvester_BringInLoad(uid, NULL_UID, false);
    }
    next_command(uid);
}

static void on_build_anim_finished(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct builderstate *bs = builderstate_get(uid);
    assert(bs);

    if(!G_EntityExists(bs->target_uid)|| (G_FlagsGet(bs->target_uid) & ENTITY_FLAG_ZOMBIE)
    || G_Combat_IsDying(bs->target_uid)) {
        finish_building(bs, uid, false);
        next_command(uid);
        return;
    }

    if(!(G_FlagsGet(bs->target_uid) & ENTITY_FLAG_COMBATABLE)) {
        complete(bs, uid);
        return;
    }

    int max_hp = G_Combat_GetMaxHP(bs->target_uid);
    int hp = MIN(G_Combat_GetCurrentHP(bs->target_uid) + bs->build_speed, max_hp);

    G_Combat_SetCurrentHP(bs->target_uid, hp);
    G_Building_UpdateProgress(bs->target_uid, hp / (float)max_hp);

    if(hp == max_hp) {
        complete(bs, uid);
    }
}

static bool try_found_site(uint32_t target)
{
    bool clear = G_Building_GetPathable(target) || G_Building_Foundable(target);
    if(!clear || !G_Building_Found(target, true))
        return false;
    E_Entity_Notify(EVENT_BUILDING_FOUNDED, target, NULL, ES_ENGINE);
    return true;
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

    if(!G_EntityExists(bs->target_uid) || G_EntityIsZombie(bs->target_uid)) {
        bs->state = STATE_NOT_BUILDING;
        bs->target_uid = UID_NONE;
        next_command(uid);
        return;
    }

    /* Others crowding the site kept it out of reach: it walks up again from the wait */
    if(!M_NavObjAdjacent(s_map, uid, bs->target_uid)) {
        bs->state = STATE_WAIT_FOUND;
        return;
    }

    if(!G_Building_IsFounded(bs->target_uid) && !try_found_site(bs->target_uid)) {
        G_Building_ClearFootprint(bs->target_uid);
        bs->state = STATE_WAIT_FOUND;
        return;
    }

    proceed_at_site(uid, bs);
}

static bool unfounded_site_queued(uint32_t uid)
{
    size_t ncmds;
    const struct command *cmds = G_Command_Get(uid, &ncmds);
    for(int i = 0; i < ncmds; i++) {
        uint32_t site = cmds[i].target;
        if(cmds[i].kind != CMD_BUILD || !G_EntityExists(site) || G_EntityIsZombie(site))
            continue;
        if((G_FlagsGet(site) & ENTITY_FLAG_BUILDING) && !G_Building_IsFounded(site))
            return true;
    }
    return false;
}

/* Whether the builder has waited at its site long enough to fetch for it itself */
static bool impatient_now(const struct builderstate *bs)
{
    return bs->state == STATE_WAIT_SUPPLY && bs->wait_ticks >= SUPPLY_WAIT_TICKS;
}

static void proceed_at_site(uint32_t uid, struct builderstate *bs)
{
    /* The ground for every site in the queue is laid before any is raised */
    if(unfounded_site_queued(uid)) {
        uint32_t site = bs->target_uid;
        bs->state = STATE_NOT_BUILDING;
        bs->target_uid = UID_NONE;
        G_Command_Push(uid, (struct command){CMD_BUILD, site});
        return;
    }

    if(!G_Building_IsSupplied(bs->target_uid)
    && G_StorageSite_IsSaturated(bs->target_uid)) {
        G_Building_Supply(bs->target_uid);
    }

    if(!G_Building_IsSupplied(bs->target_uid)) {
        uint32_t flags = G_FlagsGet(uid);
        bool supplying = false, gathering = false;
        /* One with the site for its duty sees it through like one ordered to it */
        bool ordered = bs->ordered || (G_Duty_Kind(uid) == DUTY_BUILD);
        if(flags & ENTITY_FLAG_HARVESTER) {
            bool impatient = impatient_now(bs);
            G_Harvester_Stop(uid);
            supplying = G_Harvester_SupplyBuilding(uid, bs->target_uid, impatient);
            /* Nothing in store can be had for it: it gathers the rest itself */
            if(!supplying && impatient) {
                gathering = supplying = G_Harvester_GatherForSite(uid, bs->target_uid);
            }
        }
        if(supplying) {
            bs->state = STATE_NOT_BUILDING;
            if(ordered && !gathering) {
                /* Back to it once the materials are in */
                G_Command_PushFront(uid, (struct command){CMD_BUILD, bs->target_uid});
            }
        }else if(ordered && (bs->ordered || G_Duty_ByPlayer(uid) || !impatient_now(bs))) {
            /* The materials are on their way with others: it waits at the site for them */
            if(bs->state != STATE_WAIT_SUPPLY) {
                bs->wait_ticks = 0;
            }
            bs->state = STATE_WAIT_SUPPLY;
        }else if(ordered) {
            /* One the automation sent to a site nothing can supply is let go of it */
            G_Duty_End(uid, "starved");
            G_Command_Clear(uid);
            bs->state = STATE_NOT_BUILDING;
            next_command(uid);
        }else{
            bs->state = STATE_NOT_BUILDING;
            next_command(uid);
        }
        return;
    }

    /* A finished field is worked, and its builder is done with it */
    if(G_Building_IsCompleted(bs->target_uid)
    && (G_FlagsGet(bs->target_uid) & ENTITY_FLAG_RESOURCE)
    && (G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER)) {
        finish_building(bs, uid, true);
        next_command(uid);
        return;
    }

    bs->state = STATE_BUILDING; 
    E_Entity_Notify(EVENT_BUILD_BEGIN, uid, NULL, ES_ENGINE);
    E_Entity_Register(EVENT_MOTION_START, uid, on_motion_begin, (void*)((uintptr_t)uid), G_RUNNING);
    E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_build_anim_finished, 
        (void*)((uintptr_t)uid), G_RUNNING);
}

/* Saves from before a builder let go of the finished field it went to work
 * still have it on its way there while it gathers */
static bool working_field(uint32_t uid, const struct builderstate *bs)
{
    if(!G_EntityExists(bs->target_uid) || G_EntityIsZombie(bs->target_uid))
        return false;
    if(!(G_FlagsGet(bs->target_uid) & ENTITY_FLAG_RESOURCE) || !G_Building_IsCompleted(bs->target_uid))
        return false;
    return (G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER) && G_Harvester_Gathering(uid) != NULL;
}

static void on_20hz_tick(void *user, void *event)
{
    uint32_t uid;
    struct builderstate *bs;

    kh_foreach_val_ptr(s_entity_state_table, uid, bs, {

        if(bs->state == STATE_MOVING_TO_TARGET && working_field(uid, bs)) {
            E_Entity_Unregister(EVENT_MOTION_END, uid, on_motion_end);
            bs->state = STATE_NOT_BUILDING;
            bs->target_uid = UID_NONE;
            continue;
        }

        /* An arrival whose motion end it never heard is taken as one */
        if(bs->state == STATE_MOVING_TO_TARGET) {
            if(!G_Move_Still(uid)) {
                bs->wait_ticks = 0;
            }else if(++bs->wait_ticks >= STILL_ARRIVAL_TICKS) {
                bs->wait_ticks = 0;
                on_motion_end((void*)((uintptr_t)uid), NULL);
            }
            continue;
        }

        if(bs->state == STATE_WAIT_SUPPLY) {
            if(!G_EntityExists(bs->target_uid) || G_EntityIsZombie(bs->target_uid)) {
                bs->state = STATE_NOT_BUILDING;
                bs->target_uid = UID_NONE;
                next_command(uid);
                continue;
            }
            if(++bs->wait_ticks % SUPPLY_RETRY_TICKS != 0 || !G_Move_Still(uid))
                continue;
            if(!M_NavObjAdjacent(s_map, uid, bs->target_uid)) {
                approach(uid, bs, bs->target_uid);
                continue;
            }
            proceed_at_site(uid, bs);
            continue;
        }

        if(bs->state != STATE_WAIT_FOUND)
            continue;

        if(!G_EntityExists(bs->target_uid) || G_EntityIsZombie(bs->target_uid)) {
            bs->state = STATE_NOT_BUILDING;
            bs->target_uid = UID_NONE;
            next_command(uid);
            continue;
        }

        bs->wait_ticks++;
        if(bs->wait_ticks % FOUND_RETRY_TICKS == 0) {
            /* One pushed off the footprint itself walks back before trying again */
            if(!G_Move_Still(uid))
                continue;
            if(!M_NavObjAdjacent(s_map, uid, bs->target_uid)) {
                approach(uid, bs, bs->target_uid);
                continue;
            }
            if(G_Building_IsFounded(bs->target_uid) || try_found_site(bs->target_uid)) {
                proceed_at_site(uid, bs);
                continue;
            }
        }

        bool founded = G_Building_IsFounded(bs->target_uid);
        if(!founded && bs->wait_ticks % FOUND_CLEAR_TICKS == 0) {
            int left = G_Building_ClearFootprint(bs->target_uid);
            /* The wait does not run out while those on the site are still leaving it:
             * fewer than ever before, not just fewer than a moment ago, or two
             * shuffling about on it would keep it waiting for good */
            if(left < bs->clear_left) {
                bs->wait_ticks = 0;
                bs->clear_left = left;
            }
        }

        if(bs->wait_ticks >= FOUND_WAIT_TICKS) {
            bs->state = STATE_NOT_BUILDING;
            bs->target_uid = UID_NONE;
            if(!founded) {
                E_Entity_Notify(EVENT_BUILD_FAIL_FOUND, uid, NULL, ES_ENGINE);
            }
            next_command(uid);
        }
    });
}

static void approach(uint32_t uid, struct builderstate *bs, uint32_t building)
{
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_motion_end);
    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_build_anim_finished);

    /* Taken off a building it was still working on */
    if(bs->state == STATE_BUILDING) {
        E_Entity_Notify(EVENT_BUILD_END, uid, NULL, ES_ENGINE);
    }

    bs->state = STATE_MOVING_TO_TARGET;
    bs->target_uid = building;
    E_Entity_Notify(EVENT_BUILD_TARGET_ACQUIRED, uid, (void*)((uintptr_t)building), ES_ENGINE);

    if(M_NavObjAdjacent(s_map, uid, building)) {
        on_motion_end((void*)((uintptr_t)uid), NULL);
    }else{
        G_Move_SetSurroundEntity(uid, building);
        E_Entity_Register(EVENT_MOTION_END, uid, on_motion_end, (void*)((uintptr_t)uid), G_RUNNING);
    }
}

/* A site still to be founded is taken up as one to be raised or mended */
static bool wants_builder(uint32_t building)
{
    if(!(G_FlagsGet(building) & ENTITY_FLAG_BUILDING))
        return false;
    if(G_Building_IsMarked(building) && !G_Building_IsFounded(building))
        return true;
    return G_Building_NeedsRepair(building);
}

/* Takes up the unit's next queued order once its work comes to an end */
static bool start_build(uint32_t uid, uint32_t building, bool ordered)
{
    struct builderstate *bs = builderstate_get(uid);
    assert(bs);

    if(!(G_FlagsGet(building) & ENTITY_FLAG_BUILDING))
        return false;
    /* The order given again after each load is no new order */
    struct duty prior;
    bool resumed = G_Duty_Get(uid, &prior) && prior.target == building
                && (prior.kind == DUTY_BUILD || prior.kind == DUTY_SUPPLY);
    G_Duty_Set(uid, DUTY_BUILD, building, NULL, DUTY_BY_PLAYER);

    /* A gatherer's trip would otherwise take it off to drop its load once it is there */
    if(G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER) {
        G_Harvester_Stop(uid);
        G_Harvester_ClearQueuedCmd(uid);
    }

    bs->wait_ticks = 0;
    bs->clear_left = INT_MAX;
    /* Set before the approach, which finds a site at hand and goes on at once */
    bs->ordered = ordered;
    /* A founded site short of its materials is fetched for first: the walk
     * to it is for raising it */
    if((G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER) && G_Building_IsFounded(building)
    && !G_Building_IsSupplied(building) && !G_StorageSite_IsSaturated(building)
    && !unfounded_site_queued(uid) && G_Harvester_SupplyBuilding(uid, building, false)) {
        bs->state = STATE_NOT_BUILDING;
        bs->target_uid = building;
        G_Command_PushFront(uid, (struct command){CMD_BUILD, building});
    }else{
        approach(uid, bs, building);
    }
    /* One that would go hungry on the way eats first; its duty brings it on.
     * The ground comes before the meal: a site is nothing until it is founded
     */
    if(!resumed && G_Building_IsFounded(building)) {
        G_Provision_EatFirst(uid, building);
    }
    return true;
}

static void next_command(uint32_t uid)
{
    if((G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER) && !G_Harvester_Idle(uid))
        return;
    G_Command_Next(uid);
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

    /* While a tool such as placement holds the mouse, a right click gives no order */
    if(right && !G_Move_GetClickEnabled())
        return;

    if(left && !targeting)
        return;

    if(right && G_CurrContextualAction() != CTX_ACTION_BUILD)
        return;

    uint32_t target = G_Sel_GetHovered();
    if(!G_EntityExists(target) || !wants_builder(target))
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

        if(G_Builder_PlayerOrder(curr, target)) {
            nbuilding++;
        }
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
    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
    return true;
}

void G_Builder_Shutdown(void)
{
    s_map = NULL;
    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    kh_destroy(state, s_entity_state_table);
}

bool G_Builder_Build(uint32_t uid, uint32_t building)
{
    return start_build(uid, building, false);
}

bool G_Builder_Order(uint32_t uid, uint32_t building)
{
    return start_build(uid, building, true);
}

bool G_Builder_PlayerOrder(uint32_t uid, uint32_t building)
{
    struct builderstate *bs = builderstate_get(uid);
    if(!bs || !G_EntityExists(building) || !wants_builder(building))
        return false;

    finish_building(bs, uid, false);
    G_StopEntity(uid, true, true);
    /* Before the order, which may already queue the return to the site it is to supply */
    G_NotifyOrderIssued(uid, true, true);
    return G_Builder_Order(uid, building);
}

void G_Builder_AddEntity(uint32_t uid)
{
    assert(builderstate_get(uid) == NULL);

    builderstate_set(uid, (struct builderstate){
        .state = STATE_NOT_BUILDING,
        .build_speed = 0.0,
        .target_uid = UID_NONE,
        .wait_ticks = 0,
        .clear_left = INT_MAX,
        .ordered = false,
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

    if(wants_builder(hovered))
        return CTX_ACTION_BUILD;

    return CTX_ACTION_NONE;
}

void G_Builder_Stop(uint32_t uid)
{
    struct builderstate *bs = builderstate_get(uid);
    assert(bs);
    finish_building(bs, uid, false);
}

bool G_Builder_WaitingSupply(uint32_t uid)
{
    struct builderstate *bs = builderstate_get(uid);
    return bs && bs->state == STATE_WAIT_SUPPLY;
}

bool G_Builder_Idle(uint32_t uid)
{
    struct builderstate *bs = builderstate_get(uid);
    assert(bs);
    return (bs->state == STATE_NOT_BUILDING);
}

uint32_t G_Builder_Target(uint32_t uid)
{
    struct builderstate *bs = builderstate_get(uid);
    if(!bs || bs->state == STATE_NOT_BUILDING)
        return NULL_UID;
    return bs->target_uid;
}

size_t G_Builder_DebugDesc(uint32_t uid, char *out, size_t len)
{
    struct builderstate *bs = builderstate_get(uid);
    if(!bs)
        return pf_snprintf(out, len, "none");
    return pf_snprintf(out, len, "state=%d target=%u wait=%d clear_left=%d ordered=%d",
        (int)bs->state, bs->target_uid, bs->wait_ticks, bs->clear_left, (int)bs->ordered);
}

int G_Builder_CountOn(uint32_t building)
{
    int ret = 0;
    struct builderstate bs;
    kh_foreach_value(s_entity_state_table, bs, {
        if(bs.state != STATE_NOT_BUILDING && bs.target_uid == building)
            ret++;
    });
    return ret;
}

size_t G_Builder_Targets(size_t maxout, uint32_t out[])
{
    if(!s_entity_state_table)
        return 0;
    size_t ret = 0;
    struct builderstate bs;
    kh_foreach_value(s_entity_state_table, bs, {
        if(ret < maxout && bs.state != STATE_NOT_BUILDING) {
            out[ret++] = bs.target_uid;
        }
    });
    return ret;
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

        struct attr wait_ticks = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.wait_ticks
        };
        CHK_TRUE_RET(Attr_Write(stream, &wait_ticks, "builder_wait_ticks"));

        struct attr clear_left = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.clear_left
        };
        CHK_TRUE_RET(Attr_Write(stream, &clear_left, "builder_clear_left"));

        struct attr ordered = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.ordered
        };
        CHK_TRUE_RET(Attr_Write(stream, &ordered, "builder_ordered"));
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

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int wait_ticks = attr.val.as_int;

        /* Saves from before builders counted the units left on a footprint */
        int clear_left = INT_MAX;
        if(Attr_NextKeyIs(stream, "builder_clear_left")) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            clear_left = attr.val.as_int;
        }

        bool ordered = false;
        if(Attr_NextKeyIs(stream, "builder_ordered")) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_BOOL);
            ordered = attr.val.as_bool;
        }

        struct builderstate *bs = builderstate_get(uid);
        CHK_TRUE_RET(bs);

        bs->state = state;
        bs->build_speed = speed;
        bs->target_uid = target;
        bs->wait_ticks = wait_ticks;
        bs->clear_left = clear_left;
        bs->ordered = ordered;

        switch(state) {
        case STATE_NOT_BUILDING:
        case STATE_WAIT_FOUND:
        case STATE_WAIT_SUPPLY:
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

