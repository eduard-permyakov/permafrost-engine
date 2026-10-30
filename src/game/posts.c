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
/* How long a post waits for its holder to come back from a meal */
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
    HOLDER_INSIDE
};

struct holder{
    uint32_t          workshop;
    enum holder_state state;
    int               approaches;
    uint32_t          since_tick;
    /* Where it stood when it went in, to be let out next to */
    vec2_t            entered_from;
};

/* A holder gone to eat, whose post is kept for it */
struct away{
    uint32_t uid;
    int      ticks_left;
};

struct workshop{
    int              posts;
    int              locked;
    enum post_demand demand;
    int              nholders;
    uint32_t         holders[MAX_POSTS];
    int              naway;
    struct away      away[MAX_POSTS];
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

static void drop_away(struct workshop *ws, int idx)
{
    ws->away[idx] = ws->away[--ws->naway];
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

static bool at_door(uint32_t unit, uint32_t workshop)
{
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
    struct workshop *ws = workshop_get(workshop);
    if(ws) {
        drop_holder(ws, unit);
        if(away && ws->naway < MAX_POSTS) {
            ws->away[ws->naway++] = (struct away){unit, AWAY_TICKS};
        }
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

static void update_walking(uint32_t unit, struct holder *h)
{
    struct workshop *ws = workshop_get(h->workshop);
    if(!ws || !usable_workshop(h->workshop)) {
        release(unit, "gone", false);
        return;
    }
    if(ws->demand == POSTS_IDLE) {
        release(unit, "no_work", false);
        return;
    }
    if(ws->nholders > ws->posts - ws->locked) {
        release(unit, "locked", false);
        return;
    }
    if(G_Provision_WantsFood(unit)) {
        release(unit, "hungry", false);
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
    struct workshop *ws = workshop_get(h->workshop);
    if(!ws || !usable_workshop(h->workshop)) {
        release(unit, "gone", false);
        return;
    }
    if(ws->demand == POSTS_IDLE) {
        release(unit, "no_work", false);
        return;
    }
    if(ws->nholders > ws->posts - ws->locked) {
        release(unit, "locked", false);
        return;
    }
    if(G_Provision_WantsFood(unit)) {
        release(unit, "hungry", true);
    }
}

static int open_posts(const struct workshop *ws)
{
    return ws->posts - ws->locked - ws->nholders;
}

static void update_workshop(struct workshop *ws)
{
    for(int i = ws->naway - 1; i >= 0; i--) {
        if(--ws->away[i].ticks_left <= 0 || !G_EntityExists(ws->away[i].uid)
        || G_EntityIsZombie(ws->away[i].uid)) {
            drop_away(ws, i);
        }
    }
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
        if(h->state == HOLDER_WALKING) {
            update_walking(units[i], h);
        }else{
            update_inside(units[i], h);
        }
    }
}

static void on_player_order(void *user, void *event)
{
    uint32_t uid = (uintptr_t)event;
    struct holder *h = holder_get(uid);
    if(!h || h->state != HOLDER_WALKING)
        return;
    struct workshop *ws = workshop_get(h->workshop);
    if(ws) {
        drop_holder(ws, uid);
    }
    holder_remove(uid);
}

/* Whether the unit can be sent by hand to the workshop, which has a post that is
 * not filled, whether or not it is kept empty */
static bool can_man(uint32_t workshop, uint32_t unit)
{
    const struct workshop *ws = workshop_get(workshop);
    if(!ws || ws->demand == POSTS_IDLE || !usable_workshop(workshop))
        return false;
    if(!G_EntityExists(unit) || G_EntityIsZombie(unit))
        return false;
    if(!(G_FlagsGet(unit) & ENTITY_FLAG_HARVESTER) || G_Harvester_GetCurrTotalCarry(unit) > 0)
        return false;
    if(G_GetFactionID(unit) != G_GetFactionID(workshop))
        return false;
    return (ws->nholders < ws->posts);
}

static void man_selection(uint32_t workshop)
{
    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);
    if(sel_type != SELECTION_TYPE_PLAYER)
        return;

    int nsent = 0;
    for(int i = 0; i < vec_size(sel); i++) {
        if(G_Posts_Man(workshop, vec_AT(sel, i))) {
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
    int status;
    khiter_t k = kh_put(holder, s_holders, unit, &status);
    if(status == -1)
        return false;
    kh_value(s_holders, k) = (struct holder){
        .workshop = workshop,
        .state = HOLDER_WALKING,
        .since_tick = s_tick
    };
    ws->holders[ws->nholders++] = unit;
    for(int i = 0; i < ws->naway; i++) {
        if(ws->away[i].uid == unit) {
            drop_away(ws, i);
            break;
        }
    }
    s_vacancies_tick = ~((uint32_t)0);
    return true;
}

bool G_Posts_Man(uint32_t workshop, uint32_t unit)
{
    ASSERT_IN_MAIN_THREAD();

    const struct holder *h = holder_get(unit);
    if(h && h->workshop == workshop)
        return true;
    if(!can_man(workshop, unit))
        return false;
    if(h) {
        release(unit, "moved", false);
        if(holder_get(unit))
            return false;
    }
    if(G_FlagsGet(unit) & ENTITY_FLAG_GARRISONED)
        return false;

    /* Not as the player's order, whose event lands after the take and would undo it */
    G_NotifyOrderIssued(unit, true, false);
    struct workshop *ws = workshop_get(workshop);
    int locked = ws->locked;
    if(open_posts(ws) <= 0) {
        ws->locked = ws->posts - ws->nholders - 1;
    }
    if(!G_Posts_Take(workshop, unit)) {
        ws->locked = locked;
        return false;
    }
    G_Automation_TraceFollowOn(unit, "post", "manned", NULL, workshop);
    return true;
}

uint32_t G_Posts_Evict(uint32_t workshop)
{
    ASSERT_IN_MAIN_THREAD();

    struct workshop *ws = workshop_get(workshop);
    if(!ws || ws->nholders == 0)
        return NULL_UID;

    /* One still on its way is turned back before one at work */
    uint32_t unit = ws->holders[ws->nholders - 1];
    for(int i = 0; i < ws->nholders; i++) {
        const struct holder *h = holder_get(ws->holders[i]);
        if(h && h->state == HOLDER_WALKING) {
            unit = ws->holders[i];
            break;
        }
    }
    release(unit, "evicted", false);
    if(holder_get(unit))
        return NULL_UID;
    ws->locked = MAX(ws->locked, ws->posts - ws->nholders - ws->naway);
    return unit;
}

int G_Posts_CurrContextualAction(void)
{
    uint32_t hovered = G_Sel_GetHovered();
    if(!G_EntityExists(hovered) || !(G_FlagsGet(hovered) & ENTITY_FLAG_BUILDING))
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

    /* Ahead of a look at the table, which only stands while a map is loaded */
    if(!(G_FlagsGet(vec_AT(sel, 0)) & ENTITY_FLAG_HARVESTER))
        return CTX_ACTION_NONE;

    if(!can_man(hovered, vec_AT(sel, 0)))
        return CTX_ACTION_NONE;
    return CTX_ACTION_MAN;
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
                    .awaited = (i < ws->naway) ? ws->away[i].uid : NULL_UID,
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
            {.type = TYPE_INT, .val.as_int = ws.naway},
        };
        for(int i = 0; i < ARR_SIZE(attrs); i++) {
            CHK_TRUE_RET(Attr_Write(stream, &attrs[i], "workshop"));
        }
        for(int i = 0; i < ws.naway; i++) {
            struct attr away[] = {
                {.type = TYPE_INT, .val.as_int = ws.away[i].uid},
                {.type = TYPE_INT, .val.as_int = ws.away[i].ticks_left},
            };
            CHK_TRUE_RET(Attr_Write(stream, &away[0], "away"));
            CHK_TRUE_RET(Attr_Write(stream, &away[1], "away"));
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
            ws->away[ws->naway++] = (struct away){away, attr.val.as_int};
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

        int status;
        khiter_t k = kh_put(holder, s_holders, uid, &status);
        CHK_TRUE_RET(status != -1);
        kh_value(s_holders, k) = (struct holder){
            .workshop = vals[1],
            .state = vals[2],
            .approaches = vals[3],
            .since_tick = s_tick,
            .entered_from = attr.val.as_vec2
        };
        ws->holders[ws->nholders++] = uid;
        Sched_TryYield();
    }
    return true;
}
