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
#define MEM_FILE_SUB MEM_SUB_GAME_POSTS

#include "posts.h"
#include "duty.h"
#include "public/game.h"
#include "game_private.h"
#include "movement.h"
#include "combat.h"
#include "position.h"
#include "harvester.h"
#include "builder.h"
#include "provision.h"
#include "../main.h"
#include "../event.h"
#include "../entity.h"
#include "../sched.h"
#include "../script/public/script.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"
#include "../map/public/map.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_POSTS)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_POSTS)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_POSTS)

#define ARR_SIZE(a)         (sizeof(a)/sizeof((a)[0]))
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))

#define MAX_POSTS           (4)
#define MAX_VACANCIES       (256)
/* Each holder inside is looked at once a second, a twentieth of them each tick */
#define BUCKETS             (20)
#define DOOR_DIST           (25.0f)
/* Stops short of the door before a holder gives up on getting in */
#define MAX_APPROACHES      (4)
#define WALK_TIMEOUT_TICKS  (20 * 90)
/* How long a post waits for its holder to come back from a meal, or to come
 * at all for one sent by hand that eats or brings its load in first */
#define AWAY_TICKS          (20 * 120)
/* How long one that could not get in is not sent to the workshop again */
#define SHUN_TICKS          (20 * 30)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

enum holder_state{
    HOLDER_WALKING,
    HOLDER_INSIDE,
    /* Gone to eat or to bring its load in, its post kept for it */
    HOLDER_AWAY
};

struct holder{
    uint32_t          workshop;
    enum holder_state state;
    int               approaches;
    uint32_t          since_tick;
    /* Where it stood when it went in, to be let out next to */
    vec2_t            entered_from;
    /* Sent by the player's hand, it stays while the workshop has no work */
    bool              ordered;
    /* Sent to bring its load in before it goes back */
    bool              dropping;
    /* On a meal break, which lasts until it is back inside */
    bool              meal;
};

struct workshop{
    int              posts;
    int              locked;
    enum post_demand demand;
    int              nholders;
    uint32_t         holders[MAX_POSTS];
    uint32_t         shunned;
    uint32_t         shunned_tick;
    /* When it last stood with work and an open post; 0 when it does not */
    uint32_t         open_tick;
};

KHASH_MAP_INIT_INT(holder, struct holder)
KHASH_MAP_INIT_INT(workshop, struct workshop)

static void release(uint32_t unit, const char *why, bool away);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map    *s_map;
static khash_t(holder)     *s_holders;
static khash_t(workshop)   *s_workshops;
static uint32_t             s_tick;
static uint32_t             s_vacancies_tick;
static size_t               s_nvacancies;
static struct post_vacancy  s_vacancies[MAX_VACANCIES];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct holder *holder_get(uint32_t uid)
{
    khiter_t k = kh_get(holder, s_holders, uid);
    if(k == kh_end(s_holders))
        return NULL;
    return &kh_value(s_holders, k);
}

static void holder_remove(uint32_t uid)
{
    khiter_t k = kh_get(holder, s_holders, uid);
    if(k != kh_end(s_holders)) {
        kh_del(holder, s_holders, k);
        G_Duty_End(uid, NULL);
    }
}

static struct workshop *workshop_get(uint32_t uid)
{
    khiter_t k = kh_get(workshop, s_workshops, uid);
    if(k == kh_end(s_workshops))
        return NULL;
    return &kh_value(s_workshops, k);
}

static void drop_holder(struct workshop *ws, uint32_t unit)
{
    for(int i = 0; i < ws->nholders; i++) {
        if(ws->holders[i] != unit)
            continue;
        ws->holders[i] = ws->holders[--ws->nholders];
        return;
    }
}

static int count_inside(const struct workshop *ws)
{
    int ret = 0;
    for(int i = 0; i < ws->nholders; i++) {
        const struct holder *h = holder_get(ws->holders[i]);
        if(h && h->state == HOLDER_INSIDE) {
            ret++;
        }
    }
    return ret;
}

static bool usable_workshop(uint32_t uid)
{
    return G_EntityExists(uid) && !G_EntityIsZombie(uid);
}

/* Beside the workshop, or beside the buildings it stands joined to, which
 * the units' arrival takes as one */
static bool at_door(uint32_t unit, uint32_t workshop)
{
    if(M_NavObjAdjacent(s_map, unit, workshop))
        return true;
    struct obb obb;
    Entity_CurrentOBB(workshop, &obb, true);
    return M_NavObjAdjacentToStaticWith(s_map, G_Pos_GetXZ(unit),
        G_GetSelectionRadius(unit) + DOOR_DIST, &obb);
}

static bool approach(uint32_t unit, uint32_t workshop)
{
    vec2_t pos = G_Pos_GetXZ(unit);
    if(!M_NavLocationsReachable(s_map, Entity_NavLayer(unit), pos, G_Pos_GetXZ(workshop))) {
        vec2_t door;
        if(!M_NavClosestReachableAdjacentPos(s_map, Entity_NavLayer(unit), pos, workshop, true, &door))
            return false;
        G_Move_SetDest(unit, door, false);
        return true;
    }
    G_Move_SetSurroundEntity(unit, workshop);
    return true;
}

static void go_in(uint32_t unit, struct holder *h)
{
    G_Sel_Remove(unit);
    h->entered_from = G_Pos_GetXZ(unit);
    h->state = HOLDER_INSIDE;
    h->meal = false;
    uint32_t flags = G_FlagsGet(unit);
    G_FlagsSet(unit, flags | ENTITY_FLAG_GARRISONED);
    G_Combat_StopAttack(unit);
    G_Move_Unblock(unit);
    G_Pos_Garrison(unit);
}

static bool go_out(uint32_t unit, const struct holder *h)
{
    enum nav_layer layer = Entity_NavLayer(unit);
    vec2_t out;
    if(!M_NavClosestReachableAdjacentPos(s_map, layer, h->entered_from, h->workshop, true, &out)
    && !M_NavClosestPathable(s_map, layer, h->entered_from, &out))
        return false;

    uint32_t flags = G_FlagsGet(unit);
    G_FlagsSet(unit, flags & ~ENTITY_FLAG_GARRISONED);
    vec3_t pos = (vec3_t){out.x, M_HeightAtPoint(s_map, out), out.z};
    G_Pos_Ungarrison(unit, pos);
    G_Move_BlockAt(unit, pos);
    return true;
}

/* Lets the unit go from its post, out of the workshop if it is inside; one
 * gone to eat has the post kept for it */
static void release(uint32_t unit, const char *why, bool away)
{
    struct holder *h = holder_get(unit);
    if(!h)
        return;
    if(h->state == HOLDER_INSIDE && !go_out(unit, h))
        return;
    if(h->state == HOLDER_WALKING && G_EntityExists(unit)) {
        G_StopEntity(unit, true, false);
    }

    uint32_t workshop = h->workshop;
    if(away) {
        h->state = HOLDER_AWAY;
        h->since_tick = s_tick;
        h->dropping = false;
        if(why) {
            G_Automation_TraceFollowOn(unit, "post", why, NULL, workshop);
        }
        return;
    }
    struct workshop *ws = workshop_get(workshop);
    if(ws) {
        drop_holder(ws, unit);
    }
    holder_remove(unit);
    if(why) {
        G_Automation_TraceFollowOn(unit, "post", why, NULL, workshop);
    }
}

static void shun(struct workshop *ws, uint32_t unit)
{
    ws->shunned = unit;
    ws->shunned_tick = s_tick;
}

/* Lets the holder go for good when its workshop is gone, has no work for one
 * not sent by hand, or keeps its post empty */
static bool let_go(uint32_t unit, struct holder *h)
{
    struct workshop *ws = workshop_get(h->workshop);
    if(!ws || !usable_workshop(h->workshop)) {
        release(unit, "gone", false);
        return true;
    }
    if(ws->demand == POSTS_IDLE && !h->ordered) {
        release(unit, "no_work", false);
        return true;
    }
    if(ws->nholders > ws->posts - ws->locked) {
        release(unit, "locked", false);
        return true;
    }
    return false;
}

static bool send(uint32_t unit, struct holder *h)
{
    G_StopEntity(unit, false, false);
    if(!approach(unit, h->workshop))
        return false;
    h->state = HOLDER_WALKING;
    h->approaches = 0;
    h->since_tick = s_tick;
    return true;
}

/* Whether one away from its post is fed, empty-handed and free to go back to
 * it; one with a load is sent to bring it in first, into the workshop if it
 * takes it, and goes in with it if it cannot be rid of it */
static bool ready_for_post(uint32_t unit, struct holder *h)
{
    uint32_t flags = G_FlagsGet(unit);
    if(flags & ENTITY_FLAG_GARRISONED)
        return false;
    if(G_Provision_WantsFood(unit) || G_Provision_Fetching(unit))
        return false;
    if(!(flags & ENTITY_FLAG_HARVESTER) || G_Harvester_GetCurrTotalCarry(unit) == 0)
        return true;
    if(h->dropping)
        return G_Harvester_Idle(unit);
    h->dropping = true;
    bool store = (G_FlagsGet(h->workshop) & ENTITY_FLAG_STORAGE_SITE);
    if(store && G_Harvester_BringInLoad(unit, h->workshop, false))
        return false;
    return !G_Harvester_BringInLoad(unit, NULL_UID, false);
}

static void update_walking(uint32_t unit, struct holder *h)
{
    if(let_go(unit, h))
        return;
    struct workshop *ws = workshop_get(h->workshop);
    if(G_Provision_WantsFood(unit)) {
        release(unit, "hungry", true);
        return;
    }
    if(s_tick - h->since_tick > WALK_TIMEOUT_TICKS) {
        shun(ws, unit);
        release(unit, "unreachable", false);
        return;
    }
    if(!G_Move_Still(unit))
        return;
    if(at_door(unit, h->workshop)) {
        go_in(unit, h);
        G_Automation_TraceFollowOn(unit, "post", "inside", NULL, h->workshop);
        return;
    }
    if(++h->approaches > MAX_APPROACHES || !approach(unit, h->workshop)) {
        shun(ws, unit);
        release(unit, "unreachable", false);
    }
}

static void update_inside(uint32_t unit, struct holder *h)
{
    if(let_go(unit, h))
        return;
    if(G_Provision_WantsFood(unit)) {
        release(unit, "hungry", true);
    }
}

static bool wants_meal(uint32_t unit)
{
    return G_Provision_WantsFood(unit) || G_Provision_Fetching(unit);
}

static void update_away(uint32_t unit, struct holder *h)
{
    if(let_go(unit, h))
        return;
    if(wants_meal(unit)) {
        h->meal = true;
    }
    if(!G_EntityExists(unit) || G_EntityIsZombie(unit) || s_tick - h->since_tick > AWAY_TICKS) {
        release(unit, "timeout", false);
        return;
    }
    if(!ready_for_post(unit, h))
        return;
    if(!send(unit, h)) {
        shun(workshop_get(h->workshop), unit);
        release(unit, "unreachable", false);
        return;
    }
    G_Automation_TraceFollowOn(unit, "post", "back", NULL, h->workshop);
}

static int open_posts(const struct workshop *ws)
{
    return ws->posts - ws->locked - ws->nholders;
}

static void update_workshop(struct workshop *ws)
{
    if(ws->shunned != NULL_UID && s_tick - ws->shunned_tick > SHUN_TICKS) {
        ws->shunned = NULL_UID;
    }
    if(ws->demand != POSTS_FILL || open_posts(ws) <= 0) {
        ws->open_tick = 0;
    }else if(ws->open_tick == 0) {
        ws->open_tick = s_tick;
    }
}

static void on_20hz_tick(void *user, void *event)
{
    s_tick++;
    int bucket = s_tick % BUCKETS;

    uint32_t uid;
    struct workshop *ws;
    kh_foreach_val_ptr(s_workshops, uid, ws, {
        update_workshop(ws);
    });

    /* A release takes the holder out of the table, so they are gathered first */
    uint32_t units[256];
    size_t nunits = 0;
    struct holder *h;
    kh_foreach_val_ptr(s_holders, uid, h, {
        if(nunits == ARR_SIZE(units))
            break;
        if(h->state == HOLDER_INSIDE && uid % BUCKETS != bucket)
            continue;
        units[nunits++] = uid;
    });

    for(int i = 0; i < nunits; i++) {
        struct holder *h = holder_get(units[i]);
        if(!h)
            continue;
        switch(h->state) {
        case HOLDER_WALKING:
            update_walking(units[i], h);
            break;
        case HOLDER_INSIDE:
            update_inside(units[i], h);
            break;
        case HOLDER_AWAY:
            update_away(units[i], h);
            break;
        }
    }
}

static void on_player_order(void *user, void *event)
{
    uint32_t uid = (uintptr_t)event;
    struct holder *h = holder_get(uid);
    if(!h || h->state == HOLDER_INSIDE)
        return;
    struct workshop *ws = workshop_get(h->workshop);
    if(ws) {
        drop_holder(ws, uid);
    }
    holder_remove(uid);
}

/* A holder the automation put at the workshop, to make room for one sent by
 * hand: one on its way or away before one at work */
static uint32_t bumpable_holder(const struct workshop *ws, uint32_t unit)
{
    uint32_t ret = NULL_UID;
    for(int i = 0; i < ws->nholders; i++) {
        uint32_t curr = ws->holders[i];
        const struct holder *h = holder_get(curr);
        if(curr == unit || !h || h->ordered)
            continue;
        if(h->state != HOLDER_INSIDE)
            return curr;
        ret = curr;
    }
    return ret;
}

/* Whether the unit can be sent by hand to the workshop, which has a post that is
 * not filled, or held by one the automation put there, whether or not it is kept
 * empty, and whether or not it has work; one holding a post there already can */
static bool can_man(uint32_t workshop, uint32_t unit)
{
    const struct workshop *ws = workshop_get(workshop);
    if(!ws || !usable_workshop(workshop))
        return false;
    if(!G_EntityExists(unit) || G_EntityIsZombie(unit))
        return false;
    uint32_t flags = G_FlagsGet(unit);
    if(!(flags & ENTITY_FLAG_HARVESTER) || !(flags & ENTITY_FLAG_GARRISON))
        return false;
    if(G_GetFactionID(unit) != G_GetFactionID(workshop))
        return false;
    const struct holder *h = holder_get(unit);
    if(h && h->workshop == workshop)
        return true;
    return (ws->nholders < ws->posts) || (bumpable_holder(ws, unit) != NULL_UID);
}

/* The workshop that takes the unit sent to this one: this one, or with no post
 * for it here, one of its own kind that stands joined to it */
static uint32_t man_target(uint32_t workshop, uint32_t unit)
{
    if(can_man(workshop, unit))
        return workshop;
    if(!workshop_get(workshop))
        return NULL_UID;

    uint32_t members[64];
    int cluster = G_Cluster_Of(workshop);
    size_t nmembers = cluster ? G_Cluster_Members(cluster, ARR_SIZE(members), members) : 0;
    for(int i = 0; i < nmembers; i++) {
        if(S_ScriptTypeID(members[i]) == S_ScriptTypeID(workshop) && can_man(members[i], unit))
            return members[i];
    }
    return NULL_UID;
}

static void man_selection(uint32_t workshop)
{
    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);
    if(sel_type != SELECTION_TYPE_PLAYER)
        return;

    int nsent = 0;
    for(int i = 0; i < vec_size(sel); i++) {
        uint32_t curr = vec_AT(sel, i);
        uint32_t target = man_target(workshop, curr);
        if(target != NULL_UID && G_Posts_Man(target, curr)) {
            G_Automation_MakePlayerManaged(curr);
            nsent++;
        }
    }
    if(nsent) {
        Entity_Ping(workshop);
    }
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);
    if(mouse_event->button != SDL_BUTTON_RIGHT)
        return;

    if(G_MouseOverMinimap() || G_MouseInTargetMode())
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    /* While a tool such as placement holds the mouse, a right click gives no order */
    if(!G_Move_GetClickEnabled())
        return;

    if(G_CurrContextualAction() != CTX_ACTION_MAN)
        return;

    man_selection(G_Sel_GetHovered());
}

static struct holder *add_holder(struct workshop *ws, uint32_t workshop, uint32_t unit,
                                 enum holder_state state, bool ordered)
{
    int status;
    khiter_t k = kh_put(holder, s_holders, unit, &status);
    if(status == -1)
        return NULL;
    kh_value(s_holders, k) = (struct holder){
        .workshop = workshop,
        .state = state,
        .since_tick = s_tick,
        .ordered = ordered
    };
    ws->holders[ws->nholders++] = unit;
    s_vacancies_tick = ~((uint32_t)0);
    G_Duty_Set(unit, DUTY_POST, workshop, NULL, DUTY_BY_PLAYER);
    return &kh_value(s_holders, k);
}

static int compare_vacancies(const void *a, const void *b)
{
    const struct post_vacancy *va = a, *vb = b;
    if(va->priority != vb->priority)
        return vb->priority - va->priority;
    const struct workshop *wa = workshop_get(va->workshop);
    const struct workshop *wb = workshop_get(vb->workshop);
    if(wa->nholders != wb->nholders)
        return wa->nholders - wb->nholders;
    return (va->workshop > vb->workshop) - (va->workshop < vb->workshop);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Posts_Init(const struct map *map)
{
    if(!(s_holders = kh_init(holder)))
        return false;
    if(!(s_workshops = kh_init(workshop))) {
        kh_destroy(holder, s_holders);
        return false;
    }
    s_map = map;
    s_tick = 0;
    s_vacancies_tick = ~((uint32_t)0);
    s_nvacancies = 0;
    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
    E_Global_Register(EVENT_PLAYER_ORDER_ISSUED, on_player_order, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    return true;
}

void G_Posts_Shutdown(void)
{
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    E_Global_Unregister(EVENT_PLAYER_ORDER_ISSUED, on_player_order);
    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
    kh_destroy(workshop, s_workshops);
    kh_destroy(holder, s_holders);
}

void G_Posts_RemoveEntity(uint32_t uid)
{
    struct holder *h = holder_get(uid);
    if(h) {
        struct workshop *ws = workshop_get(h->workshop);
        if(ws) {
            drop_holder(ws, uid);
        }
        holder_remove(uid);
    }

    struct workshop *ws = workshop_get(uid);
    if(!ws)
        return;
    uint32_t holders[MAX_POSTS];
    int nholders = ws->nholders;
    memcpy(holders, ws->holders, sizeof(holders));
    for(int i = 0; i < nholders; i++) {
        release(holders[i], "gone", false);
    }
    /* One that could not be let out goes with the workshop's record */
    for(int i = 0; i < nholders; i++) {
        holder_remove(holders[i]);
    }
    khiter_t k = kh_get(workshop, s_workshops, uid);
    if(k != kh_end(s_workshops)) {
        kh_del(workshop, s_workshops, k);
    }
}

bool G_Posts_SetPosts(uint32_t uid, int nposts)
{
    ASSERT_IN_MAIN_THREAD();

    nposts = MIN(MAX(nposts, 0), MAX_POSTS);
    if(nposts == 0) {
        G_Posts_RemoveEntity(uid);
        return true;
    }
    struct workshop *ws = workshop_get(uid);
    if(!ws) {
        int status;
        khiter_t k = kh_put(workshop, s_workshops, uid, &status);
        if(status == -1)
            return false;
        ws = &kh_value(s_workshops, k);
        *ws = (struct workshop){
            .demand = POSTS_IDLE,
            .shunned = NULL_UID
        };
    }
    ws->posts = nposts;
    ws->locked = MIN(ws->locked, nposts);
    return true;
}

void G_Posts_SetDemand(uint32_t uid, enum post_demand demand)
{
    struct workshop *ws = workshop_get(uid);
    if(!ws)
        return;
    ws->demand = demand;
}

void G_Posts_SetLocked(uint32_t uid, int locked)
{
    struct workshop *ws = workshop_get(uid);
    if(!ws)
        return;
    ws->locked = MIN(MAX(locked, 0), ws->posts);
}

bool G_Posts_Get(uint32_t uid, int *out_posts, int *out_locked, int *out_inside, int *out_coming)
{
    const struct workshop *ws = workshop_get(uid);
    if(!ws)
        return false;
    *out_posts = ws->posts;
    *out_locked = ws->locked;
    *out_inside = count_inside(ws);
    *out_coming = ws->nholders - *out_inside;
    return true;
}

int G_Posts_Eating(uint32_t uid)
{
    const struct workshop *ws = workshop_get(uid);
    if(!ws)
        return 0;
    int ret = 0;
    for(int i = 0; i < ws->nholders; i++) {
        const struct holder *h = holder_get(ws->holders[i]);
        if(h && (h->meal || (h->state == HOLDER_AWAY && wants_meal(ws->holders[i])))) {
            ret++;
        }
    }
    return ret;
}

uint32_t G_Posts_WorkshopOf(uint32_t unit)
{
    const struct holder *h = holder_get(unit);
    return h ? h->workshop : NULL_UID;
}

bool G_Posts_Holds(uint32_t unit)
{
    return holder_get(unit) != NULL;
}

bool G_Posts_Take(uint32_t workshop, uint32_t unit)
{
    ASSERT_IN_MAIN_THREAD();

    struct workshop *ws = workshop_get(workshop);
    if(!ws || open_posts(ws) <= 0 || holder_get(unit))
        return false;
    if(G_FlagsGet(unit) & ENTITY_FLAG_GARRISONED)
        return false;

    G_StopEntity(unit, false, false);
    if(!approach(unit, workshop)) {
        shun(ws, unit);
        return false;
    }
    return (add_holder(ws, workshop, unit, HOLDER_WALKING, false) != NULL);
}

bool G_Posts_Man(uint32_t workshop, uint32_t unit)
{
    ASSERT_IN_MAIN_THREAD();

    struct holder *h = holder_get(unit);
    if(h && h->workshop == workshop) {
        h->ordered = true;
        E_Global_NotifyImmediate(EVENT_POST_MANNED, (void*)((uintptr_t)unit), ES_ENGINE);
        return true;
    }
    if(!can_man(workshop, unit))
        return false;
    if(h) {
        release(unit, "moved", false);
        if(holder_get(unit))
            return false;
    }
    if(G_FlagsGet(unit) & ENTITY_FLAG_GARRISONED)
        return false;
    struct workshop *ws = workshop_get(workshop);
    if(ws->nholders >= ws->posts) {
        uint32_t bumped = bumpable_holder(ws, unit);
        release(bumped, "bumped", false);
        if(holder_get(bumped))
            return false;
    }

    /* Not as the player's order, whose event lands after the take and would undo it */
    G_NotifyOrderIssued(unit, true, false);
    int locked = ws->locked;
    if(open_posts(ws) <= 0) {
        ws->locked = ws->posts - ws->nholders - 1;
    }
    /* One hungry or with a load in its hands eats or brings it in first */
    if(!(h = add_holder(ws, workshop, unit, HOLDER_AWAY, true))) {
        ws->locked = locked;
        return false;
    }
    if(G_Provision_WantsFood(unit) && !G_Provision_Fetching(unit)) {
        G_Provision_Fetch(unit);
    }
    if(ready_for_post(unit, h) && !send(unit, h)) {
        shun(ws, unit);
        drop_holder(ws, unit);
        holder_remove(unit);
        ws->locked = locked;
        return false;
    }
    G_Automation_TraceFollowOn(unit, "post", "manned", NULL, workshop);
    E_Global_NotifyImmediate(EVENT_POST_MANNED, (void*)((uintptr_t)unit), ES_ENGINE);
    return true;
}

uint32_t G_Posts_Evict(uint32_t workshop)
{
    ASSERT_IN_MAIN_THREAD();

    struct workshop *ws = workshop_get(workshop);
    if(!ws || ws->nholders == 0)
        return NULL_UID;

    /* One still on its way or away is turned back before one at work */
    uint32_t unit = ws->holders[ws->nholders - 1];
    for(int i = 0; i < ws->nholders; i++) {
        const struct holder *h = holder_get(ws->holders[i]);
        if(h && h->state != HOLDER_INSIDE) {
            unit = ws->holders[i];
            break;
        }
    }
    release(unit, "evicted", false);
    if(holder_get(unit))
        return NULL_UID;
    ws->locked = MAX(ws->locked, ws->posts - ws->nholders);
    return unit;
}

bool G_Posts_Leave(uint32_t unit)
{
    ASSERT_IN_MAIN_THREAD();

    if(!holder_get(unit))
        return false;
    release(unit, "left", false);
    return (holder_get(unit) == NULL);
}

int G_Posts_CurrContextualAction(void)
{
    uint32_t hovered = G_Sel_GetHovered();
    if(!G_EntityExists(hovered) || !(G_FlagsGet(hovered) & ENTITY_FLAG_BUILDING))
        return CTX_ACTION_NONE;

    /* Short of a unit for its queue, it takes the one sent to it for that */
    if(G_Garrison_WantsIngredient(hovered))
        return CTX_ACTION_NONE;

    if(M_MouseOverMinimap(s_map))
        return CTX_ACTION_NONE;

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    if(S_UI_MouseOverWindow(mouse_x, mouse_y))
        return CTX_ACTION_NONE;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);
    if(vec_size(sel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return CTX_ACTION_NONE;

    for(int i = 0; i < vec_size(sel); i++) {
        uint32_t curr = vec_AT(sel, i);
        /* Ahead of a look at the table, which only stands while a map is loaded */
        if(!(G_FlagsGet(curr) & ENTITY_FLAG_HARVESTER))
            continue;
        if(man_target(hovered, curr) != NULL_UID)
            return CTX_ACTION_MAN;
    }
    return CTX_ACTION_NONE;
}

size_t G_Posts_Vacancies(size_t maxout, struct post_vacancy out[])
{
    if(s_vacancies_tick != s_tick) {
        s_vacancies_tick = s_tick;
        s_nvacancies = 0;

        uint32_t uid;
        struct workshop *ws;
        kh_foreach_val_ptr(s_workshops, uid, ws, {
            if(ws->demand != POSTS_FILL || !usable_workshop(uid))
                continue;
            int open = open_posts(ws);
            for(int i = 0; i < open && s_nvacancies < MAX_VACANCIES; i++) {
                bool storage = (G_FlagsGet(uid) & ENTITY_FLAG_STORAGE_SITE);
                s_vacancies[s_nvacancies++] = (struct post_vacancy){
                    .workshop = uid,
                    .faction = G_GetFactionID(uid),
                    .district = G_District_Of(uid),
                    .priority = storage ? G_StorageSite_GetPriority(uid) : SS_PRIORITY_NORMAL,
                    .awaited = NULL_UID,
                    .shunned = ws->shunned,
                    .waited_ticks = ws->open_tick ? s_tick - ws->open_tick : 0
                };
            }
        });
        qsort(s_vacancies, s_nvacancies, sizeof(s_vacancies[0]), compare_vacancies);
    }
    size_t ret = MIN(maxout, s_nvacancies);
    memcpy(out, s_vacancies, ret * sizeof(struct post_vacancy));
    return ret;
}

bool G_Posts_SaveState(struct SDL_RWops *stream)
{
    struct attr num_posts = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_workshops)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_posts, "num_posts"));

    uint32_t uid;
    struct workshop ws;
    kh_foreach(s_workshops, uid, ws, {

        struct attr attrs[] = {
            {.type = TYPE_INT, .val.as_int = uid},
            {.type = TYPE_INT, .val.as_int = ws.posts},
            {.type = TYPE_INT, .val.as_int = ws.locked},
            {.type = TYPE_INT, .val.as_int = ws.demand},
            /* Holders away are saved with the others; older saves list them here */
            {.type = TYPE_INT, .val.as_int = 0},
        };
        for(int i = 0; i < ARR_SIZE(attrs); i++) {
            CHK_TRUE_RET(Attr_Write(stream, &attrs[i], "workshop"));
        }
        Sched_TryYield();
    });

    struct attr num_holders = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_holders)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_holders, "num_holders"));

    struct holder h;
    kh_foreach(s_holders, uid, h, {

        struct attr attrs[] = {
            {.type = TYPE_INT,  .val.as_int = uid},
            {.type = TYPE_INT,  .val.as_int = h.workshop},
            {.type = TYPE_INT,  .val.as_int = h.state},
            {.type = TYPE_INT,  .val.as_int = h.approaches},
            {.type = TYPE_VEC2, .val.as_vec2 = h.entered_from},
        };
        for(int i = 0; i < ARR_SIZE(attrs); i++) {
            CHK_TRUE_RET(Attr_Write(stream, &attrs[i], "holder"));
        }
        struct attr ordered = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = h.ordered
        };
        CHK_TRUE_RET(Attr_Write(stream, &ordered, "holder_ordered"));
        struct attr meal = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = h.meal
        };
        CHK_TRUE_RET(Attr_Write(stream, &meal, "holder_meal"));
        Sched_TryYield();
    });
    return true;
}

bool G_Posts_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;
    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_posts = attr.val.as_int;

    for(int i = 0; i < num_posts; i++) {

        int vals[5];
        for(int j = 0; j < ARR_SIZE(vals); j++) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            vals[j] = attr.val.as_int;
        }
        uint32_t uid = vals[0];
        CHK_TRUE_RET(G_Posts_SetPosts(uid, vals[1]));
        struct workshop *ws = workshop_get(uid);
        CHK_TRUE_RET(ws);
        ws->locked = MIN(MAX(vals[2], 0), ws->posts);
        ws->demand = vals[3];
        CHK_TRUE_RET(vals[4] >= 0 && vals[4] <= MAX_POSTS);
        for(int j = 0; j < vals[4]; j++) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            uint32_t away = attr.val.as_int;
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            if(G_EntityExists(away) && !holder_get(away) && ws->nholders < MAX_POSTS) {
                CHK_TRUE_RET(add_holder(ws, uid, away, HOLDER_AWAY, false));
            }
        }
        Sched_TryYield();
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_holders = attr.val.as_int;

    for(int i = 0; i < num_holders; i++) {

        int vals[4];
        for(int j = 0; j < ARR_SIZE(vals); j++) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            vals[j] = attr.val.as_int;
        }
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);

        uint32_t uid = vals[0];
        struct workshop *ws = workshop_get(vals[1]);
        CHK_TRUE_RET(G_EntityExists(uid) && ws && ws->nholders < MAX_POSTS);
        CHK_TRUE_RET(vals[2] >= HOLDER_WALKING && vals[2] <= HOLDER_AWAY);

        struct holder *h = add_holder(ws, vals[1], uid, vals[2], false);
        CHK_TRUE_RET(h);
        h->approaches = vals[3];
        h->entered_from = attr.val.as_vec2;

        if(Attr_NextKeyIs(stream, "holder_ordered")) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_BOOL);
            h->ordered = attr.val.as_bool;
        }
        if(Attr_NextKeyIs(stream, "holder_meal")) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_BOOL);
            h->meal = attr.val.as_bool;
        }
        Sched_TryYield();
    }
    return true;
}
