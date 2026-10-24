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
 *
 */

#define MEM_FILE_SYS MEM_SYS_GAME
#define MEM_FILE_SUB MEM_SUB_GAME_PROVISION

#include "provision.h"
#include "public/game.h"
#include "game_private.h"
#include "movement.h"
#include "combat.h"
#include "position.h"
#include "storage_site.h"
#include "harvester.h"
#include "builder.h"
#include "../event.h"
#include "../entity.h"
#include "../sched.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/string_intern.h"
#include "../map/public/map.h"

#include <assert.h>
#include <string.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_PROVISION)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_PROVISION)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_PROVISION)

#define TICK_MS             (50)
/* Each unit is looked at once a second, a twentieth of them each tick */
#define BUCKETS             (20)
/* A unit visiting a store eats there if its next meal is due this soon */
#define EAT_AHEAD_MS        (60 * 1000)
/* A worker whose next meal is due this soon eats before it takes on more work */
#define WORK_AHEAD_MS       (90 * 1000)
#define GOOD_LEN            (32)
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define FETCH_RETRY_MS      (5 * 1000)
/* Standing still this long on the way to a store, or jostled in a crowd at it
 * for this long without getting any nearer, counts as having got there */
#define FETCH_STALL_MS      (10 * 1000)
#define FETCH_MIN_PROGRESS  (16.0f)
/* A unit kept off a crowded store by others takes what it came for this close */
#define STORE_REACH         (30.0f)
#define MAX_UPKEEP_GOODS    (4)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

/* What a unit lives on: the meals it has in it, how often it eats, and how
 * hungry it is when it has none */
struct provision{
    char     good[GOOD_LEN];
    int      meals;
    int      max_meals;
    uint32_t interval_ms;
    uint32_t starve_after_ms;
    uint32_t next_due_ms;
    int      tier;
    uint32_t tier_since_ms;
    /* Goes for food by itself as soon as it is hungry */
    bool     automatic;
    /* On its way to eat at this site */
    uint32_t fetching;
    uint32_t fetch_ms;
    /* How far from the store it was when the stall was last looked at */
    float    fetch_dist;
    /* No store had food the last time it looked: look again then, and work meanwhile */
    uint32_t retry_ms;
    bool     no_food;
    /* A unit with no work to go back to returns here once it has eaten */
    bool     go_back;
    vec2_t   post;
    /* The good a gatherer sent by the player goes back to gathering about its post */
    char     resume[GOOD_LEN];
    /* Got there this many times without reaching the store, each time from further out */
    int      tries;
};

KHASH_MAP_INIT_INT(prov, struct provision)

/* What a ranged unit shoots: the good its shots come in, the shots in one of 
 * it, and how many it carries when full */
struct quiver{
    char     good[GOOD_LEN];
    int      shots_per_unit;
    int      max_units;
    /* Goes for more by itself when running low */
    bool     automatic;
    uint32_t fetching;
    uint32_t fetch_ms;
    uint32_t retry_ms;
    /* Empty when last told, to tell again only on a change */
    bool     empty;
    /* Where it stood when it went for more, to go back to */
    vec2_t   post;
    int      tries;
};

KHASH_MAP_INIT_INT(quiver, struct quiver)

static void on_20hz_tick(void *user, void *event);
static void on_arrive(void *user, void *event);
static void arrive(uint32_t uid);
static float dist_to(uint32_t uid, uint32_t site);
static void on_ordered(void *user, void *event);
static void note_upkeep_good(const char *good);
static uint32_t first_meal_delay_ms(uint32_t uid, uint32_t interval_ms);
static bool carrying(uint32_t uid);
static bool at_store(uint32_t uid, uint32_t site, int tries);
static void ordered_gathering(uint32_t uid, char *out, size_t maxout);
static bool free_to_fetch(uint32_t uid);
static void on_arrive_quiver(void *user, void *event);
static void on_ordered_quiver(void *user, void *event);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map *s_map;
static khash_t(prov) *s_table;
static khash_t(quiver) *s_quivers;
static uint32_t       s_now_ms;
static uint32_t       s_tick;
/* Every good some unit lives on */
static char           s_upkeep_goods[MAX_UPKEEP_GOODS][GOOD_LEN];
static int            s_num_upkeep_goods;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct provision *prov_get(uint32_t uid)
{
    khiter_t k = kh_get(prov, s_table, uid);
    if(k == kh_end(s_table))
        return NULL;
    return &kh_value(s_table, k);
}

/* Units made together do not all go hungry together */
static uint32_t first_meal_delay_ms(uint32_t uid, uint32_t interval_ms)
{
    uint32_t spread = interval_ms / 2;
    if(spread == 0)
        return interval_ms;
    return interval_ms - spread + (uint32_t)((uid * 2654435761u) % spread);
}

static void note_upkeep_good(const char *good)
{
    for(int i = 0; i < s_num_upkeep_goods; i++) {
        if(!strcmp(s_upkeep_goods[i], good))
            return;
    }
    if(s_num_upkeep_goods == MAX_UPKEEP_GOODS)
        return;
    pf_strlcpy(s_upkeep_goods[s_num_upkeep_goods++], good, GOOD_LEN);
}

static void set_tier(uint32_t uid, struct provision *p, int tier)
{
    if(p->tier == tier)
        return;
    p->tier = tier;
    p->tier_since_ms = s_now_ms;
    E_Global_Notify(EVENT_PROVISION_CHANGED, (void*)(uintptr_t)uid, ES_ENGINE);
}

static void eat(uint32_t uid, struct provision *p, uint32_t site)
{
    int stock = G_StorageSite_GetCurr(site, p->good);
    if(stock <= 0)
        return;
    G_StorageSite_SetCurr(site, p->good, stock - 1);
    G_StorageSite_RecordConsumed(G_GetFactionID(site), p->good, 1);
    p->meals = p->max_meals;
    p->next_due_ms = s_now_ms + p->interval_ms;
    set_tier(uid, p, PROVISION_FED);
}

struct food_search{
    int         faction;
    const char *good;
    uint32_t    eater;
};

static bool has_food(uint32_t site, void *arg)
{
    struct food_search *fs = arg;
    if(!(G_FlagsGet(site) & ENTITY_FLAG_STORAGE_SITE))
        return false;
    if(G_EntityIsZombie(site) || G_GetFactionID(site) != fs->faction)
        return false;
    if(G_StorageSite_GetUseAlt(site))
        return false;
    if(G_StorageSite_GetCurr(site, fs->good) <= 0)
        return false;
    if((G_FlagsGet(fs->eater) & ENTITY_FLAG_HARVESTER) && !G_Harvester_CanReachSite(fs->eater, site))
        return false;
    return true;
}

static bool fetch(uint32_t uid, struct provision *p)
{
    struct food_search arg = (struct food_search){
        .faction = G_GetFactionID(uid),
        .good = p->good,
        .eater = uid
    };
    uint32_t site = G_Pos_NearestWithPred(G_Pos_GetXZ(uid), has_food, &arg, 0.0f);
    p->no_food = (site == NULL_UID);
    if(site == NULL_UID) {
        p->retry_ms = s_now_ms + FETCH_RETRY_MS;
        return false;
    }

    /* A load the store with food does not take goes to one that does first */
    if(carrying(uid)) {
        if(!G_Harvester_BringInLoad(uid, site) && !G_Harvester_BringInLoad(uid, NULL_UID)) {
            p->retry_ms = s_now_ms + FETCH_RETRY_MS;
            return false;
        }
        return true;
    }

    if(p->fetching == NULL_UID) {
        p->go_back = !(G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER);
        p->post = G_Pos_GetXZ(uid);
        p->tries = 0;
        ordered_gathering(uid, p->resume, sizeof(p->resume));
    }
    p->fetching = site;
    p->fetch_ms = s_now_ms;
    p->fetch_dist = dist_to(uid, site);
    if(G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER) {
        G_Harvester_Stop(uid);
    }
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_ordered);
    E_Entity_Register(EVENT_MOTION_END, uid, on_arrive, (void*)(uintptr_t)uid, G_RUNNING);
    E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_ordered, (void*)(uintptr_t)uid, G_RUNNING);
    G_Move_SetSurroundEntity(uid, site);
    return true;
}

static float dist_to(uint32_t uid, uint32_t site)
{
    vec2_t a = G_Pos_GetXZ(uid), b = G_Pos_GetXZ(site);
    vec2_t delta;
    PFM_Vec2_Sub(&a, &b, &delta);
    return PFM_Vec2_Len(&delta);
}

static void on_arrive(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    if(!G_Move_Still(uid))
        return;
    arrive(uid);
}

static void arrive(uint32_t uid)
{
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_ordered);

    struct provision *p = prov_get(uid);
    if(!p || p->fetching == NULL_UID)
        return;
    uint32_t site = p->fetching;
    p->fetching = NULL_UID;
    if(!G_EntityExists(site) || G_EntityIsZombie(site))
        return;
    /* Sent to build on the way, it eats once the building is done */
    if(!free_to_fetch(uid))
        return;
    /* Others crowding the store kept it out of reach: try again */
    if(!at_store(uid, site, p->tries)) {
        bool go_back = p->go_back;
        vec2_t post = p->post;
        int tries = p->tries + 1;
        char resume[GOOD_LEN];
        pf_strlcpy(resume, p->resume, sizeof(resume));
        fetch(uid, p);
        p->go_back = go_back;
        p->post = post;
        p->tries = tries;
        pf_strlcpy(p->resume, resume, sizeof(p->resume));
        return;
    }
    eat(uid, p, site);
    if(p->resume[0]) {
        uint32_t node = G_Harvester_NearestResource(uid, p->post, p->resume, 0.0f);
        if(node != NULL_UID) {
            G_Harvester_Gather(uid, node);
        }
        p->resume[0] = '\0';
    }else if(p->go_back) {
        G_Move_SetDest(uid, p->post, true);
    }
}

/* The good the harvester gathers by the player's order, or an empty string */
static void ordered_gathering(uint32_t uid, char *out, size_t maxout)
{
    out[0] = '\0';
    if(!(G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER) || G_Automation_ChoseWork(uid))
        return;
    const char *rname = G_Harvester_Gathering(uid);
    if(rname) {
        pf_strlcpy(out, rname, maxout);
    }
}

static bool at_store(uint32_t uid, uint32_t site, int tries)
{
    if(M_NavObjAdjacent(s_map, uid, site))
        return true;
    vec2_t a = G_Pos_GetXZ(uid), b = G_Pos_GetXZ(site);
    vec2_t delta;
    PFM_Vec2_Sub(&a, &b, &delta);
    float reach = G_GetSelectionRadius(uid) + G_GetSelectionRadius(site) + STORE_REACH * (1 + tries);
    return PFM_Vec2_Len(&delta) <= reach;
}

static bool carrying(uint32_t uid)
{
    return (G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER) && G_Harvester_GetCurrTotalCarry(uid) > 0;
}

static void on_ordered(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_ordered);

    struct provision *p = prov_get(uid);
    if(p) {
        p->fetching = NULL_UID;
        p->resume[0] = '\0';
    }
}

/* Whether the unit can leave what it is doing to go and eat */
static bool free_to_fetch(uint32_t uid)
{
    uint32_t flags = G_FlagsGet(uid);
    if(flags & (ENTITY_FLAG_GARRISONED | ENTITY_FLAG_ZOMBIE))
        return false;
    if((flags & ENTITY_FLAG_COMBATABLE) && !G_Combat_Idle(uid))
        return false;
    /* On its way into a building, to be made into something else */
    if((flags & ENTITY_FLAG_GARRISON) && G_Garrison_IsIngredient(uid))
        return false;
    /* A site it was sent to found or build would be left as a ghost */
    if((flags & ENTITY_FLAG_BUILDER) && !G_Builder_Idle(uid))
        return false;
    return true;
}

static void update(uint32_t uid, struct provision *p)
{
    if(p->interval_ms == 0)
        return;
    /* Held in a building to be made into something else, it is kept there */
    uint32_t flags = G_FlagsGet(uid);
    if((flags & ENTITY_FLAG_GARRISONED) && (flags & ENTITY_FLAG_GARRISON) && G_Garrison_IsIngredient(uid)) {
        p->next_due_ms += TICK_MS * BUCKETS;
        p->tier_since_ms += TICK_MS * BUCKETS;
        return;
    }
    if(s_now_ms >= p->next_due_ms) {
        if(p->meals > 0) {
            p->meals--;
            p->next_due_ms = s_now_ms + p->interval_ms;
        }else if(p->tier == PROVISION_FED) {
            set_tier(uid, p, PROVISION_HUNGRY);
        }
    }
    if(p->tier == PROVISION_HUNGRY && s_now_ms - p->tier_since_ms >= p->starve_after_ms) {
        set_tier(uid, p, PROVISION_STARVING);
    }
    /* One jostled at the store gets no second arrival, and one kept moving in
     * the crowd about it none at all */
    if(p->fetching != NULL_UID && s_now_ms - p->fetch_ms >= FETCH_STALL_MS) {
        float dist = dist_to(uid, p->fetching);
        if(G_Move_Still(uid) || dist > p->fetch_dist - FETCH_MIN_PROGRESS) {
            arrive(uid);
        }else{
            p->fetch_ms = s_now_ms;
            p->fetch_dist = dist;
        }
    }
    if(p->tier != PROVISION_FED && p->automatic && p->fetching == NULL_UID 
    && s_now_ms >= p->retry_ms && free_to_fetch(uid)) {
        fetch(uid, p);
    }
}

static struct quiver *quiver_get(uint32_t uid)
{
    khiter_t k = kh_get(quiver, s_quivers, uid);
    if(k == kh_end(s_quivers))
        return NULL;
    return &kh_value(s_quivers, k);
}

static int quiver_max_shots(const struct quiver *q)
{
    return q->shots_per_unit * q->max_units;
}

static void refill(uint32_t uid, struct quiver *q, uint32_t site)
{
    int shots = MAX(G_Combat_GetAmmo(uid), 0);
    int want = (quiver_max_shots(q) - shots + q->shots_per_unit - 1) / q->shots_per_unit;
    int take = MIN(want, G_StorageSite_GetCurr(site, q->good));
    if(take <= 0)
        return;
    G_StorageSite_SetCurr(site, q->good, G_StorageSite_GetCurr(site, q->good) - take);
    G_StorageSite_RecordConsumed(G_GetFactionID(site), q->good, take);
    G_Combat_SetAmmo(uid, MIN(shots + take * q->shots_per_unit, quiver_max_shots(q)));
}

static bool fetch_quiver(uint32_t uid, struct quiver *q)
{
    struct food_search arg = (struct food_search){
        .faction = G_GetFactionID(uid),
        .good = q->good,
        .eater = uid
    };
    uint32_t site = G_Pos_NearestWithPred(G_Pos_GetXZ(uid), has_food, &arg, 0.0f);
    if(site == NULL_UID) {
        q->retry_ms = s_now_ms + FETCH_RETRY_MS;
        return false;
    }
    q->fetching = site;
    q->fetch_ms = s_now_ms;
    q->post = G_Pos_GetXZ(uid);
    q->tries = 0;
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_quiver);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_ordered_quiver);
    E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_quiver, (void*)(uintptr_t)uid, G_RUNNING);
    E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_ordered_quiver, (void*)(uintptr_t)uid, G_RUNNING);
    G_Move_SetSurroundEntity(uid, site);
    return true;
}

static void on_arrive_quiver(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    if(!G_Move_Still(uid))
        return;
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_quiver);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_ordered_quiver);

    struct quiver *q = quiver_get(uid);
    if(!q || q->fetching == NULL_UID)
        return;
    uint32_t site = q->fetching;
    q->fetching = NULL_UID;
    if(!G_EntityExists(site) || G_EntityIsZombie(site))
        return;
    if(!at_store(uid, site, q->tries)) {
        vec2_t post = q->post;
        int tries = q->tries + 1;
        if(fetch_quiver(uid, q)) {
            q->post = post;
            q->tries = tries;
        }
        return;
    }
    refill(uid, q, site);
    G_Move_SetDest(uid, q->post, true);
}

static void on_ordered_quiver(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_quiver);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_ordered_quiver);

    struct quiver *q = quiver_get(uid);
    if(q) {
        q->fetching = NULL_UID;
    }
}

static bool eating(uint32_t uid)
{
    struct provision *p = prov_get(uid);
    return p && p->fetching != NULL_UID;
}

static void update_quiver(uint32_t uid, struct quiver *q)
{
    if(q->shots_per_unit == 0)
        return;
    int shots = G_Combat_GetAmmo(uid);
    bool empty = (shots == 0);
    if(empty != q->empty) {
        q->empty = empty;
        E_Global_Notify(EVENT_PROVISION_CHANGED, (void*)(uintptr_t)uid, ES_ENGINE);
    }
    bool low = (shots >= 0) && (shots * 4 <= quiver_max_shots(q));
    if(q->fetching != NULL_UID && s_now_ms - q->fetch_ms >= FETCH_STALL_MS && G_Move_Still(uid)) {
        on_arrive_quiver((void*)(uintptr_t)uid, NULL);
    }
    if(low && q->automatic && q->fetching == NULL_UID && s_now_ms >= q->retry_ms 
    && !eating(uid) && free_to_fetch(uid)) {
        fetch_quiver(uid, q);
    }
}

static void on_20hz_tick(void *user, void *event)
{
    s_tick++;
    s_now_ms += TICK_MS;
    int bucket = s_tick % BUCKETS;

    uint32_t uid;
    struct provision *p;
    kh_foreach_val_ptr(s_table, uid, p, {
        if(uid % BUCKETS != bucket)
            continue;
        update(uid, p);
    });

    struct quiver *q;
    kh_foreach_val_ptr(s_quivers, uid, q, {
        if(uid % BUCKETS != bucket)
            continue;
        update_quiver(uid, q);
    });
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Provision_Init(const struct map *map)
{
    if(!(s_table = kh_init(prov)))
        return false;
    if(!(s_quivers = kh_init(quiver))) {
        kh_destroy(prov, s_table);
        return false;
    }
    s_now_ms = 0;
    s_tick = 0;
    s_num_upkeep_goods = 0;
    s_map = map;
    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
    return true;
}

void G_Provision_Shutdown(void)
{
    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
    kh_destroy(prov, s_table);
    kh_destroy(quiver, s_quivers);
}

void G_Provision_RemoveEntity(uint32_t uid)
{
    khiter_t k = kh_get(prov, s_table, uid);
    if(k != kh_end(s_table)) {
        E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive);
        E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_ordered);
        kh_del(prov, s_table, k);
    }
    k = kh_get(quiver, s_quivers, uid);
    if(k != kh_end(s_quivers)) {
        E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_quiver);
        E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_ordered_quiver);
        kh_del(quiver, s_quivers, k);
    }
}

bool G_Provision_SetQuiver(uint32_t uid, const char *good, int shots_per_unit, int max_units, 
                           bool automatic)
{
    if(shots_per_unit <= 0 || max_units <= 0)
        return false;
    int status;
    khiter_t k = kh_put(quiver, s_quivers, uid, &status);
    if(status == -1)
        return false;
    struct quiver *q = &kh_value(s_quivers, k);
    if(status != 0) {
        *q = (struct quiver){0};
        q->fetching = NULL_UID;
    }
    pf_strlcpy(q->good, good, sizeof(q->good));
    q->shots_per_unit = shots_per_unit;
    q->max_units = max_units;
    q->automatic = automatic;
    if(status != 0) {
        G_Combat_SetAmmo(uid, quiver_max_shots(q));
    }
    return true;
}

void G_Provision_SetAutoResupply(uint32_t uid, bool on)
{
    struct quiver *q = quiver_get(uid);
    /* A unit being loaded gets its flag before the rest of its quiver */
    if(!q && on) {
        int status;
        khiter_t k = kh_put(quiver, s_quivers, uid, &status);
        if(status == -1)
            return;
        q = &kh_value(s_quivers, k);
        *q = (struct quiver){0};
        q->fetching = NULL_UID;
    }
    if(q) {
        q->automatic = on;
    }
}

bool G_Provision_GetAutoResupply(uint32_t uid)
{
    struct quiver *q = quiver_get(uid);
    return q && q->automatic;
}

bool G_Provision_Resupply(uint32_t uid)
{
    struct quiver *q = quiver_get(uid);
    if(!q || q->shots_per_unit == 0)
        return false;
    return fetch_quiver(uid, q);
}

int G_Provision_QuiverCapacity(uint32_t uid)
{
    struct quiver *q = quiver_get(uid);
    return q ? quiver_max_shots(q) : 0;
}

void G_Provision_OnVisit(uint32_t uid, uint32_t site)
{
    struct provision *p = prov_get(uid);
    if(!p)
        return;
    if(p->tier == PROVISION_FED && p->meals > 0)
        return;
    if(p->tier == PROVISION_FED && p->next_due_ms > s_now_ms + EAT_AHEAD_MS)
        return;
    eat(uid, p, site);
}

bool G_Provision_Set(uint32_t uid, const char *good, int max_meals, float interval_s, 
                     float starve_after_s, bool automatic)
{
    int status;
    khiter_t k = kh_put(prov, s_table, uid, &status);
    if(status == -1)
        return false;
    struct provision *p = &kh_value(s_table, k);
    if(status != 0) {
        *p = (struct provision){0};
        p->tier = PROVISION_FED;
        p->fetching = NULL_UID;
        p->meals = max_meals;
    }
    pf_strlcpy(p->good, good, sizeof(p->good));
    note_upkeep_good(good);
    p->max_meals = max_meals;
    p->interval_ms = (uint32_t)(interval_s * 1000.0f);
    p->starve_after_ms = (uint32_t)(starve_after_s * 1000.0f);
    p->next_due_ms = s_now_ms + first_meal_delay_ms(uid, p->interval_ms);
    p->automatic = automatic;
    return true;
}

bool G_Provision_WantsFood(uint32_t uid)
{
    struct provision *p = prov_get(uid);
    return p && p->automatic && p->interval_ms && p->tier != PROVISION_FED && !p->no_food;
}

bool G_Provision_Fetching(uint32_t uid)
{
    return eating(uid);
}

bool G_Provision_DueSoon(uint32_t uid)
{
    struct provision *p = prov_get(uid);
    if(!p || !p->automatic || !p->interval_ms || p->no_food)
        return false;
    if(p->tier != PROVISION_FED)
        return true;
    return p->meals == 0 && p->next_due_ms <= s_now_ms + WORK_AHEAD_MS;
}

bool G_Provision_IsUpkeepGood(const char *good)
{
    for(int i = 0; i < s_num_upkeep_goods; i++) {
        if(!strcmp(s_upkeep_goods[i], good))
            return true;
    }
    return false;
}

int G_Provision_Tier(uint32_t uid)
{
    struct provision *p = prov_get(uid);
    return p ? p->tier : PROVISION_FED;
}

uint32_t G_Provision_TimerMs(uint32_t uid)
{
    struct provision *p = prov_get(uid);
    if(!p)
        return 0;
    switch(p->tier) {
    case PROVISION_FED:
        return (p->next_due_ms > s_now_ms) ? p->next_due_ms - s_now_ms : 0;
    case PROVISION_HUNGRY: {
        uint32_t hungry_ms = s_now_ms - p->tier_since_ms;
        return (p->starve_after_ms > hungry_ms) ? p->starve_after_ms - hungry_ms : 0;
    }
    default:
        return s_now_ms - p->tier_since_ms;
    }
}

void G_Provision_SetAutomatic(uint32_t uid, bool on)
{
    struct provision *p = prov_get(uid);
    /* A unit being loaded gets its flag before the rest of its provision */
    if(!p && on) {
        int status;
        khiter_t k = kh_put(prov, s_table, uid, &status);
        if(status == -1)
            return;
        p = &kh_value(s_table, k);
        *p = (struct provision){0};
        p->tier = PROVISION_FED;
        p->fetching = NULL_UID;
    }
    if(p) {
        p->automatic = on;
    }
}

bool G_Provision_GetAutomatic(uint32_t uid)
{
    struct provision *p = prov_get(uid);
    return p && p->automatic;
}

bool G_Provision_Fetch(uint32_t uid)
{
    struct provision *p = prov_get(uid);
    if(!p)
        return false;
    return fetch(uid, p);
}

void G_Provision_Counts(int faction, int out[PROVISION_NUM_TIERS])
{
    for(int i = 0; i < PROVISION_NUM_TIERS; i++) {
        out[i] = 0;
    }
    uint32_t uid;
    struct provision *p;
    kh_foreach_val_ptr(s_table, uid, p, {
        if(G_GetFactionID(uid) != faction || p->interval_ms == 0)
            continue;
        out[p->tier]++;
    });
}

size_t G_Provision_Demand(int faction, size_t maxout, int out_districts[], float out_rates[])
{
    size_t ret = 0;
    uint32_t uid;
    struct provision *p;
    kh_foreach_val_ptr(s_table, uid, p, {
        if(G_GetFactionID(uid) != faction || p->interval_ms == 0)
            continue;
        int district = G_District_Of(uid);
        if(district == 0)
            continue;
        int i = 0;
        while(i < ret && out_districts[i] != district)
            i++;
        if(i == ret) {
            if(ret == maxout)
                continue;
            out_districts[ret] = district;
            out_rates[ret] = 0.0f;
            ret++;
        }
        out_rates[i] += 60000.0f / p->interval_ms;
    });
    return ret;
}

bool G_Provision_SaveState(struct SDL_RWops *stream)
{
    struct attr now = (struct attr){
        .type = TYPE_INT,
        .val.as_int = s_now_ms
    };
    CHK_TRUE_RET(Attr_Write(stream, &now, "now_ms"));

    struct attr num = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num, "num_ents"));

    uint32_t uid;
    struct provision p;
    kh_foreach(s_table, uid, p, {

        struct attr attrs[] = {
            {.type = TYPE_INT,  .val.as_int = uid},
            {.type = TYPE_STRING},
            {.type = TYPE_INT,  .val.as_int = p.meals},
            {.type = TYPE_INT,  .val.as_int = p.max_meals},
            {.type = TYPE_INT,  .val.as_int = p.interval_ms},
            {.type = TYPE_INT,  .val.as_int = p.starve_after_ms},
            {.type = TYPE_INT,  .val.as_int = p.next_due_ms},
            {.type = TYPE_INT,  .val.as_int = p.tier},
            {.type = TYPE_INT,  .val.as_int = p.tier_since_ms},
            {.type = TYPE_BOOL, .val.as_bool = p.automatic},
        };
        pf_strlcpy(attrs[1].val.as_string, p.good, sizeof(attrs[1].val.as_string));
        for(int i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
            CHK_TRUE_RET(Attr_Write(stream, &attrs[i], "provision"));
        }
        Sched_TryYield();
    });

    struct attr num_quivers = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_quivers)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_quivers, "num_quivers"));

    struct quiver q;
    kh_foreach(s_quivers, uid, q, {

        struct attr attrs[] = {
            {.type = TYPE_INT,  .val.as_int = uid},
            {.type = TYPE_STRING},
            {.type = TYPE_INT,  .val.as_int = q.shots_per_unit},
            {.type = TYPE_INT,  .val.as_int = q.max_units},
            {.type = TYPE_BOOL, .val.as_bool = q.automatic},
        };
        pf_strlcpy(attrs[1].val.as_string, q.good, sizeof(attrs[1].val.as_string));
        for(int i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
            CHK_TRUE_RET(Attr_Write(stream, &attrs[i], "quiver"));
        }
        Sched_TryYield();
    });
    return true;
}

bool G_Provision_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    s_now_ms = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num = attr.val.as_int;

    for(int i = 0; i < num; i++) {

        struct attr a[10];
        static const int types[10] = {TYPE_INT, TYPE_STRING, TYPE_INT, TYPE_INT, TYPE_INT, 
                                      TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_BOOL};
        for(int j = 0; j < 10; j++) {
            CHK_TRUE_RET(Attr_Parse(stream, &a[j], true));
            CHK_TRUE_RET(a[j].type == types[j]);
        }

        int status;
        khiter_t k = kh_put(prov, s_table, a[0].val.as_int, &status);
        CHK_TRUE_RET(status != -1);
        struct provision *p = &kh_value(s_table, k);
        pf_strlcpy(p->good, a[1].val.as_string, sizeof(p->good));
        note_upkeep_good(p->good);
        p->meals = a[2].val.as_int;
        p->max_meals = a[3].val.as_int;
        p->interval_ms = a[4].val.as_int;
        p->starve_after_ms = a[5].val.as_int;
        p->next_due_ms = a[6].val.as_int;
        p->tier = a[7].val.as_int;
        p->tier_since_ms = a[8].val.as_int;
        p->automatic = a[9].val.as_bool;
        p->fetching = NULL_UID;
        p->retry_ms = 0;
        p->no_food = false;
        p->resume[0] = '\0';
        Sched_TryYield();
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_quivers = attr.val.as_int;

    for(int i = 0; i < num_quivers; i++) {

        struct attr a[5];
        static const int types[5] = {TYPE_INT, TYPE_STRING, TYPE_INT, TYPE_INT, TYPE_BOOL};
        for(int j = 0; j < 5; j++) {
            CHK_TRUE_RET(Attr_Parse(stream, &a[j], true));
            CHK_TRUE_RET(a[j].type == types[j]);
        }

        int status;
        khiter_t k = kh_put(quiver, s_quivers, a[0].val.as_int, &status);
        CHK_TRUE_RET(status != -1);
        struct quiver *q = &kh_value(s_quivers, k);
        *q = (struct quiver){0};
        pf_strlcpy(q->good, a[1].val.as_string, sizeof(q->good));
        q->shots_per_unit = a[2].val.as_int;
        q->max_units = a[3].val.as_int;
        q->automatic = a[4].val.as_bool;
        q->fetching = NULL_UID;
        q->empty = (G_Combat_GetAmmo(a[0].val.as_int) == 0);
        Sched_TryYield();
    }
    return true;
}
