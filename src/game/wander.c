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

#include "wander.h"
#include "public/game.h"
#include "../movement/public/movement.h"
#include "position.h"
#include "../main.h"
#include "../event.h"
#include "../entity.h"
#include "../sched.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"
#include "../map/public/map.h"
#include "../pf_math.h"

#include <assert.h>
#include <math.h>

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)         (sizeof(a)/sizeof((a)[0]))
#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

#define TICKS_PER_S         (20)
/* How often the wanderers are looked over, in 20 Hz ticks */
#define CHECK_TICKS         (4)
/* An order given this recently may not have set the entity moving yet */
#define SETTLE_TICKS        (10)
/* A walk that has not ended by now is given up */
#define WALK_MAX_TICKS      (30 * TICKS_PER_S)

#define STEP_MIN            (10.0f)
#define STEP_MAX            (32.0f)
#define LONG_STEP_MIN       (40.0f)
#define LONG_STEP_MAX       (75.0f)
#define LONG_STEP_CHANCE    (0.2f)
#define MAX_STEPS_IN_BOUT   (3)
/* The wanderer turns back to its anchor once this far out, as a part of the radius */
#define TURN_BACK_AT        (0.7f)
#define NUM_TRIES           (6)

#define PAUSE_MIN_S         (1.5f)
#define PAUSE_MAX_S         (4.5f)
#define GRAZE_MIN_S         (10.0f)
#define GRAZE_MAX_S         (28.0f)
#define FIRST_REST_MAX_S    (10.0f)
#define BLOCKED_REST_MIN_S  (3.0f)
#define BLOCKED_REST_MAX_S  (6.0f)
#define SHAKEN_REST_MIN_S   (4.0f)
#define SHAKEN_REST_MAX_S   (9.0f)

enum wander_state {
    WANDER_REST,
    WANDER_WALK,
    WANDER_FLEE,
};

struct wanderer {
    vec2_t            anchor;
    float             radius;
    enum wander_state state;
    /* When a rest or flight ends */
    uint32_t          until_tick;
    /* When the current walk or flight was ordered */
    uint32_t          order_tick;
    /* Steps left of this bout, before it settles down to graze */
    int               steps_left;
    float             heading;
    /* The speed to go back to after a flight */
    float             walk_speed;
};

KHASH_MAP_INIT_INT(wanderer, struct wanderer)

static const struct map  *s_map;
static khash_t(wanderer) *s_wanderers;
static uint32_t           s_tick;
static uint32_t           s_rng = 0x9e3779b9;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct wanderer *wanderer_get(uint32_t uid)
{
    khiter_t k = kh_get(wanderer, s_wanderers, uid);
    return (k == kh_end(s_wanderers)) ? NULL : &kh_value(s_wanderers, k);
}

static float rand_unit(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (s_rng >> 8) / 16777216.0f;
}

static float rand_range(float lo, float hi)
{
    return lo + (hi - lo) * rand_unit();
}

static uint32_t ticks_from_s(float seconds)
{
    return (uint32_t)(seconds * TICKS_PER_S);
}

static void rest(struct wanderer *w, float min_s, float max_s)
{
    w->state = WANDER_REST;
    w->until_tick = s_tick + ticks_from_s(rand_range(min_s, max_s));
}

static vec2_t point_along(vec2_t from, float angle, float dist)
{
    return (vec2_t){from.x + cosf(angle) * dist, from.z + sinf(angle) * dist};
}

static float dist_between(vec2_t a, vec2_t b)
{
    vec2_t delta;
    PFM_Vec2_Sub(&a, &b, &delta);
    return PFM_Vec2_Len(&delta);
}

static bool can_walk_to(uint32_t uid, vec2_t from, vec2_t to)
{
    enum nav_layer layer = Entity_NavLayer(uid);
    return G_PointInsideMap(to)
        && M_NavPositionPathable(s_map, layer, to)
        && M_NavLocationsReachable(s_map, layer, from, to);
}

/* Keeps on roughly as it was going, until far enough out to head back */
static float next_heading(const struct wanderer *w, vec2_t pos)
{
    if(dist_between(pos, w->anchor) > w->radius * TURN_BACK_AT) {
        float home = atan2f(w->anchor.z - pos.z, w->anchor.x - pos.x);
        return home + rand_range(-M_PI / 4.0f, M_PI / 4.0f);
    }
    return w->heading + rand_range(-M_PI / 3.0f, M_PI / 3.0f);
}

static bool take_step(uint32_t uid, struct wanderer *w)
{
    vec2_t pos = G_Pos_GetXZ(uid);
    bool far = (rand_unit() < LONG_STEP_CHANCE);

    for(int i = 0; i < NUM_TRIES; i++) {

        float angle = next_heading(w, pos);
        float dist = far ? rand_range(LONG_STEP_MIN, LONG_STEP_MAX) : rand_range(STEP_MIN, STEP_MAX);
        vec2_t dest = point_along(pos, angle, MIN(dist, w->radius));

        /* One driven past the edge may still make its way back */
        float from_anchor = dist_between(dest, w->anchor);
        if(from_anchor > w->radius && from_anchor >= dist_between(pos, w->anchor)) {
            w->heading = atan2f(w->anchor.z - pos.z, w->anchor.x - pos.x);
            continue;
        }
        if(!can_walk_to(uid, pos, dest)) {
            w->heading = rand_range(-M_PI, M_PI);
            continue;
        }
        w->heading = angle;
        w->state = WANDER_WALK;
        w->order_tick = s_tick;
        Move_SetDest(uid, dest, false);
        return true;
    }
    return false;
}

static void end_flight(uint32_t uid, struct wanderer *w)
{
    Move_SetMaxSpeed(uid, w->walk_speed);
    if(!Move_Still(uid)) {
        Move_Stop(uid);
    }
    w->steps_left = 1;
    rest(w, SHAKEN_REST_MIN_S, SHAKEN_REST_MAX_S);
}

static void update_wanderer(uint32_t uid, struct wanderer *w)
{
    bool settled = (s_tick - w->order_tick) >= SETTLE_TICKS;

    switch(w->state) {
    case WANDER_REST:
        if((int32_t)(s_tick - w->until_tick) < 0 || !Move_Still(uid))
            break;
        if(w->steps_left <= 0) {
            w->steps_left = 1 + (int)(rand_unit() * MAX_STEPS_IN_BOUT);
        }
        if(take_step(uid, w)) {
            w->steps_left--;
        }else{
            rest(w, BLOCKED_REST_MIN_S, BLOCKED_REST_MAX_S);
        }
        break;
    case WANDER_WALK:
        if(!settled)
            break;
        if(Move_Still(uid)) {
            if(w->steps_left > 0) {
                rest(w, PAUSE_MIN_S, PAUSE_MAX_S);
            }else{
                rest(w, GRAZE_MIN_S, GRAZE_MAX_S);
            }
        }else if(s_tick - w->order_tick >= WALK_MAX_TICKS) {
            Move_Stop(uid);
        }
        break;
    case WANDER_FLEE:
        if((int32_t)(s_tick - w->until_tick) >= 0 || (settled && Move_Still(uid))) {
            end_flight(uid, w);
        }
        break;
    default: assert(0);
    }
}

static void on_20hz_tick(void *user, void *event)
{
    s_tick++;
    if(s_tick % CHECK_TICKS)
        return;

    uint32_t uid;
    struct wanderer *w;
    for(khiter_t k = kh_begin(s_wanderers); k != kh_end(s_wanderers); k++) {
        if(!kh_exist(s_wanderers, k))
            continue;
        uid = kh_key(s_wanderers, k);
        w = &kh_value(s_wanderers, k);
        update_wanderer(uid, w);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Wander_Init(const struct map *map)
{
    s_map = map;
    if(!(s_wanderers = kh_init(wanderer)))
        return false;
    s_tick = 0;
    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
    return true;
}

void G_Wander_Shutdown(void)
{
    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
    kh_destroy(wanderer, s_wanderers);
}

bool G_Wander_Start(uint32_t uid, vec2_t anchor, float radius)
{
    ASSERT_IN_MAIN_THREAD();

    if(!(G_FlagsGet(uid) & ENTITY_FLAG_MOVABLE) || radius <= 0.0f)
        return false;

    int status;
    khiter_t k = kh_put(wanderer, s_wanderers, uid, &status);
    if(status == -1)
        return false;

    float walk_speed = 0.0f;
    if(status == 0 && kh_value(s_wanderers, k).state == WANDER_FLEE) {
        walk_speed = kh_value(s_wanderers, k).walk_speed;
    }else{
        Move_GetMaxSpeed(uid, &walk_speed);
    }
    Move_SetMaxSpeed(uid, walk_speed);

    kh_value(s_wanderers, k) = (struct wanderer){
        .anchor = anchor,
        .radius = radius,
        .state = WANDER_REST,
        .until_tick = s_tick + ticks_from_s(rand_range(0.0f, FIRST_REST_MAX_S)),
        .order_tick = s_tick,
        .steps_left = 0,
        .heading = rand_range(-M_PI, M_PI),
        .walk_speed = walk_speed,
    };
    return true;
}

void G_Wander_Stop(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(wanderer, s_wanderers, uid);
    if(k == kh_end(s_wanderers))
        return;

    struct wanderer *w = &kh_value(s_wanderers, k);
    if(w->state == WANDER_FLEE) {
        Move_SetMaxSpeed(uid, w->walk_speed);
    }
    if(w->state != WANDER_REST) {
        Move_Stop(uid);
    }
    kh_del(wanderer, s_wanderers, k);
}

void G_Wander_RemoveEntity(uint32_t uid)
{
    khiter_t k = kh_get(wanderer, s_wanderers, uid);
    if(k != kh_end(s_wanderers)) {
        kh_del(wanderer, s_wanderers, k);
    }
}

bool G_Wander_Active(uint32_t uid)
{
    return (wanderer_get(uid) != NULL);
}

bool G_Wander_Flee(uint32_t uid, vec2_t threat, float seconds, float speed)
{
    ASSERT_IN_MAIN_THREAD();

    /* Straight away from the threat if the ground allows, else as near that as it does */
    static const float s_fan[] = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.5f, -1.5f, 2.1f, -2.1f};

    struct wanderer *w = wanderer_get(uid);
    if(!w || seconds <= 0.0f || speed <= 0.0f)
        return false;

    vec2_t pos = G_Pos_GetXZ(uid);
    float away = w->heading;
    if(dist_between(pos, threat) > 0.5f) {
        away = atan2f(pos.z - threat.z, pos.x - threat.x);
    }

    float dist = speed * seconds;
    for(int i = 0; i < ARR_SIZE(s_fan); i++) {

        vec2_t dest = point_along(pos, away + s_fan[i], dist);
        if(!can_walk_to(uid, pos, dest))
            continue;

        if(w->state != WANDER_FLEE) {
            Move_GetMaxSpeed(uid, &w->walk_speed);
        }
        Move_SetMaxSpeed(uid, speed);
        Move_SetDest(uid, dest, false);

        w->state = WANDER_FLEE;
        w->heading = away + s_fan[i];
        w->order_tick = s_tick;
        w->until_tick = s_tick + ticks_from_s(seconds);
        return true;
    }
    return false;
}

bool G_Wander_Fleeing(uint32_t uid)
{
    struct wanderer *w = wanderer_get(uid);
    return (w && w->state == WANDER_FLEE);
}

bool G_Wander_SaveState(struct SDL_RWops *stream)
{
    struct attr num_wanderers = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_wanderers)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_wanderers, "num_wanderers"));

    uint32_t uid;
    struct wanderer w;
    kh_foreach(s_wanderers, uid, w, {

        struct attr attrs[] = {
            {.type = TYPE_INT,   .val.as_int = uid},
            {.type = TYPE_VEC2,  .val.as_vec2 = w.anchor},
            {.type = TYPE_FLOAT, .val.as_float = w.radius},
            {.type = TYPE_INT,   .val.as_int = w.state},
            {.type = TYPE_INT,   .val.as_int = (int32_t)(w.until_tick - s_tick)},
            {.type = TYPE_INT,   .val.as_int = s_tick - w.order_tick},
            {.type = TYPE_INT,   .val.as_int = w.steps_left},
            {.type = TYPE_FLOAT, .val.as_float = w.heading},
            {.type = TYPE_FLOAT, .val.as_float = w.walk_speed},
        };
        for(int i = 0; i < ARR_SIZE(attrs); i++) {
            CHK_TRUE_RET(Attr_Write(stream, &attrs[i], "wanderer"));
        }
        Sched_TryYield();
    });
    return true;
}

bool G_Wander_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;
    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_wanderers = attr.val.as_int;

    for(int i = 0; i < num_wanderers; i++) {

        struct attr attrs[9];
        static const int s_types[ARR_SIZE(attrs)] = {
            TYPE_INT, TYPE_VEC2, TYPE_FLOAT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_FLOAT, TYPE_FLOAT
        };
        for(int j = 0; j < ARR_SIZE(attrs); j++) {
            CHK_TRUE_RET(Attr_Parse(stream, &attrs[j], true));
            CHK_TRUE_RET(attrs[j].type == s_types[j]);
        }

        uint32_t uid = attrs[0].val.as_int;
        CHK_TRUE_RET(attrs[3].val.as_int >= WANDER_REST && attrs[3].val.as_int <= WANDER_FLEE);
        if(!G_EntityExists(uid))
            continue;

        int status;
        khiter_t k = kh_put(wanderer, s_wanderers, uid, &status);
        CHK_TRUE_RET(status != -1);
        kh_value(s_wanderers, k) = (struct wanderer){
            .anchor = attrs[1].val.as_vec2,
            .radius = attrs[2].val.as_float,
            .state = attrs[3].val.as_int,
            .until_tick = s_tick + (uint32_t)attrs[4].val.as_int,
            .order_tick = s_tick - (uint32_t)attrs[5].val.as_int,
            .steps_left = attrs[6].val.as_int,
            .heading = attrs[7].val.as_float,
            .walk_speed = attrs[8].val.as_float,
        };
        Sched_TryYield();
    }
    return true;
}
