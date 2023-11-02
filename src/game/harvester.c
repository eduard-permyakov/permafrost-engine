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

#include "harvester.h"
#include "movement.h"
#include "resource.h"
#include "storage_site.h"
#include "game_private.h"
#include "public/game.h"
#include "../sched.h"
#include "../event.h"
#include "../entity.h"
#include "../cursor.h"
#include "../lib/public/vec.h"
#include "../lib/public/khash.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/string_intern.h"
#include "../lib/public/mpool_allocator.h"
#include "../lib/public/attr.h"

#include <stddef.h>
#include <assert.h>
#include <string.h>

#define REACQUIRE_RADIUS    (50.0)
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define MIN(a, b)           ((a) < (b) ? (a) : (b))

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

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

KHASH_MAP_INIT_STR(int, int)
KHASH_MAP_INIT_STR(float, float)

VEC_TYPE(name, const char*)
VEC_IMPL(static, name, const char*)

enum harvester_state{
    STATE_NOT_HARVESTING,
    STATE_HARVESTING_SEEK_RESOURCE,
    STATE_HARVESTING,
    STATE_HARVESTING_SEEK_STORAGE,
    STATE_TRANSPORT_GETTING,
    STATE_TRANSPORT_PUTTING,
    STATE_TRANSPORT_SEEK_RESOURCE,
    STATE_TRANSPORT_HARVESTING,
};

struct hstate{
    enum harvester_state state;
    enum tstrategy strategy;
    uint32_t    ss_uid;
    uint32_t    res_uid;
    vec2_t      res_last_pos;
    const char *res_name;         /* borrowed */
    kh_float_t *gather_speeds;    /* How much of each resource the entity gets each cycle */
    kh_int_t   *max_carry;        /* The maximum amount of each resource the entity can carry */
    kh_int_t   *curr_carry;       /* The amount of each resource the entity currently holds */
    vec_name_t  priority;         /* The order in which the harvester will transport resources */
    bool        drop_off_only;
    float       accum;            /* How much we gathered - only integer amounts are taken */ 
    struct{
        enum{
            CMD_NONE,
            CMD_GATHER,
            CMD_TRANSPORT,
            CMD_BUILD,
            CMD_SUPPLY,
        }cmd;
        uint32_t uid_arg;
    }queued;
    uint32_t    transport_src_uid;
    uint32_t    transport_dest_uid;
};

struct searcharg{
    uint32_t ent;
    const char *rname;
    enum tstrategy strat;
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

KHASH_MAP_INIT_INT(state, struct hstate)

static void on_motion_begin_harvest(void *user, void *event);
static void on_motion_begin_travel(void *user, void *event);
static void on_arrive_at_resource(void *user, void *event);
static void on_arrive_at_resource_source(void *user, void *event);
static void on_arrive_at_storage(void *user, void *event);
static void on_arrive_at_transport_source(void *user, void *event);
static void on_arrive_at_transport_dest(void *user, void *event);
static void on_harvest_anim_finished(void *user, void *event);
static void on_harvest_anim_finished_source(void *user, void *event);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static mpa_buff_t        s_mpool;
static khash_t(stridx)  *s_stridx;
static mp_strbuff_t      s_stringpool;
static khash_t(state)   *s_entity_state_table;
static const struct map *s_map;

static bool              s_gather_on_lclick = false;
static bool              s_pick_up_on_lclick = false;
static bool              s_drop_off_on_lclick = false;
static bool              s_transport_on_lclick = false;

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

static int ss_desired(uint32_t uid, const char *rname)
{
    if(G_StorageSite_GetUseAlt(uid)) {
        return G_StorageSite_GetAltDesired(uid, rname);
    }else{
        return G_StorageSite_GetDesired(uid, rname);
    }
}

static int ss_capacity(uint32_t uid, const char *rname)
{
    if(G_StorageSite_GetUseAlt(uid)) {
        return G_StorageSite_GetAltCapacity(uid, rname);
    }else{
        return G_StorageSite_GetCapacity(uid, rname);
    }
}

static struct hstate *hstate_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;

    return &kh_value(s_entity_state_table, k);
}

static bool hstate_set(uint32_t uid, struct hstate hs)
{
    int status;
    khiter_t k = kh_put(state, s_entity_state_table, uid, &status);
    if(status == -1 || status == 0)
        return false;
    kh_value(s_entity_state_table, k) = hs;
    return true;
}

static void hstate_remove(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
}

static void hstate_destroy(struct hstate *hs)
{
    vec_name_destroy(&hs->priority);
    kh_destroy(float, hs->gather_speeds);
    kh_destroy(int, hs->max_carry);
    kh_destroy(int, hs->curr_carry);
}

static bool hstate_init(struct hstate *hs)
{
    vec_name_init_alloc(&hs->priority, prealloc, pfree);
    if(!vec_name_resize(&hs->priority, sizeof(buff_t) / sizeof(char*)))
        return false;

    hs->gather_speeds = kh_init(float);
    hs->max_carry = kh_init(int);
    hs->curr_carry = kh_init(int);

    if(!hs->gather_speeds || !hs->max_carry || !hs->curr_carry) {

        hstate_destroy(hs);
        return false;
    }

    hs->ss_uid = NULL_UID;
    hs->res_uid = NULL_UID;
    hs->res_last_pos = (vec2_t){0};
    hs->res_name = NULL;
    hs->state = STATE_NOT_HARVESTING;
    hs->drop_off_only = false;
    hs->accum = 0.0f;
    hs->strategy = TRANSPORT_STRATEGY_NEAREST;
    hs->queued.cmd = CMD_NONE;
    hs->transport_src_uid = NULL_UID;
    hs->transport_dest_uid = NULL_UID;
    return true;
}

static bool hstate_set_key_int(khash_t(int) *table, const char *name, int val)
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
    if(status == -1) {
        return false;
    }

    assert(status == 1);
    kh_value(table, k) = val;
    return true;
}

static bool hstate_get_key_int(khash_t(int) *table, const char *name, int *out)
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

static bool hstate_set_key_float(khash_t(float) *table, const char *name, float val)
{
    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(float, table, key);
    if(k != kh_end(table)) {
        kh_value(table, k) = val;
        return true;
    }

    int status;
    k = kh_put(float, table, key, &status);
    if(status == -1) {
        return false;
    }

    assert(status == 1);
    kh_value(table, k) = val;
    return true;
}

static bool hstate_get_key_float(khash_t(float) *table, const char *name, float *out)
{
    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(float, table, key);
    if(k == kh_end(table))
        return false;
    *out = kh_value(table, k);
    return true;
}

static bool compare_keys(const char **a, const char **b)
{
    /* Since all strings are interned, we can use direct pointer comparision */
    return *a == *b;
}

static void hstate_remove_prio(struct hstate *hs, const char *name)
{
    int idx = vec_name_indexof(&hs->priority, name, compare_keys);
    if(idx == -1)
        return;
    vec_name_del(&hs->priority, idx);
}

static void hstate_insert_prio(struct hstate *hs, const char *name)
{
    int idx = vec_name_indexof(&hs->priority, name, compare_keys);
    if(idx != -1)
        return;

    for(idx = 0; idx < vec_size(&hs->priority); idx++) {
        if(strcmp(vec_AT(&hs->priority, idx), name) > 0)
            break;
    }

    if(idx < vec_size(&hs->priority)) {
        if(!vec_name_resize(&hs->priority, vec_size(&hs->priority) + 1))
            return;

        memmove(hs->priority.array + idx + 1, hs->priority.array + idx, 
            sizeof(const char*) * (vec_size(&hs->priority) - idx));

        hs->priority.array[idx] = name;
        hs->priority.size++;

    }else{
        vec_name_push(&hs->priority, name);
    }
}

static bool valid_storage_site_dropoff(uint32_t curr, void *arg)
{
    struct searcharg *sarg = arg;
    struct hstate *hs = hstate_get(sarg->ent);
    uint32_t curr_flags = G_FlagsGet(curr);

    if(!(curr_flags & ENTITY_FLAG_STORAGE_SITE))
        return false;
    if(G_GetFactionID(sarg->ent) != G_GetFactionID(curr))
        return false;

    int stored = G_StorageSite_GetCurr(curr, sarg->rname);
    int cap = ss_capacity(curr, sarg->rname);

    if(cap == 0)
        return false;
    if(stored == cap)
        return false;

    return true;
}

static bool valid_storage_site_source(uint32_t curr, void *arg)
{
    struct searcharg *sarg = arg;
    struct hstate *hs = hstate_get(sarg->ent);
    uint32_t curr_flags = G_FlagsGet(curr);

    if(!(curr_flags & ENTITY_FLAG_STORAGE_SITE))
        return false;
    if(G_GetFactionID(sarg->ent) != G_GetFactionID(curr))
        return false;
    if(curr == sarg->ent)
        return false;
    if(G_StorageSite_GetDoNotTake(curr))
        return false;

    /* Don't get resources from build sites - this prevents builders 
     * from 'stealing' resources back-and-forth from two nearby 
     * build sites */
    if((curr_flags & ENTITY_FLAG_BUILDING) && !G_Building_IsSupplied(curr))
        return false;

    int stored = G_StorageSite_GetCurr(curr, sarg->rname);
    int cap = ss_capacity(curr, sarg->rname);
    int desired = ss_desired(curr, sarg->rname);

    if(sarg->strat == TRANSPORT_STRATEGY_EXCESS && (desired >= stored))
        return false;
    if(cap == 0)
        return false;
    if(stored == 0)
        return false;

    return true;
}

static bool valid_resource(uint32_t uid, void *arg)
{
    const char *name = arg;
    uint32_t flags = G_FlagsGet(uid);

    if(!(flags & ENTITY_FLAG_RESOURCE))
        return false;
    if((flags & ENTITY_FLAG_BUILDING) && !G_Building_IsCompleted(uid))
        return false;
    if(strcmp(name, G_Resource_GetName(uid)))
        return false;
    return true;
}

uint32_t nearest_storage_site_dropoff(uint32_t uid, const char *rname)
{
    vec2_t pos = G_Pos_GetXZ(uid);
    struct searcharg arg = (struct searcharg){uid, rname};
    return G_Pos_NearestWithPred(pos, valid_storage_site_dropoff, (void*)&arg, 0.0f);
}

uint32_t nearest_storage_site_source(uint32_t uid, const char *rname, enum tstrategy strat)
{
    vec2_t pos = G_Pos_GetXZ(uid);
    struct searcharg arg = (struct searcharg){uid, rname, strat};
    uint32_t ret = G_Pos_NearestWithPred(pos, valid_storage_site_source, (void*)&arg, 0.0f);

    if((ret == NULL_UID) && (strat == TRANSPORT_STRATEGY_EXCESS)) {
        arg = (struct searcharg){uid, rname, TRANSPORT_STRATEGY_NEAREST};
        ret = G_Pos_NearestWithPred(pos, valid_storage_site_source, (void*)&arg, 0.0f);
    }
    return ret;
}

uint32_t nearest_resource(uint32_t uid, const char *name)
{
    vec2_t pos = G_Pos_GetXZ(uid);
    return G_Pos_NearestWithPred(pos, valid_resource, (void*)name, REACQUIRE_RADIUS);
}

static void finish_harvesting(struct hstate *hs, uint32_t uid)
{
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished_source);
    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin_harvest);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_resource);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_resource_source);

    hs->state = STATE_NOT_HARVESTING;
    hs->res_uid = NULL_UID;
    hs->res_last_pos = (vec2_t){0};
    hs->res_name = NULL;
    hs->accum = 0.0f;

    E_Entity_Notify(EVENT_HARVEST_END, uid, NULL, ES_ENGINE);
}

static const char *carried_resource_name(struct hstate *hs)
{
    const char *key;
    int curr;

    kh_foreach(hs->curr_carry, key, curr, {
        if(curr > 0)
            return key;
    });
    return NULL;
}

static void entity_drop_off(uint32_t uid, uint32_t ss)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hs->state = STATE_HARVESTING_SEEK_STORAGE;
    hs->ss_uid = ss;

    E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_storage, 
        (void*)((uintptr_t)uid), G_RUNNING);
    E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
        (void*)((uintptr_t)uid), G_RUNNING);

    E_Entity_Notify(EVENT_STORAGE_TARGET_ACQUIRED, uid, (void*)(uintptr_t)ss, ES_ENGINE);
    G_Move_SetSurroundEntity(uid, ss);
}

static void entity_try_drop_off(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    if(!G_Harvester_GetCurrTotalCarry(uid)) {
        hs->state = STATE_NOT_HARVESTING;
        return;
    }

    uint32_t ss = nearest_storage_site_dropoff(uid, carried_resource_name(hs));
    if(ss == NULL_UID) {
        hs->state = STATE_NOT_HARVESTING;
    }else{
        entity_drop_off(uid, ss);
    }
}

static void entity_try_gather_nearest(uint32_t uid, const char *rname)
{
    uint32_t newtarget = nearest_resource(uid, rname);
    if(newtarget != NULL_UID) {
        G_Harvester_Gather(uid, newtarget);
    }else{
        entity_try_drop_off(uid);
    }
}

static void entity_try_retarget(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    const char *rname = hs->res_name;

    finish_harvesting(hs, uid);
    entity_try_gather_nearest(uid, rname);
}

uint32_t target_resource(struct hstate *hs, const char *rname)
{
    bool resource = false;
    if((hs->res_uid != NULL_UID) && !(G_FlagsGet(hs->res_uid) & ENTITY_FLAG_ZOMBIE)) {
        resource = true;
    }
    if(!resource) {
        return G_Pos_NearestWithPred(hs->res_last_pos, valid_resource, 
            (void*)rname, REACQUIRE_RADIUS);
    }
    return hs->res_uid;
}

static void clear_queued_command(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    int cmd = hs->queued.cmd;
    uint32_t arg = hs->queued.uid_arg;
    hs->queued.cmd = CMD_NONE;

    switch(cmd) {
    case CMD_GATHER: {
        if(G_EntityExists(arg)) {
            G_Harvester_Gather(uid, arg);
        }
        break;
    }
    case CMD_TRANSPORT: {
        if(G_EntityExists(arg)) {
            G_Harvester_Transport(uid, arg);
        }
        break;
    }
    case CMD_BUILD: {
        if((G_FlagsGet(uid) & ENTITY_FLAG_BUILDER) && G_EntityExists(arg)) {
            G_Builder_Build(uid, arg);
        }
        break;
    }
    case CMD_SUPPLY: {
        if(G_EntityExists(arg)) {
            G_Harvester_SupplyBuilding(uid, arg);
        }
        break;
    }
    default:
        break;
    }
    hs->drop_off_only = false;
}

uint32_t transport_source(uint32_t uid, uint32_t storage, const char *rname)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return nearest_storage_site_source(storage, rname, hs->strategy);
}

static void finish_transporing(struct hstate *hs)
{
    hs->transport_dest_uid = NULL_UID;
    hs->transport_src_uid = NULL_UID;
    hs->res_name = NULL;
    hs->state = STATE_NOT_HARVESTING;
}

static void on_harvest_anim_finished(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    if(!G_EntityExists(hs->res_uid) || (G_FlagsGet(hs->res_uid) & ENTITY_FLAG_ZOMBIE)) {
        /* If the resource was exhausted, switch targets to the nearest 
         * resource of the same type */
        entity_try_retarget(uid);
        return;
    }

    const char *rname = G_Resource_GetName(hs->res_uid);
    int resource_left = G_Resource_GetAmount(hs->res_uid);

    float gather_speed = G_Harvester_GetGatherSpeed(uid, rname);
    int gathered = hs->accum + gather_speed;
    hs->accum += gather_speed;

    if(gathered == 0) {
        return;
    }
    hs->accum = 0;

    int old_carry = G_Harvester_GetCurrCarry(uid, rname);
    int max_carry = G_Harvester_GetMaxCarry(uid, rname);

    int new_carry = MIN(max_carry, old_carry + MIN(gathered, resource_left));
    resource_left = MAX(0, resource_left - (new_carry - old_carry));

    G_Resource_SetAmount(hs->res_uid, resource_left);
    G_Harvester_SetCurrCarry(uid, rname, new_carry);

    if(resource_left == 0) {

        E_Entity_Notify(EVENT_RESOURCE_EXHAUSTED, hs->res_uid, NULL, ES_ENGINE);
        G_Zombiefy(hs->res_uid, true);

        if(new_carry < max_carry) {
            entity_try_retarget(uid);
            return;
        }
    }

    /* Bring the resource to the nearest storage site, if possible */
    if(new_carry == max_carry) {

        E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished);
        E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin_harvest);

        E_Entity_Notify(EVENT_HARVEST_END, uid, NULL, ES_ENGINE);
        entity_try_drop_off(uid);
    }
}

static void on_motion_begin_harvest(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct hstate *hs = hstate_get(uid);

    assert(hs);
    assert(hs->state == STATE_HARVESTING
        || hs->state == STATE_TRANSPORT_HARVESTING);

    finish_harvesting(hs, uid);
}

static void on_motion_begin_travel(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct hstate *hs = hstate_get(uid);

    assert(hs);
    assert(hs->state == STATE_HARVESTING_SEEK_RESOURCE
        || hs->state == STATE_HARVESTING_SEEK_STORAGE
        || hs->state == STATE_TRANSPORT_GETTING
        || hs->state == STATE_TRANSPORT_PUTTING
        || hs->state == STATE_TRANSPORT_SEEK_RESOURCE);

    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished_source);
    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin_harvest);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_resource);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_resource_source);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_storage);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_source);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_dest);
}

static void on_arrive_at_resource(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;

    if(!G_Move_Still(uid)) {
        return; 
    }

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_resource);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel);

    struct hstate *hs = hstate_get(uid);
    assert(hs);
    assert(hs->res_uid != NULL_UID);

    if(!G_EntityExists(hs->res_uid)
    || (G_FlagsGet(hs->res_uid) & ENTITY_FLAG_ZOMBIE)
    || !M_NavObjAdjacent(s_map, uid, hs->res_uid)) {

        /* harvester could not reach the resource */
        entity_try_gather_nearest(uid, hs->res_name);
        return; 
    }

    if(G_Harvester_GetCurrCarry(uid, hs->res_name) == G_Harvester_GetMaxCarry(uid, hs->res_name)) {

        /* harvester cannot carry any more of the resource */
        entity_try_drop_off(uid);
        return;
    }

    E_Entity_Notify(EVENT_HARVEST_BEGIN, uid, NULL, ES_ENGINE);
    hs->state = STATE_HARVESTING; 
    E_Entity_Register(EVENT_MOTION_START, uid, on_motion_begin_harvest, (void*)((uintptr_t)uid), G_RUNNING);
    E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished, 
        (void*)((uintptr_t)uid), G_RUNNING);
}

static void on_arrive_at_storage(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;

    if(!G_Move_Still(uid)) {
        return; 
    }

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_storage);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel);

    struct hstate *hs = hstate_get(uid);
    assert(hs);
    assert(hs->ss_uid != NULL_UID);

    if(!G_EntityExists(hs->ss_uid)
    || !(G_FlagsGet(hs->ss_uid) & ENTITY_FLAG_STORAGE_SITE)
    || !M_NavObjAdjacent(s_map, uid, hs->ss_uid)) {
        /* harvester could not reach the storage site */
        entity_try_drop_off(uid);
        return; 
    }

    const char *rname = carried_resource_name(hs);
    assert(rname);

    int carry = G_Harvester_GetCurrCarry(uid, rname);
    int cap = ss_capacity(hs->ss_uid, rname);
    int curr = G_StorageSite_GetCurr(hs->ss_uid, rname);
    int left = cap - curr;

    if(left > 0) {
        E_Entity_Notify(EVENT_RESOURCE_DROPPED_OFF, uid, NULL, ES_ENGINE);
    }

    if(left >= carry) {
        G_Harvester_SetCurrCarry(uid, rname, 0);
        G_StorageSite_SetCurr(hs->ss_uid, rname, curr + carry);

        uint32_t resource = target_resource(hs, rname);
        if(G_EntityExists(resource) && !hs->drop_off_only) {
            G_Harvester_Gather(uid, resource);
        }else{
            hs->state = STATE_NOT_HARVESTING;
            hs->ss_uid = NULL_UID;
            hs->res_name = NULL;
            clear_queued_command(uid);
        }

    }else{

        G_Harvester_SetCurrCarry(uid, rname, carry - left);
        G_StorageSite_SetCurr(hs->ss_uid, rname, cap);

        entity_try_drop_off(uid);
    }
}

static void on_arrive_at_transport_source(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;

    if(!G_Move_Still(uid))
        return; 

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_source);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel);

    struct hstate *hs = hstate_get(uid);
    assert(hs);

    if(!G_EntityExists(hs->transport_src_uid)
    || !(G_FlagsGet(hs->transport_src_uid) & ENTITY_FLAG_STORAGE_SITE)
    || !M_NavObjAdjacent(s_map, uid, hs->transport_src_uid)
    || G_StorageSite_GetDoNotTake(hs->transport_src_uid)) {

        /* harvester could not reach the storage site, 
         * or the 'do not take' option for it was set */
        finish_transporing(hs);

        /* If the destination is still there, re-try */
        if(G_EntityExists(hs->transport_dest_uid)) {
            G_Harvester_Transport(uid, hs->transport_dest_uid);
        }
        return;
    }

    int store_cap = ss_capacity(hs->transport_src_uid, hs->res_name);
    int store_curr = G_StorageSite_GetCurr(hs->transport_src_uid, hs->res_name);
    int desired = ss_desired(hs->transport_src_uid, hs->res_name);
    int excess = store_curr - desired;

    int max_carry = G_Harvester_GetMaxCarry(uid, hs->res_name);
    int curr_carry = G_Harvester_GetCurrCarry(uid, hs->res_name);
    int carry_cap = max_carry - curr_carry;

    /* Figure out how much resource to take. Add it to our carry .
     */
    int take = 0;
    switch(hs->strategy) {
    case TRANSPORT_STRATEGY_EXCESS: {
        /* If there are absolutely no valid storage sites which have excess 
         * of our target resource, then we are allowed to overstep the 'desired' 
         * limit */
        if(hs->transport_src_uid == nearest_storage_site_source(uid, hs->res_name, hs->strategy)) {
            take = MIN(carry_cap, store_curr);
        }else{
            take = MAX(MIN(carry_cap, excess), 0);
        }
        break;
    }
    case TRANSPORT_STRATEGY_GATHERING: /* fallthrough */
    case TRANSPORT_STRATEGY_NEAREST: {
        take = MIN(carry_cap, store_curr);
        break;
    }
    default: assert(0);
    }

    G_Harvester_SetCurrCarry(uid, hs->res_name, curr_carry + take);
    G_StorageSite_SetCurr(hs->transport_src_uid, hs->res_name, store_curr - take);

    if(take > 0) {
        E_Entity_Notify(EVENT_RESOURCE_PICKED_UP, uid, NULL, ES_ENGINE);
    }

    if(!G_EntityExists(hs->transport_dest_uid)) {

        hs->transport_dest_uid = NULL_UID;
        hs->transport_src_uid = NULL_UID;
        hs->res_name = NULL;
        hs->state = STATE_NOT_HARVESTING;
        return;
    }

    /* If we're under our max carry, search for another eligible storage site
     * where we can get the rest of the desired resource. Make a stop there 
     * as well.
     */
    if(curr_carry + take < max_carry) {
    
        uint32_t newsrc = transport_source(uid, hs->transport_dest_uid, hs->res_name);
        if(G_EntityExists(newsrc)) {
            E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_transport_source, 
                (void*)((uintptr_t)uid), G_RUNNING);
            E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
                (void*)((uintptr_t)uid), G_RUNNING);

            G_Move_SetSurroundEntity(uid, newsrc);
            hs->transport_src_uid = newsrc;
            return;
        }
    }

    if(curr_carry + take == 0) {
    
        hs->transport_dest_uid = NULL_UID;
        hs->transport_src_uid = NULL_UID;
        hs->res_name = NULL;
        hs->state = STATE_NOT_HARVESTING;
        return;
    }

    /* Lastly, drop off our stuff at the destination
     */
    E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_transport_dest, 
        (void*)((uintptr_t)uid), G_RUNNING);
    E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
        (void*)((uintptr_t)uid), G_RUNNING);

    G_Move_SetSurroundEntity(uid, hs->transport_dest_uid);
    hs->state = STATE_TRANSPORT_PUTTING;
    E_Entity_Notify(EVENT_TRANSPORT_TARGET_ACQUIRED, uid, 
        (void*)(uintptr_t)hs->transport_dest_uid, ES_ENGINE);
}

static void on_arrive_at_transport_dest(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;

    if(!G_Move_Still(uid))
        return; 

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_dest);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel);

    struct hstate *hs = hstate_get(uid);
    assert(hs);

    if(!G_EntityExists(hs->transport_dest_uid)
    || !(G_FlagsGet(hs->transport_dest_uid) & ENTITY_FLAG_STORAGE_SITE)
    || !M_NavObjAdjacent(s_map, uid, hs->transport_dest_uid)) {
        /* harvester could not reach the destination storage site */
        finish_transporing(hs);
        return;
    }

    const char *rname = carried_resource_name(hs);
    assert(rname);

    int carry = G_Harvester_GetCurrCarry(uid, rname);
    int cap = ss_capacity(hs->transport_dest_uid, rname);
    int curr = G_StorageSite_GetCurr(hs->transport_dest_uid, rname);
    int left = cap - curr;

    if(left > 0) {
        E_Entity_Notify(EVENT_RESOURCE_DROPPED_OFF, uid, NULL, ES_ENGINE);
    }

    if(left >= carry) {
        G_Harvester_SetCurrCarry(uid, rname, 0);
        G_StorageSite_SetCurr(hs->transport_dest_uid, rname, curr + carry);

    }else{

        G_Harvester_SetCurrCarry(uid, rname, carry - left);
        G_StorageSite_SetCurr(hs->transport_dest_uid, rname, cap);
    }

    finish_transporing(hs);
    G_Harvester_Transport(uid, hs->transport_dest_uid);
}

static void selection_try_order_gather(bool targeting)
{
    if(!targeting && G_CurrContextualAction() != CTX_ACTION_GATHER)
        return;

    uint32_t target = G_Sel_GetHovered();
    if(!G_EntityExists(target) || !(G_FlagsGet(target) & ENTITY_FLAG_RESOURCE))
        return;
    if((G_FlagsGet(target) & ENTITY_FLAG_BUILDING) && !G_Building_IsCompleted(target))
        return;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);
    size_t ngather = 0;
    const char *rname = G_Resource_GetName(target);

    if(sel_type != SELECTION_TYPE_PLAYER)
        return;

    for(int i = 0; i < vec_size(sel); i++) {

        uint32_t curr = vec_AT(sel, i);
        uint32_t flags = G_FlagsGet(curr);

        if(!(flags & ENTITY_FLAG_HARVESTER))
            continue;

        if(G_Harvester_GetMaxCarry(curr, rname) == 0
        || G_Harvester_GetGatherSpeed(curr, rname) == 0.0f)
            continue;

        struct hstate *hs = hstate_get(curr);
        assert(hs);

        G_StopEntity(curr, true);
        G_Harvester_Gather(curr, target);
        G_NotifyOrderIssued(curr);
        ngather++;
    }

    if(ngather) {
        Entity_Ping(target);
    }
}

static void selection_try_order_pick_up(bool targeting)
{
    if(!targeting && (G_CurrContextualAction() == CTX_ACTION_TRANSPORT))
        return;

    uint32_t target = G_Sel_GetHovered();
    if(!G_EntityExists(target) || !(G_FlagsGet(target) & ENTITY_FLAG_STORAGE_SITE))
        return;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);
    size_t ncarry = 0;

    if(sel_type != SELECTION_TYPE_PLAYER)
        return;

    for(int i = 0; i < vec_size(sel); i++) {

        uint32_t curr = vec_AT(sel, i);
        uint32_t flags = G_FlagsGet(curr);

        if(!(flags & ENTITY_FLAG_HARVESTER))
            continue;

        struct hstate *hs = hstate_get(curr);
        assert(hs);

        if(G_Harvester_GetCurrTotalCarry(curr) > 0)
            continue;

        G_StopEntity(curr, true);
        G_Harvester_PickUp(curr, target);
        G_NotifyOrderIssued(curr);
        ncarry++;
    }

    if(ncarry) {
        Entity_Ping(target);
    }
}

static void selection_try_order_drop_off(bool targeting)
{
    if(!targeting && G_CurrContextualAction() != CTX_ACTION_DROP_OFF)
        return;

    uint32_t target = G_Sel_GetHovered();
    if(!G_EntityExists(target) || !(G_FlagsGet(target) & ENTITY_FLAG_STORAGE_SITE))
        return;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);
    size_t ncarry = 0;

    if(sel_type != SELECTION_TYPE_PLAYER)
        return;

    for(int i = 0; i < vec_size(sel); i++) {

        uint32_t curr = vec_AT(sel, i);
        uint32_t flags = G_FlagsGet(curr);

        if(!(flags & ENTITY_FLAG_HARVESTER))
            continue;

        struct hstate *hs = hstate_get(curr);
        assert(hs);

        if(G_Harvester_GetCurrTotalCarry(curr) == 0)
            continue;

        G_StopEntity(curr, true);
        G_Harvester_DropOff(curr, target);
        G_NotifyOrderIssued(curr);
        ncarry++;
    }

    if(ncarry) {
        Entity_Ping(target);
    }
}

static void selection_try_order_transport(bool targeting)
{
    if(!targeting && (G_CurrContextualAction() != CTX_ACTION_TRANSPORT))
        return;

    uint32_t target = G_Sel_GetHovered();
    if(!G_EntityExists(target) || !(G_FlagsGet(target) & ENTITY_FLAG_STORAGE_SITE))
        return;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);
    size_t ntransport = 0;

    if(sel_type != SELECTION_TYPE_PLAYER)
        return;

    for(int i = 0; i < vec_size(sel); i++) {

        uint32_t curr = vec_AT(sel, i);
        uint32_t flags = G_FlagsGet(curr);

        if(!(flags & ENTITY_FLAG_HARVESTER))
            continue;

        struct hstate *hs = hstate_get(curr);
        assert(hs);

        G_StopEntity(curr, true);
        G_Harvester_Transport(curr, target);
        G_NotifyOrderIssued(curr);
        ntransport++;
    }

    if(ntransport) {
        Entity_Ping(target);
    }
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    bool targeting = G_Harvester_InTargetMode();
    bool right = (mouse_event->button == SDL_BUTTON_RIGHT);
    bool left = (mouse_event->button == SDL_BUTTON_LEFT);

    bool gather = s_gather_on_lclick;
    bool pickup = s_pick_up_on_lclick;
    bool dropoff = s_drop_off_on_lclick;
    bool transport = s_transport_on_lclick;

    s_gather_on_lclick = false;
    s_pick_up_on_lclick = false;
    s_drop_off_on_lclick = false;
    s_transport_on_lclick = false;

    if(G_MouseOverMinimap())
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    if(right && targeting)
        return;

    if(left && !targeting)
        return;

    int action = G_CurrContextualAction();

    if(right 
    && (action != CTX_ACTION_GATHER) 
    && (action != CTX_ACTION_DROP_OFF) 
    && (action != CTX_ACTION_TRANSPORT))
        return;

    if(right || (left && gather)) {
        selection_try_order_gather(targeting);
    }
    if(right || (left && pickup)) {
        selection_try_order_pick_up(targeting);
    }
    if(right || (left && dropoff)) {
        selection_try_order_drop_off(targeting);
    }
    if(right || (left && transport)) {
        selection_try_order_transport(targeting);
    }
}

static const char *transport_resource(struct hstate *hs, uint32_t target)
{
    const char *ret = NULL;
    for(int i = 0; i < vec_size(&hs->priority); i++) {

        const char *rname = vec_AT(&hs->priority, i);
        int desired = ss_desired(target, rname);
        int stored = G_StorageSite_GetCurr(target, rname);

        if(desired > stored) {
            ret = rname;
            break;
        }
    }
    return ret;
}

static const char *pick_up_resource(struct hstate *hs, uint32_t target)
{
    const char *ret = NULL;
    for(int i = 0; i < vec_size(&hs->priority); i++) {

        const char *rname = vec_AT(&hs->priority, i);
        int stored = G_StorageSite_GetCurr(target, rname);

        if(stored > 0) {
            ret = rname;
            break;
        }
    }
    return ret;
}

static bool harvester_can_gather(struct hstate *hs, const char *rname)
{
    float speed = 0.0f;
    hstate_get_key_float(hs->gather_speeds, rname, &speed);
    return (speed > 0.0f);
}

static void entity_try_gather_nearest_source(uint32_t harvester, 
                                             const char *rname, uint32_t dest)
{
    uint32_t newtarget = nearest_resource(harvester, rname);
    if(!G_EntityExists(newtarget)) {
        /* In case there are no more resources to gather, fall back 
         * to an alternate transporting strategy */
        G_Harvester_Transport(harvester, dest);
        return;
    }

    struct hstate *hs = hstate_get(harvester);
    assert(hs);
    hs->res_uid = newtarget;
    hs->res_last_pos = G_Pos_GetXZ(newtarget);
    hs->res_name = rname;

    if(M_NavObjAdjacent(s_map, harvester, newtarget)) {
        on_arrive_at_resource_source((void*)((uintptr_t)harvester), NULL);
    }else{
        E_Entity_Register(EVENT_MOTION_END, harvester, on_arrive_at_resource_source, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        E_Entity_Register(EVENT_ORDER_ISSUED, harvester, on_motion_begin_travel, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        G_Move_SetSurroundEntity(harvester, newtarget);
    }
}

static void on_arrive_at_resource_source(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    if(!G_Move_Still(uid)) {
        return; 
    }

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_resource_source);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel);

    if(!G_EntityExists(hs->transport_dest_uid)) {

        hs->transport_dest_uid = NULL_UID;
        hs->transport_src_uid = NULL_UID;
        hs->res_name = NULL;
        hs->state = STATE_NOT_HARVESTING;
        return;
    }

    assert(hs->res_uid != NULL_UID);

    if(!G_EntityExists(hs->res_uid)
    || (G_FlagsGet(hs->res_uid) & ENTITY_FLAG_ZOMBIE)
    || !M_NavObjAdjacent(s_map, uid, hs->res_uid)) {

        /* harvester could not reach the resource */
        entity_try_gather_nearest_source(uid, hs->res_name, hs->res_uid);
        return; 
    }

    if(G_Harvester_GetCurrCarry(uid, hs->res_name) == G_Harvester_GetMaxCarry(uid, hs->res_name)) {

        /* harvester cannot carry any more of the resource */
        E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_transport_dest, 
            (void*)((uintptr_t)uid), G_RUNNING);
        E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
            (void*)((uintptr_t)uid), G_RUNNING);

        G_Move_SetSurroundEntity(uid, hs->res_uid);
        hs->state = STATE_TRANSPORT_PUTTING;
        E_Entity_Notify(EVENT_TRANSPORT_TARGET_ACQUIRED, uid, 
            (void*)(uintptr_t)hs->transport_dest_uid, ES_ENGINE);
        return;
    }

    E_Entity_Notify(EVENT_HARVEST_BEGIN, uid, NULL, ES_ENGINE);
    hs->state = STATE_TRANSPORT_HARVESTING; 
    E_Entity_Register(EVENT_MOTION_START, uid, on_motion_begin_harvest, (void*)((uintptr_t)uid), G_RUNNING);
    E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished_source, 
        (void*)((uintptr_t)uid), G_RUNNING);
}

static void on_harvest_anim_finished_source(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    if(!G_EntityExists(hs->transport_dest_uid)) {

        finish_harvesting(hs, uid);
        hs->transport_dest_uid = NULL_UID;
        hs->transport_src_uid = NULL_UID;
        return;
    }

    if(!G_EntityExists(hs->res_uid) || (G_FlagsGet(hs->res_uid) & ENTITY_FLAG_ZOMBIE)) {
        /* If the resource was exhausted, switch targets to the nearest 
         * resource of the same type */
        const char *rname = hs->res_name;
        finish_harvesting(hs, uid);
        entity_try_gather_nearest_source(uid, rname, hs->transport_dest_uid);
        return;
    }

    const char *rname = G_Resource_GetName(hs->res_uid);
    int resource_left = G_Resource_GetAmount(hs->res_uid);

    float gather_speed = G_Harvester_GetGatherSpeed(uid, rname);
    int gathered = hs->accum + gather_speed;
    hs->accum += gather_speed;

    if(gathered == 0) {
        return;
    }
    hs->accum = 0;

    int old_carry = G_Harvester_GetCurrCarry(uid, rname);
    int max_carry = G_Harvester_GetMaxCarry(uid, rname);

    int new_carry = MIN(max_carry, old_carry + MIN(gathered, resource_left));
    resource_left = MAX(0, resource_left - (new_carry - old_carry));

    G_Resource_SetAmount(hs->res_uid, resource_left);
    G_Harvester_SetCurrCarry(uid, rname, new_carry);

    if(resource_left == 0) {

        E_Entity_Notify(EVENT_RESOURCE_EXHAUSTED, hs->res_uid, NULL, ES_ENGINE);
        G_Zombiefy(hs->res_uid, true);

        if(new_carry < max_carry) {
            const char *rname = hs->res_name;
            finish_harvesting(hs, uid);
            entity_try_gather_nearest_source(uid, rname, hs->transport_dest_uid);
            return;
        }
    }

    if(!carried_resource_name(hs)) {

        finish_harvesting(hs, uid);
        hs->transport_dest_uid = NULL_UID;
        hs->transport_src_uid = NULL_UID;
        return;
    }

    /* Lastly, drop off our stuff at the destination
     */
    if(new_carry == max_carry) {

        E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished_source);
        E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin_harvest);
        E_Entity_Notify(EVENT_HARVEST_END, uid, NULL, ES_ENGINE);
    
        E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_transport_dest, 
            (void*)((uintptr_t)uid), G_RUNNING);
        E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
            (void*)((uintptr_t)uid), G_RUNNING);

        G_Move_SetSurroundEntity(uid, hs->res_uid);
        hs->state = STATE_TRANSPORT_PUTTING;
        E_Entity_Notify(EVENT_TRANSPORT_TARGET_ACQUIRED, uid, 
            (void*)(uintptr_t)hs->transport_dest_uid, ES_ENGINE);
    }
}

static bool harvester_transport_from_resources(uint32_t harvester, uint32_t storage)
{
    struct hstate *hs = hstate_get(harvester);
    assert(hs);

    const char *rname = transport_resource(hs, storage);
    assert(rname);

    vec2_t pos = G_Pos_GetXZ(harvester);
    uint32_t resource = G_Pos_NearestWithPred(pos, valid_resource, (void*)rname, 0);
    if(resource == NULL_UID)
        return false;

    if(!harvester_can_gather(hs, rname))
        return false;

    hs->state = STATE_TRANSPORT_SEEK_RESOURCE;
    hs->transport_dest_uid = storage;
    hs->res_uid = resource;
    hs->res_last_pos = G_Pos_GetXZ(resource);
    hs->res_name = G_Resource_GetName(resource);
    E_Entity_Notify(EVENT_HARVEST_TARGET_ACQUIRED, harvester, 
        (void*)(uintptr_t)resource, ES_ENGINE);

    if(M_NavObjAdjacent(s_map, harvester, resource)) {
        on_arrive_at_resource_source((void*)((uintptr_t)harvester), NULL);
    }else{
        E_Entity_Register(EVENT_MOTION_END, harvester, on_arrive_at_resource_source, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        E_Entity_Register(EVENT_ORDER_ISSUED, harvester, on_motion_begin_travel, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        G_Move_SetSurroundEntity(harvester, resource);
    }

    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Harvester_Init(const struct map *map)
{
    mpa_buff_init(&s_mpool, 1024, 0);

    if(!mpa_buff_reserve(&s_mpool, 1024))
        goto fail_mpool; 
    if(!(s_entity_state_table = kh_init(state)))
        goto fail_table;
    if(!si_init(&s_stringpool, &s_stridx, 512))
        goto fail_strintern;

    s_map = map;
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    return true;

fail_strintern:
    kh_destroy(state, s_entity_state_table);
fail_table:
    mpa_buff_destroy(&s_mpool);
fail_mpool:
    return false;
}

void G_Harvester_Shutdown(void)
{
    s_map = NULL;
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);

    si_shutdown(&s_stringpool, s_stridx);
    kh_destroy(state, s_entity_state_table);
    mpa_buff_destroy(&s_mpool);
}

bool G_Harvester_AddEntity(uint32_t uid)
{
    struct hstate hs;
    if(!hstate_init(&hs))
        return false;
    if(!hstate_set(uid, hs))
        return false;
    return true;
}

void G_Harvester_RemoveEntity(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    if(!hs)
        return;

    G_Harvester_Stop(uid);
    hstate_destroy(hs);
    hstate_remove(uid);
}

bool G_Harvester_SetGatherSpeed(uint32_t uid, const char *rname, float speed)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return hstate_set_key_float(hs->gather_speeds, rname, speed);
}

float G_Harvester_GetGatherSpeed(uint32_t uid, const char *rname)
{
    float ret = DEFAULT_GATHER_SPEED;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hstate_get_key_float(hs->gather_speeds, rname, &ret);
    return ret;
}

bool G_Harvester_SetMaxCarry(uint32_t uid, const char *rname, int max)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return false;

    if(max == 0) {
        hstate_remove_prio(hs, key);
    }else{
        hstate_insert_prio(hs, key);
    }
    return hstate_set_key_int(hs->max_carry, rname, max);
}

int G_Harvester_GetMaxCarry(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_MAX_CARRY;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hstate_get_key_int(hs->max_carry, rname, &ret);
    return ret;
}

bool G_Harvester_SetCurrCarry(uint32_t uid, const char *rname, int curr)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return hstate_set_key_int(hs->curr_carry, rname, curr);
}

int G_Harvester_GetCurrCarry(uint32_t uid, const char *rname)
{
    int ret = 0;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hstate_get_key_int(hs->curr_carry, rname, &ret);
    return ret;
}

void G_Harvester_ClearCurrCarry(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    kh_clear(int, hs->curr_carry);

    if(hs->state == STATE_HARVESTING_SEEK_STORAGE 
    || hs->state == STATE_TRANSPORT_PUTTING) {

        E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_storage);
        E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_dest);
        hs->state = STATE_NOT_HARVESTING;
    }
}

void G_Harvester_SetStrategy(uint32_t uid, enum tstrategy strat)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    hs->strategy = strat;
}

int G_Harvester_GetStrategy(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return hs->strategy;
}

bool G_Harvester_IncreaseTransportPrio(uint32_t uid, const char *rname)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return false;

    int idx = vec_name_indexof(&hs->priority, key, compare_keys);
    if(idx == -1)
        return false;
    if(idx == 0)
        return false;

    const char *tmp = vec_AT(&hs->priority, idx - 1);
    vec_AT(&hs->priority, idx - 1) = key;
    vec_AT(&hs->priority, idx) = tmp;

    return true;
}

bool G_Harvester_DecreaseTransportPrio(uint32_t uid, const char *rname)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return false;

    int idx = vec_name_indexof(&hs->priority, key, compare_keys);
    if(idx == -1)
        return false;
    if(idx == vec_size(&hs->priority) - 1)
        return false;

    const char *tmp = vec_AT(&hs->priority, idx + 1);
    vec_AT(&hs->priority, idx + 1) = key;
    vec_AT(&hs->priority, idx) = tmp;

    return true;
}

int G_Harvester_GetTransportPrio(uint32_t uid, size_t maxout, const char *out[])
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    size_t ret = MIN(maxout, vec_size(&hs->priority));

    memcpy(out, hs->priority.array, ret * sizeof(const char*));
    return ret;
}

int G_Harvester_GetCurrTotalCarry(uint32_t uid)
{
    int ret = 0;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    const char *key;
    (void)key;
    int curr;

    kh_foreach(hs->curr_carry, key, curr, {
        ret += curr;
    });

    return ret;
}

void G_Harvester_SetGatherOnLeftClick(void)
{
    s_gather_on_lclick  = true;
    s_pick_up_on_lclick = false;
    s_drop_off_on_lclick = false;
    s_transport_on_lclick = false;
}

void G_Harvester_SetDropOffOnLeftClick(void)
{
    s_gather_on_lclick  = false;
    s_pick_up_on_lclick = false;
    s_drop_off_on_lclick = true;
    s_transport_on_lclick = false;
}

void G_Harvester_SetTransportOnLeftClick(void)
{
    s_gather_on_lclick  = false;
    s_pick_up_on_lclick = false;
    s_drop_off_on_lclick = false;
    s_transport_on_lclick = true;
}

void G_Harvester_SetPickUpOnLeftClick(void)
{
    s_gather_on_lclick  = false;
    s_pick_up_on_lclick = true;
    s_drop_off_on_lclick = false;
    s_transport_on_lclick = false;
}

bool G_Harvester_Gather(uint32_t harvester, uint32_t resource)
{
    struct hstate *hs = hstate_get(harvester);
    assert(hs);

    uint32_t resource_flags = G_FlagsGet(resource);
    if(!(resource_flags & ENTITY_FLAG_RESOURCE))
        return false;
    if((resource_flags & ENTITY_FLAG_BUILDING) && !G_Building_IsCompleted(resource))
        return false;

    if(G_Harvester_GetCurrTotalCarry(harvester)) {

        const char *carryname = carried_resource_name(hs);
        if(strcmp(carryname, G_Resource_GetName(resource))) {
        
            uint32_t target = nearest_storage_site_dropoff(harvester, carryname);
            if(!target)
                return false;

            hs->queued.cmd = CMD_GATHER;
            hs->queued.uid_arg = resource;

            G_Harvester_DropOff(harvester, target);
            return true;
        }
    }

    hs->state = STATE_HARVESTING_SEEK_RESOURCE;
    hs->res_uid = resource;
    hs->res_last_pos = G_Pos_GetXZ(resource);
    hs->res_name = G_Resource_GetName(resource);
    E_Entity_Notify(EVENT_HARVEST_TARGET_ACQUIRED, harvester, 
        (void*)(uintptr_t)resource, ES_ENGINE);

    if(M_NavObjAdjacent(s_map, harvester, resource)) {
        on_arrive_at_resource((void*)((uintptr_t)harvester), NULL);
    }else{
        E_Entity_Register(EVENT_MOTION_END, harvester, on_arrive_at_resource, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        E_Entity_Register(EVENT_ORDER_ISSUED, harvester, on_motion_begin_travel, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        G_Move_SetSurroundEntity(harvester, resource);
    }

    return true;
}

bool G_Harvester_DropOff(uint32_t harvester, uint32_t storage)
{
    struct hstate *hs = hstate_get(harvester);
    assert(hs);

    uint32_t storage_flags = G_FlagsGet(storage);
    if(!(storage_flags & ENTITY_FLAG_STORAGE_SITE))
        return false;

    if(G_Harvester_GetCurrTotalCarry(harvester) == 0)
        return true;

    G_Harvester_Stop(harvester);
    hs->drop_off_only = true;
    entity_drop_off(harvester, storage);
    return true;
}

bool G_Harvester_PickUp(uint32_t harvester, uint32_t storage)
{
    struct hstate *hs = hstate_get(harvester);
    assert(hs);

    uint32_t storage_flags = G_FlagsGet(storage);
    if(!(storage_flags & ENTITY_FLAG_STORAGE_SITE))
        return false;

    if(G_Harvester_GetCurrTotalCarry(harvester) > 0)
        return true;

    const char *rname = pick_up_resource(hs, storage);
    if(!rname)
        return false;

    G_Harvester_Stop(harvester);
    hs->state = STATE_TRANSPORT_GETTING;
    hs->transport_dest_uid = NULL_UID;
    hs->transport_src_uid = storage;
    hs->res_name = rname;
    E_Entity_Notify(EVENT_TRANSPORT_TARGET_ACQUIRED, harvester, 
        (void*)(uintptr_t)storage, ES_ENGINE);

    if(M_NavObjAdjacent(s_map, harvester, storage)) {
        on_arrive_at_transport_source((void*)((uintptr_t)harvester), NULL);
    }else{
        E_Entity_Register(EVENT_MOTION_END, harvester, on_arrive_at_transport_source, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        E_Entity_Register(EVENT_ORDER_ISSUED, harvester, on_motion_begin_travel, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        G_Move_SetSurroundEntity(harvester, storage);
    }

    return true;
}

bool G_Harvester_Transport(uint32_t harvester, uint32_t storage)
{
    struct hstate *hs = hstate_get(harvester);
    assert(hs);

    uint32_t storage_flags = G_FlagsGet(storage);
    if(!(storage_flags & ENTITY_FLAG_STORAGE_SITE))
        return false;

    if(hs->queued.cmd != CMD_NONE) {
        clear_queued_command(harvester);
        return false;
    }

    if(G_Harvester_GetCurrTotalCarry(harvester)) {

        const char *carryname = carried_resource_name(hs);
        if(!G_StorageSite_Desires(storage, carryname)) {
            storage = nearest_storage_site_dropoff(harvester, carryname);
        }

        if(storage == NULL_UID)
            return false;

        G_Harvester_DropOff(harvester, storage);
        hs->queued.cmd = CMD_TRANSPORT;
        hs->queued.uid_arg = storage;

        return true;
    }

    const char *rname = transport_resource(hs, storage);
    if(!rname)
        return false;

    if(hs->strategy == TRANSPORT_STRATEGY_GATHERING
    && harvester_transport_from_resources(harvester, storage)) {
        return true;
    }

    uint32_t src = transport_source(harvester, storage, rname);
    if(src == NULL_UID)
        return false;

    G_Harvester_Stop(harvester);
    hs->state = STATE_TRANSPORT_GETTING;
    hs->transport_dest_uid = storage;
    hs->transport_src_uid = src;
    hs->res_name = rname;
    E_Entity_Notify(EVENT_TRANSPORT_TARGET_ACQUIRED, harvester, 
        (void*)(uintptr_t)storage, ES_ENGINE);

    if(M_NavObjAdjacent(s_map, harvester, src)) {
        on_arrive_at_transport_source((void*)((uintptr_t)harvester), NULL);
    }else{
        E_Entity_Register(EVENT_MOTION_END, harvester, on_arrive_at_transport_source, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        E_Entity_Register(EVENT_ORDER_ISSUED, harvester, on_motion_begin_travel, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        G_Move_SetSurroundEntity(harvester, src);
    }

    return true;
}

bool G_Harvester_SupplyBuilding(uint32_t harvester, uint32_t building)
{
    struct hstate *hs = hstate_get(harvester);
    assert(hs);

    if(G_Harvester_GetCurrTotalCarry(harvester)
    && !G_StorageSite_Desires(building, carried_resource_name(hs))) {

        const char *carryname = carried_resource_name(hs);
        uint32_t nearest = nearest_storage_site_dropoff(harvester, carryname);

        if(nearest == NULL_UID)
            return false;

        hs->queued.cmd = CMD_SUPPLY;
        hs->queued.uid_arg = building;

        G_Harvester_DropOff(harvester, nearest);
        return true;
    }

    if(!G_Harvester_Transport(harvester, building)) {
        return false;
    }

    hs->queued.cmd = CMD_BUILD;
    hs->queued.uid_arg = building;
    return true;
}

void G_Harvester_Stop(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    if(hs->state == STATE_HARVESTING) {
        E_Entity_Notify(EVENT_HARVEST_END, uid, NULL, ES_ENGINE);
    }
    hs->state = STATE_NOT_HARVESTING;

    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished_source);
    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin_harvest);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_resource);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_storage);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_source);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_resource_source);
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_dest);
}

void G_Harvester_ClearQueuedCmd(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hs->queued.cmd = CMD_NONE;
    hs->queued.uid_arg = NULL_UID;
}

bool G_Harvester_InTargetMode(void)
{
    return s_gather_on_lclick 
        || s_pick_up_on_lclick
        || s_drop_off_on_lclick 
        || s_transport_on_lclick;
}

bool G_Harvester_HasRightClickAction(void)
{
    uint32_t hovered = G_Sel_GetHovered();
    if(!G_EntityExists(hovered))
        return false;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);

    if(vec_size(sel) == 0)
        return false;

    uint32_t first = vec_AT(sel, 0);
    if((G_FlagsGet(first) & ENTITY_FLAG_HARVESTER) && (G_FlagsGet(hovered) & ENTITY_FLAG_RESOURCE))
        return true;

    return false;
}

int G_Harvester_CurrContextualAction(void)
{
    uint32_t hovered = G_Sel_GetHovered();
    if(!G_EntityExists(hovered))
        return CTX_ACTION_NONE;

    if(M_MouseOverMinimap(s_map))
        return CTX_ACTION_NONE;

    if((G_FlagsGet(hovered) & ENTITY_FLAG_BUILDING)
    && !G_Building_IsFounded(hovered))
        return CTX_ACTION_NONE;

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    if(S_UI_MouseOverWindow(mouse_x, mouse_y))
        return CTX_ACTION_NONE;

    if(G_Harvester_InTargetMode())
        return CTX_ACTION_NONE;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);

    if(vec_size(sel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return CTX_ACTION_NONE;

    uint32_t first = vec_AT(sel, 0);
    if(!(G_FlagsGet(first) & ENTITY_FLAG_HARVESTER))
        return CTX_ACTION_NONE;

    if(G_FlagsGet(hovered) & ENTITY_FLAG_RESOURCE
    && G_Harvester_GetGatherSpeed(first, G_Resource_GetName(hovered)) > 0)
        return CTX_ACTION_GATHER;

    if(G_GetFactionID(hovered) != G_GetFactionID(first))
        return false;

    if(G_FlagsGet(hovered) & ENTITY_FLAG_STORAGE_SITE
    && G_Harvester_GetCurrTotalCarry(first) > 0)
        return CTX_ACTION_DROP_OFF;

    if(G_FlagsGet(hovered) & ENTITY_FLAG_STORAGE_SITE)
        return CTX_ACTION_TRANSPORT;

    return CTX_ACTION_NONE;
}

bool G_Harvester_GetContextualCursor(char *out, size_t maxout)
{
    uint32_t hovered = G_Sel_GetHovered();
    if(!G_EntityExists(hovered))
        return false;

    if(!(G_FlagsGet(hovered) & ENTITY_FLAG_RESOURCE))
        return false;

    const char *name = G_Resource_GetCursor(hovered);
    pf_strlcpy(out, name, maxout);
    return true;
}

bool G_Harvester_SaveState(struct SDL_RWops *stream)
{
    struct attr num_ents = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_entity_state_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_ents, "num_ents"));
    Sched_TryYield();

    uint32_t key;
    struct hstate curr;

    kh_foreach(s_entity_state_table, key, curr, {

        struct attr uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = key
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid, "uid"));

        struct attr state = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.state
        };
        CHK_TRUE_RET(Attr_Write(stream, &state, "state"));

        struct attr strategy = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.strategy
        };
        CHK_TRUE_RET(Attr_Write(stream, &strategy, "strategy"));

        struct attr ss_uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.ss_uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &ss_uid, "ss_uid"));
        Sched_TryYield();

        struct attr res_uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.res_uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &res_uid, "res_uid"));

        struct attr res_last_pos = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.res_last_pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &res_last_pos, "res_last_pos"));

        struct attr has_res_name = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool  = curr.res_name != NULL
        };
        CHK_TRUE_RET(Attr_Write(stream, &has_res_name, "has_res_name"));

        if(has_res_name.val.as_bool) {
        
            struct attr res_name = (struct attr){ .type = TYPE_STRING, };
            pf_strlcpy(res_name.val.as_string, curr.res_name, sizeof(res_name.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &res_name, "res_name"));
        }

        struct attr num_speeds = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr.gather_speeds)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_speeds, "num_speeds"));

        const char *speed_key;
        float speed_amount;
        kh_foreach(curr.gather_speeds, speed_key, speed_amount, {
        
            struct attr speed_key_attr = (struct attr){ .type = TYPE_STRING, };
            pf_strlcpy(speed_key_attr.val.as_string, speed_key, sizeof(speed_key_attr.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &speed_key_attr, "speed_key"));

            struct attr speed_amount_attr = (struct attr){
                .type = TYPE_FLOAT,
                .val.as_float = speed_amount
            };
            CHK_TRUE_RET(Attr_Write(stream, &speed_amount_attr, "speed_amount"));
        });
        Sched_TryYield();

        struct attr num_max = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr.max_carry)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_max, "num_max"));

        const char *max_key;
        int max_amount;
        kh_foreach(curr.max_carry, max_key, max_amount, {
        
            struct attr max_key_attr = (struct attr){ .type = TYPE_STRING, };
            pf_strlcpy(max_key_attr.val.as_string, max_key, sizeof(max_key_attr.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &max_key_attr, "max_key"));

            struct attr max_amount_attr = (struct attr){
                .type = TYPE_INT,
                .val.as_int = max_amount
            };
            CHK_TRUE_RET(Attr_Write(stream, &max_amount_attr, "max_amount"));
        });
        Sched_TryYield();

        struct attr num_carry = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr.curr_carry)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_carry, "num_carry"));

        const char *curr_key;
        int curr_amount;
        kh_foreach(curr.curr_carry, curr_key, curr_amount, {
        
            struct attr curr_key_attr = (struct attr){ .type = TYPE_STRING, };
            pf_strlcpy(curr_key_attr.val.as_string, curr_key, sizeof(curr_key_attr.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &curr_key_attr, "curr_key"));

            struct attr curr_amount_attr = (struct attr){
                .type = TYPE_INT,
                .val.as_int = curr_amount
            };
            CHK_TRUE_RET(Attr_Write(stream, &curr_amount_attr, "curr_amount"));
        });
        Sched_TryYield();

        struct attr num_priorities = (struct attr){
            .type = TYPE_INT,
            .val.as_int = vec_size(&curr.priority)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_priorities, "num_priorities"));

        for(int i = 0; i < vec_size(&curr.priority); i++) {
        
            struct attr prio_attr = (struct attr){ .type = TYPE_STRING, };
            pf_strlcpy(prio_attr.val.as_string, vec_AT(&curr.priority, i), sizeof(prio_attr.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &prio_attr, "prio"));
        }
        Sched_TryYield();

        struct attr drop_off_only = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool  = curr.drop_off_only
        };
        CHK_TRUE_RET(Attr_Write(stream, &drop_off_only, "drop_off_only"));

        struct attr accum = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float  = curr.accum
        };
        CHK_TRUE_RET(Attr_Write(stream, &accum, "accum"));

        struct attr cmd_type = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.queued.cmd
        };
        CHK_TRUE_RET(Attr_Write(stream, &cmd_type, "cmd_type"));

        struct attr cmd_arg = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.queued.uid_arg
        };
        CHK_TRUE_RET(Attr_Write(stream, &cmd_arg, "cmd_arg"));

        struct attr transport_src_uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.transport_src_uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &transport_src_uid, "transport_src_uid"));

        struct attr transport_dest_uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.transport_dest_uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &transport_dest_uid, "transport_dest_uid"));
        Sched_TryYield();
    });
    return true;
}

bool G_Harvester_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_ents = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_ents; i++) {

        uint32_t uid;
        struct hstate *hs;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uid = attr.val.as_int;

        /* The entity should have already been loaded from the scripting state */
        khiter_t k = kh_get(state, s_entity_state_table, uid);
        CHK_TRUE_RET(k != kh_end(s_entity_state_table));
        hs = &kh_value(s_entity_state_table, k);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        hs->state = attr.val.as_int;

        switch(hs->state) {
        case STATE_NOT_HARVESTING:
            break;
        case STATE_HARVESTING_SEEK_RESOURCE:
            E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_resource, 
                (void*)((uintptr_t)uid), G_RUNNING);
            E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
                (void*)((uintptr_t)uid), G_RUNNING);
            break;
        case STATE_HARVESTING:
            E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished, 
                (void*)((uintptr_t)uid), G_RUNNING);
            E_Entity_Register(EVENT_MOTION_START, uid, on_motion_begin_harvest, 
                (void*)((uintptr_t)uid), G_RUNNING);
            break;
        case STATE_HARVESTING_SEEK_STORAGE:
            E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_storage, 
                (void*)((uintptr_t)uid), G_RUNNING);
            E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
                (void*)((uintptr_t)uid), G_RUNNING);
            break;
        case STATE_TRANSPORT_GETTING:
            E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_transport_source, 
                (void*)((uintptr_t)uid), G_RUNNING);
            E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
                (void*)((uintptr_t)uid), G_RUNNING);
            break;
        case STATE_TRANSPORT_PUTTING:
            E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_transport_dest, 
                (void*)((uintptr_t)uid), G_RUNNING);
            E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
                (void*)((uintptr_t)uid), G_RUNNING);
            break;
        case STATE_TRANSPORT_SEEK_RESOURCE:
            E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_resource_source, 
                (void*)((uintptr_t)uid), G_RUNNING);
            E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
                (void*)((uintptr_t)uid), G_RUNNING);
            break;
        case STATE_TRANSPORT_HARVESTING:
            E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished_source, 
                (void*)((uintptr_t)uid), G_RUNNING);
            E_Entity_Register(EVENT_MOTION_START, uid, on_motion_begin_harvest, 
                (void*)((uintptr_t)uid), G_RUNNING);
            break;
        default: 
            return false;
        }
        Sched_TryYield();

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        hs->strategy = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        hs->ss_uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        hs->res_uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        hs->res_last_pos = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        bool has_res_name = attr.val.as_bool;

        if(has_res_name) {
        
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_STRING);
            const char *key = si_intern(attr.val.as_string, &s_stringpool, s_stridx);
            hs->res_name = key;
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int num_speeds = attr.val.as_int;

        for(int j = 0; j < num_speeds; j++) {
        
            struct attr keyattr;
            CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
            CHK_TRUE_RET(keyattr.type == TYPE_STRING);
            const char *key = keyattr.val.as_string;

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_FLOAT);
            float val = attr.val.as_float;

            G_Harvester_SetGatherSpeed(uid, key, val);
        }
        Sched_TryYield();

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int num_max  = attr.val.as_int;

        for(int j = 0; j < num_max; j++) {
        
            struct attr keyattr;
            CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
            CHK_TRUE_RET(keyattr.type == TYPE_STRING);
            const char *key = keyattr.val.as_string;

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            int val = attr.val.as_int;

            G_Harvester_SetMaxCarry(uid, key, val);
        }
        Sched_TryYield();

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int num_curr  = attr.val.as_int;

        for(int j = 0; j < num_curr; j++) {
        
            struct attr keyattr;
            CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
            CHK_TRUE_RET(keyattr.type == TYPE_STRING);
            const char *key = keyattr.val.as_string;

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            int val = attr.val.as_int;

            G_Harvester_SetCurrCarry(uid, key, val);
        }
        Sched_TryYield();

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int num_prios  = attr.val.as_int;

        vec_name_reset(&hs->priority);
        for(int j = 0; j < num_prios; j++) {
        
            struct attr keyattr;
            CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
            CHK_TRUE_RET(keyattr.type == TYPE_STRING);

            const char *key = si_intern(keyattr.val.as_string, &s_stringpool, s_stridx);
            vec_name_push(&hs->priority, key);
        }
        Sched_TryYield();

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        hs->drop_off_only = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        hs->accum = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        hs->queued.cmd = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        hs->queued.uid_arg = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        hs->transport_src_uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        hs->transport_dest_uid = attr.val.as_int;
        Sched_TryYield();
    };

    return true;
}

