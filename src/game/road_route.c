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


#include "road_route.h"
#include "public/game.h"
#include "game_private.h"
#include "movement.h"
#include "../event.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"

#include <assert.h>
#include <math.h>
#include <string.h>
#include <SDL.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_ROADS)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_ROADS)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_ROADS)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

#define ARR_SIZE(a)          (sizeof(a)/sizeof(a[0]))
#define MAX(a, b)            ((a) > (b) ? (a) : (b))
#define MAX_ROUTE_WAYPOINTS  (64)
#define MAX_ROUTE_TILES      (2048)
/* How far from a road a route may begin or end */
#define ROUTE_REACH          (96.0f)
/* How far from the ordered point a road-locked unit's stop may be */
#define ROUTE_LOCKED_REACH   (512.0f)
/* Shorter trips are not worth a detour to the road */
#define ROUTE_MIN_DIST       (128.0f)
/* A straight stretch of road gets a waypoint at least this often, in tiles */
#define ROUTE_STRAIGHT_TILES (6)
/* Plus the unit's radius: within this of a waypoint the next one is ordered,
 * early enough that the unit does not brake for it */
#define ROUTE_ADVANCE_DIST   (24.0f)
/* 20 Hz ticks for an order to show in the unit's movement state */
#define ROUTE_ADOPT_TICKS    (3)
/* 20 Hz ticks standing still before a waypoint is given up on */
#define ROUTE_STALL_TICKS    (40)
#define ROUTE_DEST_TOLERANCE (12.0f)

enum route_final{
    ROUTE_FINAL_NONE,
    ROUTE_FINAL_POINT,
    ROUTE_FINAL_ENTITY,
};

struct road_unit{
    /* Kept to the roads, or sent along them when the detour is at most 
     * 'preference' times the straight way (0 for never) */
    bool             locked;
    float            preference;
    bool             routing;
    int              nwaypoints, next;
    vec2_t           waypoints[MAX_ROUTE_WAYPOINTS];
    /* What the unit is ordered to once past the last waypoint */
    enum route_final final;
    vec2_t           final_xz;
    uint32_t         final_uid;
    bool             attack;
    /* The waypoint last ordered, and for how many ticks */
    vec2_t           issued;
    int              issued_age;
    int              still_ticks;
};

KHASH_MAP_INIT_INT(runit, struct road_unit)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(runit)  *s_units;
static struct road_desc s_tiles[MAX_ROUTE_TILES];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static float dist(vec2_t a, vec2_t b)
{
    vec2_t d;
    PFM_Vec2_Sub(&a, &b, &d);
    return PFM_Vec2_Len(&d);
}

static struct road_unit *unit_get(uint32_t uid)
{
    khiter_t k = kh_get(runit, s_units, uid);
    if(k == kh_end(s_units))
        return NULL;
    return &kh_value(s_units, k);
}

static struct road_unit *unit_get_or_add(uint32_t uid)
{
    int status;
    khiter_t k = kh_get(runit, s_units, uid);
    if(k != kh_end(s_units))
        return &kh_value(s_units, k);
    k = kh_put(runit, s_units, uid, &status);
    if(status == -1)
        return NULL;
    memset(&kh_value(s_units, k), 0, sizeof(struct road_unit));
    return &kh_value(s_units, k);
}

/* A waypoint where the road turns, at least every ROUTE_STRAIGHT_TILES along a
 * straight, and at its end */
static int route_waypoints(const struct road_desc *tiles, size_t ntiles, vec2_t *out, int maxout)
{
    int ret = 0;
    int since = 0;
    for(size_t i = 1; i < ntiles && ret < maxout; i++) {

        since++;
        bool last = (i == ntiles - 1);
        bool turn = !last 
            && ((tiles[i].r - tiles[i-1].r) != (tiles[i+1].r - tiles[i].r)
             || (tiles[i].c - tiles[i-1].c) != (tiles[i+1].c - tiles[i].c));
        if(!last && !turn && since < ROUTE_STRAIGHT_TILES)
            continue;
        out[ret++] = G_Roads_Centre(tiles[i]);
        since = 0;
    }
    return ret;
}

/* Plans the roads from the unit to the goal into 'u'. A road-locked unit takes 
 * them whenever they lead there; any other unit only within its limit. 
 */
static bool route_plan(uint32_t uid, vec2_t goal, bool locked, float limit, struct road_unit *u)
{
    vec2_t pos = G_Pos_GetXZ(uid);
    float direct = dist(pos, goal);
    if(!locked && direct < ROUTE_MIN_DIST)
        return false;

    struct road_desc entry, exit;
    if(!G_Roads_Nearest(pos, ROUTE_REACH, &entry))
        return false;
    if(!G_Roads_Nearest(goal, locked ? ROUTE_LOCKED_REACH : ROUTE_REACH, &exit))
        return false;

    size_t ntiles = G_Roads_Route(entry, exit, MAX_ROUTE_TILES, s_tiles);
    if(ntiles == 0)
        return false;

    if(!locked) {
        if(ntiles < 3)
            return false;
        float along = 0.0f;
        for(size_t i = 1; i < ntiles; i++)
            along += dist(G_Roads_Centre(s_tiles[i - 1]), G_Roads_Centre(s_tiles[i]));
        float total = dist(pos, G_Roads_Centre(entry)) + along + dist(G_Roads_Centre(exit), goal);
        if(total > limit * direct)
            return false;
    }

    u->nwaypoints = route_waypoints(s_tiles, ntiles, u->waypoints, MAX_ROUTE_WAYPOINTS);
    if(u->nwaypoints == 0)
        u->waypoints[u->nwaypoints++] = G_Roads_Centre(entry);
    u->next = 0;
    return true;
}

static void route_finish(uint32_t uid, struct road_unit *u)
{
    u->routing = false;
    switch(u->final) {
    case ROUTE_FINAL_POINT:
        G_Move_SetDest(uid, u->final_xz, u->attack);
        break;
    case ROUTE_FINAL_ENTITY:
        if(G_EntityExists(u->final_uid))
            G_Move_SetSurroundEntity(uid, u->final_uid);
        break;
    default:
        break;
    }
}

static void route_issue_next(uint32_t uid, struct road_unit *u)
{
    if(u->next >= u->nwaypoints) {
        route_finish(uid, u);
        return;
    }
    u->issued = u->waypoints[u->next];
    u->issued_age = 0;
    u->still_ticks = 0;
    G_Move_SetDest(uid, u->issued, u->attack);
}

static bool route_start(uint32_t uid, vec2_t goal, bool attack, enum route_final final, 
                        uint32_t final_uid)
{
    struct road_unit *u = unit_get(uid);
    if(!u || (!u->locked && u->preference <= 0.0f))
        return false;
    if(!route_plan(uid, goal, u->locked, u->preference, u)) {
        u->routing = false;
        return false;
    }

    u->routing = true;
    u->final = final;
    u->final_xz = goal;
    u->final_uid = final_uid;
    u->attack = attack;
    route_issue_next(uid, u);
    return true;
}

static void on_20hz_tick(void *user, void *event)
{
    for(khiter_t k = kh_begin(s_units); k != kh_end(s_units); k++) {

        if(!kh_exist(s_units, k))
            continue;
        struct road_unit *u = &kh_value(s_units, k);
        if(!u->routing)
            continue;
        uint32_t uid = kh_key(s_units, k);
        if(!G_EntityExists(uid)) {
            u->routing = false;
            continue;
        }

        /* Another order took over */
        u->issued_age++;
        if(u->issued_age >= ROUTE_ADOPT_TICKS) {
            vec2_t dest;
            bool attack;
            uint32_t surrounded;
            if(!G_Move_GetDest(uid, &dest, &attack)
            || dist(dest, u->issued) > ROUTE_DEST_TOLERANCE
            || G_Move_GetSurrounding(uid, &surrounded)) {
                u->routing = false;
                continue;
            }
        }

        vec2_t pos = G_Pos_GetXZ(uid);
        float reach = ROUTE_ADVANCE_DIST + G_GetSelectionRadius(uid);
        bool stalled = false;
        if(u->issued_age >= ROUTE_ADOPT_TICKS && G_Move_Still(uid)) {
            stalled = (++u->still_ticks > ROUTE_STALL_TICKS);
        }else{
            u->still_ticks = 0;
        }

        if(dist(pos, u->issued) < reach || stalled) {
            u->next++;
            route_issue_next(uid, u);
        }
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_RoadRoute_Init(void)
{
    if(!(s_units = kh_init(runit)))
        return false;
    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
    return true;
}

void G_RoadRoute_Shutdown(void)
{
    if(!s_units)
        return;
    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
    kh_destroy(runit, s_units);
    s_units = NULL;
}

void G_RoadRoute_RemoveEntity(uint32_t uid)
{
    khiter_t k = kh_get(runit, s_units, uid);
    if(k != kh_end(s_units))
        kh_del(runit, s_units, k);
}

bool G_RoadRoute_MoveTo(uint32_t uid, vec2_t xz, bool attack)
{
    struct road_unit *u = unit_get(uid);
    if(!u || (!u->locked && u->preference <= 0.0f))
        return false;

    G_RoadRoute_Cancel(uid);
    if(route_start(uid, xz, attack, u->locked ? ROUTE_FINAL_NONE : ROUTE_FINAL_POINT, 0))
        return true;
    /* A unit kept to the roads stays put when they do not lead there */
    return u->locked;
}

void G_RoadRoute_SurroundEntity(uint32_t uid, uint32_t target)
{
    G_RoadRoute_Cancel(uid);
    if(route_start(uid, G_Pos_GetXZ(target), false, ROUTE_FINAL_ENTITY, target))
        return;
    G_Move_SetSurroundEntity(uid, target);
}

void G_RoadRoute_Cancel(uint32_t uid)
{
    struct road_unit *u = unit_get(uid);
    if(u)
        u->routing = false;
}

bool G_RoadRoute_Routing(uint32_t uid)
{
    struct road_unit *u = unit_get(uid);
    return u && u->routing;
}

void G_RoadRoute_SetLocked(uint32_t uid, bool locked)
{
    struct road_unit *u = unit_get_or_add(uid);
    if(!u)
        return;
    u->locked = locked;
    if(!locked && u->preference <= 0.0f)
        G_RoadRoute_RemoveEntity(uid);
}

bool G_RoadRoute_GetLocked(uint32_t uid)
{
    struct road_unit *u = unit_get(uid);
    return u && u->locked;
}

void G_RoadRoute_SetPreference(uint32_t uid, float limit)
{
    struct road_unit *u = unit_get_or_add(uid);
    if(!u)
        return;
    u->preference = MAX(limit, 0.0f);
    if(!u->locked && u->preference <= 0.0f)
        G_RoadRoute_RemoveEntity(uid);
}

float G_RoadRoute_GetPreference(uint32_t uid)
{
    struct road_unit *u = unit_get(uid);
    return u ? u->preference : 0.0f;
}

bool G_RoadRoute_SaveState(struct SDL_RWops *stream)
{
    struct attr num = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_units)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num, "num_road_units"));

    for(khiter_t k = kh_begin(s_units); k != kh_end(s_units); k++) {

        if(!kh_exist(s_units, k))
            continue;
        const struct road_unit *u = &kh_value(s_units, k);

        struct attr attrs[] = {
            {.type = TYPE_INT,   .val.as_int = kh_key(s_units, k)},
            {.type = TYPE_BOOL,  .val.as_bool = u->locked},
            {.type = TYPE_FLOAT, .val.as_float = u->preference},
            {.type = TYPE_BOOL,  .val.as_bool = u->routing},
            {.type = TYPE_INT,   .val.as_int = u->nwaypoints},
            {.type = TYPE_INT,   .val.as_int = u->next},
            {.type = TYPE_INT,   .val.as_int = u->final},
            {.type = TYPE_VEC2,  .val.as_vec2 = u->final_xz},
            {.type = TYPE_INT,   .val.as_int = u->final_uid},
            {.type = TYPE_BOOL,  .val.as_bool = u->attack},
            {.type = TYPE_VEC2,  .val.as_vec2 = u->issued},
        };
        for(int i = 0; i < ARR_SIZE(attrs); i++)
            CHK_TRUE_RET(Attr_Write(stream, &attrs[i], "road_unit"));

        for(int i = 0; i < u->nwaypoints; i++) {
            struct attr wp = (struct attr){.type = TYPE_VEC2, .val.as_vec2 = u->waypoints[i]};
            CHK_TRUE_RET(Attr_Write(stream, &wp, "waypoint"));
        }
    }
    return true;
}

static bool parse(struct SDL_RWops *stream, int type, struct attr *out)
{
    return Attr_Parse(stream, out, true) && out->type == type;
}

bool G_RoadRoute_LoadState(struct SDL_RWops *stream)
{
    kh_clear(runit, s_units);

    struct attr attr;
    CHK_TRUE_RET(parse(stream, TYPE_INT, &attr));
    const int num = attr.val.as_int;

    for(int i = 0; i < num; i++) {

        struct road_unit u = {0};
        CHK_TRUE_RET(parse(stream, TYPE_INT, &attr));
        uint32_t uid = attr.val.as_int;
        CHK_TRUE_RET(parse(stream, TYPE_BOOL, &attr));
        u.locked = attr.val.as_bool;
        CHK_TRUE_RET(parse(stream, TYPE_FLOAT, &attr));
        u.preference = attr.val.as_float;
        CHK_TRUE_RET(parse(stream, TYPE_BOOL, &attr));
        u.routing = attr.val.as_bool;
        CHK_TRUE_RET(parse(stream, TYPE_INT, &attr));
        u.nwaypoints = attr.val.as_int;
        CHK_TRUE_RET(u.nwaypoints >= 0 && u.nwaypoints <= MAX_ROUTE_WAYPOINTS);
        CHK_TRUE_RET(parse(stream, TYPE_INT, &attr));
        u.next = attr.val.as_int;
        CHK_TRUE_RET(parse(stream, TYPE_INT, &attr));
        u.final = attr.val.as_int;
        CHK_TRUE_RET(parse(stream, TYPE_VEC2, &attr));
        u.final_xz = attr.val.as_vec2;
        CHK_TRUE_RET(parse(stream, TYPE_INT, &attr));
        u.final_uid = attr.val.as_int;
        CHK_TRUE_RET(parse(stream, TYPE_BOOL, &attr));
        u.attack = attr.val.as_bool;
        CHK_TRUE_RET(parse(stream, TYPE_VEC2, &attr));
        u.issued = attr.val.as_vec2;

        for(int j = 0; j < u.nwaypoints; j++) {
            CHK_TRUE_RET(parse(stream, TYPE_VEC2, &attr));
            u.waypoints[j] = attr.val.as_vec2;
        }

        struct road_unit *dst = unit_get_or_add(uid);
        CHK_TRUE_RET(dst);
        *dst = u;
    }
    return true;
}
