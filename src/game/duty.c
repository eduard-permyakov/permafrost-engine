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
#define MEM_FILE_SUB MEM_SUB_GAME_DUTY
#include "duty.h"
#include "public/game.h"
#include "game_private.h"
#include "movement.h"
#include "harvester.h"
#include "builder.h"
#include "posts.h"
#include "automation.h"
#include "../main.h"
#include "../event.h"
#include "../entity.h"
#include "../sched.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_string.h"
#include "../map/public/map.h"
#include "../pf_math.h"
#include "position.h"

#include <assert.h>
#include <string.h>
#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_DUTY)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_DUTY)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_DUTY)

#define ARR_SIZE(a)         (sizeof(a)/sizeof((a)[0]))
#define MAX_GOODS           (16)
/* Resumes that come to nothing in a row before the duty is given up */
#define MAX_FAILS           (3)
/* How often the suppliers standing by are looked at for a new request */
#define WAIT_CHECK_TICKS    (20)
/* How close to its target a supplier stands by */
#define WAIT_REACH          (30.0f)
/* How many looks a gatherer the automation keeps waits for room, about 30 s */
#define GATHER_WAIT_CHECKS  (30)
/* How long a tag waits for the duty it was set for */
#define TAG_TICKS           (1)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

struct good_count{
    char good[DUTY_GOOD_LEN];
    int  gatherers;
    int  suppliers;
};

/* Who is at work on a target, by kind; the goods only for gathering and supplying */
struct target_counts{
    int               builders;
    int               holders;
    int               ingredients;
    int               ngoods;
    struct good_count goods[MAX_GOODS];
};

KHASH_MAP_INIT_INT(duty, struct duty)
KHASH_MAP_INIT_INT(counts, struct target_counts)
/* Who gives a unit's next duty, and when it was said */
struct tag {
    enum duty_by by;
    uint32_t     tick;
};

KHASH_MAP_INIT_INT(tag, struct tag)

static void on_20hz_tick(void *user, void *event);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(duty)   *s_duties;
static khash_t(counts) *s_counts;
/* Who gives a unit's next duty, set by the one about to give it */
static khash_t(tag)    *s_tags;
static uint32_t         s_tick;
static const struct map *s_map;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct duty *duty_get(uint32_t uid)
{
    khiter_t k = kh_get(duty, s_duties, uid);
    if(k == kh_end(s_duties))
        return NULL;
    return &kh_value(s_duties, k);
}

static bool has_good(enum duty_kind kind)
{
    return (kind == DUTY_GATHER || kind == DUTY_SUPPLY);
}

static struct good_count *good_count_get(struct target_counts *tc, const char *good, bool create)
{
    for(int i = 0; i < tc->ngoods; i++) {
        if(!strcmp(tc->goods[i].good, good))
            return &tc->goods[i];
    }
    if(!create || tc->ngoods == MAX_GOODS)
        return NULL;
    struct good_count *ret = &tc->goods[tc->ngoods++];
    *ret = (struct good_count){0};
    pf_strlcpy(ret->good, good, sizeof(ret->good));
    return ret;
}

static bool counts_empty(const struct target_counts *tc)
{
    if(tc->builders || tc->holders || tc->ingredients)
        return false;
    for(int i = 0; i < tc->ngoods; i++) {
        if(tc->goods[i].gatherers || tc->goods[i].suppliers)
            return false;
    }
    return true;
}

static void count_add(uint32_t target, enum duty_kind kind, const char *good, int delta)
{
    if(target == NULL_UID)
        return;
    int status;
    khiter_t k = kh_put(counts, s_counts, target, &status);
    if(status == -1)
        return;
    struct target_counts *tc = &kh_value(s_counts, k);
    if(status != 0) {
        *tc = (struct target_counts){0};
    }
    switch(kind) {
    case DUTY_BUILD:      tc->builders += delta;    break;
    case DUTY_POST:       tc->holders += delta;     break;
    case DUTY_INGREDIENT: tc->ingredients += delta; break;
    case DUTY_GATHER:
    case DUTY_SUPPLY: {
        struct good_count *gc = good_count_get(tc, good, true);
        if(gc) {
            if(kind == DUTY_GATHER) {
                gc->gatherers += delta;
            }else{
                gc->suppliers += delta;
            }
        }
        break;
    }
    default:
        break;
    }
    if(counts_empty(tc)) {
        kh_del(counts, s_counts, k);
    }
}

static void notify_changed(uint32_t uid)
{
    E_Global_Notify(EVENT_DUTY_CHANGED, (void*)((uintptr_t)uid), ES_ENGINE);
}

static bool take_tag(uint32_t uid, enum duty_by *out)
{
    khiter_t k = kh_get(tag, s_tags, uid);
    if(k == kh_end(s_tags))
        return false;
    struct tag t = kh_value(s_tags, k);
    kh_del(tag, s_tags, k);
    /* One left over from an order that came to nothing is not this duty's */
    if(s_tick - t.tick > TAG_TICKS)
        return false;
    *out = t.by;
    return true;
}

static void drop_tag(uint32_t uid)
{
    khiter_t k = kh_get(tag, s_tags, uid);
    if(k != kh_end(s_tags)) {
        kh_del(tag, s_tags, k);
    }
}

static bool target_gone(uint32_t target)
{
    return target == NULL_UID || !G_EntityExists(target) || G_EntityIsZombie(target);
}

static void end(uint32_t uid, struct duty *d, const char *why)
{
    count_add(d->target, d->kind, d->good, -1);
    if(why) {
        G_Automation_TraceFollowOn(uid, "duty", why, has_good(d->kind) ? d->good : NULL, d->target);
    }
    khiter_t k = kh_get(duty, s_duties, uid);
    if(k != kh_end(s_duties)) {
        kh_del(duty, s_duties, k);
    }
    notify_changed(uid);
}

static bool fail(uint32_t uid, struct duty *d, const char *why)
{
    if(++d->fails < MAX_FAILS)
        return false;
    end(uid, d, why);
    return false;
}

static bool near_target(uint32_t uid, uint32_t target)
{
    if(M_NavObjAdjacent(s_map, uid, target))
        return true;
    vec2_t a = G_Pos_GetXZ(uid), b = G_Pos_GetXZ(target);
    vec2_t delta;
    PFM_Vec2_Sub(&a, &b, &delta);
    float reach = G_GetSelectionRadius(uid) + G_GetSelectionRadius(target) + WAIT_REACH;
    return PFM_Vec2_Len(&delta) <= reach;
}

/* A supplier with nothing to carry stands by its target until it asks again */
static bool stand_by(uint32_t uid, struct duty *d)
{
    d->fails = 0;
    if(!d->waiting) {
        d->waiting = true;
        notify_changed(uid);
    }
    if(!near_target(uid, d->target) && G_Move_Still(uid)) {
        G_Move_SetSurroundEntity(uid, d->target);
    }
    return true;
}

static bool resume_gather(uint32_t uid, struct duty *d)
{
    uint32_t node = (d->by == DUTY_BY_PLAYER) ? G_Harvester_ResumeNode(uid, d->good)
                                              : G_Automation_GatherNode(uid, d->good);
    if(node == NULL_UID)
        return fail(uid, d, "no_node");
    if(!G_Harvester_Gather(uid, node))
        return fail(uid, d, "no_node");
    d->fails = 0;
    return true;
}

static bool resume_build(uint32_t uid, struct duty *d)
{
    if(G_Building_IsCompleted(d->target)) {
        end(uid, d, "built");
        return false;
    }
    bool ok = (d->by == DUTY_BY_PLAYER) ? G_Builder_Order(uid, d->target)
                                        : G_Builder_Build(uid, d->target);
    if(!ok)
        return fail(uid, d, "no_site");
    d->fails = 0;
    return true;
}

static bool resume_supply(uint32_t uid, struct duty *d)
{
    if(!(G_FlagsGet(d->target) & ENTITY_FLAG_STORAGE_SITE)) {
        end(uid, d, "gone");
        return false;
    }
    /* A trip the automation gave ends with the trip: it gives the next */
    if(d->by != DUTY_BY_PLAYER) {
        end(uid, d, NULL);
        return false;
    }
    G_Harvester_ClearQueuedCmd(uid);
    if(G_Harvester_Transport(uid, d->target)) {
        d->fails = 0;
        if(d->waiting) {
            d->waiting = false;
            notify_changed(uid);
        }
        return true;
    }
    /* A load the target does not take goes to a store that does first */
    if(G_Harvester_GetCurrTotalCarry(uid)) {
        if(G_Harvester_BringInLoad(uid, NULL_UID, false))
            return true;
        return fail(uid, d, "no_store");
    }
    return stand_by(uid, d);
}

static bool resume_ingredient(uint32_t uid, struct duty *d)
{
    if(G_Garrison_IsIngredient(uid))
        return true;
    int tries = d->fails + 1;
    if(tries >= MAX_FAILS) {
        end(uid, d, "no_door");
        return false;
    }
    if(!G_Garrison_EnterAsIngredient(d->target, uid))
        return fail(uid, d, "no_door");
    /* Taking the duty over again zeroes the tries that came to nothing */
    if((d = duty_get(uid)) != NULL) {
        d->fails = tries;
    }
    return true;
}

/* Walking off, it is at its meal: the load goes in once it stands again */
static void resume_waiting_gatherer(uint32_t uid, struct duty *d)
{
    if(!G_Harvester_GetCurrTotalCarry(uid)) {
        d->waiting = false;
        notify_changed(uid);
        return;
    }
    if(G_Move_Still(uid)) {
        G_Harvester_BringInLoad(uid, NULL_UID, true);
    }
    /* One the automation keeps is free for other work while the stores are full */
    d = duty_get(uid);
    if(d && d->waiting && d->by != DUTY_BY_PLAYER && ++d->fails >= GATHER_WAIT_CHECKS) {
        end(uid, d, "no_room");
    }
}

static void check_waiting(void)
{
    uint32_t waiting[512];
    size_t nwaiting = 0;

    uint32_t uid;
    struct duty *d;
    kh_foreach_val_ptr(s_duties, uid, d, {
        if(nwaiting == ARR_SIZE(waiting))
            break;
        if(d->waiting) {
            waiting[nwaiting++] = uid;
        }
    });

    for(int i = 0; i < nwaiting; i++) {
        uid = waiting[i];
        d = duty_get(uid);
        if(!d || !G_EntityExists(uid) || G_EntityIsZombie(uid))
            continue;
        if(d->kind == DUTY_GATHER) {
            resume_waiting_gatherer(uid, d);
            continue;
        }
        if(target_gone(d->target)) {
            end(uid, d, "gone");
            continue;
        }
        if(G_Harvester_CanSupply(uid, d->target)) {
            resume_supply(uid, d);
        }
    }
}

static void on_20hz_tick(void *user, void *event)
{
    s_tick++;
    if(s_tick % WAIT_CHECK_TICKS == 0) {
        check_waiting();
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Duty_Init(const struct map *map)
{
    s_map = map;
    if(!(s_duties = kh_init(duty)))
        goto fail_duties;
    if(!(s_counts = kh_init(counts)))
        goto fail_counts;
    if(!(s_tags = kh_init(tag)))
        goto fail_tags;
    s_tick = 0;
    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
    return true;

fail_tags:
    kh_destroy(counts, s_counts);
fail_counts:
    kh_destroy(duty, s_duties);
fail_duties:
    return false;
}

void G_Duty_Shutdown(void)
{
    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
    kh_destroy(tag, s_tags);
    kh_destroy(counts, s_counts);
    kh_destroy(duty, s_duties);
}

void G_Duty_RemoveEntity(uint32_t uid)
{
    drop_tag(uid);
    struct duty *own = duty_get(uid);
    if(own) {
        end(uid, own, NULL);
    }

    uint32_t holders[512];
    size_t nholders = 0;
    uint32_t curr;
    struct duty *d;
    kh_foreach_val_ptr(s_duties, curr, d, {
        if(nholders == ARR_SIZE(holders))
            break;
        if(d->target == uid) {
            holders[nholders++] = curr;
        }
    });
    for(int i = 0; i < nholders; i++) {
        d = duty_get(holders[i]);
        if(!d)
            continue;
        /* A gatherer keeps its good: its next load finds it a store */
        if(d->kind == DUTY_GATHER) {
            count_add(d->target, d->kind, d->good, -1);
            d->target = NULL_UID;
            continue;
        }
        end(holders[i], d, "gone");
    }
    khiter_t k = kh_get(counts, s_counts, uid);
    if(k != kh_end(s_counts)) {
        kh_del(counts, s_counts, k);
    }
}

void G_Duty_Set(uint32_t uid, enum duty_kind kind, uint32_t target, const char *good, enum duty_by by)
{
    assert(kind != DUTY_NONE);
    enum duty_by tagged;
    bool was_tagged = take_tag(uid, &tagged);
    if(was_tagged) {
        by = tagged;
    }
    const char *name = (has_good(kind) && good) ? good : "";

    struct duty *d = duty_get(uid);
    if(d && d->kind == kind && !strcmp(d->good, name)) {
        if(d->target != target) {
            count_add(d->target, kind, d->good, -1);
            d->target = target;
            count_add(target, kind, d->good, 1);
        }
        d->fails = 0;
        if(d->waiting) {
            d->waiting = false;
            notify_changed(uid);
        }
        return;
    }
    if(d) {
        /* A player's order ends the duty first: this one only takes over from it */
        if(!was_tagged) {
            by = d->by;
        }
        count_add(d->target, d->kind, d->good, -1);
    }else{
        int status;
        khiter_t k = kh_put(duty, s_duties, uid, &status);
        if(status == -1)
            return;
        d = &kh_value(s_duties, k);
    }
    *d = (struct duty){
        .kind = kind,
        .by = by,
        .target = target,
        .since_tick = s_tick,
    };
    pf_strlcpy(d->good, name, sizeof(d->good));
    count_add(target, kind, d->good, 1);
    notify_changed(uid);
}

void G_Duty_SetBy(uint32_t uid, enum duty_by by)
{
    struct duty *d = duty_get(uid);
    if(!d) {
        G_Duty_TagNext(uid, by);
        return;
    }
    if(d->by != by) {
        d->by = by;
        notify_changed(uid);
    }
}

void G_Duty_TagNext(uint32_t uid, enum duty_by by)
{
    int status;
    khiter_t k = kh_put(tag, s_tags, uid, &status);
    if(status == -1)
        return;
    kh_value(s_tags, k) = (struct tag){by, s_tick};
}

void G_Duty_End(uint32_t uid, const char *why)
{
    drop_tag(uid);
    struct duty *d = duty_get(uid);
    if(d) {
        end(uid, d, why);
    }
}

bool G_Duty_Get(uint32_t uid, struct duty *out)
{
    struct duty *d = duty_get(uid);
    if(!d)
        return false;
    *out = *d;
    return true;
}

enum duty_kind G_Duty_Kind(uint32_t uid)
{
    struct duty *d = duty_get(uid);
    return d ? d->kind : DUTY_NONE;
}

bool G_Duty_ByPlayer(uint32_t uid)
{
    struct duty *d = duty_get(uid);
    return d && d->by == DUTY_BY_PLAYER;
}

uint32_t G_Duty_AgeTicks(uint32_t uid)
{
    struct duty *d = duty_get(uid);
    return d ? (s_tick - d->since_tick) : 0;
}

bool G_Duty_Waiting(uint32_t uid)
{
    struct duty *d = duty_get(uid);
    return d && d->waiting;
}

void G_Duty_SetWaiting(uint32_t uid, bool waiting)
{
    struct duty *d = duty_get(uid);
    if(!d || d->waiting == waiting)
        return;
    d->waiting = waiting;
    notify_changed(uid);
}

bool G_Duty_Resume(uint32_t uid)
{
    struct duty *d = duty_get(uid);
    if(!d)
        return false;
    if(d->kind != DUTY_GATHER && target_gone(d->target)) {
        end(uid, d, "gone");
        return false;
    }
    switch(d->kind) {
    case DUTY_GATHER:     return resume_gather(uid, d);
    case DUTY_BUILD:      return resume_build(uid, d);
    case DUTY_SUPPLY:     return resume_supply(uid, d);
    case DUTY_INGREDIENT: return resume_ingredient(uid, d);
    case DUTY_POST:
        /* The post keeps itself for its holder and sends it back */
        if(G_Posts_Holds(uid))
            return true;
        end(uid, d, "gone");
        return false;
    default:
        return false;
    }
}

bool G_Duty_StandBy(uint32_t uid)
{
    struct duty *d = duty_get(uid);
    if(!d || d->kind != DUTY_SUPPLY || target_gone(d->target))
        return false;
    return stand_by(uid, d);
}

void G_Duty_OnCompleted(uint32_t site)
{
    bool stores = !!(G_FlagsGet(site) & ENTITY_FLAG_STORAGE_SITE);
    uint32_t done[512];
    size_t ndone = 0;

    uint32_t uid;
    struct duty *d;
    kh_foreach_val_ptr(s_duties, uid, d, {
        if(ndone == ARR_SIZE(done))
            break;
        if(d->target != site)
            continue;
        if(d->kind == DUTY_BUILD || (d->kind == DUTY_SUPPLY && !stores)) {
            done[ndone++] = uid;
        }
    });
    for(int i = 0; i < ndone; i++) {
        d = duty_get(done[i]);
        if(d) {
            end(done[i], d, "built");
        }
    }
}

int G_Duty_CountAt(uint32_t target, enum duty_kind kind, const char *good)
{
    khiter_t k = kh_get(counts, s_counts, target);
    if(k == kh_end(s_counts))
        return 0;
    const struct target_counts *tc = &kh_value(s_counts, k);
    switch(kind) {
    case DUTY_BUILD:      return tc->builders;
    case DUTY_POST:       return tc->holders;
    case DUTY_INGREDIENT: return tc->ingredients;
    case DUTY_GATHER:
    case DUTY_SUPPLY: {
        int ret = 0;
        for(int i = 0; i < tc->ngoods; i++) {
            if(good && strcmp(tc->goods[i].good, good))
                continue;
            ret += (kind == DUTY_GATHER) ? tc->goods[i].gatherers : tc->goods[i].suppliers;
        }
        return ret;
    }
    default:
        return 0;
    }
}

const char *G_Duty_KindName(enum duty_kind kind)
{
    switch(kind) {
    case DUTY_GATHER:     return "gather";
    case DUTY_BUILD:      return "build";
    case DUTY_SUPPLY:     return "supply";
    case DUTY_POST:       return "post";
    case DUTY_INGREDIENT: return "ingredient";
    default:              return "none";
    }
}

const char *G_Duty_ByName(enum duty_by by)
{
    switch(by) {
    case DUTY_BY_CITY: return "city";
    case DUTY_BY_AUTO: return "automation";
    default:           return "player";
    }
}

bool G_Duty_ParseBy(const char *name, enum duty_by *out)
{
    if(!strcmp(name, "player")) {
        *out = DUTY_BY_PLAYER;
    }else if(!strcmp(name, "city")) {
        *out = DUTY_BY_CITY;
    }else if(!strcmp(name, "automation")) {
        *out = DUTY_BY_AUTO;
    }else{
        return false;
    }
    return true;
}

bool G_Duty_SaveState(struct SDL_RWops *stream)
{
    struct attr num_duties = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_duties)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_duties, "num_duties"));

    uint32_t uid;
    struct duty d;
    kh_foreach(s_duties, uid, d, {

        struct attr attrs[] = {
            {.type = TYPE_INT,  .val.as_int = uid},
            {.type = TYPE_INT,  .val.as_int = d.kind},
            {.type = TYPE_INT,  .val.as_int = d.by},
            {.type = TYPE_INT,  .val.as_int = d.target},
            {.type = TYPE_INT,  .val.as_int = s_tick - d.since_tick},
            {.type = TYPE_BOOL, .val.as_bool = d.waiting},
        };
        for(int i = 0; i < ARR_SIZE(attrs); i++) {
            CHK_TRUE_RET(Attr_Write(stream, &attrs[i], "duty"));
        }
        struct attr good = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(good.val.as_string, d.good, sizeof(good.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &good, "duty_good"));
        Sched_TryYield();
    });
    return true;
}

bool G_Duty_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;
    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_duties = attr.val.as_int;

    for(int i = 0; i < num_duties; i++) {

        int vals[5];
        for(int j = 0; j < ARR_SIZE(vals); j++) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            vals[j] = attr.val.as_int;
        }
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        bool waiting = attr.val.as_bool;
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);

        uint32_t uid = vals[0];
        CHK_TRUE_RET(vals[1] > DUTY_NONE && vals[1] <= DUTY_INGREDIENT);
        CHK_TRUE_RET(vals[2] >= DUTY_BY_PLAYER && vals[2] <= DUTY_BY_AUTO);
        if(!G_EntityExists(uid))
            continue;

        int status;
        khiter_t k = kh_put(duty, s_duties, uid, &status);
        CHK_TRUE_RET(status != -1);
        struct duty *d = &kh_value(s_duties, k);
        /* One a loaded module already gave the unit yields to the saved one */
        if(status == 0) {
            count_add(d->target, d->kind, d->good, -1);
        }
        *d = (struct duty){
            .kind = vals[1],
            .by = vals[2],
            .target = vals[3],
            .since_tick = s_tick - (uint32_t)vals[4],
            .waiting = waiting,
        };
        pf_strlcpy(d->good, attr.val.as_string, sizeof(d->good));
        count_add(d->target, d->kind, d->good, 1);
        Sched_TryYield();
    }
    return true;
}
