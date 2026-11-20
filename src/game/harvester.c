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
#define MEM_FILE_SUB MEM_SUB_GAME_HARVESTER

#include "harvester.h"
#include "duty.h"
#include "../move/public/movement.h"
#include "resource.h"
#include "storage_site.h"
#include "provision.h"
#include "game_private.h"
#include "public/game.h"
#include "../sched/public/sched.h"
#include "../event/public/event.h"
#include "../entity.h"
#include "../cursor.h"
#include "../settings.h"
#include "../camera/public/camera.h"
#include "../lib/public/vec.h"
#include "../lib/public/khash.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/string_intern.h"
#include "../lib/public/mpool_allocator.h"
#include "../lib/public/attr.h"

#include <stddef.h>
#include <assert.h>
#include <string.h>

#include "../perf/public/mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_HARVESTER)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_HARVESTER)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_HARVESTER)


#define UID_NONE            (~((uint32_t)0))
#define REACQUIRE_RADIUS    (150.0)
#define ARR_SIZE(a)         (sizeof(a)/sizeof((a)[0]))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define MIN(a, b)           ((a) < (b) ? (a) : (b))

/* Standing still this long on its way is taken as having arrived */
#define STILL_ARRIVAL_TICKS (60)
/* How often a harvester on its way to a building checks it can still get there */
#define REACH_CHECK_TICKS   (40)
/* Pressing at a crowded resource this long without coming nearer, a gatherer
 * goes on to the next one, as it would have had it come up against the crowd
 * and stopped */
#define PRESS_GIVE_UP_TICKS (80)
#define PRESS_STEP          (1.0f)
#define PRESS_FAR           (12.0f)
/* A carrier short of a full load stops at most this many more sources on a
 * trip, each only for at least this much (or what it has room for) */
#define MAX_TOP_UPS         (2)
#define MIN_TOP_UP          (5)
/* How much farther than the nearest store a load goes to one asking for it */
#define DROP_ASK_DETOUR     (1.5f)
#define DROP_ASK_SLACK      (60.0f)
/* How far off a hunter looses its arrows, and how much farther it may stand */
#define HUNT_RANGE          (48.0f)
#define HUNT_RANGE_SLACK    (16.0f)
/* How far off its mark a hunter may face and still shoot (cosine of half the angle) */
#define HUNT_AIM_COS        (0.995f)
/* And how far its game may have strayed while it drew the bow */
#define HUNT_SHOT_COS       (0.96f)
/* How far past touching a carcass may lie and still be butchered */
#define BUTCHER_SLACK       (16.0f)
/* How far from the node the player sent it to a gatherer goes on to others of
 * the good, when that one is crowded, out of reach or used up */
#define ORDER_RADIUS        (300.0f)
/* Each gatherer bound for a node makes it read this much further off */
#define CROWD_FACTOR        (2.0f)
/* How many nodes a gatherer remembers it could not get beside */
#define MAX_FAILED          (8)
/* Ticks making no way towards an awaited store before the unit is taken as there,
 * or sent off again; how close to the store counts as there */
#define STALL_GIVE_UP_TICKS (200)
#define ARRIVAL_REACH       (30.0f)
/* How many times a carrier kept off a store by a crowd reaches a step farther */
#define MAX_REACH_TRIES     (2)
/* A trip longer than this says nothing about the route */
#define MAX_ROUTE_S         (600)
/* Goods a target is this near as short of are as short */
#define LACK_SLACK          (0.05f)
#define NODE_GAP            (1.0f)

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
    kh_int_t   *do_not_transport; /* Per-resource flag to disable transporting */
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
            CMD_REPLENISH,
        }cmd;
        uint32_t uid_arg;
    }queued;
    uint32_t    transport_src_uid;
    uint32_t    transport_dest_uid;
    /* For a transport the automation assigned: how much to bring. The 
     * carrier takes no more, makes no other stops, and goes idle after. */
    int         job_amount;
    /* When the load's trip began, for the route the target is timed by */
    uint32_t    trip_start_s;
    /* Sources stopped at on this trip after the first; not saved, a trip
     * under way when the session loads starts its count over */
    int         top_ups;
    /* The store it last came to rest short of, and how many times running;
     * not saved, a load under way when the session loads starts its count over */
    uint32_t    reach_site;
    int         reach_fails;
    /* Ticks it has stood still on its way somewhere, to catch an arrival it never heard */
    int         still_ticks;
    /* Ticks it has made no way toward the resource it seeks, and where it last did */
    int         press_ticks;
    vec2_t      press_pos;
    uint32_t    press_target;
    /* Where the node the gathering was last ordered at stood */
    vec2_t      order_pos;
    /* The nodes it could not get beside since it last got to one, passed
     * over when it goes on to the next; not saved */
    uint32_t    failed[MAX_FAILED];
    int         nfailed;
    /* The site a builder gathers for when nothing in store can be had for it:
     * its load goes there, and it goes back to building it; not saved */
    uint32_t    gather_for;
    /* The replenishable resource it gathered out, which it fetches for and
     * waits by until the resource is replenished */
    uint32_t    sowing;
};

struct searcharg{
    uint32_t ent;
    uint32_t storage;
    const char *rname;
    enum tstrategy strat;
    /* The room a drop-off must have left */
    int room;
};

/* Whose loads on the way to a site a transporter leaves the site's want to: a
 * carrier's, all of them; a builder's, those that come sooner than it would
 * bring one; a builder tired of waiting, only those other builders fetch */
enum en_route_rule{
    EN_ROUTE_ALL,
    EN_ROUTE_SOONER,
    EN_ROUTE_BUILDERS
};

struct valid_resource_arg{
    vec2_t         ent_pos;
    enum nav_layer ent_layer;
    bool           ent_water;
    const char    *rname;
    uint32_t       exclude;
    /* Of the one searching, the nodes it could not get beside; NULL for none */
    const struct hstate *failed_of;
    /* The way there is judged apart, for a search that weighs many */
    bool           skip_reach;
};

typedef char buff_t[512];

MPOOL_ALLOCATOR_TYPE(buff, buff_t)
MPOOL_ALLOCATOR_PROTOTYPES(static, buff, buff_t)
MPOOL_ALLOCATOR_IMPL(static, buff, buff_t)

#undef kmalloc
#undef kcalloc
#undef krealloc
#undef kfree

#define kmalloc  PF_MALLOC
#define kcalloc  PF_CALLOC
#define krealloc PF_REALLOC
#define kfree    PF_FREE

KHASH_MAP_INIT_INT(state, struct hstate)

static void on_motion_begin_harvest(void *user, void *event);
static void on_motion_begin_travel(void *user, void *event);
static void on_arrive_at_resource(void *user, void *event);
static void on_arrive_at_resource_source(void *user, void *event);
static void on_arrive_at_storage(void *user, void *event);
static void on_arrive_at_transport_source(void *user, void *event);
static bool gather_order(uint32_t harvester, uint32_t resource);
static bool pick_up_order(uint32_t harvester, uint32_t storage);
static uint32_t best_storage_site_source(uint32_t uid, uint32_t storage, const char *rname,
                                         enum tstrategy strat, float *out_worth);
static bool supply_building(uint32_t harvester, uint32_t building, bool impatient);
static void on_arrive_at_transport_dest(void *user, void *event);
static void on_harvest_anim_finished(void *user, void *event);
static void on_harvest_anim_finished_source(void *user, void *event);
static void on_20hz_tick(void *user, void *event);
static int  home_bound(uint32_t uid);
static bool hunted(uint32_t res);
static bool in_hunt_range(uint32_t uid, uint32_t res);
static bool aimed_at_game(uint32_t uid, uint32_t res, float min_cos);
static bool failed_node(const struct hstate *hs, uint32_t node);
static void hunt_hit(uint32_t uid, struct hstate *hs);
static void walk_up(uint32_t uid, uint32_t res);
static bool beyond_reach(uint32_t uid, uint32_t res);
static bool regather(uint32_t uid, uint32_t resource);
static int  amount_en_route(uint32_t target, const char *rname, uint32_t exclude, float within);
static int  dest_room(uint32_t uid, const struct hstate *hs);
static float reach_after(uint32_t uid, uint32_t site, int fails);
static float distance_to(uint32_t uid, uint32_t site);
static void note_short_arrival(uint32_t uid, struct hstate *hs, uint32_t site);
static void wait_to_sow(uint32_t uid, struct hstate *hs);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static mpa_buff_t        s_mpool;
static khash_t(stridx)  *s_stridx;
/* A builder fetching or gathering for its site keeps the site for its duty */
static bool              s_for_site;
static mp_strbuff_t      s_stringpool;
static khash_t(state)   *s_entity_state_table;
static const struct map *s_map;
/* The 20 Hz ticks so far, to spread the reach checks over */
static uint32_t          s_tick;

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

/* What the site asks to be brought up to: what it is being built with, or
 * what it was told to hold, keep and work from, whoever is asking */
static int ss_wants(uint32_t uid, const char *rname)
{
    if(G_StorageSite_GetUseAlt(uid)) {
        return G_StorageSite_GetAltDesired(uid, rname);
    }else{
        return G_StorageSite_Request(uid, rname);
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
    kh_destroy(int, hs->do_not_transport);
}

static bool hstate_init(struct hstate *hs)
{
    vec_name_init_alloc(&hs->priority, prealloc, pfree);
    if(!vec_name_resize(&hs->priority, sizeof(buff_t) / sizeof(char*)))
        return false;

    hs->gather_speeds = kh_init(float);
    hs->max_carry = kh_init(int);
    hs->curr_carry = kh_init(int);
    hs->do_not_transport = kh_init(int);

    if(!hs->gather_speeds || !hs->max_carry 
    || !hs->curr_carry || !hs->do_not_transport) {

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
    hs->job_amount = 0;
    hs->top_ups = 0;
    hs->reach_site = NULL_UID;
    hs->reach_fails = 0;
    hs->still_ticks = 0;
    hs->press_ticks = 0;
    hs->press_pos = (vec2_t){0.0f, 0.0f};
    hs->press_target = NULL_UID;
    hs->nfailed = 0;
    hs->gather_for = NULL_UID;
    hs->sowing = NULL_UID;
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

static bool storage_site_reachable(uint32_t ent, uint32_t site)
{
    enum nav_layer ent_layer = Entity_NavLayer(ent);
    uint32_t ent_flags = G_FlagsGet(ent);
    uint32_t site_flags = G_FlagsGet(site);

    vec2_t ent_pos = G_Pos_GetXZ(ent);
    vec2_t site_pos = G_Pos_GetXZ(site);

    if(site_flags & ENTITY_FLAG_WATER) {
        if(ent_flags & ENTITY_FLAG_WATER) {
            if(!M_NavLocationsReachable(s_map, ent_layer, ent_pos, site_pos))
                return false;
        }else{
            struct obb obb;
            Entity_CurrentOBB(site, &obb, false);
            if(!M_NavIsAdjacentToIslandOBB(s_map, ent_layer, &obb, ent_pos))
                return false;
        }
    }else{
        /* Water units can only transport to/from water sites */
        if(ent_flags & ENTITY_FLAG_WATER)
            return false;
        if(!M_NavLocationsReachable(s_map, ent_layer, ent_pos, site_pos))
            return false;
        /* Not one walled in by the buildings about it, on every side of its
         * cluster */
        uint32_t members[CLUSTER_FOOTPRINT_MAX];
        size_t nmembers = G_Cluster_Footprint(site, ARR_SIZE(members), members);
        bool open = false;
        for(int i = 0; i < nmembers && !open; i++) {
            struct obb obb;
            Entity_CurrentOBB(members[i], &obb, false);
            open = M_NavObjReachableNowOBB(s_map, ent_layer, ent_pos, &obb);
        }
        if(!open)
            return false;
    }
    return true;
}

static bool same_pool(uint32_t a, uint32_t b)
{
    if(a == NULL_UID || b == NULL_UID)
        return false;
    int pool = G_Cluster_Of(a);
    return (pool != 0) && (G_Cluster_Of(b) == pool);
}

/* The home district of a worker the automation keeps to it, else 0 */
static int home_bound(uint32_t uid)
{
    return G_Automation_GetAutomaticWork(uid) ? G_Automation_GetHomeDistrict(uid) : 0;
}

static bool valid_storage_site_dropoff(uint32_t curr, void *arg)
{
    struct searcharg *sarg = arg;
    struct hstate *hs = hstate_get(sarg->ent);
    uint32_t curr_flags = G_FlagsGet(curr);
    uint32_t ent_flags = G_FlagsGet(sarg->ent);

    if(!(curr_flags & ENTITY_FLAG_STORAGE_SITE))
        return false;
    if(G_GetFactionID(sarg->ent) != G_GetFactionID(curr))
        return false;
    /* Goods leave a district by cart, not on its workers' backs */
    int home = home_bound(sarg->ent);
    if(home && G_District_Of(curr) != home)
        return false;
    /* Not back into the pool the load was just taken from */
    if(curr == sarg->storage || same_pool(curr, sarg->storage))
        return false;
    if(!storage_site_reachable(sarg->ent, curr))
        return false;

    int stored = G_StorageSite_GetCurr(curr, sarg->rname);
    int cap = ss_capacity(curr, sarg->rname);

    if(cap == 0)
        return false;
    if(stored == cap || cap - stored < sarg->room)
        return false;
    if(!G_StorageSite_Accepts(curr, sarg->rname))
        return false;

    return true;
}

static bool dropoff_has_room(uint32_t uid, uint32_t site, const char *rname)
{
    struct searcharg arg = (struct searcharg){uid, NULL_UID, rname};
    return valid_storage_site_dropoff(site, &arg);
}

static bool building_site(uint32_t uid)
{
    if(uid == NULL_UID || !G_EntityExists(uid) || G_EntityIsZombie(uid))
        return false;
    if(!(G_FlagsGet(uid) & ENTITY_FLAG_STORAGE_SITE))
        return false;
    /* A field being sown again is supplied as a building being built is */
    if((G_FlagsGet(uid) & ENTITY_FLAG_RESOURCE) && G_Resource_IsReplenishing(uid))
        return true;
    return G_StorageSite_GetUseAlt(uid);
}

/* What a building being built may take of a store: all but the player's
 * reserve and what keeps the realm's people alive; a field being sown again
 * may take what a workshop works from as well
 */
static int site_spare(uint32_t src, uint32_t dest, const char *rname)
{
    int short_of = G_Automation_UpkeepShort(G_GetFactionID(src), rname);
    if((G_FlagsGet(dest) & ENTITY_FLAG_RESOURCE) && G_Resource_IsReplenishing(dest))
        return G_StorageSite_SurplusForSowing(src, rname) - short_of;
    return G_StorageSite_SurplusForSite(src, rname) - short_of;
}

/* What a source can give the destination */
static int source_spare(uint32_t src, uint32_t dest, const char *rname)
{
    if(building_site(dest))
        return site_spare(src, dest, rname);
    return G_StorageSite_Surplus(src, rname);
}

static bool valid_storage_site_source(uint32_t curr, void *arg)
{
    struct searcharg *sarg = arg;
    struct hstate *hs = hstate_get(sarg->ent);
    uint32_t curr_flags = G_FlagsGet(curr);
    uint32_t ent_flags = G_FlagsGet(sarg->ent);

    if(!(curr_flags & ENTITY_FLAG_STORAGE_SITE))
        return false;
    if(G_GetFactionID(sarg->ent) != G_GetFactionID(curr))
        return false;
    if(curr == sarg->storage)
        return false;
    /* A pool holds its goods in common: carried from one of its sites to
     * another, a load goes round and round, and standing beside the pool a
     * carrier is beside both at once */
    if(same_pool(curr, sarg->storage))
        return false;
    if(G_StorageSite_GetDoNotTake(ent_flags, curr))
        return false;
    if(!storage_site_reachable(sarg->ent, curr))
        return false;

    /* Don't get resources from build sites - this prevents builders 
     * from 'stealing' resources back-and-forth from two nearby 
     * build sites */
    if((curr_flags & ENTITY_FLAG_BUILDING) && !G_Building_IsSupplied(curr))
        return false;

    int stored = G_StorageSite_GetCurr(curr, sarg->rname);
    int cap = ss_capacity(curr, sarg->rname);
    int desired = ss_wants(curr, sarg->rname);

    if(sarg->strat == TRANSPORT_STRATEGY_EXCESS && (desired >= stored))
        return false;
    if(cap == 0)
        return false;
    if(stored == 0)
        return false;
    /* What is kept back is not for building with */
    if(building_site(sarg->storage) && site_spare(curr, sarg->storage, sarg->rname) <= 0)
        return false;
    /* Nor is what is kept, held or worked from for carrying off, whoever asks */
    if(!building_site(sarg->storage) && G_StorageSite_Surplus(curr, sarg->rname) <= 0)
        return false;

    return true;
}

/* A node on the water is worked from the bank by one on land, and one on
 * the water works nothing on land */
static bool resource_reachable(enum nav_layer layer, bool water, vec2_t pos, uint32_t res)
{
    bool res_water = !!(G_FlagsGet(res) & ENTITY_FLAG_WATER);
    if(res_water == water)
        return M_NavLocationsReachable(s_map, layer, pos, G_Pos_GetXZ(res));
    if(water)
        return false;
    struct obb obb;
    Entity_CurrentOBB(res, &obb, false);
    return M_NavIsAdjacentToIslandOBB(s_map, layer, &obb, pos);
}

static bool valid_resource(uint32_t uid, void *arg)
{
    struct valid_resource_arg *vrarg = arg;
    uint32_t flags = G_FlagsGet(uid);

    if(!(flags & ENTITY_FLAG_RESOURCE))
        return false;
    if(flags & ENTITY_FLAG_ZOMBIE)
        return false;
    if((vrarg->exclude != NULL_UID) && (vrarg->exclude == uid))
        return false;
    if(vrarg->failed_of && failed_node(vrarg->failed_of, uid))
        return false;
    if((flags & ENTITY_FLAG_BUILDING) && !G_Building_IsCompleted(uid))
        return false;
    if(G_Resource_IsReplenishing(uid) || G_Resource_GetDormant(uid))
        return false;
    if(strcmp(vrarg->rname, G_Resource_GetName(uid)))
        return false;
    if(!vrarg->skip_reach && !resource_reachable(vrarg->ent_layer, vrarg->ent_water, vrarg->ent_pos, uid))
        return false;
    return true;
}

/* The store the loads from the node will go to, as the count of a store's gatherers reads it */
static uint32_t drop_site_for(uint32_t uid, uint32_t node, const char *rname)
{
    struct searcharg arg = (struct searcharg){uid, NULL_UID, rname};
    return G_StorageSite_NearestWithPred(G_Pos_GetXZ(node), valid_storage_site_dropoff, (void*)&arg, 0.0f);
}

/* How far from the store's centre a carrier that came to rest short of it so
 * many times running may stand and still reach it */
static float reach_after(uint32_t uid, uint32_t site, int fails)
{
    return G_GetSelectionRadius(uid) + G_GetSelectionRadius(site)
         + ARRIVAL_REACH * (1 + MIN(fails, MAX_REACH_TRIES));
}

static float distance_to(uint32_t uid, uint32_t site)
{
    vec2_t a = G_Pos_GetXZ(uid), b = G_Pos_GetXZ(site);
    vec2_t delta;
    PFM_Vec2_Sub(&a, &b, &delta);
    return PFM_Vec2_Len(&delta);
}

/* Beside the store, or as near as a crowd about it lets one get */
static bool within_reach(uint32_t uid, uint32_t site)
{
    if(M_NavObjAdjacent(s_map, uid, site))
        return true;
    struct hstate *hs = hstate_get(uid);
    int fails = (hs && hs->reach_site == site) ? hs->reach_fails : 0;
    return distance_to(uid, site) <= reach_after(uid, site, fails);
}

/* A carrier come to rest short of the store, but no farther than the
 * farthest reach, reaches it a step farther at its next arrival there */
static void note_short_arrival(uint32_t uid, struct hstate *hs, uint32_t site)
{
    if(!G_EntityExists(site))
        return;
    if(hs->reach_site != site) {
        hs->reach_site = site;
        hs->reach_fails = 0;
    }
    if(distance_to(uid, site) <= reach_after(uid, site, MAX_REACH_TRIES)) {
        hs->reach_fails = MIN(hs->reach_fails + 1, MAX_REACH_TRIES);
    }
}

/* The nearest store with room for the whole load, else the nearest with any */
/* Whether the store asks for more of the good than is on its way to it */
static bool asks_for(uint32_t site, const char *rname)
{
    return G_StorageSite_Request(site, rname)
         > G_StorageSite_GetCurr(site, rname) + amount_en_route(site, rname, NULL_UID, INFINITY);
}

static bool valid_asking_dropoff(uint32_t curr, void *arg)
{
    struct searcharg *sarg = arg;
    return valid_storage_site_dropoff(curr, arg) && asks_for(curr, sarg->rname);
}

/* The store a load is brought to: one asking for the good within a modest
 * detour of the nearest with room for it, so that a gatherer at the hub's
 * door walks on to the tavern short of fruit rather than leave the load to
 * a cart, else the nearest with room */
uint32_t nearest_storage_site_dropoff(uint32_t uid, const char *rname)
{
    vec2_t pos = G_Pos_GetXZ(uid);
    struct searcharg arg = (struct searcharg){uid, NULL_UID, rname};
    arg.room = G_Harvester_GetCurrCarry(uid, rname);
    uint32_t ret = G_StorageSite_NearestWithPred(pos, valid_storage_site_dropoff, (void*)&arg, 0.0f);
    if(ret == NULL_UID && arg.room > 1) {
        arg.room = 0;
        ret = G_StorageSite_NearestWithPred(pos, valid_storage_site_dropoff, (void*)&arg, 0.0f);
    }
    if(ret != NULL_UID && !asks_for(ret, rname)) {
        vec2_t at = G_Pos_GetXZ(ret), delta;
        PFM_Vec2_Sub(&at, &pos, &delta);
        float within = PFM_Vec2_Len(&delta) * DROP_ASK_DETOUR + DROP_ASK_SLACK;
        uint32_t asking = G_StorageSite_NearestWithPred(pos, valid_asking_dropoff, (void*)&arg, within);
        if(asking != NULL_UID) {
            ret = asking;
        }
    }
    return ret;
}

uint32_t nearest_storage_site_source(uint32_t uid, uint32_t storage, const char *rname, enum tstrategy strat)
{
    vec2_t pos = G_Pos_GetXZ(storage);
    struct searcharg arg = (struct searcharg){uid, storage, rname, strat};
    uint32_t ret = G_StorageSite_NearestWithPred(pos, valid_storage_site_source, (void*)&arg, 0.0f);

    if((ret == NULL_UID) && (strat == TRANSPORT_STRATEGY_EXCESS)) {
        arg = (struct searcharg){uid, storage, rname, TRANSPORT_STRATEGY_NEAREST};
        ret = G_StorageSite_NearestWithPred(pos, valid_storage_site_source, (void*)&arg, 0.0f);
    }
    return ret;
}

/* The source a load is best fetched from: the fullest load for the walk to
 * it and on to the target */
static uint32_t best_storage_site_source(uint32_t uid, uint32_t storage, const char *rname,
                                         enum tstrategy strat, float *out_worth)
{
    *out_worth = 0.0f;
    struct searcharg arg = (struct searcharg){uid, storage, rname, strat};
    int room = G_Harvester_GetMaxCarry(uid, rname) - G_Harvester_GetCurrCarry(uid, rname);
    if(room <= 0)
        return NULL_UID;
    vec2_t pos = G_Pos_GetXZ(uid);
    vec2_t dest = G_Pos_GetXZ(storage);

    vec_entity_t sites;
    vec_entity_init(&sites);
    G_StorageSite_GetAll(&sites);

    uint32_t best = NULL_UID;
    float best_score = 0.0f;
    for(int i = 0; i < vec_size(&sites); i++) {
        uint32_t site = vec_AT(&sites, i);
        if(G_EntityIsZombie(site) || !valid_storage_site_source(site, &arg))
            continue;
        int load = MIN(room, source_spare(site, storage, rname));
        if(load <= 0)
            continue;
        vec2_t at = G_Pos_GetXZ(site);
        vec2_t there, on;
        PFM_Vec2_Sub(&at, &pos, &there);
        PFM_Vec2_Sub(&dest, &at, &on);
        float score = load / (PFM_Vec2_Len(&there) + PFM_Vec2_Len(&on) + 1.0f);
        if(score > best_score) {
            best = site;
            best_score = score;
        }
    }
    vec_entity_destroy(&sites);
    *out_worth = best_score;
    return best;
}

uint32_t nearest_resource(uint32_t uid, const char *name)
{
    assert(name);
    vec2_t pos = G_Pos_GetXZ(uid);
    struct valid_resource_arg arg = (struct valid_resource_arg){
        .ent_pos = pos,
        .ent_layer = Entity_NavLayer(uid),
        .ent_water = !!(G_FlagsGet(uid) & ENTITY_FLAG_WATER),
        .rname = name,
        .exclude = UID_NONE
    };
    return G_Pos_NearestWithPred(pos, valid_resource, (void*)&arg, REACQUIRE_RADIUS);
}

uint32_t nearest_resource_with_exclusion(uint32_t uid, const char *name, uint32_t exclude)
{
    assert(name);
    vec2_t pos = G_Pos_GetXZ(uid);
    struct valid_resource_arg arg = (struct valid_resource_arg){
        .ent_pos = pos,
        .ent_layer = Entity_NavLayer(uid),
        .ent_water = !!(G_FlagsGet(uid) & ENTITY_FLAG_WATER),
        .rname = name,
        .exclude = exclude,
        .failed_of = hstate_get(uid)
    };
    return G_Pos_NearestWithPred(pos, valid_resource, (void*)&arg, REACQUIRE_RADIUS);
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

    /* A gatherer's loads count for the store they go to */
    const char *carried = carried_resource_name(hs);
    struct duty duty;
    if(carried && G_Duty_Get(uid, &duty) && duty.kind == DUTY_GATHER && !strcmp(duty.good, carried)) {
        G_Duty_Set(uid, DUTY_GATHER, ss, carried, duty.by);
    }
    if(G_Duty_Kind(uid) == DUTY_GATHER) {
        G_Duty_SetWaiting(uid, false);
    }

    /* Sent on to another store on the way, it arrives only once */
    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_storage);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel);
    E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_storage, 
        (void*)((uintptr_t)uid), G_RUNNING);
    E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
        (void*)((uintptr_t)uid), G_RUNNING);

    E_Entity_Notify(EVENT_STORAGE_TARGET_ACQUIRED, uid, (void*)(uintptr_t)ss, ES_ENGINE);
    Move_SetSurroundEntity(uid, ss);
}

/* Whether the site gathered for is one the worker builds, not a store it feeds */
static bool under_construction(uint32_t site)
{
    return (G_FlagsGet(site) & ENTITY_FLAG_BUILDING) && !G_Building_IsCompleted(site);
}

/* Whether the worker gathering for a site still has one to bring the load to:
 * a builder's while it is not supplied, a store while it asks for the good */
static bool gathering_for_site(const struct hstate *hs)
{
    uint32_t site = hs->gather_for;
    if(site == NULL_UID || !G_EntityExists(site) || G_EntityIsZombie(site))
        return false;
    if(under_construction(site))
        return !G_Building_IsSupplied(site);
    return (G_FlagsGet(site) & ENTITY_FLAG_STORAGE_SITE) && hs->res_name && asks_for(site, hs->res_name);
}

/* A builder done gathering for its site leaves off gathering for good and builds it */
static void back_to_building(uint32_t uid, struct hstate *hs, uint32_t site)
{
    hs->state = STATE_NOT_HARVESTING;
    hs->ss_uid = NULL_UID;
    hs->res_uid = NULL_UID;
    hs->res_name = NULL;
    hs->gather_for = NULL_UID;
    G_Builder_Order(uid, site);
}

static void entity_try_drop_off(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    if(!G_Harvester_GetCurrTotalCarry(uid)) {
        hs->state = STATE_NOT_HARVESTING;
        return;
    }

    /* Its site supplied by others meanwhile, the builder goes back to build it */
    uint32_t site = hs->gather_for;
    if(site != NULL_UID && !gathering_for_site(hs)) {
        hs->gather_for = NULL_UID;
        if(G_EntityExists(site) && !G_EntityIsZombie(site) && under_construction(site)) {
            back_to_building(uid, hs, site);
            return;
        }
    }

    uint32_t ss = gathering_for_site(hs) ? hs->gather_for
                : nearest_storage_site_dropoff(uid, carried_resource_name(hs));
    if(ss == NULL_UID) {
        hs->state = STATE_NOT_HARVESTING;
        /* A gatherer with no room for its load waits until a store has some */
        if(G_Duty_Kind(uid) == DUTY_GATHER) {
            G_Duty_SetWaiting(uid, true);
        }
    }else{
        entity_drop_off(uid, ss);
    }
}

/* Whether the gathering is the player's: ordered, not chosen by the automation */
static bool gathering_by_order(uint32_t uid)
{
    return (G_Duty_Kind(uid) == DUTY_GATHER) && G_Duty_ByPlayer(uid);
}

/* How many gatherers stand about a node at once: its rim in worker widths */
static int node_capacity(uint32_t node, float worker_radius)
{
    float rim;
    if(G_FlagsGet(node) & ENTITY_FLAG_BUILDING) {
        struct obb obb;
        Entity_CurrentOBB(node, &obb, true);
        rim = 4.0f * (obb.half_lengths[0] + obb.half_lengths[2]);
    }else{
        rim = 2.0f * M_PI * (G_GetSelectionRadius(node) + worker_radius);
    }
    return MAX(1, (int)(rim / (2.0f * worker_radius + NODE_GAP)));
}

static int gatherers_aimed_at(uint32_t node, uint32_t exclude)
{
    int ret = 0;
    uint32_t uid;
    struct hstate *hs;
    kh_foreach_val_ptr(s_entity_state_table, uid, hs, {
        if(uid != exclude && hs->res_uid == node
        && (hs->state == STATE_HARVESTING_SEEK_RESOURCE || hs->state == STATE_HARVESTING
            || hs->state == STATE_HARVESTING_SEEK_STORAGE)) {
            ret++;
        }
    });
    return ret;
}

static bool failed_node(const struct hstate *hs, uint32_t node)
{
    for(int i = 0; i < hs->nfailed; i++) {
        if(hs->failed[i] == node)
            return true;
    }
    return false;
}

static void note_failed_node(struct hstate *hs, uint32_t node)
{
    if(failed_node(hs, node))
        return;
    if(hs->nfailed == MAX_FAILED) {
        memmove(hs->failed, hs->failed + 1, sizeof(hs->failed[0]) * (MAX_FAILED - 1));
        hs->nfailed--;
    }
    hs->failed[hs->nfailed++] = node;
}

/* The node of the good a gatherer by order goes on to: one about the node it
 * was sent to with room about it and a spot to stand beside it, the nearest,
 * the gatherers already bound for each weighing against it */
static uint32_t ordered_node(uint32_t uid, const char *rname, uint32_t exclude)
{
    struct hstate *hs = hstate_get(uid);
    uint32_t nodes[256];
    float scores[256];
    size_t nnodes = G_Harvester_ResourcesNear(uid, hs->order_pos, rname, ORDER_RADIUS,
        ARR_SIZE(nodes), nodes);
    float worker_radius = G_GetSelectionRadius(uid);
    vec2_t pos = G_Pos_GetXZ(uid);

    for(int i = 0; i < nnodes; i++) {
        int aimed = gatherers_aimed_at(nodes[i], uid);
        if(nodes[i] == exclude || failed_node(hs, nodes[i]) || aimed >= node_capacity(nodes[i], worker_radius)) {
            scores[i] = INFINITY;
            continue;
        }
        vec2_t node_pos = G_Pos_GetXZ(nodes[i]);
        vec2_t delta;
        PFM_Vec2_Sub(&node_pos, &pos, &delta);
        scores[i] = PFM_Vec2_Len(&delta) * (1.0f + CROWD_FACTOR * aimed);
    }
    for(int tries = 0; tries < nnodes; tries++) {
        int best = -1;
        for(int i = 0; i < nnodes; i++) {
            if(scores[i] < INFINITY && (best == -1 || scores[i] < scores[best]))
                best = i;
        }
        if(best == -1)
            break;
        vec2_t beside;
        if(G_Harvester_ResourceReachable(uid, nodes[best])
        && (hunted(nodes[best]) || G_ClosestAdjacentPos(uid, nodes[best], &beside)))
            return nodes[best];
        scores[best] = INFINITY;
    }
    return NULL_UID;
}

static void entity_try_gather_nearest(uint32_t uid, const char *rname)
{
    if(gathering_by_order(uid)) {
        uint32_t newtarget = ordered_node(uid, rname, NULL_UID);
        if(newtarget != NULL_UID) {
            regather(uid, newtarget);
        }else{
            entity_try_drop_off(uid);
        }
        return;
    }
    /* The automation weighs the crowd at each node; by order it is the nearest */
    uint32_t newtarget = G_Automation_BestResource(uid, rname);
    if(newtarget == NULL_UID && !home_bound(uid)) {
        newtarget = nearest_resource(uid, rname);
    }
    if(newtarget != NULL_UID) {
        regather(uid, newtarget);
    }else{
        entity_try_drop_off(uid);
    }
}

static void entity_try_gather_nearest_with_exclusion(uint32_t uid, const char *rname, uint32_t exclude)
{
    uint32_t newtarget;
    if(gathering_by_order(uid)) {
        newtarget = ordered_node(uid, rname, exclude);
    }else{
        newtarget = nearest_resource_with_exclusion(uid, rname, exclude);
    }
    int home = home_bound(uid);
    if(newtarget != NULL_UID && home && G_District_Of(newtarget) != home) {
        newtarget = NULL_UID;
    }
    if(newtarget != NULL_UID) {
        regather(uid, newtarget);
    }else{
        entity_try_drop_off(uid);
    }
}

static void entity_try_retarget(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    const char *rname = hs->res_name;

    finish_harvesting(hs, uid);
    if(!rname) {
        entity_try_drop_off(uid);
        return;
    }
    entity_try_gather_nearest(uid, rname);
}

/* With nothing to replenish its resource with yet, the gatherer waits by it
 * for its duty's next check
 */
static void wait_to_sow(uint32_t uid, struct hstate *hs)
{
    uint32_t resource = G_Harvester_Sowing(uid);
    if(resource == NULL_UID) {
        hs->sowing = NULL_UID;
        entity_try_retarget(uid);
        return;
    }
    if(hs->state != STATE_NOT_HARVESTING) {
        finish_harvesting(hs, uid);
    }
    G_Duty_SetWaiting(uid, true);
    if(!M_NavObjAdjacent(s_map, uid, resource) && Move_Still(uid)) {
        Move_SetSurroundEntity(uid, resource);
    }
}

uint32_t target_resource(uint32_t uid, struct hstate *hs, const char *rname)
{
    bool resource = false;
    if((hs->res_uid != NULL_UID) && G_EntityExists(hs->res_uid)
    && !(G_FlagsGet(hs->res_uid) & ENTITY_FLAG_ZOMBIE)) {
        resource = true;
    }
    if(!resource) {
        struct valid_resource_arg arg = (struct valid_resource_arg){
            .ent_pos = G_Pos_GetXZ(uid),
            .ent_layer = Entity_NavLayer(uid),
            .ent_water = !!(G_FlagsGet(uid) & ENTITY_FLAG_WATER),
            .rname = rname,
            .exclude = UID_NONE,
        };
        return G_Pos_NearestWithPred(hs->res_last_pos, valid_resource, 
            (void*)&arg, REACQUIRE_RADIUS);
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
        if(G_EntityExists(arg) && !G_EntityIsZombie(arg)) {
            G_Harvester_Gather(uid, arg);
        }else{
            entity_try_gather_nearest(uid, hs->res_name);
        }
        break;
    }
    case CMD_TRANSPORT: {
        if(G_EntityExists(arg) && !G_EntityIsZombie(arg)) {
            G_Harvester_Transport(uid, arg);
        }
        break;
    }
    case CMD_BUILD: {
        if((G_FlagsGet(uid) & ENTITY_FLAG_BUILDER) && G_EntityExists(arg)
            && !G_EntityIsZombie(arg)) {
            G_Builder_Build(uid, arg);
        }
        break;
    }
    case CMD_SUPPLY: {
        if(G_EntityExists(arg) && !G_EntityIsZombie(arg)) {
            G_Harvester_SupplyBuilding(uid, arg, false);
        }
        break;
    }
    case CMD_REPLENISH: {
        if(G_EntityExists(arg) && !G_EntityIsZombie(arg)
        && !G_Harvester_ReplenishResource(uid, arg)) {
            /* The drop-off leg already cleared the harvest state; retarget by
             * the resource the plot replenishes.
             */
            if(arg == G_Harvester_Sowing(uid)) {
                wait_to_sow(uid, hs);
            }else if(G_FlagsGet(arg) & ENTITY_FLAG_RESOURCE) {
                entity_try_gather_nearest(uid, G_Resource_GetName(arg));
            }else{
                entity_try_drop_off(uid);
            }
        }
        break;
    }
    default:
        break;
    }
    hs->drop_off_only = false;
}

/* The source for the target's good, and how good a load for the walk it gives */
static uint32_t transport_source_worth(uint32_t uid, uint32_t storage, const char *rname, float *out_worth)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    uint32_t ret = best_storage_site_source(uid, storage, rname, hs->strategy, out_worth);
    if(ret == NULL_UID && hs->strategy == TRANSPORT_STRATEGY_EXCESS) {
        ret = best_storage_site_source(uid, storage, rname, TRANSPORT_STRATEGY_NEAREST, out_worth);
    }
    return ret;
}

uint32_t transport_source(uint32_t uid, uint32_t storage, const char *rname)
{
    float worth;
    return transport_source_worth(uid, storage, rname, &worth);
}

bool G_Harvester_HasTransportSource(uint32_t uid, uint32_t storage, const char *rname)
{
    struct hstate *hs = hstate_get(uid);
    if(!hs)
        return false;
    if(hs->strategy == TRANSPORT_STRATEGY_GATHERING
    && G_Harvester_GetGatherSpeed(uid, rname) > 0)
        return true;
    return transport_source(uid, storage, rname) != NULL_UID;
}

static void finish_transporing(struct hstate *hs)
{
    hs->transport_dest_uid = NULL_UID;
    hs->transport_src_uid = NULL_UID;
    hs->res_name = NULL;
    hs->state = STATE_NOT_HARVESTING;
    hs->job_amount = 0;
    hs->top_ups = 0;
}

/* Another source worth a stop on the way to fill up: never the one just
 * left, and only for a worthwhile load, so that a carrier does not shuttle
 * between producers a unit at a time instead of delivering.
 */
static uint32_t top_up_source(uint32_t uid, const struct hstate *hs, int carry_cap)
{
    if(hs->top_ups >= MAX_TOP_UPS)
        return NULL_UID;
    uint32_t src = transport_source(uid, hs->transport_dest_uid, hs->res_name);
    if(!G_EntityExists(src) || G_EntityIsZombie(src) || src == hs->transport_src_uid)
        return NULL_UID;
    if(source_spare(src, hs->transport_dest_uid, hs->res_name) < MIN(carry_cap, MIN_TOP_UP))
        return NULL_UID;
    return src;
}

static void on_harvest_anim_finished(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    if(!G_EntityExists(hs->res_uid) || (G_FlagsGet(hs->res_uid) & ENTITY_FLAG_ZOMBIE)
    || G_Resource_GetDormant(hs->res_uid)) {
        /* If the resource was exhausted or lies dormant, switch targets to the
         * nearest resource of the same type */
        entity_try_retarget(uid);
        return;
    }

    if(hunted(hs->res_uid)) {
        /* No arrow is wasted on game in full flight */
        if(Move_Wander_Fleeing(hs->res_uid))
            return;
        /* Game on the move may have left the bowshot or the line of the arrow */
        if(in_hunt_range(uid, hs->res_uid) && aimed_at_game(uid, hs->res_uid, HUNT_SHOT_COS)) {
            hunt_hit(uid, hs);
        }else{
            walk_up(uid, hs->res_uid);
        }
        return;
    }
    /* Game another brought down is butchered from beside it, not a bowshot off */
    if(beyond_reach(uid, hs->res_uid)) {
        walk_up(uid, hs->res_uid);
        return;
    }

    const char *rname = G_Resource_GetName(hs->res_uid);
    int resource_left = G_Resource_GetAmount(hs->res_uid);

    float gather_speed = G_Harvester_GetGatherSpeed(uid, rname);
    int gathered = hs->accum + gather_speed;
    hs->accum += gather_speed;

    if(gathered == 0)
        return;
    hs->accum = 0;

    int old_carry = G_Harvester_GetCurrCarry(uid, rname);
    int max_carry = G_Harvester_GetMaxCarry(uid, rname);

    int new_carry = MIN(max_carry, old_carry + MIN(gathered, resource_left));
    resource_left = MAX(0, resource_left - (new_carry - old_carry));

    G_Resource_SetAmount(hs->res_uid, resource_left);
    G_Harvester_SetCurrCarry(uid, rname, new_carry);
    G_StorageSite_RecordProduced(G_GetFactionID(uid), rname, new_carry - old_carry);
    G_StorageSite_RecordGathered(uid, rname, new_carry - old_carry);
    G_Automation_NoteGathered(uid, rname, new_carry - old_carry);

    if(resource_left == 0) {

        if(G_Resource_GetReplenishable(hs->res_uid)) {

            if(!G_Resource_IsReplenishing(hs->res_uid)) {
                G_Resource_SetReplenishing(hs->res_uid);
            }
            hs->sowing = hs->res_uid;
            if(!G_Harvester_ReplenishResource(uid, hs->res_uid)) {
                wait_to_sow(uid, hs);
                return;
            }
        }else{
            E_Entity_NotifyImmediate(EVENT_RESOURCE_EXHAUSTED, hs->res_uid, NULL, ES_ENGINE);
            G_Zombiefy(hs->res_uid, true);

            if(new_carry < max_carry) {
                entity_try_retarget(uid);
                return;
            }
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

static bool hunted(uint32_t res)
{
    return G_Resource_GetHuntHits(res) > 0;
}

static vec2_t game_direction(uint32_t uid, uint32_t res)
{
    vec2_t a = G_Pos_GetXZ(uid);
    vec2_t b = G_Pos_GetXZ(res);
    vec2_t dir;
    PFM_Vec2_Sub(&b, &a, &dir);
    return dir;
}

static bool in_hunt_range(uint32_t uid, uint32_t res)
{
    vec2_t a = G_Pos_GetXZ(uid);
    vec2_t b = G_Pos_GetXZ(res);
    vec2_t delta;
    PFM_Vec2_Sub(&b, &a, &delta);
    return PFM_Vec2_Len(&delta) <= HUNT_RANGE + HUNT_RANGE_SLACK + G_GetSelectionRadius(res);
}

/* Well clear of the resource, as a hunter whose game another brought down stands */
static bool beyond_reach(uint32_t uid, uint32_t res)
{
    vec2_t a = G_Pos_GetXZ(uid);
    vec2_t b = G_Pos_GetXZ(res);
    vec2_t delta;
    PFM_Vec2_Sub(&b, &a, &delta);
    float reach = G_GetSelectionRadius(uid) + G_GetSelectionRadius(res) + BUTCHER_SLACK;
    return PFM_Vec2_Len(&delta) > reach && !M_NavObjAdjacent(s_map, uid, res);
}

/* Where a harvester works a resource from: beside it, or for game still
 * standing, within a bowshot */
static bool at_resource(uint32_t uid, uint32_t res)
{
    if(hunted(res))
        return in_hunt_range(uid, res);
    return M_NavObjAdjacent(s_map, uid, res);
}

static quat_t rot_toward(vec2_t dir)
{
    float angle = atan2(dir.z, dir.x) - M_PI/2.0f;
    return (quat_t){0.0f, sinf(angle / 2.0f), 0.0f, cosf(angle / 2.0f)};
}

static bool aimed_at_game(uint32_t uid, uint32_t res, float min_cos)
{
    vec2_t dir = game_direction(uid, res);
    if(PFM_Vec2_Len(&dir) < 1.0f)
        return true;

    quat_t want = rot_toward(dir);
    quat_t curr = Entity_GetRot(uid);
    float dot = want.x * curr.x + want.y * curr.y + want.z * curr.z + want.w * curr.w;
    return fabsf(dot) >= min_cos;
}

/* Turns a hunter toward its game, returning whether it already faces it */
static bool face_game(uint32_t uid, uint32_t res)
{
    if(aimed_at_game(uid, res, HUNT_AIM_COS))
        return true;
    Move_SetChangeDirection(uid, rot_toward(game_direction(uid, res)));
    return false;
}

/* An arrow struck: the last brings the game down, and the hunter goes to
 * work the carcass */
static void hunt_hit(uint32_t uid, struct hstate *hs)
{
    uint32_t res = hs->res_uid;
    int hits = G_Resource_GetHuntHits(res) - 1;
    G_Resource_SetHuntHits(res, hits);
    E_Entity_Notify(EVENT_HUNT_HIT, res, (void*)((uintptr_t)uid), ES_ENGINE);
    if(hits > 0)
        return;
    Move_Wander_Stop(res);
    walk_up(uid, res);
}

/* Stops working the resource from where it stands and goes beside it */
static void walk_up(uint32_t uid, uint32_t res)
{
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished);
    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin_harvest);
    E_Entity_Notify(EVENT_HARVEST_END, uid, NULL, ES_ENGINE);
    G_Harvester_Gather(uid, res);
}

/* A pile on the ground is picked up rather than worked: as much as can be 
 * carried, at once, and then taken to be stored. */
static void take_instant(uint32_t uid, struct hstate *hs)
{
    const char *rname = G_Resource_GetName(hs->res_uid);
    int left = G_Resource_GetAmount(hs->res_uid);
    int old_carry = G_Harvester_GetCurrCarry(uid, rname);
    int max_carry = G_Harvester_GetMaxCarry(uid, rname);
    int taken = MIN(max_carry - old_carry, left);

    G_Resource_SetAmount(hs->res_uid, left - taken);
    G_Harvester_SetCurrCarry(uid, rname, old_carry + taken);

    if(left - taken == 0) {
        E_Entity_NotifyImmediate(EVENT_RESOURCE_EXHAUSTED, hs->res_uid, NULL, ES_ENGINE);
        G_Zombiefy(hs->res_uid, true);
    }
    entity_try_drop_off(uid);
}

/* A field is walked onto, not around: one held up in the crowd on it is
 * there, and is stopped where it stands
 */
static bool held_up_on_field(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    if(!G_EntityExists(hs->res_uid)
    || (G_FlagsGet(hs->res_uid) & (ENTITY_FLAG_COLLISION | ENTITY_FLAG_ZOMBIE))
    || !M_NavObjAdjacent(s_map, uid, hs->res_uid))
        return false;
    Move_Stop(uid);
    return true;
}

static void on_arrive_at_resource(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;

    if(!Move_Still(uid) && !held_up_on_field(uid))
        return; 

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_resource);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel);

    struct hstate *hs = hstate_get(uid);
    assert(hs);
    assert(hs->res_uid != NULL_UID);

    if(!G_EntityExists(hs->res_uid)
    || (G_FlagsGet(hs->res_uid) & ENTITY_FLAG_ZOMBIE)
    || !at_resource(uid, hs->res_uid)) {

        /* harvester could not reach the resource; one the realm no longer
         * wants at the good takes other work instead of the next node */
        if(G_EntityExists(hs->res_uid)) {
            note_failed_node(hs, hs->res_uid);
        }
        if(hs->res_name && !G_Automation_LeavesGathering(uid, hs->res_name, hs->res_uid)) {
            entity_try_gather_nearest_with_exclusion(uid, hs->res_name, hs->res_uid);
        }else{
            hs->state = STATE_NOT_HARVESTING;
        }
        return; 
    }
    hs->nfailed = 0;
    /* A job that ended on the way may have taken the name with it */
    if(!hs->res_name) {
        hs->res_name = G_Resource_GetName(hs->res_uid);
    }

    if(G_Harvester_GetCurrCarry(uid, hs->res_name) == G_Harvester_GetMaxCarry(uid, hs->res_name)) {

        /* harvester cannot carry any more of the resource */
        entity_try_drop_off(uid);
        return;
    }

    if(G_Resource_GetInstant(hs->res_uid)) {
        take_instant(uid, hs);
        return;
    }

    /* It shoots once it has turned to face the game */
    if(hunted(hs->res_uid) && !face_game(uid, hs->res_uid)) {
        E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_resource, 
            (void*)((uintptr_t)uid), G_RUNNING);
        E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
            (void*)((uintptr_t)uid), G_RUNNING);
        return;
    }

    E_Entity_Notify(EVENT_HARVEST_BEGIN, uid, NULL, ES_ENGINE);
    hs->state = STATE_HARVESTING; 
    E_Entity_Register(EVENT_MOTION_START, uid, on_motion_begin_harvest, (void*)((uintptr_t)uid), G_RUNNING);
    E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished, 
        (void*)((uintptr_t)uid), G_RUNNING);
}

static void cart_leg_log(uint32_t uid, const char *event, uint32_t src, uint32_t dest)
{
    struct sval setting;
    if(Settings_Get("pf.debug.log_perf_csv", &setting) != SS_OKAY || !setting.as_bool)
        return;
    struct hstate *hs = hstate_get(uid);
    vec2_t pos = G_Pos_GetXZ(uid);
    fprintf(stdout, "[cart-leg] %u,%u,%s,%d,%u,%u,%d,%d,%d,%.2f,%.2f\n", Move_TraceTick(), uid,
        event, hs ? (int)hs->state : -1, src, dest,
        src != NULL_UID && G_EntityExists(src) ? (int)M_NavObjAdjacent(s_map, uid, src) : -1,
        dest != NULL_UID && G_EntityExists(dest) ? (int)M_NavObjAdjacent(s_map, uid, dest) : -1,
        G_Harvester_GetCurrTotalCarry(uid), pos.x, pos.z);
}

/* Replenishes a resource once it holds all it is replenished with, whoever brought the last of it */
static bool sow_if_stocked(uint32_t site)
{
    if(!(G_FlagsGet(site) & ENTITY_FLAG_RESOURCE)
    || !G_Resource_IsReplenishing(site)
    || !G_StorageSite_IsSaturated(site))
        return false;
    G_Resource_SetReplenished(site);
    return true;
}

static void on_arrive_at_storage(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;

    if(!Move_Still(uid))
        return; 

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_storage);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel);

    struct hstate *hs = hstate_get(uid);
    assert(hs);
    assert(hs->ss_uid != NULL_UID);

    if(!G_EntityExists(hs->ss_uid)
    || !(G_FlagsGet(hs->ss_uid) & ENTITY_FLAG_STORAGE_SITE)
    || !within_reach(uid, hs->ss_uid)) {
        /* harvester could not reach the storage site */
        note_short_arrival(uid, hs, hs->ss_uid);
        entity_try_drop_off(uid);
        return; 
    }
    hs->reach_fails = 0;

    /* One that brought its load in already, and found nothing to gather
     * after, has nothing to drop off */
    const char *rname = carried_resource_name(hs);
    if(!rname) {
        G_Harvester_Stop(uid);
        hs->ss_uid = NULL_UID;
        return;
    }

    int carry = G_Harvester_GetCurrCarry(uid, rname);
    int cap = ss_capacity(hs->ss_uid, rname);
    int curr = G_StorageSite_GetCurr(hs->ss_uid, rname);
    int left = cap - curr;

    if(left > 0) {
        E_Entity_Notify(EVENT_RESOURCE_DROPPED_OFF, uid, NULL, ES_ENGINE);
    }
    G_Provision_OnVisit(uid, hs->ss_uid);

    if(left >= carry) {
        G_Harvester_SetCurrCarry(uid, rname, 0);
        G_StorageSite_SetCurr(hs->ss_uid, rname, curr + carry);
        sow_if_stocked(hs->ss_uid);

        /* A builder that gathered for its site goes back to building it */
        if(hs->gather_for != NULL_UID && hs->gather_for == hs->ss_uid && under_construction(hs->gather_for)) {
            back_to_building(uid, hs, hs->gather_for);
            return;
        }
        /* One that fed a store goes on while it asks, and is done once it does not */
        bool fed = (hs->gather_for != NULL_UID && hs->gather_for == hs->ss_uid);
        bool feeding = fed && gathering_for_site(hs);
        if(fed && !feeding) {
            hs->gather_for = NULL_UID;
            if(G_Duty_Kind(uid) == DUTY_GATHER && !G_Duty_ByPlayer(uid)) {
                G_Duty_End(uid, "leaves");
            }
        }

        uint32_t resource = target_resource(uid, hs, rname);
        /* Its duty ended on the way: the load was the last of it */
        bool done = hs->drop_off_only || (G_Duty_Kind(uid) != DUTY_GATHER);
        if(done || (!feeding && G_Automation_LeavesGathering(uid, rname, resource))) {
            hs->state = STATE_NOT_HARVESTING;
            hs->ss_uid = NULL_UID;
            hs->res_name = NULL;
            clear_queued_command(uid);
        }else if(G_EntityExists(resource) && !G_EntityIsZombie(resource)) {
            regather(uid, resource);
        }else{
            entity_try_gather_nearest(uid, rname);
        }

    }else{

        G_Harvester_SetCurrCarry(uid, rname, carry - left);
        G_StorageSite_SetCurr(hs->ss_uid, rname, cap);
        sow_if_stocked(hs->ss_uid);

        /* Its site full of the good, the builder goes back to it with the rest in hand */
        if(hs->gather_for != NULL_UID && hs->gather_for == hs->ss_uid && under_construction(hs->gather_for)) {
            back_to_building(uid, hs, hs->gather_for);
            return;
        }
        entity_try_drop_off(uid);
    }
}

/* What the target has room for once the loads already on their way to it
 * are in: a load larger than that would come back */
static int room_at(uint32_t uid, uint32_t dest, const char *rname)
{
    if(!G_EntityExists(dest) || G_EntityIsZombie(dest)
    || !(G_FlagsGet(dest) & ENTITY_FLAG_STORAGE_SITE))
        return 0;
    int room = ss_capacity(dest, rname) - G_StorageSite_GetCurr(dest, rname)
             - amount_en_route(dest, rname, uid, INFINITY);
    return MAX(room, 0);
}

static int dest_room(uint32_t uid, const struct hstate *hs)
{
    return room_at(uid, hs->transport_dest_uid, hs->res_name);
}

static void on_arrive_at_transport_source(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;

    if(!Move_Still(uid))
        return;

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_source);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel);

    struct hstate *hs = hstate_get(uid);
    assert(hs);
    uint32_t ent_flags = G_FlagsGet(uid);

    cart_leg_log(uid, "arrive-src", hs->transport_src_uid, hs->transport_dest_uid);
    if((!G_EntityExists(hs->transport_src_uid) || G_EntityIsZombie(hs->transport_src_uid))
    || !(G_FlagsGet(hs->transport_src_uid) & ENTITY_FLAG_STORAGE_SITE)
    || !M_NavObjAdjacent(s_map, uid, hs->transport_src_uid)
    || G_StorageSite_GetDoNotTake(ent_flags, hs->transport_src_uid)) {

        /* harvester could not reach the storage site, 
         * or the 'do not take' option for it was set */
        cart_leg_log(uid, "src-fail", hs->transport_src_uid, hs->transport_dest_uid);
        uint32_t dest = hs->transport_dest_uid;
        bool job = (hs->job_amount > 0);
        finish_transporing(hs);

        /* An assigned job ends here; the automation gives the next one */
        if(job) {
            G_Duty_End(uid, NULL);
            return;
        }
        /* If the destination is still there, re-try */
        if(G_EntityExists(dest) && !G_EntityIsZombie(dest)) {
            G_Harvester_Transport(uid, dest);
        }
        return;
    }

    G_Provision_OnVisit(uid, hs->transport_src_uid);
    int store_cap = ss_capacity(hs->transport_src_uid, hs->res_name);
    int store_curr = G_StorageSite_GetCurr(hs->transport_src_uid, hs->res_name);
    int desired = ss_wants(hs->transport_src_uid, hs->res_name);
    int excess = store_curr - desired;

    int max_carry = G_Harvester_GetMaxCarry(uid, hs->res_name);
    int curr_carry = G_Harvester_GetCurrCarry(uid, hs->res_name);
    int carry_cap = max_carry - curr_carry;

    /* Figure out how much resource to take. Add it to our carry .
     */
    int spare = source_spare(hs->transport_src_uid, hs->transport_dest_uid, hs->res_name);
    int room = dest_room(uid, hs);
    int take = 0;
    if(hs->job_amount > 0) {
        /* A job tops up with what the pool came to need while the carrier was on its way */
        int need = MAX(hs->job_amount,
            G_Automation_Shortfall(hs->transport_dest_uid, hs->res_name, hs->job_amount));
        take = MIN(carry_cap, MIN(need, source_spare(hs->transport_src_uid, hs->transport_dest_uid, hs->res_name)));
        take = MAX(take, 0);
    }else if(building_site(hs->transport_dest_uid)) {
        take = MAX(MIN(carry_cap, MIN(room, source_spare(hs->transport_src_uid, hs->transport_dest_uid, hs->res_name))), 0);
    }else switch(hs->strategy) {
    case TRANSPORT_STRATEGY_EXCESS: {
        take = MAX(MIN(carry_cap, MIN(room, MIN(excess, spare))), 0);
        break;
    }
    case TRANSPORT_STRATEGY_GATHERING: /* fallthrough */
    case TRANSPORT_STRATEGY_NEAREST: {
        take = MAX(MIN(carry_cap, MIN(room, spare)), 0);
        break;
    }
    default: assert(0);
    }

    G_Harvester_SetCurrCarry(uid, hs->res_name, curr_carry + take);
    G_StorageSite_SetCurr(hs->transport_src_uid, hs->res_name, store_curr - take);

    if(take > 0) {
        E_Entity_Notify(EVENT_RESOURCE_PICKED_UP, uid, NULL, ES_ENGINE);
    }

    if(!G_EntityExists(hs->transport_dest_uid)
    ||  G_EntityIsZombie(hs->transport_dest_uid)) {

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
    if(curr_carry + take < MIN(max_carry, room) && hs->job_amount == 0) {

        uint32_t newsrc = top_up_source(uid, hs, MIN(max_carry, room) - curr_carry - take);
        if(newsrc != NULL_UID) {
            hs->top_ups++;
            E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_transport_source, 
                (void*)((uintptr_t)uid), G_RUNNING);
            E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
                (void*)((uintptr_t)uid), G_RUNNING);

            cart_leg_log(uid, "next-src", newsrc, hs->transport_dest_uid);
            Move_SetSurroundEntity(uid, newsrc);
            hs->transport_src_uid = newsrc;
            return;
        }
    }

    if(curr_carry + take == 0) {
    
        cart_leg_log(uid, "empty-src", hs->transport_src_uid, hs->transport_dest_uid);
        finish_transporing(hs);
        return;
    }

    /* Lastly, drop off our stuff at the destination
     */
    E_Entity_Register(EVENT_MOTION_END, uid, on_arrive_at_transport_dest, 
        (void*)((uintptr_t)uid), G_RUNNING);
    E_Entity_Register(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel, 
        (void*)((uintptr_t)uid), G_RUNNING);

    cart_leg_log(uid, "to-dest", hs->transport_src_uid, hs->transport_dest_uid);
    Move_SetSurroundEntity(uid, hs->transport_dest_uid);
    hs->state = STATE_TRANSPORT_PUTTING;
    E_Entity_Notify(EVENT_TRANSPORT_TARGET_ACQUIRED, uid, 
        (void*)(uintptr_t)hs->transport_dest_uid, ES_ENGINE);
}

static void on_arrive_at_transport_dest(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;

    if(!Move_Still(uid))
        return; 

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_transport_dest);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel);

    struct hstate *hs = hstate_get(uid);
    assert(hs);

    /* Handle the case where we were supplying a resource that 
     * got replenshed> If the unit is capable of gathering it, 
     * begin work immediately.
     */
    uint32_t dest_uid = hs->transport_dest_uid;
    cart_leg_log(uid, "arrive-dest", hs->transport_src_uid, dest_uid);
    if(!G_EntityExists(dest_uid) || G_EntityIsZombie(dest_uid)) {
        finish_transporing(hs);
        return;
    }
    /* Taken over on the way, it is no longer ours to supply */
    if(G_GetFactionID(dest_uid) != G_GetFactionID(uid)) {
        cart_leg_log(uid, "dest-lost", hs->transport_src_uid, dest_uid);
        finish_transporing(hs);
        return;
    }

    if((G_FlagsGet(dest_uid) & ENTITY_FLAG_RESOURCE)
    && !G_Resource_IsReplenishing(dest_uid)
    && G_Harvester_GetGatherSpeed(uid, G_Resource_GetName(dest_uid)) > 0
    && (!(G_FlagsGet(dest_uid) & ENTITY_FLAG_BUILDING) || G_Building_IsCompleted(dest_uid))) {

        finish_transporing(hs);
        G_Harvester_Gather(uid, dest_uid);
        return;
    }

    if(!(G_FlagsGet(dest_uid) & ENTITY_FLAG_STORAGE_SITE)
    || !within_reach(uid, dest_uid)) {
        /* harvester could not reach the destination storage site */
        cart_leg_log(uid, "dest-fail", hs->transport_src_uid, dest_uid);
        note_short_arrival(uid, hs, dest_uid);
        finish_transporing(hs);
        return;
    }
    hs->reach_fails = 0;

    const char *rname = carried_resource_name(hs);
    assert(rname);

    int carry = G_Harvester_GetCurrCarry(uid, rname);
    int cap = ss_capacity(hs->transport_dest_uid, rname);
    int curr = G_StorageSite_GetCurr(hs->transport_dest_uid, rname);
    int left = cap - curr;

    if(left > 0) {
        E_Entity_Notify(EVENT_RESOURCE_DROPPED_OFF, uid, NULL, ES_ENGINE);
    }

    if(hs->trip_start_s) {
        uint32_t now = G_StorageSite_NowS();
        if(now > hs->trip_start_s && now - hs->trip_start_s < MAX_ROUTE_S) {
            G_StorageSite_NoteRoute(hs->transport_dest_uid, now - hs->trip_start_s);
        }
        hs->trip_start_s = 0;
    }

    G_StorageSite_RecordCarried(hs->transport_src_uid, hs->transport_dest_uid, rname, MIN(left, carry));
    if(left >= carry) {
        G_Harvester_SetCurrCarry(uid, rname, 0);
        G_StorageSite_SetCurr(hs->transport_dest_uid, rname, curr + carry);

    }else{

        G_Harvester_SetCurrCarry(uid, rname, carry - left);
        G_StorageSite_SetCurr(hs->transport_dest_uid, rname, cap);
    }

    uint32_t ss_uid = hs->transport_dest_uid;

    if(sow_if_stocked(ss_uid)) {

        finish_transporing(hs);
        if(G_Harvester_GetGatherSpeed(uid, rname) > 0) {
            G_Harvester_Gather(uid, ss_uid);
        }
        return;
    }

    uint32_t dest = hs->transport_dest_uid;
    bool job = (hs->job_amount > 0);
    finish_transporing(hs);

    /* An assigned job ends here; the automation gives the next one */
    if(job) {
        G_Duty_End(uid, NULL);
        if(G_Harvester_GetCurrTotalCarry(uid)) {
            uint32_t store = nearest_storage_site_dropoff(uid, rname);
            if(store != NULL_UID) {
                G_Harvester_DropOff(uid, store);
            }
        }
        return;
    }
    if(dest == G_Harvester_Sowing(uid)) {
        G_Harvester_ResumeSowing(uid);
        return;
    }
    /* A builder that fetched for its site goes back to it */
    if(hs->queued.cmd != CMD_NONE) {
        clear_queued_command(uid);
        return;
    }
    /* A standing supplier goes on while its duty holds */
    struct duty duty;
    if(!G_Duty_Get(uid, &duty) || duty.kind != DUTY_SUPPLY || duty.target != dest)
        return;
    if(!G_Harvester_Transport(uid, dest)) {
        cart_leg_log(uid, "idle", NULL_UID, dest);
    }
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
    if(G_Resource_IsReplenishing(target) || G_Resource_GetDormant(target))
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

        if((G_Harvester_GetMaxCarry(curr, rname) == 0)
        || (G_Harvester_GetGatherSpeed(curr, rname) == 0.0f))
            continue;

        struct hstate *hs = hstate_get(curr);
        assert(hs);

        G_StopEntity(curr, true, true);
        G_Harvester_Gather(curr, target);
        G_NotifyOrderIssued(curr, false, true);
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

        G_StopEntity(curr, true, true);
        G_Harvester_PickUp(curr, target);
        G_NotifyOrderIssued(curr, false, true);
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

        G_StopEntity(curr, true, true);
        G_Harvester_DropOff(curr, target);
        G_NotifyOrderIssued(curr, false, true);
        ncarry++;
    }

    if(ncarry) {
        Entity_Ping(target);
    }
}

/* The selected workers join the city of the hub clicked, bringing in what they carry */
static void selection_join_city(void)
{
    uint32_t target = G_Sel_GetHovered();
    int district = G_EntityExists(target) ? G_District_HubOf(target) : 0;
    if(!district)
        return;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);
    if(sel_type != SELECTION_TYPE_PLAYER)
        return;

    size_t njoined = 0;
    for(int i = 0; i < vec_size(sel); i++) {
        uint32_t curr = vec_AT(sel, i);
        uint32_t flags = G_FlagsGet(curr);
        if(!(flags & ENTITY_FLAG_HARVESTER) || !(flags & ENTITY_FLAG_BUILDER))
            continue;
        if(G_GetFactionID(curr) != G_GetFactionID(target))
            continue;
        G_StopEntity(curr, true, false);
        if(G_Harvester_GetCurrTotalCarry(curr) && (G_FlagsGet(target) & ENTITY_FLAG_STORAGE_SITE)) {
            G_Harvester_BringInLoad(curr, target, false);
        }
        G_Automation_JoinCity(curr, district);
        njoined++;
    }
    if(njoined) {
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

        G_StopEntity(curr, true, true);
        G_Harvester_Transport(curr, target);
        G_NotifyOrderIssued(curr, false, true);
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

    /* While a tool such as placement holds the mouse, a right click gives no order */
    if(right && !G_Move_GetClickEnabled())
        return;

    if(left && !targeting)
        return;

    int action = G_CurrContextualAction();

    if(right && action == CTX_ACTION_JOIN_CITY) {
        selection_join_city();
        return;
    }

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
        int do_not_transport = 0;
        hstate_get_key_int(hs->do_not_transport, rname, &do_not_transport);
        if(do_not_transport)
            continue;

        int desired = ss_wants(target, rname);
        int stored = G_StorageSite_GetCurr(target, rname);

        if(desired > stored) {
            ret = rname;
            break;
        }
    }
    return ret;
}

/* How far a unit has to walk by 'src' to 'dest' */
static float trip_length(uint32_t uid, uint32_t src, uint32_t dest)
{
    vec2_t pos = G_Pos_GetXZ(uid);
    vec2_t src_pos = G_Pos_GetXZ(src);
    vec2_t dest_pos = G_Pos_GetXZ(dest);
    vec2_t a, b;
    PFM_Vec2_Sub(&src_pos, &pos, &a);
    PFM_Vec2_Sub(&dest_pos, &src_pos, &b);
    return PFM_Vec2_Len(&a) + PFM_Vec2_Len(&b);
}

static bool draws_on(uint32_t target, const char *rname)
{
    return !G_StorageSite_GetUseAlt(target) && G_StorageSite_GetDraw(target, rname) > 0.0f;
}

/* What the target eats of the good while the unit fetches it a load by 'src':
 * the stock it would come to, not the stock it has, is what the load is for
 */
static int drawn_over_trip(uint32_t uid, uint32_t src, uint32_t target, const char *rname)
{
    float speed = 0.0f;
    if(!Move_GetEffectiveSpeed(uid, &speed) || speed <= 0.0f)
        return 0;
    float trip_s = trip_length(uid, src, target) / speed;
    return (int)(G_StorageSite_GetDraw(target, rname) * trip_s / 60.0f);
}

/* How far a transporter still has to go to bring its load to its target */
static float distance_left(uint32_t uid, const struct hstate *hs)
{
    bool getting = (hs->state == STATE_TRANSPORT_GETTING && G_EntityExists(hs->transport_src_uid));
    return trip_length(uid, getting ? hs->transport_src_uid : hs->transport_dest_uid, hs->transport_dest_uid);
}

/* How much of the resource other transporters are already bringing to the 
 * target: the loads of those on their way to it and the planned loads of 
 * those still fetching for it; only of those with less than 'within' left
 * to go, and of builders fetching for it, so that a builder fetches what
 * it would bring sooner than the carrier bound for the site, or one who
 * went off to eat on the way */
static int amount_en_route(uint32_t target, const char *rname, uint32_t exclude, float within)
{
    int ret = 0;
    uint32_t uid;
    struct hstate *hs;

    kh_foreach_val_ptr(s_entity_state_table, uid, hs, {

        if(uid == exclude || hs->transport_dest_uid != target)
            continue;
        if(!hs->res_name || strcmp(hs->res_name, rname))
            continue;

        bool fetching_to_build = (hs->queued.cmd == CMD_BUILD && hs->queued.uid_arg == target);
        if(!fetching_to_build && within < INFINITY && distance_left(uid, hs) > within)
            continue;

        int amount = 0;
        if(hs->state == STATE_TRANSPORT_GETTING) {
            amount = hs->job_amount ? hs->job_amount : G_Harvester_GetMaxCarry(uid, rname);
        }else if(hs->state == STATE_TRANSPORT_PUTTING) {
            hstate_get_key_int(hs->curr_carry, rname, &amount);
        }
        ret += amount;
    });
    return ret;
}

/* What will reach the target before one setting out now could bring its own,
 * whoever is bringing it: a builder tired of waiting does not wait on a far load */
static int amount_arriving_within(uint32_t target, const char *rname, uint32_t exclude, float within)
{
    int ret = 0;
    uint32_t uid;
    struct hstate *hs;
    kh_foreach_val_ptr(s_entity_state_table, uid, hs, {
        if(uid == exclude || hs->transport_dest_uid != target)
            continue;
        if(!hs->res_name || strcmp(hs->res_name, rname) || distance_left(uid, hs) > within)
            continue;
        int amount = 0;
        if(hs->state == STATE_TRANSPORT_GETTING) {
            amount = hs->job_amount ? hs->job_amount : G_Harvester_GetMaxCarry(uid, rname);
        }else if(hs->state == STATE_TRANSPORT_PUTTING) {
            hstate_get_key_int(hs->curr_carry, rname, &amount);
        }
        ret += amount;
    });
    return ret;
}

/* Like 'transport_resource', skipping the resources nothing can be fetched 
 * from, so that a site wanting several goods is not starved of all of them by 
 * the first one being out, and the ones other transporters are already 
 * bringing enough of, so that a lower priority one gets its turn */
static const char *transport_resource_with_source(uint32_t uid, struct hstate *hs,
                                                  uint32_t target, enum en_route_rule rule,
                                                  uint32_t *out_src)
{
    const char *best = NULL;
    uint32_t best_src = NULL_UID;
    float best_lack = 0.0f, best_worth = 0.0f;

    for(int i = 0; i < vec_size(&hs->priority); i++) {

        const char *rname = vec_AT(&hs->priority, i);
        int do_not_transport = 0;
        hstate_get_key_int(hs->do_not_transport, rname, &do_not_transport);
        if(do_not_transport)
            continue;

        int wanted = ss_wants(target, rname);
        int stored = G_StorageSite_GetCurr(target, rname);
        bool draws = (rule == EN_ROUTE_ALL) && wanted > 0 && draws_on(target, rname);
        if(!draws && wanted <= stored)
            continue;
        if(!draws && rule == EN_ROUTE_ALL && wanted <= stored + amount_en_route(target, rname, uid, INFINITY))
            continue;
        /* The good the target is shortest of comes first; of goods it is as
         * short of, the one the better load is to be had of for the walk */
        float lack = (wanted - stored) / (float)wanted;
        if(best && lack < best_lack - LACK_SLACK)
            continue;

        float worth = 0.0f;
        uint32_t src = transport_source_worth(uid, target, rname, &worth);
        if(src == NULL_UID)
            continue;
        if(best && lack <= best_lack + LACK_SLACK && worth <= best_worth)
            continue;
        if(draws && wanted <= stored + amount_en_route(target, rname, uid, INFINITY)
                              - drawn_over_trip(uid, src, target, rname))
            continue;
        float within = trip_length(uid, src, target);
        int coming = (rule == EN_ROUTE_BUILDERS) ? amount_arriving_within(target, rname, uid, within)
                                                 : amount_en_route(target, rname, uid, within);
        if(rule != EN_ROUTE_ALL && wanted <= stored + coming)
            continue;
        best = rname;
        best_src = src;
        best_lack = lack;
        best_worth = worth;
    }
    *out_src = best_src;
    return best;
}

static const char *pick_up_resource(struct hstate *hs, uint32_t target)
{
    const char *ret = NULL;
    for(int i = 0; i < vec_size(&hs->priority); i++) {

        const char *rname = vec_AT(&hs->priority, i);
        if(G_StorageSite_Surplus(target, rname) > 0) {
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
    if(!G_EntityExists(newtarget) || G_EntityIsZombie(newtarget)) {
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
        Move_SetSurroundEntity(harvester, newtarget);
    }
}

static void on_arrive_at_resource_source(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    if(!Move_Still(uid))
        return; 

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_arrive_at_resource_source);
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_motion_begin_travel);

    if(!G_EntityExists(hs->transport_dest_uid)
    || G_EntityIsZombie(hs->transport_dest_uid)) {

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

        Move_SetSurroundEntity(uid, hs->res_uid);
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

    if(!G_EntityExists(hs->transport_dest_uid) || G_EntityIsZombie(hs->transport_dest_uid)) {

        finish_harvesting(hs, uid);
        hs->transport_dest_uid = NULL_UID;
        hs->transport_src_uid = NULL_UID;
        return;
    }

    if(!G_EntityExists(hs->res_uid) || (G_FlagsGet(hs->res_uid) & ENTITY_FLAG_ZOMBIE)
    || G_Resource_GetDormant(hs->res_uid)) {
        /* If the resource was exhausted or lies dormant, switch targets to the
         * nearest resource of the same type */
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
    G_StorageSite_RecordProduced(G_GetFactionID(uid), rname, new_carry - old_carry);
    G_StorageSite_RecordGathered(uid, rname, new_carry - old_carry);

    if(resource_left == 0) {

        E_Entity_NotifyImmediate(EVENT_RESOURCE_EXHAUSTED, hs->res_uid, NULL, ES_ENGINE);
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

        Move_SetSurroundEntity(uid, hs->transport_dest_uid);
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
    struct valid_resource_arg arg = (struct valid_resource_arg){
        .ent_pos = pos,
        .ent_layer = Entity_NavLayer(harvester),
        .ent_water = !!(G_FlagsGet(harvester) & ENTITY_FLAG_WATER),
        .rname = rname,
        .exclude = UID_NONE,
    };
    uint32_t resource = G_Pos_NearestWithPred(pos, valid_resource, (void*)&arg, 0);
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
        Move_SetSurroundEntity(harvester, resource);
    }

    return true;
}

/* Gathers at another node under the order it gathers by */
static bool regather(uint32_t uid, uint32_t resource)
{
    struct hstate *hs = hstate_get(uid);
    vec2_t order_pos = hs->order_pos;
    uint32_t failed[MAX_FAILED];
    int nfailed = hs->nfailed;
    memcpy(failed, hs->failed, sizeof(failed));
    bool ret = G_Harvester_Gather(uid, resource);
    hs->order_pos = order_pos;
    hs->nfailed = nfailed;
    memcpy(hs->failed, failed, sizeof(failed));
    return ret;
}

static void on_render_ui(void *user, void *event)
{
    struct sval setting;
    ss_e status;
    (void)status;

    status = Settings_Get("pf.debug.show_harvester_state", &setting);
    assert(status == SS_OKAY);
    if(!setting.as_bool)
        return;

    struct camera *cam = G_GetActiveCamera();
    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view); 
    Camera_MakeProjMat(cam, &proj);

    uint32_t uid;
    struct hstate *hs;
    kh_foreach_val_ptr(s_entity_state_table, uid, hs, {

        vec4_t center = (vec4_t){0.0f, 0.0f, 0.0f, 1.0f};
        mat4x4_t model;
        Entity_ModelMatrix(uid, &model);

        char text[16];
        pf_snprintf(text, sizeof(text), "[%u] %d", uid, hs->state);
        N_RenderOverlayText(text, center, &model, &view, &proj);
    });
}

/* The arrival a harvester on its way waits for, if it has what that arrival needs */
static void (*awaited_arrival(const struct hstate *hs))(void*, void*)
{
    switch(hs->state) {
    case STATE_HARVESTING_SEEK_RESOURCE:
        return (hs->res_uid != NULL_UID) ? on_arrive_at_resource : NULL;
    case STATE_HARVESTING_SEEK_STORAGE:
        return (hs->ss_uid != NULL_UID) ? on_arrive_at_storage : NULL;
    case STATE_TRANSPORT_GETTING:
        return on_arrive_at_transport_source;
    case STATE_TRANSPORT_PUTTING:
        return on_arrive_at_transport_dest;
    case STATE_TRANSPORT_SEEK_RESOURCE:
        return on_arrive_at_resource_source;
    default:
        return NULL;
    }
}

static bool transporting(const struct hstate *hs)
{
    switch(hs->state) {
    case STATE_TRANSPORT_GETTING:
    case STATE_TRANSPORT_PUTTING:
    case STATE_TRANSPORT_SEEK_RESOURCE:
    case STATE_TRANSPORT_HARVESTING:
        return true;
    default:
        return false;
    }
}

/* The building a harvester on its way is making for, or NULL_UID */
static uint32_t making_for(const struct hstate *hs)
{
    switch(hs->state) {
    case STATE_HARVESTING_SEEK_STORAGE:
        return hs->ss_uid;
    case STATE_TRANSPORT_GETTING:
        return hs->transport_src_uid;
    case STATE_TRANSPORT_PUTTING:
        return hs->transport_dest_uid;
    default:
        return NULL_UID;
    }
}

/* A building walled in or gone since it was set off for is given up, so that
 * the unit brings its load where it can and takes other work, rather than
 * pressing against the walls for good */
static void give_up_unreachable(uint32_t tick)
{
    uint32_t stranded[256];
    size_t nstranded = 0;

    uint32_t uid;
    struct hstate *hs;
    kh_foreach_val_ptr(s_entity_state_table, uid, hs, {
        if(uid % REACH_CHECK_TICKS != tick % REACH_CHECK_TICKS)
            continue;
        uint32_t target = making_for(hs);
        if(target == NULL_UID || Move_Still(uid))
            continue;
        /* Gone from under it, such as a road site laid since */
        bool gone = !G_EntityExists(target) || G_EntityIsZombie(target);
        if(!gone && !(G_FlagsGet(target) & ENTITY_FLAG_BUILDING))
            continue;
        if(!gone && storage_site_reachable(uid, target))
            continue;
        if(nstranded < ARR_SIZE(stranded)) {
            stranded[nstranded++] = uid;
        }
    });

    for(int i = 0; i < nstranded; i++) {
        if(!hstate_get(stranded[i]) || G_EntityIsZombie(stranded[i]))
            continue;
        uint32_t target = making_for(hstate_get(stranded[i]));
        bool gone = !G_EntityExists(target) || G_EntityIsZombie(target);
        G_Automation_TraceFollowOn(stranded[i], "walled", gone ? "gone" : "unreachable", "",
            gone ? NULL_UID : target);
        G_Harvester_Stop(stranded[i]);
        Move_Stop(stranded[i]);
    }
}

/* Way is made by coming closer to the target or by walking well off: a unit
 * jostled back and forth in a crowd makes none */
static bool making_way(uint32_t uid, struct hstate *hs, uint32_t target)
{
    vec2_t pos = G_Pos_GetXZ(uid);
    /* A new leg is measured from where it starts */
    if(hs->press_ticks == 0 || hs->press_target != target) {
        hs->press_pos = pos;
        hs->press_ticks = 0;
        hs->press_target = target;
    }
    vec2_t delta;
    PFM_Vec2_Sub(&pos, &hs->press_pos, &delta);
    bool closer = false;
    if(G_EntityExists(target)) {
        vec2_t goal = G_Pos_GetXZ(target);
        vec2_t now, then;
        PFM_Vec2_Sub(&goal, &pos, &now);
        PFM_Vec2_Sub(&goal, &hs->press_pos, &then);
        closer = PFM_Vec2_Len(&now) + PRESS_STEP < PFM_Vec2_Len(&then);
    }
    if(!closer && PFM_Vec2_Len(&delta) <= PRESS_FAR)
        return false;
    hs->press_pos = pos;
    hs->press_ticks = 0;
    return true;
}

/* A gatherer kept from a resource by the others at it never comes to rest, so
 * it would never hear that it cannot get in and go on to the next one: one
 * that makes no way for a while is stopped where it is, which does */
static void give_up_crowded(void)
{
    uint32_t crowded[256];
    size_t ncrowded = 0;

    uint32_t uid;
    struct hstate *hs;
    kh_foreach_val_ptr(s_entity_state_table, uid, hs, {
        if(hs->state != STATE_HARVESTING_SEEK_RESOURCE || Move_Still(uid))
            continue;
        if(making_way(uid, hs, hs->res_uid))
            continue;
        if(++hs->press_ticks < PRESS_GIVE_UP_TICKS)
            continue;
        hs->press_ticks = 0;
        if(ncrowded < ARR_SIZE(crowded)) {
            crowded[ncrowded++] = uid;
        }
    });

    for(int i = 0; i < ncrowded; i++) {
        struct hstate *curr = hstate_get(crowded[i]);
        if(!curr || G_EntityIsZombie(crowded[i]) || !G_EntityExists(curr->res_uid))
            continue;
        if(M_NavObjAdjacent(s_map, crowded[i], curr->res_uid))
            continue;
        G_Automation_TraceFollowOn(crowded[i], "crowded", "next", curr->res_name, curr->res_uid);
        Move_Stop(crowded[i]);
        on_arrive_at_resource((void*)((uintptr_t)crowded[i]), NULL);
    }
}

static uint32_t awaited_target(const struct hstate *hs)
{
    switch(hs->state) {
    case STATE_HARVESTING_SEEK_STORAGE:  return hs->ss_uid;
    case STATE_TRANSPORT_GETTING:        return hs->transport_src_uid;
    case STATE_TRANSPORT_PUTTING:        return hs->transport_dest_uid;
    case STATE_TRANSPORT_SEEK_RESOURCE:  return hs->res_uid;
    default:                             return NULL_UID;
    }
}

/* A unit held up on its way to a store never comes to rest either, when the
 * ground it was bound for closed behind it or a crowd keeps it off: one that
 * makes no way for a while is stopped and taken as arrived, which delivers
 * from within reach of the store, or sends it off again from farther out */
static void give_up_stalled(void)
{
    uint32_t stalled[256];
    size_t nstalled = 0;

    uint32_t uid;
    struct hstate *hs;
    kh_foreach_val_ptr(s_entity_state_table, uid, hs, {
        if(hs->state == STATE_HARVESTING_SEEK_RESOURCE || !awaited_arrival(hs) || Move_Still(uid))
            continue;
        if(making_way(uid, hs, awaited_target(hs)))
            continue;
        if(++hs->press_ticks < STALL_GIVE_UP_TICKS)
            continue;
        hs->press_ticks = 0;
        if(nstalled < ARR_SIZE(stalled)) {
            stalled[nstalled++] = uid;
        }
    });

    for(int i = 0; i < nstalled; i++) {
        struct hstate *curr = hstate_get(stalled[i]);
        if(!curr || G_EntityIsZombie(stalled[i]))
            continue;
        void (*handler)(void*, void*) = awaited_arrival(curr);
        if(!handler)
            continue;
        G_Automation_TraceFollowOn(stalled[i], "stalled", "arrive", curr->res_name, awaited_target(curr));
        Move_Stop(stalled[i]);
        handler((void*)((uintptr_t)stalled[i]), NULL);
    }
}

/* A unit can come to rest without the motion end its arrival waits for: it was
 * still moving when that came, or never set off. It is taken as arrived once it
 * has stood still a while, which sends it on, or back on its way. */
static void on_20hz_tick(void *user, void *event)
{
    give_up_unreachable(s_tick++);
    give_up_crowded();
    give_up_stalled();

    uint32_t arrived[256];
    size_t narrived = 0;

    uint32_t uid;
    struct hstate *hs;
    kh_foreach_val_ptr(s_entity_state_table, uid, hs, {
        if(!awaited_arrival(hs) || !Move_Still(uid)) {
            hs->still_ticks = 0;
            continue;
        }
        if(++hs->still_ticks < STILL_ARRIVAL_TICKS)
            continue;
        hs->still_ticks = 0;
        if(narrived < ARR_SIZE(arrived)) {
            arrived[narrived++] = uid;
        }
    });

    for(int i = 0; i < narrived; i++) {
        struct hstate *curr = hstate_get(arrived[i]);
        if(!curr || G_EntityIsZombie(arrived[i]))
            continue;
        void (*arrival)(void*, void*) = awaited_arrival(curr);
        if(arrival) {
            arrival((void*)((uintptr_t)arrived[i]), NULL);
        }
    }
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
    E_Global_Register(EVENT_RENDER_UI, on_render_ui, NULL, G_RUNNING);
    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
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
    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
    E_Global_Unregister(EVENT_RENDER_UI, on_render_ui);
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

size_t G_Harvester_GetGatherable(uint32_t uid, size_t maxout, const char *out[])
{
    struct hstate *hs = hstate_get(uid);
    if(!hs)
        return 0;
    size_t ret = 0;
    const char *key;
    float speed;
    kh_foreach(hs->gather_speeds, key, speed, {
        if(ret == maxout)
            break;
        if(speed > 0.0f) {
            out[ret++] = key;
        }
    });
    return ret;
}

uint32_t G_Harvester_NearestResource(uint32_t uid, vec2_t origin, const char *rname, float radius)
{
    struct valid_resource_arg arg = (struct valid_resource_arg){
        .ent_pos = G_Pos_GetXZ(uid),
        .ent_layer = Entity_NavLayer(uid),
        .ent_water = !!(G_FlagsGet(uid) & ENTITY_FLAG_WATER),
        .rname = rname,
        .exclude = UID_NONE
    };
    return G_Pos_NearestWithPred(origin, valid_resource, (void*)&arg, radius);
}

size_t G_Harvester_ResourcesNear(uint32_t uid, vec2_t origin, const char *rname, float radius,
                                 size_t maxout, uint32_t out[])
{
    struct valid_resource_arg arg = (struct valid_resource_arg){
        .ent_pos = G_Pos_GetXZ(uid),
        .ent_layer = Entity_NavLayer(uid),
        .ent_water = !!(G_FlagsGet(uid) & ENTITY_FLAG_WATER),
        .rname = rname,
        .exclude = UID_NONE,
        .skip_reach = true
    };
    /* The circle holds every entity before the test, a field's wheat and
     * the units about included, so the search takes many more than it keeps */
    uint32_t found[4096];
    size_t nfound = G_Pos_EntsInCircleWithPred(origin, radius, found, ARR_SIZE(found),
        valid_resource, (void*)&arg);
    size_t ret = MIN(nfound, maxout);
    memcpy(out, found, sizeof(uint32_t) * ret);
    return ret;
}

bool G_Harvester_ResourceReachable(uint32_t uid, uint32_t resource)
{
    return resource_reachable(Entity_NavLayer(uid), !!(G_FlagsGet(uid) & ENTITY_FLAG_WATER),
        G_Pos_GetXZ(uid), resource);
}

size_t G_Harvester_AimedTargets(size_t maxout, uint32_t out[])
{
    size_t ret = 0;
    uint32_t uid;
    struct hstate *hs;
    kh_foreach_val_ptr(s_entity_state_table, uid, hs, {
        (void)uid;
        if(ret < maxout && hs->res_uid != NULL_UID
        && (hs->state == STATE_HARVESTING_SEEK_RESOURCE || hs->state == STATE_HARVESTING
            || hs->state == STATE_HARVESTING_SEEK_STORAGE)) {
            out[ret++] = hs->res_uid;
        }
    });
    return ret;
}

static bool gather_dest_valid(uint32_t uid, uint32_t site, const char *rname)
{
    if(site == NULL_UID || !G_EntityExists(site) || G_EntityIsZombie(site))
        return false;
    if(!(G_FlagsGet(site) & ENTITY_FLAG_STORAGE_SITE))
        return false;
    if(G_GetFactionID(site) != G_GetFactionID(uid))
        return false;
    return G_StorageSite_Accepts(site, rname);
}

/* Each gatherer's good and the site it brings its next load to: the one it is
 * on its way to, else the last it dropped a load at, else the nearest that
 * takes the good */
size_t G_Harvester_DebugDesc(uint32_t uid, char *out, size_t len)
{
    struct hstate *hs = hstate_get(uid);
    if(!hs)
        return pf_snprintf(out, len, "none");
    return pf_snprintf(out, len, "state=%d src=%u dest=%u res_uid=%u ss=%u job=%d still_ticks=%d press_ticks=%d queued=%d",
        (int)hs->state, hs->transport_src_uid, hs->transport_dest_uid, hs->res_uid, hs->ss_uid,
        hs->job_amount, hs->still_ticks, hs->press_ticks, (int)hs->queued.cmd);
}

int G_Harvester_CountBound(uint32_t dest, const char *rname)
{
    if(!s_entity_state_table)
        return 0;
    int ret = 0;
    uint32_t uid;
    struct hstate *hs;
    kh_foreach_val_ptr(s_entity_state_table, uid, hs, {
        (void)uid;
        if(hs->state < STATE_TRANSPORT_GETTING || hs->transport_dest_uid != dest)
            continue;
        if(!hs->res_name || strcmp(hs->res_name, rname))
            continue;
        ret++;
    });
    return ret;
}

int G_Harvester_CountGathering(int faction, const char *rname)
{
    int ret = 0;
    uint32_t uid;
    struct hstate *hs;
    kh_foreach_val_ptr(s_entity_state_table, uid, hs, {
        if(hs->state != STATE_HARVESTING && hs->state != STATE_HARVESTING_SEEK_RESOURCE
        && hs->state != STATE_HARVESTING_SEEK_STORAGE)
            continue;
        if(!hs->res_name || strcmp(hs->res_name, rname))
            continue;
        if(G_GetFactionID(uid) != faction)
            continue;
        ret++;
    });
    return ret;
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

bool G_Harvester_SetDoNotTransport(uint32_t uid, const char *rname, bool set)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return hstate_set_key_int(hs->do_not_transport, rname, set);
}

bool G_Harvester_GetDoNotTransport(uint32_t uid, const char *rname)
{
    int ret = 0;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hstate_get_key_int(hs->do_not_transport, rname, &ret);
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

static bool gather_order(uint32_t harvester, uint32_t resource)
{
    struct hstate *hs = hstate_get(harvester);
    assert(hs);

    uint32_t resource_flags = G_FlagsGet(resource);
    if(!(resource_flags & ENTITY_FLAG_RESOURCE))
        return false;
    if((resource_flags & ENTITY_FLAG_BUILDING) && !G_Building_IsCompleted(resource))
        return false;
    if(G_Resource_GetDormant(resource))
        return false;

    const char *rname = G_Resource_GetName(resource);
    hs->sowing = NULL_UID;
    if(!(s_for_site && G_Duty_Kind(harvester) == DUTY_BUILD)) {
        G_Duty_Set(harvester, DUTY_GATHER, drop_site_for(harvester, resource, rname), rname, DUTY_BY_PLAYER);
    }

    if(G_Harvester_GetCurrTotalCarry(harvester)) {

        const char *carryname = carried_resource_name(hs);
        if(strcmp(carryname, G_Resource_GetName(resource))) {
        
            uint32_t target = nearest_storage_site_dropoff(harvester, carryname);
            if(target == NULL_UID)
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
    hs->order_pos = hs->res_last_pos;
    hs->nfailed = 0;
    hs->press_ticks = 0;
    hs->press_pos = G_Pos_GetXZ(harvester);
    hs->res_name = G_Resource_GetName(resource);
    E_Entity_Notify(EVENT_HARVEST_TARGET_ACQUIRED, harvester, 
        (void*)(uintptr_t)resource, ES_ENGINE);

    if(hunted(resource) && in_hunt_range(harvester, resource)) {
        on_arrive_at_resource((void*)((uintptr_t)harvester), NULL);
    }else if(hunted(resource)) {
        E_Entity_Register(EVENT_MOTION_END, harvester, on_arrive_at_resource, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        E_Entity_Register(EVENT_ORDER_ISSUED, harvester, on_motion_begin_travel, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        Move_SetEnterRange(harvester, resource, HUNT_RANGE);
    }else if(M_NavObjAdjacent(s_map, harvester, resource) && (resource_flags & ENTITY_FLAG_COLLISION)) {
        on_arrive_at_resource((void*)((uintptr_t)harvester), NULL);
    }else{
        E_Entity_Register(EVENT_MOTION_END, harvester, on_arrive_at_resource, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        E_Entity_Register(EVENT_ORDER_ISSUED, harvester, on_motion_begin_travel, 
            (void*)((uintptr_t)harvester), G_RUNNING);

        if(!(resource_flags & ENTITY_FLAG_COLLISION)) {
            vec2_t center = G_Pos_GetXZ(resource);
            Move_SetDest(harvester, center, false);
        }else{
            Move_SetSurroundEntity(harvester, resource);
        }
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

bool G_Harvester_BringInLoad(uint32_t harvester, uint32_t site, bool and_back)
{
    struct hstate *hs = hstate_get(harvester);
    if(!hs)
        return false;
    /* A carrier at its transport keeps the load for where it is going */
    if(transporting(hs))
        return false;
    const char *rname = carried_resource_name(hs);
    if(!rname)
        return false;
    /* One on its way to a store with room keeps to it, else each call in
     * turn would send it to whichever store had room that moment */
    if(hs->state == STATE_HARVESTING_SEEK_STORAGE && G_EntityExists(hs->ss_uid)
    && (site == NULL_UID || hs->ss_uid == site || dropoff_has_room(harvester, hs->ss_uid, rname))) {
        hs->drop_off_only = !and_back;
        return true;
    }
    if(site == NULL_UID) {
        site = nearest_storage_site_dropoff(harvester, rname);
        if(site == NULL_UID)
            return false;
    }
    if(!(G_FlagsGet(site) & ENTITY_FLAG_STORAGE_SITE) || !G_StorageSite_Accepts(site, rname))
        return false;
    if(G_StorageSite_GetCurr(site, rname) >= ss_capacity(site, rname))
        return false;
    if(!G_Harvester_DropOff(harvester, site))
        return false;
    hs->drop_off_only = !and_back;
    return true;
}

static bool pick_up_order(uint32_t harvester, uint32_t storage)
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
        Move_SetSurroundEntity(harvester, storage);
    }

    return true;
}

static bool transport(uint32_t harvester, uint32_t storage, enum en_route_rule rule)
{
    struct hstate *hs = hstate_get(harvester);
    assert(hs);

    uint32_t storage_flags = G_FlagsGet(storage);
    if(!(storage_flags & ENTITY_FLAG_STORAGE_SITE)) {

        if((storage_flags & ENTITY_FLAG_RESOURCE)
        && !G_Resource_IsReplenishing(storage)
        && (G_Harvester_GetGatherSpeed(harvester, G_Resource_GetName(storage)) > 0)
        && (!(G_FlagsGet(storage) & ENTITY_FLAG_BUILDING) || G_Building_IsCompleted(storage))) {

            G_Harvester_Gather(harvester, storage);
            return true;
        }
        return false;
    }

    /* One fetching for the resource it gathered out stays its gatherer */
    if(!(s_for_site && (G_Duty_Kind(harvester) == DUTY_BUILD || G_Harvester_Sowing(harvester) == storage))) {
        G_Duty_Set(harvester, DUTY_SUPPLY, storage, NULL, DUTY_BY_PLAYER);
    }

    if(hs->queued.cmd != CMD_NONE) {
        clear_queued_command(harvester);
        return false;
    }

    if(G_Harvester_GetCurrTotalCarry(harvester)) {

        uint32_t drop_off_storage = NULL_UID;
        const char *carryname = carried_resource_name(hs);
        if(!G_StorageSite_Desires(storage, carryname)) {
            drop_off_storage = nearest_storage_site_dropoff(harvester, carryname);
        }else{
            drop_off_storage = storage;
        }

        if(drop_off_storage == NULL_UID)
            return false;

        hs->queued.cmd = CMD_TRANSPORT;
        hs->queued.uid_arg = storage;
        G_Harvester_DropOff(harvester, drop_off_storage);

        return true;
    }

    /* A site drawing on a good may want its next load before its stock is below its request */
    const char *rname = transport_resource(hs, storage);
    if(rname && hs->strategy == TRANSPORT_STRATEGY_GATHERING
    && harvester_transport_from_resources(harvester, storage)) {
        return true;
    }

    uint32_t src = NULL_UID;
    rname = transport_resource_with_source(harvester, hs, storage, rule, &src);
    if(rname == NULL)
        return false;
    /* The source would give it nothing: it would be back here at once */
    if(room_at(harvester, storage, rname) <= 0 || source_spare(src, storage, rname) <= 0)
        return false;

    G_Harvester_Stop(harvester);
    hs->state = STATE_TRANSPORT_GETTING;
    hs->transport_dest_uid = storage;
    hs->transport_src_uid = src;
    hs->res_name = rname;
    E_Entity_Notify(EVENT_TRANSPORT_TARGET_ACQUIRED, harvester, 
        (void*)(uintptr_t)storage, ES_ENGINE);

    hs->trip_start_s = G_StorageSite_NowS();
    cart_leg_log(harvester, "issue", src, storage);
    if(M_NavObjAdjacent(s_map, harvester, src)) {
        on_arrive_at_transport_source((void*)((uintptr_t)harvester), NULL);
    }else{
        E_Entity_Register(EVENT_MOTION_END, harvester, on_arrive_at_transport_source, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        E_Entity_Register(EVENT_ORDER_ISSUED, harvester, on_motion_begin_travel, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        Move_SetSurroundEntity(harvester, src);
    }

    return true;
}

/* Sent to keep a store supplied, the carrier has it for its duty from then
 * on: with nothing to carry yet it stands by the store for its next request */
/* An order's first leg is the useful one; one the unit would go hungry on
 * waits for its meal, and its duty takes it on afterwards */
static bool eat_first_then(uint32_t harvester, uint32_t target, bool ordered)
{
    if(ordered) {
        G_Provision_EatFirst(harvester, target);
    }
    return ordered;
}

bool G_Harvester_Gather(uint32_t harvester, uint32_t resource)
{
    return eat_first_then(harvester, resource, gather_order(harvester, resource));
}

bool G_Harvester_PickUp(uint32_t harvester, uint32_t storage)
{
    return eat_first_then(harvester, storage, pick_up_order(harvester, storage));
}

bool G_Harvester_Transport(uint32_t harvester, uint32_t storage)
{
    if(transport(harvester, storage, EN_ROUTE_ALL))
        return eat_first_then(harvester, storage, true);
    if(G_Harvester_GetCurrTotalCarry(harvester) || s_for_site)
        return false;
    return G_Duty_StandBy(harvester);
}

bool G_Harvester_TransportJob(uint32_t harvester, uint32_t src, uint32_t dest, 
                              const char *rname, int amount)
{
    struct hstate *hs = hstate_get(harvester);
    if(!hs || amount <= 0)
        return false;
    if(G_Harvester_GetCurrTotalCarry(harvester))
        return false;
    if(G_Harvester_GetMaxCarry(harvester, rname) == 0)
        return false;

    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return false;

    G_Harvester_Stop(harvester);
    hs->state = STATE_TRANSPORT_GETTING;
    hs->transport_dest_uid = dest;
    hs->transport_src_uid = src;
    hs->res_name = key;
    hs->job_amount = MIN(amount, G_Harvester_GetMaxCarry(harvester, rname));
    G_Duty_Set(harvester, DUTY_SUPPLY, dest, key, DUTY_BY_PLAYER);
    E_Entity_Notify(EVENT_TRANSPORT_TARGET_ACQUIRED, harvester, 
        (void*)(uintptr_t)dest, ES_ENGINE);

    hs->trip_start_s = G_StorageSite_NowS();
    cart_leg_log(harvester, "job", src, dest);
    if(M_NavObjAdjacent(s_map, harvester, src)) {
        on_arrive_at_transport_source((void*)((uintptr_t)harvester), NULL);
    }else{
        E_Entity_Register(EVENT_MOTION_END, harvester, on_arrive_at_transport_source, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        E_Entity_Register(EVENT_ORDER_ISSUED, harvester, on_motion_begin_travel, 
            (void*)((uintptr_t)harvester), G_RUNNING);
        Move_SetSurroundEntity(harvester, src);
    }
    return true;
}

int G_Harvester_EnRoute(uint32_t dest, const char *rname)
{
    return amount_en_route(dest, rname, NULL_UID, INFINITY);
}

const char *G_Harvester_Gathering(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    if(!hs)
        return NULL;
    if(hs->state != STATE_HARVESTING && hs->state != STATE_HARVESTING_SEEK_RESOURCE
    && hs->state != STATE_HARVESTING_SEEK_STORAGE)
        return NULL;
    return hs->res_name;
}

uint32_t G_Harvester_ResumeNode(uint32_t uid, const char *rname)
{
    struct hstate *hs = hstate_get(uid);
    if(!hs)
        return NULL_UID;
    struct valid_resource_arg arg = (struct valid_resource_arg){
        .ent_pos = G_Pos_GetXZ(uid),
        .ent_layer = Entity_NavLayer(uid),
        .ent_water = !!(G_FlagsGet(uid) & ENTITY_FLAG_WATER),
        .rname = rname,
        .exclude = UID_NONE,
        .failed_of = hs
    };
    /* About the node it was sent to first, then about itself */
    uint32_t ret = G_Pos_NearestWithPred(hs->order_pos, valid_resource, (void*)&arg, ORDER_RADIUS);
    if(ret == NULL_UID) {
        ret = G_Pos_NearestWithPred(G_Pos_GetXZ(uid), valid_resource, (void*)&arg, ORDER_RADIUS);
    }
    return ret;
}

bool G_Harvester_CanSupply(uint32_t uid, uint32_t storage)
{
    struct hstate *hs = hstate_get(uid);
    if(!hs || !G_EntityExists(storage) || !(G_FlagsGet(storage) & ENTITY_FLAG_STORAGE_SITE))
        return false;
    uint32_t src;
    return transport_resource_with_source(uid, hs, storage, EN_ROUTE_ALL, &src) != NULL;
}

const char *G_Harvester_QueuedGather(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    if(!hs || hs->queued.cmd != CMD_GATHER)
        return NULL;
    uint32_t resource = hs->queued.uid_arg;
    if(!G_EntityExists(resource) || G_EntityIsZombie(resource))
        return NULL;
    return G_Resource_GetName(resource);
}

int G_Harvester_BoundFor(uint32_t dest, const char *rname)
{
    int ret = 0;
    for(khiter_t k = kh_begin(s_entity_state_table); k != kh_end(s_entity_state_table); k++) {
        if(!kh_exist(s_entity_state_table, k))
            continue;
        const struct hstate *hs = &kh_value(s_entity_state_table, k);
        bool feeding = (hs->gather_for == dest && hs->state != STATE_NOT_HARVESTING);
        if(!feeding && (hs->transport_dest_uid != dest || hs->state < STATE_TRANSPORT_GETTING))
            continue;
        if(!hs->res_name || strcmp(hs->res_name, rname))
            continue;
        ret++;
    }
    return ret;
}

int G_Harvester_PickupsPending(uint32_t src, const char *rname)
{
    int ret = 0;
    uint32_t uid;
    struct hstate *hs;

    kh_foreach_val_ptr(s_entity_state_table, uid, hs, {
        if(hs->transport_src_uid != src || hs->state != STATE_TRANSPORT_GETTING)
            continue;
        if(!hs->res_name || strcmp(hs->res_name, rname))
            continue;
        ret += hs->job_amount ? hs->job_amount : G_Harvester_GetMaxCarry(uid, rname);
    });
    return ret;
}

bool G_Harvester_CanReachSite(uint32_t harvester, uint32_t site)
{
    return storage_site_reachable(harvester, site);
}

bool G_Harvester_JobInfo(uint32_t harvester, uint32_t *out_src, uint32_t *out_dest, 
                         const char **out_rname, int *out_state)
{
    struct hstate *hs = hstate_get(harvester);
    if(!hs)
        return false;
    *out_src = hs->transport_src_uid;
    *out_dest = hs->transport_dest_uid;
    *out_rname = hs->res_name;
    *out_state = hs->state;
    return true;
}

int G_Harvester_JobAmount(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    return hs ? hs->job_amount : 0;
}

uint32_t G_Harvester_GatheringFor(uint32_t harvester)
{
    struct hstate *hs = hstate_get(harvester);
    return (hs && gathering_for_site(hs)) ? hs->gather_for : NULL_UID;
}

uint32_t G_Harvester_GatherNode(uint32_t harvester)
{
    struct hstate *hs = hstate_get(harvester);
    return hs ? hs->res_uid : NULL_UID;
}

bool G_Harvester_SupplyBuilding(uint32_t harvester, uint32_t building, bool impatient)
{
    s_for_site = true;
    bool ret = supply_building(harvester, building, impatient);
    s_for_site = false;
    return ret;
}

static bool supply_building(uint32_t harvester, uint32_t building, bool impatient)
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

    if(!transport(harvester, building, impatient ? EN_ROUTE_BUILDERS : EN_ROUTE_SOONER)) {
        return false;
    }

    hs->queued.cmd = CMD_BUILD;
    hs->queued.uid_arg = building;
    return true;
}

bool G_Harvester_GatherForSite(uint32_t harvester, uint32_t building)
{
    struct hstate *hs = hstate_get(harvester);
    if(!hs || G_Harvester_GetCurrTotalCarry(harvester))
        return false;
    const char *rname = transport_resource(hs, building);
    if(!rname)
        return false;
    return G_Harvester_GatherGoodFor(harvester, building, rname);
}

bool G_Harvester_GatherGoodFor(uint32_t harvester, uint32_t site, const char *rname)
{
    struct hstate *hs = hstate_get(harvester);
    if(!hs || G_Harvester_GetCurrTotalCarry(harvester) > G_Harvester_GetCurrCarry(harvester, rname))
        return false;
    if(G_Harvester_GetGatherSpeed(harvester, rname) <= 0)
        return false;
    uint32_t node = G_Harvester_NearestResource(harvester, G_Pos_GetXZ(site), rname, 0.0f);
    if(node == NULL_UID)
        return false;
    /* One taken off its gathering leaves it first, its load kept */
    if(hs->state != STATE_NOT_HARVESTING) {
        G_Harvester_Stop(harvester);
    }
    s_for_site = true;
    bool ret = G_Harvester_Gather(harvester, node);
    s_for_site = false;
    if(!ret)
        return false;
    hs->gather_for = site;
    return true;
}

bool G_Harvester_ReplenishResource(uint32_t harvester, uint32_t resource)
{
    struct hstate *hs = hstate_get(harvester);
    assert(hs);

    if(G_Harvester_GetCurrTotalCarry(harvester)
    && !G_StorageSite_Desires(resource, carried_resource_name(hs))) {

        const char *carryname = carried_resource_name(hs);
        uint32_t nearest = nearest_storage_site_dropoff(harvester, carryname);

        if(nearest == NULL_UID)
            return false;

        hs->queued.cmd = CMD_REPLENISH;
        hs->queued.uid_arg = resource;

        G_Harvester_DropOff(harvester, nearest);
        return true;
    }
    /* With nothing to sow it with, the harvester does not stand by: it goes on to another node */
    s_for_site = true;
    bool ret = G_Harvester_Transport(harvester, resource);
    s_for_site = false;
    return ret;
}

uint32_t G_Harvester_Sowing(uint32_t harvester)
{
    struct hstate *hs = hstate_get(harvester);
    if(!hs || hs->sowing == NULL_UID || G_Duty_Kind(harvester) != DUTY_GATHER)
        return NULL_UID;
    if(!G_EntityExists(hs->sowing) || G_EntityIsZombie(hs->sowing)
    || !(G_FlagsGet(hs->sowing) & ENTITY_FLAG_RESOURCE))
        return NULL_UID;
    return hs->sowing;
}

bool G_Harvester_ResumeSowing(uint32_t harvester)
{
    struct hstate *hs = hstate_get(harvester);
    uint32_t resource = G_Harvester_Sowing(harvester);
    if(resource == NULL_UID)
        return false;
    /* Replenished by whoever brought the last of it, it is gathered again */
    if(!G_Resource_IsReplenishing(resource)) {
        hs->sowing = NULL_UID;
        G_Duty_SetWaiting(harvester, false);
        return G_Harvester_Gather(harvester, resource);
    }
    if(hs->state != STATE_NOT_HARVESTING)
        return true;
    if(G_Harvester_ReplenishResource(harvester, resource)) {
        G_Duty_SetWaiting(harvester, false);
        return true;
    }
    wait_to_sow(harvester, hs);
    return true;
}

uint32_t G_Harvester_SupplyingSite(uint32_t harvester)
{
    struct hstate *hs = hstate_get(harvester);
    if(!hs)
        return NULL_UID;
    /* A command left queued by a trip given up is not followed */
    if(hs->queued.cmd == CMD_BUILD && G_Harvester_TransportTarget(harvester) == hs->queued.uid_arg)
        return hs->queued.uid_arg;
    if(hs->queued.cmd == CMD_SUPPLY && hs->state == STATE_HARVESTING_SEEK_STORAGE)
        return hs->queued.uid_arg;
    return NULL_UID;
}

uint32_t G_Harvester_TransportTarget(uint32_t harvester)
{
    struct hstate *hs = hstate_get(harvester);
    assert(hs);

    if(hs->state == STATE_TRANSPORT_GETTING
    || hs->state == STATE_TRANSPORT_PUTTING
    || hs->state == STATE_TRANSPORT_SEEK_RESOURCE
    || hs->state == STATE_TRANSPORT_HARVESTING) {
        return hs->transport_dest_uid;
    }
    return NULL_UID;
}

uint32_t G_Harvester_DropOffSite(uint32_t harvester)
{
    struct hstate *hs = hstate_get(harvester);
    if(!hs || hs->state != STATE_HARVESTING_SEEK_STORAGE || !G_EntityExists(hs->ss_uid))
        return NULL_UID;
    return hs->ss_uid;
}

void G_Harvester_Stop(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    if(hs->state == STATE_HARVESTING) {
        E_Entity_Notify(EVENT_HARVEST_END, uid, NULL, ES_ENGINE);
    }
    hs->state = STATE_NOT_HARVESTING;
    hs->job_amount = 0;
    hs->top_ups = 0;
    hs->gather_for = NULL_UID;
    /* A transport stopped is given up: one left set would keep the load in hand
     * from ever being brought in, and the carrier from eating */
    hs->transport_src_uid = NULL_UID;
    hs->transport_dest_uid = NULL_UID;

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

bool G_Harvester_Idle(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return (hs->state == STATE_NOT_HARVESTING);
}

bool G_Harvester_InTargetMode(void)
{
    return s_gather_on_lclick 
        || s_pick_up_on_lclick
        || s_drop_off_on_lclick 
        || s_transport_on_lclick;
}

int G_Harvester_CurrContextualAction(void)
{
    uint32_t hovered = G_Sel_GetHovered();
    if(!G_EntityExists(hovered) || G_EntityIsZombie(hovered))
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

    if((G_FlagsGet(hovered) & ENTITY_FLAG_RESOURCE)
    && (G_Harvester_GetGatherSpeed(first, G_Resource_GetName(hovered)) > 0)
    && !G_Resource_IsReplenishing(hovered)
    && !G_Resource_GetDormant(hovered))
        return CTX_ACTION_GATHER;

    if((G_FlagsGet(hovered) & ENTITY_FLAG_RESOURCE)
    && !((G_FlagsGet(hovered) & ENTITY_FLAG_BUILDING) && !G_Building_IsCompleted(hovered))
    && (G_Harvester_GetGatherSpeed(first, G_Resource_GetName(hovered)) > 0)
    && !G_Resource_IsReplenishing(hovered)
    && G_Resource_GetDormant(hovered))
        return CTX_ACTION_GATHER_BLOCKED;

    if(G_GetFactionID(hovered) != G_GetFactionID(first))
        return CTX_ACTION_NONE;

    /* A building with a unit queued takes the one sent to it as its ingredient */
    if(G_Garrison_WantsIngredient(hovered))
        return CTX_ACTION_NONE;

    /* A worker sent to a hub becomes one of its city's own */
    if((G_FlagsGet(first) & ENTITY_FLAG_BUILDER) && (G_FlagsGet(hovered) & ENTITY_FLAG_BUILDING)
    && G_Building_IsCompleted(hovered) && G_District_HubOf(hovered))
        return CTX_ACTION_JOIN_CITY;

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
    if(!G_EntityExists(hovered) || G_EntityIsZombie(hovered))
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

        struct attr num_dnt = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr.do_not_transport)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_dnt, "num_dnt"));

        int curr_flag;
        kh_foreach(curr.do_not_transport, curr_key, curr_flag, {
        
            struct attr curr_key_attr = (struct attr){ .type = TYPE_STRING, };
            pf_strlcpy(curr_key_attr.val.as_string, curr_key, sizeof(curr_key_attr.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &curr_key_attr, "curr_key"));

            struct attr curr_flag_attr = (struct attr){
                .type = TYPE_INT,
                .val.as_int = curr_flag
            };
            CHK_TRUE_RET(Attr_Write(stream, &curr_flag_attr, "curr_flag"));
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

        struct attr job_amount = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.job_amount
        };
        CHK_TRUE_RET(Attr_Write(stream, &job_amount, "job_amount"));

        struct attr order_pos = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.order_pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &order_pos, "order_pos"));

        struct attr sowing = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.sowing
        };
        CHK_TRUE_RET(Attr_Write(stream, &sowing, "sowing"));
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
        int num_dnt  = attr.val.as_int;

        for(int j = 0; j < num_dnt; j++) {
        
            struct attr keyattr;
            CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
            CHK_TRUE_RET(keyattr.type == TYPE_STRING);
            const char *key = keyattr.val.as_string;

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            bool val = !!attr.val.as_int;

            G_Harvester_SetDoNotTransport(uid, key, val);
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

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        hs->job_amount = attr.val.as_int;

        hs->order_pos = hs->res_last_pos;
        if(Attr_NextKeyIs(stream, "order_pos")) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_VEC2);
            hs->order_pos = attr.val.as_vec2;
        }
        hs->sowing = NULL_UID;
        if(Attr_NextKeyIs(stream, "sowing")) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            hs->sowing = attr.val.as_int;
        }
        Sched_TryYield();
    };

    return true;
}

