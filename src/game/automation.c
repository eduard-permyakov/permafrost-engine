/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2024 Eduard Permyakov 
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
#define MEM_FILE_SUB MEM_SUB_GAME_AUTOMATION

#include "automation.h"
#include "movement.h"
#include "harvester.h"
#include "builder.h"
#include "combat.h"
#include "position.h"
#include "storage_site.h"
#include "provision.h"
#include "harvester.h"
#include "command.h"
#include "../event.h"
#include "../entity.h"
#include "../settings.h"
#include "../camera.h"
#include "../sched.h"
#include "../perf.h"
#include "../lib/public/khash.h"
#include "../lib/public/stalloc.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/attr.h"

#include <assert.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_AUTOMATION)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_AUTOMATION)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_AUTOMATION)

#define TRANSIENT_STATE_TICKS        (2) 
#define ARR_SIZE(a)                  (sizeof(a)/sizeof((a)[0]))
#define MIN(a, b)                    ((a) < (b) ? (a) : (b))
#define MAX(a, b)                    ((a) > (b) ? (a) : (b))

#define TICK_MS                      (50)
/* How often the waiting carriers are matched to the requests */
#define JOB_PASS_TICKS               (10)
#define MAX_JOBS_PER_PASS            (32)
/* A request goes unserved this long before it says why */
#define BLOCKED_AFTER_MS             (20000)
/* A request that has waited this long counts twice as much */
#define WAIT_DOUBLING_S              (30.0f)
/* The load a request is scored by, so that a large shortfall does not
 * drown out every other one */
#define SCORED_LOAD                  (25)
/* A carrier is not sent for less than this while more could be had, until
 * the request has waited MIN_LOAD_WAIT_S */
#define MIN_LOAD                     (5)
#define MIN_LOAD_WAIT_S              (30.0f)
/* A haul this long waits for a full load */
#define FULL_LOAD_TRAVEL             (400.0f)
#define MAX_RESOURCES                (64)
#define MAX_DEMANDS                  (256)
#define MAX_WORK_SITES               (512)
/* How far a worker will go for work it finds by itself */
#define WORK_RADIUS                  (600.0f)
#define PINNED_LEN                   (32)
/* What every good counts for before anything asks for it, so that idle
 * workers still gather into stock */
#define BASE_NEED                    (10)
/* Stock of a good beyond what is asked for, past which gathering more of it waits */
#define STOCK_TARGET                 (150)
/* A good short by this much more takes one more gatherer */
#define GOOD_PER_GATHERER            (15)
/* A gatherer's output is measured over about this long */
#define RATE_WINDOW_S                (120.0f)
/* What one gatherer is taken to bring in a minute until enough is measured */
#define GATHERER_RATE                (8.0f)
#define MIN_RATE_SAMPLE_S            (30.0f)
/* How far the movement may move an idle walk's destination to where it can be reached */
#define IDLE_DEST_SLACK              (16.0f)
/* Once a second */
#define RATE_SAMPLE_TICKS            (20)
/* Stock past the spare target by at least this much takes no gatherers for
 * a rate either, so that a good used slower than asked for does not pile up */
#define RATE_STOCK_MARGIN            (50)
/* Gatherers leave off to build a site that has fewer builders than this */
#define SITE_BUILDERS                (3)
#define MAX_TRACE_STEPS              (24)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

/* 'IDLE' and 'ACTIVE' are the two core states. 'WAKING' and 
 * 'STOPPING' are transient states used to ensure that there is
 * no spurrious toggles between ACTIVE and IDLE states.
 */
enum worker_state{
    STATE_IDLE,
    STATE_WAKING,
    STATE_ACTIVE,
    STATE_STOPPING
};

struct automation_state{
    enum worker_state state;
    int               transient_ticks;
    bool              automatic_transport;
    uint32_t          transport_target;
    /* A worker left to itself finds its own work: building, then gathering 
     * what the realm is shortest of, or only the good it is pinned to */
    bool              automatic_work;
    char              pinned[PINNED_LEN];
    /* A carrier on District duty serves only the requests of its hub's 
     * district, from sources in it; 0 is Free duty */
    int               home_district;
    /* The good its own search last set it gathering; an order makes it something else */
    char              working[PINNED_LEN];
    /* An idle worker sent to wait somewhere stays idle on the way; not saved, as one
     * loaded walking only shows as busy until it gets there */
    bool              idle_walk;
    vec2_t            idle_dest;
};

/* A site's want of one resource */
struct job_request{
    uint32_t    site;
    const char *rname;
    int         shortfall;
    float       urgency;
};

/* A site that can give some of a resource away */
struct job_source{
    uint32_t site;
    int      surplus;
};

/* How long a request has been waiting, and why, if it has waited too long */
struct pending{
    uint32_t            since_tick;
    enum request_status reason;
    bool                touched;
};

VEC_TYPE(request, struct job_request)
VEC_IMPL(static inline, request, struct job_request)

/* How short a faction is of a good, how many gather it and how many it wants */
struct good_demand{
    int         faction;
    char        rname[PINNED_LEN];
    /* The spare stock it keeps up beyond what is asked for */
    int         target;
    int         need;
    int         workers;
    int         wanted;
    int         surplus;
    int         shortfall;
    bool        stocked;
};

/* A site to be built, and how many builders are on it or on their way */
struct work_site{
    uint32_t uid;
    int      faction;
    int      builders;
    /* Waiting on materials that nothing can bring it now */
    bool     starved;
};

/* What every worker looking for a job this tick weighs, gathered once for all of them */
struct work_snapshot{
    bool               valid;
    uint32_t           tick;
    vec_request_t      requests;
    size_t             ndemands;
    struct good_demand demands[MAX_DEMANDS];
    size_t             nsites;
    struct work_site   sites[MAX_WORK_SITES];
};
VEC_TYPE(source, struct job_source)
VEC_IMPL(static inline, source, struct job_source)

/* A worker's last decision about its work */
struct work_trace{
    uint32_t         tick;
    size_t           nsteps;
    struct work_step steps[MAX_TRACE_STEPS];
};

/* The realm's, or one district's, own say over the gathering of one good */
struct good_priority{
    int   faction;
    /* A district's override of its faction's setting, or 0 for the faction's own */
    int   district;
    char  rname[PINNED_LEN];
    /* Negative where not set */
    float weight;
    int   stock_target;
    /* What its gatherers are to bring in a minute; 0 where not set */
    float gather_rate;
    /* Gathered, and seconds of gatherers' time spent at it, fading over RATE_WINDOW_S */
    float gathered;
    float gatherer_s;
};

VEC_TYPE(priority, struct good_priority)
VEC_IMPL(static inline, priority, struct good_priority)

KHASH_MAP_INIT_INT(state, struct automation_state)
KHASH_MAP_INIT_INT(count, uint32_t);
KHASH_MAP_INIT_INT64(pending, struct pending)
KHASH_MAP_INIT_INT(trace, struct work_trace)

static void on_order_issued(void *user, void *event);
static bool try_assign_transport_job(uint32_t uid, struct automation_state *astate);
static bool try_assign_work(uint32_t uid, struct automation_state *astate);
static bool stocked(int surplus, int shortfall, int target);
static int gatherers_wanted(int surplus, int shortfall, int target, int for_rate);
static int gatherers_for_rate(int faction, const char *rname);
static struct good_priority *priority_get(int faction, int district, const char *rname, bool create);
static float good_weight(int faction, int district, const char *rname);
static int stock_target(int faction, const char *rname);
static vec2_t work_origin(uint32_t uid, const struct automation_state *astate);
static void reconsider_work(uint32_t uid, struct automation_state *astate);
static struct work_snapshot *snapshot_get(void);
static struct good_demand *demand_get(struct work_snapshot *snap, int faction, const char *rname);
static void trace_note(uint32_t uid, const char *kind, const char *verdict, const char *why, 
                       const char *rname, uint32_t target);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(state) *s_entity_state_table;
/* Maps storage sites to the number of automated transporters servicing it */
static khash_t(count) *s_transport_count;
/* Units whose work ended this tick, to be given a job or let go idle */
static vec_entity_t    s_stopping;
/* Keyed by site and resource */
static khash_t(pending)*s_pending;
static uint32_t        s_tick;
static struct work_snapshot s_snapshot;
static khash_t(trace)  *s_traces;
/* Settings of the realms and their districts, few enough to look up in turn */
static vec_priority_t  s_priorities;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct automation_state *astate_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;
    return &kh_value(s_entity_state_table, k);
}

static bool astate_set(uint32_t uid, struct automation_state as)
{
    int status;
    khiter_t k = kh_put(state, s_entity_state_table, uid, &status);
    if(status == -1 || status == 0)
        return false;
    kh_value(s_entity_state_table, k) = as;
    return true;
}

static void astate_remove(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
}

/* The record of the decision being made about the worker this tick, begun 
 * afresh by its first step, or by a 'fresh' one */
static struct work_trace *trace_get(uint32_t uid, bool fresh)
{
    if(!(G_FlagsGet(uid) & (ENTITY_FLAG_HARVESTER | ENTITY_FLAG_BUILDER)))
        return NULL;
    int status;
    khiter_t k = kh_put(trace, s_traces, uid, &status);
    if(status == -1)
        return NULL;
    struct work_trace *ret = &kh_value(s_traces, k);
    if(status != 0 || fresh || ret->tick != s_tick) {
        ret->tick = s_tick;
        ret->nsteps = 0;
    }
    return ret;
}

static void trace_push(uint32_t uid, struct work_step step)
{
    struct work_trace *trace = trace_get(uid, false);
    if(!trace || trace->nsteps == MAX_TRACE_STEPS)
        return;
    trace->steps[trace->nsteps++] = step;
}

static void trace_note(uint32_t uid, const char *kind, const char *verdict, const char *why, 
                       const char *rname, uint32_t target)
{
    struct work_step step = (struct work_step){
        .kind = kind,
        .verdict = verdict,
        .why = why,
        .target = target
    };
    pf_strlcpy(step.rname, rname ? rname : "", sizeof(step.rname));
    trace_push(uid, step);
}

static void trace_good(uint32_t uid, const char *verdict, const char *why, const char *rname,
                       const struct good_demand *demand, float score)
{
    struct work_step step = (struct work_step){
        .kind = "good",
        .verdict = verdict,
        .why = why,
        .target = NULL_UID,
        .need = demand ? demand->need : 0,
        .workers = demand ? demand->workers : 0,
        .wanted = demand ? demand->wanted : 0,
        .stock = demand ? demand->surplus : 0,
        .stock_target = demand ? demand->shortfall + demand->target : 0,
        .score = score
    };
    pf_strlcpy(step.rname, rname, sizeof(step.rname));
    trace_push(uid, step);
}

/* The carrying job the worker was just given */
static void trace_carry_taken(uint32_t uid)
{
    uint32_t src = NULL_UID, dest = NULL_UID;
    const char *rname = NULL;
    int state;
    G_Harvester_JobInfo(uid, &src, &dest, &rname, &state);
    trace_note(uid, "carry", "taken", "request", rname, dest);
}

/* The goods the worker weighed, the most wanted first */
static void trace_goods(uint32_t uid, size_t ncandidates, const char *candidates[], 
                        struct good_demand *demands[], const float ranks[], const char *why[])
{
    bool done[MAX_RESOURCES] = {0};
    for(int n = 0; n < ncandidates; n++) {
        int next = -1;
        for(int i = 0; i < ncandidates; i++) {
            if(!done[i] && (next == -1 || ranks[i] > ranks[next]))
                next = i;
        }
        done[next] = true;
        const char *verdict = !strcmp(why[next], "chosen") ? "taken"
                            : (!strcmp(why[next], "no_node") || !strcmp(why[next], "unreachable")) ? "failed"
                            : "skipped";
        trace_good(uid, verdict, why[next], candidates[next], demands[next], MAX(ranks[next], 0.0f));
    }
}

/* Whether the unit is still on the walk it was sent on while idle, not on another order */
static bool idle_walking(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate || !astate->idle_walk)
        return false;
    vec2_t dest;
    bool attack;
    if(!G_Move_GetDest(uid, &dest, &attack) || attack) {
        astate->idle_walk = false;
        return false;
    }
    vec2_t delta;
    PFM_Vec2_Sub(&dest, &astate->idle_dest, &delta);
    if(PFM_Vec2_Len(&delta) > IDLE_DEST_SLACK) {
        astate->idle_walk = false;
        return false;
    }
    return true;
}

static bool idle(uint32_t uid)
{
    uint32_t flags = G_FlagsGet(uid);
    if(flags & ENTITY_FLAG_GARRISONED)
        return true;
    /* On its way into a building, to be made into something else */
    if((flags & ENTITY_FLAG_GARRISON) && G_Garrison_IsIngredient(uid))
        return false;
    /* A combat-held unit is stationary even though its move state is not a still one. */
    if((flags & ENTITY_FLAG_MOVABLE) && !(flags & ENTITY_FLAG_COMBAT_HELD) && !G_Move_Still(uid)
    && !idle_walking(uid))
        return false;
    if((flags & ENTITY_FLAG_HARVESTER) && !G_Harvester_Idle(uid))
        return false;
    if((flags & ENTITY_FLAG_BUILDER) && !G_Builder_Idle(uid))
        return false;
    if((flags & ENTITY_FLAG_COMBATABLE) && !G_Combat_Idle(uid))
        return false;
    return true;
}

static void increment_assigned_transporters(uint32_t site)
{
    khiter_t k = kh_get(count, s_transport_count, site);
    if(k == kh_end(s_transport_count)) {
        int status;
        k = kh_put(count, s_transport_count, site, &status);
        assert(status != -1);
        kh_val(s_transport_count, k) = 0;
    }
    kh_val(s_transport_count, k)++;
}

static void try_decrement_assigned_transporters(uint32_t site)
{
    khiter_t k = kh_get(count, s_transport_count, site);
    if(k != kh_end(s_transport_count)) {
        kh_val(s_transport_count, k)--;
    }
}

static int get_assigned_transporters(uint32_t site)
{
    khiter_t k = kh_get(count, s_transport_count, site);
    if(k == kh_end(s_transport_count))
        return 0;
    return kh_val(s_transport_count, k);
}

/* A unit at the end of its work is given its next job before it is let go
 * idle, so that one with more work to do never shows as idle in between.
 * The job finders may add entities, so this runs outside the table walk.
 */
static void settle_stopping(void)
{
    for(int i = 0; i < vec_size(&s_stopping); i++) {

        uint32_t uid = vec_AT(&s_stopping, i);
        struct automation_state *astate = astate_get(uid);
        if(!astate || astate->state != STATE_STOPPING)
            continue;

        /* Orders queued by the player come before any job, its meal too */
        if(G_Command_Next(uid)) {
            trace_note(uid, "order", "taken", "queued", NULL, NULL_UID);
            astate->state = STATE_ACTIVE;
            continue;
        }

        /* A hungry worker's next job is its meal, and so is one about to be */
        bool due = G_Provision_DueSoon(uid);
        if(due && G_Provision_Fetch(uid)) {
            trace_note(uid, "meal", "taken", "due", NULL, NULL_UID);
            astate->state = STATE_ACTIVE;
            continue;
        }
        trace_note(uid, "meal", due ? "failed" : "skipped", due ? "no_food" : "not_due", NULL, NULL_UID);

        if(try_assign_transport_job(uid, astate) || try_assign_work(uid, astate)) {
            astate->state = STATE_ACTIVE;
            continue;
        }

        E_Global_NotifyImmediate(EVENT_UNIT_IDLE_PENDING, (void*)((uintptr_t)uid), ES_ENGINE);
        astate = astate_get(uid);
        if(!astate || astate->state != STATE_STOPPING)
            continue;

        if(!idle(uid)) {
            trace_note(uid, "script", "taken", "script", NULL, NULL_UID);
            astate->state = STATE_ACTIVE;
            continue;
        }
        astate->state = STATE_IDLE;
        trace_note(uid, "idle", "taken", "no_work", NULL, NULL_UID);
        E_Global_Notify(EVENT_UNIT_BECAME_IDLE, (void*)((uintptr_t)uid), ES_ENGINE);
    }
    vec_entity_reset(&s_stopping);
}

static void recompute_idle(void)
{
    uint32_t uid;
    struct automation_state *astate;

    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {
        
        switch(astate->state) {
        case STATE_IDLE: {
            if(!idle(uid)) {
                astate->state = STATE_WAKING;
                astate->idle_walk = false;
            }else if(astate->idle_walk && G_Move_Still(uid)) {
                astate->idle_walk = false;
            }
            break;
        }
        case STATE_WAKING: {
            if(idle(uid)) {
                astate->transient_ticks = 0;
                astate->state = STATE_IDLE;
                break;
            }
            astate->transient_ticks++;
            if(astate->transient_ticks == TRANSIENT_STATE_TICKS) {
                astate->transient_ticks = 0;
                astate->state = STATE_ACTIVE;
                E_Global_Notify(EVENT_UNIT_BECAME_ACTIVE, (void*)((uintptr_t)uid), ES_ENGINE);
            }
            break;
        }
        case STATE_ACTIVE: {
            if(idle(uid)) {
                astate->state = STATE_STOPPING;
            }
            break;
        }
        case STATE_STOPPING: {
            if(!idle(uid)) {
                astate->transient_ticks = 0;
                astate->state = STATE_ACTIVE;
                break;
            }
            astate->transient_ticks++;
            if(astate->transient_ticks == TRANSIENT_STATE_TICKS) {
                astate->transient_ticks = 0;
                if(astate->transport_target != NULL_UID) {
                    try_decrement_assigned_transporters(astate->transport_target);
                    astate->transport_target = NULL_UID;
                }
                vec_entity_push(&s_stopping, uid);
            }
            break;
        }
        default: assert(0);
        }
    });
}

static uint64_t pending_key(uint32_t site, const char *rname)
{
    return ((uint64_t)site << 32) ^ (uint64_t)(uintptr_t)rname;
}

static struct pending *pending_get(uint32_t site, const char *rname, bool create)
{
    uint64_t key = pending_key(site, rname);
    khiter_t k = kh_get(pending, s_pending, key);
    if(k != kh_end(s_pending))
        return &kh_value(s_pending, k);
    if(!create)
        return NULL;
    int status;
    k = kh_put(pending, s_pending, key, &status);
    if(status == -1)
        return NULL;
    kh_value(s_pending, k) = (struct pending){
        .since_tick = s_tick,
        .reason = REQUEST_OK,
        .touched = true
    };
    return &kh_value(s_pending, k);
}

static float priority_weight(int priority)
{
    switch(priority) {
    case SS_PRIORITY_LOW:    return 0.5f;
    case SS_PRIORITY_URGENT: return 3.0f;
    case SS_PRIORITY_UPKEEP: return 4.0f;
    default:                 return 1.0f;
    }
}

/* The site that stands for its cluster's pool, or itself */
static bool is_representative(uint32_t site)
{
    int cid = G_Cluster_Of(site);
    return (cid == 0 || G_Cluster_Anchor(cid) == site);
}

static uint32_t pool_representative(uint32_t site)
{
    int cid = G_Cluster_Of(site);
    uint32_t anchor = (cid == 0) ? NULL_UID : G_Cluster_Anchor(cid);
    return (anchor == NULL_UID) ? site : anchor;
}

static int pool_en_route(uint32_t site, const char *rname)
{
    int cid = G_Cluster_Of(site);
    if(cid == 0)
        return G_Harvester_EnRoute(site, rname);

    uint32_t members[64];
    size_t nmembers = G_Cluster_Members(cid, ARR_SIZE(members), members);
    int ret = 0;
    for(int i = 0; i < nmembers; i++) {
        ret += G_Harvester_EnRoute(members[i], rname);
    }
    return ret;
}

static int pool_bound_for(uint32_t site, const char *rname)
{
    int cid = G_Cluster_Of(site);
    if(cid == 0)
        return G_Harvester_BoundFor(site, rname);

    uint32_t members[64];
    size_t nmembers = G_Cluster_Members(cid, ARR_SIZE(members), members);
    int ret = 0;
    for(int i = 0; i < nmembers; i++) {
        ret += G_Harvester_BoundFor(members[i], rname);
    }
    return ret;
}

static int pool_pickups_pending(uint32_t site, const char *rname)
{
    int cid = G_Cluster_Of(site);
    if(cid == 0)
        return G_Harvester_PickupsPending(site, rname);

    uint32_t members[64];
    size_t nmembers = G_Cluster_Members(cid, ARR_SIZE(members), members);
    int ret = 0;
    for(int i = 0; i < nmembers; i++) {
        ret += G_Harvester_PickupsPending(members[i], rname);
    }
    return ret;
}

static uint32_t nearest_member(uint32_t site, vec2_t pos)
{
    int cid = G_Cluster_Of(site);
    if(cid == 0)
        return site;
    uint32_t ret = G_Cluster_NearestMember(cid, pos);
    return (ret == NULL_UID) ? site : ret;
}

static float distance(vec2_t a, vec2_t b)
{
    vec2_t delta;
    PFM_Vec2_Sub(&a, &b, &delta);
    return PFM_Vec2_Len(&delta);
}

/* A larder asks for a load, not a unit */
static bool worth_asking(uint32_t site, const char *rname, int want, int shortfall)
{
    enum ss_chip chip = G_StorageSite_GetChip(site, rname);
    if(chip != SS_CHIP_COLLECT && chip != SS_CHIP_RELAY)
        return true;
    if(G_StorageSite_GetKeep(site, rname) <= 0)
        return true;
    if(shortfall >= MIN(MIN_LOAD, MAX(1, want / 2)))
        return true;
    return G_StorageSite_GetCurr(site, rname) == 0;
}

static void collect_requests(vec_request_t *out, const vec_entity_t *sites)
{
    for(khint_t k = kh_begin(s_pending); k != kh_end(s_pending); k++) {
        if(kh_exist(s_pending, k)) {
            kh_value(s_pending, k).touched = false;
        }
    }

    for(int i = 0; i < vec_size(sites); i++) {

        uint32_t site = vec_AT(sites, i);
        if(G_EntityIsZombie(site) || !is_representative(site))
            continue;

        const char *names[MAX_RESOURCES];
        size_t nnames = G_StorageSite_GetStorableResources(site, ARR_SIZE(names), names);
        float weight = priority_weight(G_StorageSite_GetPriority(site));

        for(int j = 0; j < nnames; j++) {

            int want = G_StorageSite_Request(site, names[j]);
            if(want <= 0)
                continue;
            int have = G_StorageSite_GetCurr(site, names[j]) + pool_en_route(site, names[j]);
            int shortfall = want - have;
            if(shortfall <= 0)
                continue;
            if(!worth_asking(site, names[j], want, shortfall))
                continue;

            struct pending *pend = pending_get(site, names[j], true);
            if(!pend)
                continue;
            pend->touched = true;
            float waited_s = (s_tick - pend->since_tick) * TICK_MS / 1000.0f;
            float good_weight = weight;
            if(G_StorageSite_GetChip(site, names[j]) == SS_CHIP_COLLECT
            && G_Provision_IsUpkeepGood(names[j])) {
                good_weight = MAX(weight, priority_weight(SS_PRIORITY_UPKEEP));
            }

            vec_request_push(out, (struct job_request){
                .site = site,
                .rname = names[j],
                .shortfall = shortfall,
                .urgency = good_weight * (1.0f + waited_s / WAIT_DOUBLING_S) * MIN(shortfall, SCORED_LOAD)
            });
        }
    }

    /* Requests that are met are no longer waiting */
    for(khint_t k = kh_begin(s_pending); k != kh_end(s_pending); k++) {
        if(kh_exist(s_pending, k) && !kh_value(s_pending, k).touched) {
            kh_del(pending, s_pending, k);
        }
    }
}

static int compare_requests(const void *a, const void *b)
{
    const struct job_request *ra = a, *rb = b;
    if(ra->urgency > rb->urgency) return -1;
    if(ra->urgency < rb->urgency) return 1;
    return 0;
}

static int source_surplus(uint32_t site, const char *rname, bool for_site)
{
    return for_site ? G_StorageSite_SurplusForSite(site, rname) : G_StorageSite_Surplus(site, rname);
}

static void collect_sources(vec_source_t *out, const vec_entity_t *sites, const char *rname, 
                            int faction, uint32_t exclude)
{
    /* Relays pass goods on to the sites that want them, never to each other */
    bool relaying = G_StorageSite_GetChip(exclude, rname) == SS_CHIP_RELAY;
    bool for_site = G_StorageSite_GetUseAlt(exclude);
    for(int i = 0; i < vec_size(sites); i++) {
        uint32_t site = vec_AT(sites, i);
        if(G_EntityIsZombie(site) || !is_representative(site))
            continue;
        if(G_GetFactionID(site) != faction)
            continue;
        if(site == exclude)
            continue;
        if(relaying && G_StorageSite_GetChip(site, rname) == SS_CHIP_RELAY)
            continue;
        /* Counting the pickups pending walks every carrier, so it is left for
         * the sites with something to spare */
        int surplus = source_surplus(site, rname, for_site);
        if(surplus <= 0)
            continue;
        surplus -= pool_pickups_pending(site, rname);
        if(surplus <= 0)
            continue;
        vec_source_push(out, (struct job_source){ .site = site, .surplus = surplus });
    }
}

/* Why no site can give a request any of its good: there is none outside the
 * buildings being built, all there is is set aside, or all that is spare is
 * already promised to carriers */
static enum request_status no_source_reason(const vec_entity_t *sites, const struct job_request *req)
{
    int faction = G_GetFactionID(req->site);
    bool for_site = G_StorageSite_GetUseAlt(req->site);
    int held = 0, spare = 0;
    for(int i = 0; i < vec_size(sites); i++) {
        uint32_t site = vec_AT(sites, i);
        if(G_EntityIsZombie(site) || !is_representative(site) || site == req->site)
            continue;
        if(G_GetFactionID(site) != faction || G_StorageSite_GetUseAlt(site))
            continue;
        held += G_StorageSite_GetCurr(site, req->rname);
        spare += source_surplus(site, req->rname, for_site);
    }
    if(held == 0)
        return REQUEST_NO_SOURCE;
    return (spare > 0) ? REQUEST_PROMISED : REQUEST_KEPT;
}

static bool carrier_can_take(uint32_t carrier, const char *rname)
{
    if(G_Harvester_GetMaxCarry(carrier, rname) == 0)
        return false;
    if(G_Harvester_GetDoNotTransport(carrier, rname))
        return false;
    return true;
}

static void note_blocked(const struct job_request *req, enum request_status reason)
{
    struct pending *pend = pending_get(req->site, req->rname, false);
    if(!pend)
        return;
    if((s_tick - pend->since_tick) * TICK_MS < BLOCKED_AFTER_MS)
        return;
    bool changed = (pend->reason != reason);
    pend->reason = reason;
    if(changed) {
        E_Global_Notify(EVENT_REQUEST_BLOCKED, (void*)(uintptr_t)req->site, ES_ENGINE);
    }
}

/* The load a haul this long is worth, any at all once the request has waited long enough */
static int min_load_for(float travel, int max_carry, int shortfall, float waited_s)
{
    float fullness = MIN(travel / FULL_LOAD_TRAVEL, 1.0f);
    int want = MAX(MIN_LOAD, (int)(max_carry * fullness));
    float patience = MIN_LOAD_WAIT_S * (1.0f + fullness);
    if(waited_s >= patience)
        want = 1;
    return MIN(want, shortfall);
}

/* Gives the waiting carriers the most pressing requests: each request takes 
 * the carrier and source that get its goods to it soonest. Only a pass over 
 * all the waiting carriers can 'judge' why a request goes unserved. */
static void assign_jobs(vec_entity_t *carriers, bool judge)
{
    if(vec_size(carriers) == 0)
        return;
    PERF_ENTER();

    vec_entity_t sites;
    vec_entity_init(&sites);
    G_StorageSite_GetAll(&sites);

    vec_request_t requests;
    vec_request_init(&requests);
    PERF_PUSH("automation::collect_requests");
    collect_requests(&requests, &sites);
    PERF_POP();
    qsort(requests.array, vec_size(&requests), sizeof(struct job_request), compare_requests);

    vec_source_t sources;
    vec_source_init(&sources);
    int njobs = 0;

    for(int i = 0; i < vec_size(&requests) && njobs < MAX_JOBS_PER_PASS; i++) {

        /* With no carrier left there is nothing more to give out, and only a
         * judging pass looks further */
        if(vec_size(carriers) == 0 && !judge)
            break;

        struct job_request *req = &vec_AT(&requests, i);
        int faction = G_GetFactionID(req->site);
        vec2_t dest_pos = G_Pos_GetXZ(req->site);

        vec_source_reset(&sources);
        collect_sources(&sources, &sites, req->rname, faction, req->site);

        bool any_carrier = false;
        bool any_reachable = false;
        bool any_district_bar = false;
        bool any_small = false;
        int dest_district = G_District_Of(req->site);
        struct pending *req_pend = pending_get(req->site, req->rname, false);
        float waited_s = req_pend ? (s_tick - req_pend->since_tick) * TICK_MS / 1000.0f : 0.0f;

        while(req->shortfall > 0 && vec_size(carriers) > 0 && njobs < MAX_JOBS_PER_PASS) {

            int best_carrier = -1, best_source = -1;
            float best_travel = 0.0f;

            for(int c = 0; c < vec_size(carriers); c++) {

                uint32_t carrier = vec_AT(carriers, c);
                if(G_GetFactionID(carrier) != faction)
                    continue;
                if(!carrier_can_take(carrier, req->rname))
                    continue;
                struct automation_state *cstate = astate_get(carrier);
                int home = cstate ? cstate->home_district : 0;
                if(home && dest_district != home) {
                    any_district_bar = true;
                    continue;
                }
                any_carrier = true;
                vec2_t cpos = G_Pos_GetXZ(carrier);
                uint32_t flags = G_FlagsGet(carrier);
                int max_carry = G_Harvester_GetMaxCarry(carrier, req->rname);

                for(int s = 0; s < vec_size(&sources); s++) {
                    struct job_source *src = &vec_AT(&sources, s);
                    vec2_t spos = G_Pos_GetXZ(src->site);
                    float travel = distance(cpos, spos) + distance(spos, dest_pos);
                    if(src->surplus < min_load_for(travel, max_carry, req->shortfall, waited_s)) {
                        any_small = any_small || src->surplus > 0;
                        continue;
                    }
                    if(home && G_District_Of(src->site) != home) {
                        any_district_bar = true;
                        continue;
                    }
                    if(G_StorageSite_GetDoNotTake(flags, src->site))
                        continue;
                    if(best_carrier == -1 || travel < best_travel) {
                        uint32_t srcm = nearest_member(src->site, cpos);
                        uint32_t dstm = nearest_member(req->site, spos);
                        if(!G_Harvester_CanReachSite(carrier, srcm)
                        || !G_Harvester_CanReachSite(carrier, dstm))
                            continue;
                        best_carrier = c;
                        best_source = s;
                        best_travel = travel;
                    }
                }
            }

            if(best_carrier == -1)
                break;
            any_reachable = true;

            uint32_t carrier = vec_AT(carriers, best_carrier);
            struct job_source *src = &vec_AT(&sources, best_source);
            vec2_t cpos = G_Pos_GetXZ(carrier);
            uint32_t srcm = nearest_member(src->site, cpos);
            uint32_t dstm = nearest_member(req->site, G_Pos_GetXZ(srcm));
            int amount = MIN(req->shortfall, MIN(src->surplus, G_Harvester_GetMaxCarry(carrier, req->rname)));

            vec_entity_del(carriers, best_carrier);
            if(!G_Harvester_TransportJob(carrier, srcm, dstm, req->rname, amount))
                continue;

            struct automation_state *astate = astate_get(carrier);
            if(astate) {
                if(astate->transport_target != NULL_UID) {
                    try_decrement_assigned_transporters(astate->transport_target);
                }
                increment_assigned_transporters(dstm);
                astate->transport_target = dstm;
            }
            req->shortfall -= amount;
            src->surplus -= amount;
            njobs++;

            /* A request waits from its last load, so that one left unserved 
             * comes to outweigh the ones being served */
            struct pending *pend = pending_get(req->site, req->rname, false);
            if(pend) {
                pend->since_tick = s_tick;
            }
        }

        if(!judge)
            continue;

        /* Waiting for a load worth the trip, or for one on its way, is not being blocked */
        if(req->shortfall > 0 && !any_reachable && !any_small 
        && !pool_bound_for(req->site, req->rname)) {
            if(vec_size(&sources) == 0) {
                note_blocked(req, no_source_reason(&sites, req));
            }else if(!any_reachable && any_district_bar) {
                note_blocked(req, REQUEST_NOT_CONNECTED);
            }else if(!any_carrier) {
                note_blocked(req, REQUEST_NO_CARRIER);
            }else if(!any_reachable) {
                note_blocked(req, REQUEST_UNREACHABLE);
            }
        }else{
            struct pending *pend = pending_get(req->site, req->rname, false);
            if(pend) {
                pend->reason = REQUEST_OK;
            }
        }
    }

    vec_source_destroy(&sources);
    vec_request_destroy(&requests);
    vec_entity_destroy(&sites);
    PERF_RETURN_VOID();
}

static bool waiting_carrier(uint32_t uid, struct automation_state *astate)
{
    if(!(G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER))
        return false;
    if(G_FlagsGet(uid) & ENTITY_FLAG_GARRISONED)
        return false;
    if(!astate->automatic_transport)
        return false;
    if(G_Harvester_GetCurrTotalCarry(uid))
        return false;
    if(G_Provision_WantsFood(uid))
        return false;
    return true;
}

struct site_search{
    int faction;
    /* Only sites with fewer builders than this, or any with 0 */
    int max_builders;
    /* Only sites that have their materials and can be worked on */
    bool supplied;
};

/* Whether a site being built still lacks a good that no site of its faction can spare */
static bool site_starved(uint32_t uid, const vec_entity_t *sites)
{
    if(!G_Building_IsFounded(uid) || G_Building_IsSupplied(uid))
        return false;
    int faction = G_GetFactionID(uid);
    const char *names[MAX_RESOURCES];
    size_t nnames = G_StorageSite_GetStorableResources(uid, ARR_SIZE(names), names);
    for(int i = 0; i < nnames; i++) {
        int want = G_StorageSite_Request(uid, names[i]);
        if(want - G_StorageSite_GetCurr(uid, names[i]) - pool_en_route(uid, names[i]) <= 0)
            continue;
        int spare = 0;
        for(int j = 0; j < vec_size(sites) && spare <= 0; j++) {
            uint32_t src = vec_AT(sites, j);
            if(src == uid || G_EntityIsZombie(src) || !is_representative(src) || G_GetFactionID(src) != faction)
                continue;
            spare += G_StorageSite_SurplusForSite(src, names[i]) - pool_pickups_pending(src, names[i]);
        }
        if(spare <= 0)
            return true;
    }
    return false;
}

static bool unfinished_site(uint32_t uid)
{
    if(!G_EntityExists(uid) || !(G_FlagsGet(uid) & ENTITY_FLAG_BUILDING) || G_EntityIsZombie(uid))
        return false;
    return G_Building_IsMarked(uid) && !G_Building_IsCompleted(uid);
}

/* The nearest of this tick's unfinished sites within reach that the search admits */
static struct work_site *nearest_site(struct work_snapshot *snap, vec2_t pos, 
                                      const struct site_search *ss)
{
    struct work_site *ret = NULL;
    float best = WORK_RADIUS;
    for(int i = 0; i < snap->nsites; i++) {
        struct work_site *curr = &snap->sites[i];
        if(curr->faction != ss->faction)
            continue;
        if(ss->max_builders && curr->builders >= ss->max_builders)
            continue;
        vec2_t delta, site_pos = G_Pos_GetXZ(curr->uid);
        PFM_Vec2_Sub(&site_pos, &pos, &delta);
        float dist = PFM_Vec2_Len(&delta);
        if(dist > best)
            continue;
        if(!unfinished_site(curr->uid))
            continue;
        if(ss->supplied && !G_Building_IsSupplied(curr->uid))
            continue;
        if(!ss->supplied && curr->starved)
            continue;
        best = dist;
        ret = curr;
    }
    return ret;
}

/* The total of the faction's requests for a resource, met by gathering */
static int resource_need(int faction, const char *rname, const vec_request_t *requests)
{
    int ret = BASE_NEED;
    for(int i = 0; i < vec_size(requests); i++) {
        const struct job_request *req = &vec_AT(requests, i);
        if(G_GetFactionID(req->site) != faction)
            continue;
        if(strcmp(req->rname, rname))
            continue;
        ret += req->shortfall;
    }
    return ret;
}

/* How many may gather the good: as many as bring in its rate, and one more 
 * for each GOOD_PER_GATHERER still short of what is asked for and the stock 
 * beyond it, so that a lone wanted good does not draw everyone */
static int gatherers_wanted(int surplus, int shortfall, int target, int for_rate)
{
    int short_by = shortfall + target - surplus;
    if(short_by <= -MAX(target, RATE_STOCK_MARGIN))
        return 0;
    if(short_by <= 0)
        return for_rate;
    return for_rate + 1 + short_by / GOOD_PER_GATHERER;
}

/* What one of the faction's gatherers brings in of the good a minute, walks
 * to and from the store included */
static float gatherer_rate(const struct good_priority *realm)
{
    if(!realm || realm->gatherer_s < MIN_RATE_SAMPLE_S || realm->gathered <= 0.0f)
        return GATHERER_RATE;
    return realm->gathered * 60.0f / realm->gatherer_s;
}

static int gatherers_for_rate(int faction, const char *rname)
{
    const struct good_priority *realm = priority_get(faction, 0, rname, false);
    if(!realm || realm->gather_rate <= 0.0f)
        return 0;
    return (int)ceilf(realm->gather_rate / gatherer_rate(realm));
}

static void sample_gather_rates(float dt_s)
{
    float fade = expf(-dt_s / RATE_WINDOW_S);
    for(int i = 0; i < vec_size(&s_priorities); i++) {
        struct good_priority *curr = &vec_AT(&s_priorities, i);
        if(curr->district != 0)
            continue;
        curr->gathered *= fade;
        curr->gatherer_s = curr->gatherer_s * fade 
                         + G_Harvester_CountGathering(curr->faction, curr->rname) * dt_s;
    }
}

/* Whether the faction can spare enough of the good, beyond what is asked for */
static bool stocked(int surplus, int shortfall, int target)
{
    return surplus >= shortfall + target;
}

static struct good_priority *priority_get(int faction, int district, const char *rname, bool create)
{
    for(int i = 0; i < vec_size(&s_priorities); i++) {
        struct good_priority *curr = &vec_AT(&s_priorities, i);
        if(curr->faction == faction && curr->district == district && !strcmp(curr->rname, rname))
            return curr;
    }
    if(!create)
        return NULL;
    struct good_priority prio = (struct good_priority){
        .faction = faction,
        .district = district,
        .weight = -1.0f,
        .stock_target = -1
    };
    pf_strlcpy(prio.rname, rname, sizeof(prio.rname));
    if(!vec_priority_push(&s_priorities, prio))
        return NULL;
    return &vec_AT(&s_priorities, vec_size(&s_priorities) - 1);
}

/* How much the good's gathering is favoured where the worker works: by its 
 * district's own weight if it has one, else by its faction's */
static float good_weight(int faction, int district, const char *rname)
{
    const struct good_priority *own = district ? priority_get(-1, district, rname, false) : NULL;
    if(own && own->weight >= 0.0f)
        return own->weight;
    const struct good_priority *realm = priority_get(faction, 0, rname, false);
    return (realm && realm->weight >= 0.0f) ? realm->weight : 1.0f;
}

static int stock_target(int faction, const char *rname)
{
    const struct good_priority *realm = priority_get(faction, 0, rname, false);
    return (realm && realm->stock_target >= 0) ? realm->stock_target : STOCK_TARGET;
}

/* The district a worker's work counts for: its home, else where it stands */
static int work_district(uint32_t uid, const struct automation_state *astate)
{
    return astate->home_district ? astate->home_district : G_District_Of(uid);
}

/* Where a worker looks for work: about its home hub when it is away from its
 * district, else about itself */
static vec2_t work_origin(uint32_t uid, const struct automation_state *astate)
{
    vec2_t pos = G_Pos_GetXZ(uid);
    if(astate->home_district == 0 || G_District_Of(uid) == astate->home_district)
        return pos;
    uint32_t anchor = G_Cluster_Anchor(astate->home_district);
    if(anchor == NULL_UID || !G_EntityExists(anchor))
        return pos;
    return G_Pos_GetXZ(anchor);
}

/* Drops the gathering a worker's new standing orders no longer cover */
static void reconsider_work(uint32_t uid, struct automation_state *astate)
{
    if(!astate->automatic_work || !(G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER))
        return;
    uint32_t src, dest;
    const char *rname;
    int state;
    if(!G_Harvester_JobInfo(uid, &src, &dest, &rname, &state))
        return;
    if(src != NULL_UID || dest != NULL_UID || !rname)
        return;
    bool off_good = astate->pinned[0] && strcmp(astate->pinned, rname);
    bool off_home = astate->home_district && G_District_Of(uid) != astate->home_district;
    if(!off_good && !off_home)
        return;
    G_Harvester_Stop(uid);
    try_assign_work(uid, astate);
}

static struct work_snapshot *snapshot_get(void)
{
    if(s_snapshot.valid && s_snapshot.tick == s_tick)
        return &s_snapshot;
    PERF_ENTER();

    vec_entity_t sites;
    vec_entity_init(&sites);
    G_StorageSite_GetAll(&sites);
    vec_request_reset(&s_snapshot.requests);
    collect_requests(&s_snapshot.requests, &sites);

    s_snapshot.nsites = 0;
    for(int i = 0; i < vec_size(&sites) && s_snapshot.nsites < MAX_WORK_SITES; i++) {
        uint32_t uid = vec_AT(&sites, i);
        if(!unfinished_site(uid))
            continue;
        s_snapshot.sites[s_snapshot.nsites++] = (struct work_site){
            .uid = uid,
            .faction = G_GetFactionID(uid),
            .builders = G_Builder_CountOn(uid),
            .starved = site_starved(uid, &sites)
        };
    }
    vec_entity_destroy(&sites);

    s_snapshot.ndemands = 0;
    s_snapshot.tick = s_tick;
    s_snapshot.valid = true;
    PERF_RETURN(&s_snapshot);
}

static struct good_demand *demand_get(struct work_snapshot *snap, int faction, const char *rname)
{
    for(int i = 0; i < snap->ndemands; i++) {
        struct good_demand *curr = &snap->demands[i];
        if(curr->faction == faction && !strcmp(curr->rname, rname))
            return curr;
    }
    if(snap->ndemands == MAX_DEMANDS)
        return NULL;

    int surplus, shortfall;
    G_Automation_Balance(faction, rname, &surplus, &shortfall);
    int target = stock_target(faction, rname);

    struct good_demand *ret = &snap->demands[snap->ndemands++];
    *ret = (struct good_demand){
        .faction = faction,
        .target = target,
        .need = resource_need(faction, rname, &snap->requests),
        .workers = G_Harvester_CountGathering(faction, rname),
        .wanted = gatherers_wanted(surplus, shortfall, target, gatherers_for_rate(faction, rname)),
        .surplus = surplus,
        .shortfall = shortfall,
        .stocked = stocked(surplus, shortfall, target)
    };
    pf_strlcpy(ret->rname, rname, sizeof(ret->rname));
    return ret;
}

/* Why the nearest unfinished site, if any, took no builder */
static void note_passed_site(uint32_t uid, struct work_snapshot *snap, vec2_t pos, int faction)
{
    struct work_site *nearest = NULL;
    float best = 0.0f;
    for(int i = 0; i < snap->nsites; i++) {
        struct work_site *curr = &snap->sites[i];
        if(curr->faction != faction || !unfinished_site(curr->uid))
            continue;
        float dist = distance(G_Pos_GetXZ(curr->uid), pos);
        if(nearest == NULL || dist < best) {
            nearest = curr;
            best = dist;
        }
    }
    if(nearest == NULL) {
        trace_note(uid, "build", "skipped", "no_site", NULL, NULL_UID);
        return;
    }
    const char *why = (best > WORK_RADIUS) ? "far"
                    : nearest->starved ? "no_material"
                    : "crewed";
    trace_note(uid, "build", "skipped", why, NULL, nearest->uid);
}

static bool try_assign_work(uint32_t uid, struct automation_state *astate)
{
    /* Held in a building, as a defender or to be made into something else */
    if(G_FlagsGet(uid) & ENTITY_FLAG_GARRISONED) {
        trace_note(uid, "work", "skipped", "inside", NULL, NULL_UID);
        return false;
    }
    if(!astate->automatic_work) {
        trace_note(uid, "work", "skipped", "manual", NULL, NULL_UID);
        return false;
    }
    if(G_Provision_WantsFood(uid)) {
        trace_note(uid, "work", "skipped", "hungry", NULL, NULL_UID);
        return false;
    }

    uint32_t flags = G_FlagsGet(uid);
    int faction = G_GetFactionID(uid);
    vec2_t pos = work_origin(uid, astate);
    struct work_snapshot *snap = snapshot_get();

    if((flags & ENTITY_FLAG_BUILDER) && astate->pinned[0]) {
        trace_note(uid, "build", "skipped", "pinned", astate->pinned, NULL_UID);
    }else if(flags & ENTITY_FLAG_BUILDER) {
        /* Builders spread over the sites that can be worked on; one waiting on its 
         * materials gets one to found it, and the rest gather and carry them */
        struct site_search arg = (struct site_search){ 
            .faction = faction, 
            .max_builders = SITE_BUILDERS,
            .supplied = true
        };
        struct work_site *site = nearest_site(snap, pos, &arg);
        if(!site) {
            arg = (struct site_search){ .faction = faction, .max_builders = 1 };
            site = nearest_site(snap, pos, &arg);
        }
        if(site && G_Builder_Build(uid, site->uid)) {
            trace_note(uid, "build", "taken", "site", NULL, site->uid);
            site->builders++;
            return true;
        }
        if(site) {
            trace_note(uid, "build", "failed", "unreachable", NULL, site->uid);
        }else{
            note_passed_site(uid, snap, pos, faction);
        }
    }

    if(!(flags & ENTITY_FLAG_HARVESTER))
        return false;
    /* A load left in hand is brought in before anything else */
    if(G_Harvester_GetCurrTotalCarry(uid)) {
        bool ret = G_Harvester_BringInLoad(uid, NULL_UID);
        trace_note(uid, "load", ret ? "taken" : "failed", "in_hand", NULL, NULL_UID);
        return ret;
    }

    const char *candidates[MAX_RESOURCES];
    size_t ncandidates;
    if(astate->pinned[0]) {
        candidates[0] = astate->pinned;
        ncandidates = 1;
    }else{
        ncandidates = G_Harvester_GetGatherable(uid, ARR_SIZE(candidates), candidates);
    }
    if(ncandidates == 0)
        return false;

    /* Most wanted per worker already at it, weighted by how fast this one
     * gathers it, so that a specialist keeps to his trade, and by how much 
     * the realm or the worker's district favours it */
    int district = work_district(uid, astate);
    float scores[MAX_RESOURCES];
    struct good_demand *demands[MAX_RESOURCES];
    const char *why[MAX_RESOURCES];
    for(int i = 0; i < ncandidates; i++) {
        demands[i] = demand_get(snap, faction, candidates[i]);
        bool pinned = !strcmp(astate->pinned, candidates[i]);
        float weight = good_weight(faction, district, candidates[i]);
        if(!pinned && weight <= 0.0f) {
            scores[i] = -1.0f;
            why[i] = "not_wanted";
            continue;
        }
        int wanted = demands[i] ? (int)ceilf(demands[i]->wanted * MAX(weight, 1.0f)) : 0;
        if(!demands[i] || (!pinned && demands[i]->workers >= wanted)) {
            scores[i] = -1.0f;
            why[i] = (demands[i] && demands[i]->stocked) ? "stocked" : "enough";
            continue;
        }
        scores[i] = weight * demands[i]->need * G_Harvester_GetGatherSpeed(uid, candidates[i]) 
                  / (1.0f + demands[i]->workers);
        why[i] = "lower";
    }
    float ranks[MAX_RESOURCES];
    memcpy(ranks, scores, sizeof(float) * ncandidates);
    int chosen = -1;

    for(int tries = 0; tries < ncandidates; tries++) {
        int best = -1;
        for(int i = 0; i < ncandidates; i++) {
            if(scores[i] < 0.0f)
                continue;
            if(best == -1 || scores[i] > scores[best])
                best = i;
        }
        if(best == -1)
            break;
        uint32_t node = G_Harvester_NearestResource(uid, pos, candidates[best], WORK_RADIUS);
        /* Nothing near: go as far as it takes rather than stand idle */
        if(node == NULL_UID && astate->home_district == 0) {
            node = G_Harvester_NearestResource(uid, pos, candidates[best], 0.0f);
        }
        /* One of no district goes by the weight of the district it would gather in */
        bool pinned = !strcmp(astate->pinned, candidates[best]);
        int at = (node != NULL_UID) ? G_District_At(faction, false, G_Pos_GetXZ(node), NULL) : 0;
        if(node != NULL_UID && astate->home_district == 0 && !pinned
        && good_weight(faction, at, candidates[best]) <= 0.0f) {
            why[best] = "not_wanted";
            scores[best] = -1.0f;
            continue;
        }
        if(node != NULL_UID && G_Harvester_Gather(uid, node)) {
            pf_strlcpy(astate->working, candidates[best], sizeof(astate->working));
            why[best] = "chosen";
            chosen = best;
            break;
        }
        why[best] = (node == NULL_UID) ? "no_node" : "unreachable";
        scores[best] = -1.0f;
    }

    trace_goods(uid, ncandidates, candidates, demands, ranks, why);
    if(chosen == -1)
        return false;
    demands[chosen]->workers++;
    return true;
}

/* Why the worker is not a carrier waiting for a job */
static const char *not_waiting_why(uint32_t uid, struct automation_state *astate)
{
    if(G_FlagsGet(uid) & ENTITY_FLAG_GARRISONED)
        return "inside";
    if(!astate->automatic_transport)
        return "off";
    if(G_Harvester_GetCurrTotalCarry(uid))
        return "laden";
    return "hungry";
}

static bool try_assign_transport_job(uint32_t uid, struct automation_state *astate)
{
    if(!waiting_carrier(uid, astate)) {
        trace_note(uid, "carry", "skipped", not_waiting_why(uid, astate), NULL, NULL_UID);
        return false;
    }

    vec_entity_t carriers;
    vec_entity_init(&carriers);
    vec_entity_push(&carriers, uid);
    assign_jobs(&carriers, false);
    bool assigned = (vec_size(&carriers) == 0);
    vec_entity_destroy(&carriers);

    if(assigned) {
        trace_carry_taken(uid);
    }else{
        trace_note(uid, "carry", "skipped", "nothing", NULL, NULL_UID);
    }
    return assigned;
}

static void assign_transport_jobs(void)
{
    vec_entity_t carriers, laden;
    vec_entity_init(&carriers);
    vec_entity_init(&laden);

    uint32_t uid;
    struct automation_state *astate;
    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {
        if(astate->state != STATE_IDLE)
            continue;
        /* A carrier whose drop-off fell through brings the load in itself */
        if(astate->automatic_transport && (G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER)
        && G_Harvester_GetCurrTotalCarry(uid)) {
            vec_entity_push(&laden, uid);
            continue;
        }
        if(!waiting_carrier(uid, astate))
            continue;
        vec_entity_push(&carriers, uid);
    });

    for(int i = 0; i < vec_size(&laden); i++) {
        uint32_t uid = vec_AT(&laden, i);
        bool ret = G_Harvester_BringInLoad(uid, NULL_UID);
        trace_note(uid, "load", ret ? "taken" : "failed", "in_hand", NULL, NULL_UID);
    }

    vec_entity_t waiting;
    vec_entity_init(&waiting);
    vec_entity_copy(&waiting, &carriers);
    assign_jobs(&carriers, true);

    for(int i = 0; i < vec_size(&waiting); i++) {
        uint32_t uid = vec_AT(&waiting, i);
        struct automation_state *astate = astate_get(uid);
        if(astate && astate->transport_target != NULL_UID) {
            trace_carry_taken(uid);
        }else{
            trace_note(uid, "carry", "skipped", "nothing", NULL, NULL_UID);
        }
    }
    vec_entity_destroy(&waiting);
    vec_entity_destroy(&laden);
    vec_entity_destroy(&carriers);
}

static void assign_work(void)
{
    vec_entity_t workers;
    vec_entity_init(&workers);

    uint32_t uid;
    struct automation_state *astate;
    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {
        if(astate->state != STATE_IDLE || !astate->automatic_work)
            continue;
        vec_entity_push(&workers, uid);
    });

    for(int i = 0; i < vec_size(&workers); i++) {
        uint32_t uid = vec_AT(&workers, i);
        struct automation_state *astate = astate_get(uid);
        /* A carrier handed a job in this pass is still counted idle until the next tick */
        if(!astate || astate->state != STATE_IDLE || !idle(uid))
            continue;
        if(!try_assign_work(uid, astate)) {
            trace_note(uid, "idle", "taken", "no_work", NULL, NULL_UID);
        }
    }
    vec_entity_destroy(&workers);
}

static void on_20hz_tick(void *user, void *event)
{
    s_tick++;
    PERF_PUSH("automation::recompute_idle");
    recompute_idle();
    PERF_POP();
    PERF_PUSH("automation::settle_stopping");
    settle_stopping();
    PERF_POP();
    if(s_tick % RATE_SAMPLE_TICKS == 0) {
        sample_gather_rates(RATE_SAMPLE_TICKS / 20.0f);
    }
    if(s_tick % JOB_PASS_TICKS == 0) {
        PERF_PUSH("automation::assign_transport_jobs");
        assign_transport_jobs();
        PERF_POP();
        PERF_PUSH("automation::assign_work");
        assign_work();
        PERF_POP();
    }
}

static void on_update_ui(void *user, void *event)
{
    struct sval setting;
    ss_e status;
    (void)status;

    status = Settings_Get("pf.debug.show_automation_state", &setting);
    assert(status == SS_OKAY);
    if(!setting.as_bool)
        return;

    struct camera *cam = G_GetActiveCamera();
    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view); 
    Camera_MakeProjMat(cam, &proj);

    /* Show relevant storage site state */
    vec_entity_t sites;
    vec_entity_init(&sites);
    G_StorageSite_GetAll(&sites);

    for(int i = 0; i < vec_size(&sites); i++) {

        uint32_t uid = vec_AT(&sites, i);
        vec4_t center = (vec4_t){0.0f, 0.0f, 0.0f, 1.0f};
        mat4x4_t model;
        Entity_ModelMatrix(uid, &model);

        char text[16];
        pf_snprintf(text, sizeof(text), "SITE: [%u] [%u]", uid,
            get_assigned_transporters(uid));
        N_RenderOverlayText(text, center, &model, &view, &proj);
    }
    vec_entity_destroy(&sites);

    /* Show relevant transporter state */
    uint32_t uid;
    struct automation_state *astate;

    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {

        if(!(G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER))
            continue;

        char text[16] = {0};
        switch(astate->state) {
        case STATE_IDLE:
        case STATE_WAKING:
            pf_snprintf(text, sizeof(text), "[%u] IDLE", uid);
            break;
        case STATE_ACTIVE:
        case STATE_STOPPING:
            pf_snprintf(text, sizeof(text), "[%u] ACTIVE", uid);
            break;
        }

        vec4_t center = (vec4_t){0.0f, 0.0f, 0.0f, 1.0f};
        mat4x4_t model;
        Entity_ModelMatrix(uid, &model);
        N_RenderOverlayText(text, center, &model, &view, &proj);

        if(astate->automatic_transport) {
            char text[32];
            pf_snprintf(text, sizeof(text), "AUTO [%u]", astate->transport_target);
            vec4_t off = (vec4_t){0.0f, -10.0f, 0.0f, 1.0f};
            N_RenderOverlayText(text, off, &model, &view, &proj);
        }
    });
}

static void on_order_issued(void *user, void *event)
{
    uint32_t uid = (uintptr_t)event;

    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return;
    astate->working[0] = '\0';
    if(trace_get(uid, true)) {
        trace_note(uid, "order", "taken", "player", NULL, NULL_UID);
    }

    if(!astate->automatic_transport)
        return;

    uint32_t target = G_Harvester_TransportTarget(uid);
    if(astate->transport_target != target) {
        try_decrement_assigned_transporters(astate->transport_target);
        if(target != NULL_UID) {
            increment_assigned_transporters(target);
        }
        astate->transport_target = target;
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Automation_AddEntity(uint32_t uid)
{
    struct automation_state state = (struct automation_state){
        .state = STATE_IDLE,
        .transient_ticks = 0,
        .automatic_transport = false,
        .transport_target = NULL_UID,
        .automatic_work = false,
        .pinned = {0},
        .home_district = 0,
    };
    return astate_set(uid, state);
}

void G_Automation_RemoveEntity(uint32_t uid)
{
    khiter_t k = kh_get(count, s_transport_count, uid);
    if(k != kh_end(s_transport_count)) {
        kh_del(count, s_transport_count, k);
    }

    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return;
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_order_issued);
    astate_remove(uid);

    k = kh_get(trace, s_traces, uid);
    if(k != kh_end(s_traces)) {
        kh_del(trace, s_traces, k);
    }
}

bool G_Automation_Init(void)
{
    if((s_entity_state_table = kh_init(state)) == NULL)
        goto fail_entity_state_table;
    if((s_transport_count = kh_init(count)) == NULL)
        goto fail_transport_count_table;
    vec_entity_init(&s_stopping);
    if((s_pending = kh_init(pending)) == NULL)
        goto fail_pending;
    if((s_traces = kh_init(trace)) == NULL)
        goto fail_traces;
    s_tick = 0;
    vec_request_init(&s_snapshot.requests);
    vec_priority_init(&s_priorities);
    s_snapshot.valid = false;

    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
    E_Global_Register(EVENT_UPDATE_UI, on_update_ui, NULL, G_RUNNING);
    E_Global_Register(EVENT_ORDER_ISSUED, on_order_issued, NULL, G_RUNNING);
    return true;

fail_traces:
    kh_destroy(pending, s_pending);
fail_pending:
    vec_entity_destroy(&s_stopping);
    kh_destroy(count, s_transport_count);
fail_transport_count_table:
    kh_destroy(state, s_entity_state_table);
fail_entity_state_table:
    return false;
}

enum request_status G_Automation_RequestStatus(uint32_t site, const char *rname, int *out_waited_ms)
{
    *out_waited_ms = 0;
    int cid = G_Cluster_Of(site);
    if(cid) {
        site = G_Cluster_Anchor(cid);
    }

    /* Waiting entries are keyed by the interned name the sites report */
    const char *names[MAX_RESOURCES];
    size_t nnames = G_StorageSite_GetStorableResources(site, ARR_SIZE(names), names);
    for(int i = 0; i < nnames; i++) {
        if(strcmp(names[i], rname))
            continue;
        struct pending *pend = pending_get(site, names[i], false);
        if(!pend)
            return REQUEST_OK;
        *out_waited_ms = (s_tick - pend->since_tick) * TICK_MS;
        return pend->reason;
    }
    return REQUEST_OK;
}

size_t G_Automation_GetTrace(uint32_t uid, size_t maxout, struct work_step out[], int *out_age_ms)
{
    *out_age_ms = 0;
    khiter_t k = kh_get(trace, s_traces, uid);
    if(k == kh_end(s_traces))
        return 0;
    const struct work_trace *trace = &kh_value(s_traces, k);
    size_t ret = MIN(trace->nsteps, maxout);
    memcpy(out, trace->steps, ret * sizeof(struct work_step));
    *out_age_ms = (s_tick - trace->tick) * TICK_MS;
    return ret;
}

int G_Automation_AssignedTransporters(uint32_t site)
{
    int cid = G_Cluster_Of(site);
    if(cid == 0)
        return get_assigned_transporters(site);

    uint32_t members[64];
    size_t nmembers = G_Cluster_Members(cid, ARR_SIZE(members), members);
    int ret = 0;
    for(int i = 0; i < nmembers; i++) {
        ret += get_assigned_transporters(members[i]);
    }
    return ret;
}

void G_Automation_Shutdown(void)
{
    E_Global_Unregister(EVENT_ORDER_ISSUED, on_order_issued);
    E_Global_Unregister(EVENT_UPDATE_UI, on_update_ui);
    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
    vec_request_destroy(&s_snapshot.requests);
    vec_priority_destroy(&s_priorities);
    vec_entity_destroy(&s_stopping);
    kh_destroy(trace, s_traces);
    kh_destroy(pending, s_pending);
    kh_destroy(count, s_transport_count);
    kh_destroy(state, s_entity_state_table);
}

void G_Automation_GetIdle(vec_entity_t *out)
{
    size_t ret = 0;

    uint32_t uid;
    struct automation_state *astate;

    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {
        if(astate->state != STATE_IDLE)
            continue;
        vec_entity_push(out, uid);
    });
}

bool G_Automation_WalkIdle(uint32_t uid, vec2_t xz)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate || astate->state != STATE_IDLE || !(G_FlagsGet(uid) & ENTITY_FLAG_MOVABLE))
        return false;
    /* One going to eat is not idle for long, and the walk would call off its meal */
    if(!G_PointInsideMap(xz) || G_Provision_WantsFood(uid) || G_Provision_Fetching(uid))
        return false;
    G_Move_SetDest(uid, xz, false);
    astate->idle_walk = true;
    astate->idle_dest = xz;
    return true;
}

bool G_Automation_IsIdle(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return true;
    return (astate->state == STATE_IDLE);
}

void G_Automation_SetAutomaticTransport(uint32_t uid, bool on)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return;

    assert(G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER);
    bool prev = astate->automatic_transport;

    if(on && !prev) {
        assert(astate->transport_target == NULL_UID);
        uint32_t target = G_Harvester_TransportTarget(uid);
        if(target != NULL_UID) {
            increment_assigned_transporters(target);
            astate->transport_target = target;
        }
    }else if(!on && prev){
        if(astate->transport_target != NULL_UID) {
            try_decrement_assigned_transporters(astate->transport_target);
            astate->transport_target = NULL_UID;
        }
    }
    astate->automatic_transport = on;
}

void G_Automation_SetHomeDistrict(uint32_t uid, int district)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return;
    if(astate->home_district == district)
        return;
    astate->home_district = district;
    reconsider_work(uid, astate);
}

int G_Automation_GetHomeDistrict(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return 0;
    return astate->home_district;
}

void G_Automation_SetAutomaticWork(uint32_t uid, bool on)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return;
    astate->automatic_work = on;
}

bool G_Automation_GetAutomaticWork(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return false;
    return astate->automatic_work;
}

void G_Automation_SetPinnedWork(uint32_t uid, const char *rname)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return;
    if(!strcmp(astate->pinned, rname ? rname : ""))
        return;
    pf_strlcpy(astate->pinned, rname ? rname : "", sizeof(astate->pinned));
    reconsider_work(uid, astate);
}

bool G_Automation_LeavesGathering(uint32_t uid, const char *rname)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate || !astate->automatic_work)
        return false;
    if(strcmp(astate->working, rname) || !strcmp(astate->pinned, rname))
        return false;
    int faction = G_GetFactionID(uid);
    struct work_snapshot *snap = snapshot_get();
    struct good_demand *demand = demand_get(snap, faction, rname);
    /* The few that keep up its rate stay on */
    if(demand && demand->stocked && demand->workers > demand->wanted) {
        demand->workers--;
        return true;
    }
    if(good_weight(faction, work_district(uid, astate), rname) <= 0.0f)
        return true;
    if(astate->pinned[0] || !(G_FlagsGet(uid) & ENTITY_FLAG_BUILDER))
        return false;
    /* Only for a site that can be worked: one waiting on its materials needs gatherers more */
    struct site_search arg = (struct site_search){
        .faction = faction,
        .max_builders = SITE_BUILDERS,
        .supplied = true
    };
    return nearest_site(snap, work_origin(uid, astate), &arg) != NULL;
}

void G_Automation_Balance(int faction, const char *rname, int *out_surplus, int *out_shortfall)
{
    vec_entity_t sites;
    vec_entity_init(&sites);
    G_StorageSite_GetAll(&sites);
    *out_surplus = 0;
    *out_shortfall = 0;
    for(int i = 0; i < vec_size(&sites); i++) {
        uint32_t site = vec_AT(&sites, i);
        if(G_EntityIsZombie(site) || !is_representative(site) || G_GetFactionID(site) != faction)
            continue;
        *out_surplus += G_StorageSite_Surplus(site, rname);
        int want = G_StorageSite_Request(site, rname);
        if(want <= 0)
            continue;
        int have = G_StorageSite_GetCurr(site, rname) + pool_en_route(site, rname);
        *out_shortfall += MAX(want - have, 0);
    }
    vec_entity_destroy(&sites);
}

int G_Automation_Shortfall(uint32_t site, const char *rname, int own_load)
{
    /* A site finished, cancelled or razed while the carrier was on its way */
    if(!G_EntityExists(site) || G_EntityIsZombie(site)
    || !(G_FlagsGet(site) & ENTITY_FLAG_STORAGE_SITE))
        return 0;
    uint32_t rep = pool_representative(site);
    int want = G_StorageSite_Request(rep, rname);
    int have = G_StorageSite_GetCurr(rep, rname) + pool_en_route(rep, rname) - own_load;
    return MAX(want - have, 0);
}

void G_Automation_TraceFollowOn(uint32_t uid, const char *kind, const char *why, 
                                const char *rname, uint32_t target)
{
    if(!trace_get(uid, true))
        return;
    trace_note(uid, kind, "taken", why, rname, target);
}

bool G_Automation_ChoseWork(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    return astate && astate->working[0];
}

const char *G_Automation_GetPinnedWork(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return "";
    return astate->pinned;
}

bool G_Automation_GetAutomaticTransport(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return false;
    return astate->automatic_transport;
}

void G_Automation_SetGoodWeight(int faction, const char *rname, float weight)
{
    struct good_priority *prio = priority_get(faction, 0, rname, true);
    if(prio) {
        prio->weight = MAX(weight, 0.0f);
    }
    s_snapshot.valid = false;
}

float G_Automation_GetGoodWeight(int faction, const char *rname)
{
    return good_weight(faction, 0, rname);
}

void G_Automation_SetStockTarget(int faction, const char *rname, int target)
{
    struct good_priority *prio = priority_get(faction, 0, rname, true);
    if(prio) {
        prio->stock_target = MAX(target, 0);
    }
    s_snapshot.valid = false;
}

int G_Automation_GetStockTarget(int faction, const char *rname)
{
    return stock_target(faction, rname);
}

void G_Automation_SetGatherRate(int faction, const char *rname, float per_min)
{
    struct good_priority *prio = priority_get(faction, 0, rname, true);
    if(prio && prio->gather_rate != MAX(per_min, 0.0f)) {
        prio->gather_rate = MAX(per_min, 0.0f);
        s_snapshot.valid = false;
    }
}

float G_Automation_GetGatherRate(int faction, const char *rname)
{
    const struct good_priority *prio = priority_get(faction, 0, rname, false);
    return prio ? prio->gather_rate : 0.0f;
}

float G_Automation_GathererRate(int faction, const char *rname)
{
    return gatherer_rate(priority_get(faction, 0, rname, false));
}

void G_Automation_NoteGathered(uint32_t uid, const char *rname, int amount)
{
    struct good_priority *prio = priority_get(G_GetFactionID(uid), 0, rname, false);
    if(prio && amount > 0) {
        prio->gathered += amount;
    }
}

void G_Automation_SetDistrictWeight(int district, const char *rname, float weight)
{
    struct good_priority *prio = priority_get(-1, district, rname, weight >= 0.0f);
    if(prio) {
        prio->weight = weight;
    }
    s_snapshot.valid = false;
}

float G_Automation_GetDistrictWeight(int district, const char *rname)
{
    const struct good_priority *prio = priority_get(-1, district, rname, false);
    return prio ? prio->weight : -1.0f;
}

bool G_Automation_SavePriorities(struct SDL_RWops *stream)
{
    struct attr num_priorities = (struct attr){
        .type = TYPE_INT,
        .val.as_int = vec_size(&s_priorities)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_priorities, "num_good_priorities"));

    for(int i = 0; i < vec_size(&s_priorities); i++) {
        const struct good_priority *prio = &vec_AT(&s_priorities, i);

        struct attr faction = (struct attr){
            .type = TYPE_INT,
            .val.as_int = prio->faction
        };
        CHK_TRUE_RET(Attr_Write(stream, &faction, "faction"));

        struct attr district = (struct attr){
            .type = TYPE_INT,
            .val.as_int = prio->district
        };
        CHK_TRUE_RET(Attr_Write(stream, &district, "district"));

        struct attr rname = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(rname.val.as_string, prio->rname, sizeof(rname.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &rname, "rname"));

        struct attr weight = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = prio->weight
        };
        CHK_TRUE_RET(Attr_Write(stream, &weight, "weight"));

        struct attr target = (struct attr){
            .type = TYPE_INT,
            .val.as_int = prio->stock_target
        };
        CHK_TRUE_RET(Attr_Write(stream, &target, "stock_target"));
    }
    return true;
}

bool G_Automation_LoadPriorities(struct SDL_RWops *stream)
{
    struct attr attr;
    vec_priority_reset(&s_priorities);
    s_snapshot.valid = false;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_priorities = attr.val.as_int;

    for(int i = 0; i < num_priorities; i++) {

        struct good_priority prio;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        prio.faction = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        prio.district = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        pf_strlcpy(prio.rname, attr.val.as_string, sizeof(prio.rname));

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        prio.weight = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        prio.stock_target = attr.val.as_int;

        CHK_TRUE_RET(vec_priority_push(&s_priorities, prio));
    }
    return true;
}

bool G_Automation_SaveState(struct SDL_RWops *stream)
{
    struct attr num_ents = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_entity_state_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_ents, "num_ents"));

    uint32_t key;
    struct automation_state astate;

    kh_foreach(s_entity_state_table, key, astate, {

        struct attr uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = key
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid, "uid"));

        struct attr state = (struct attr){
            .type = TYPE_INT,
            .val.as_int = astate.state
        };
        CHK_TRUE_RET(Attr_Write(stream, &state, "state"));

        struct attr transient_ticks = (struct attr){
            .type = TYPE_INT,
            .val.as_int = astate.transient_ticks
        };
        CHK_TRUE_RET(Attr_Write(stream, &transient_ticks, "transient_ticks"));

        struct attr automatic_transport = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = astate.automatic_transport
        };
        CHK_TRUE_RET(Attr_Write(stream, &automatic_transport, "automatic_transport"));

        struct attr transport_target = (struct attr){
            .type = TYPE_INT,
            .val.as_int = astate.transport_target
        };
        CHK_TRUE_RET(Attr_Write(stream, &transport_target, "transport_target"));

        struct attr automatic_work = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = astate.automatic_work
        };
        CHK_TRUE_RET(Attr_Write(stream, &automatic_work, "automatic_work"));

        struct attr pinned = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(pinned.val.as_string, astate.pinned, sizeof(pinned.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &pinned, "pinned"));

        struct attr home_district = (struct attr){
            .type = TYPE_INT,
            .val.as_int = astate.home_district
        };
        CHK_TRUE_RET(Attr_Write(stream, &home_district, "home_district"));

        struct attr working = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(working.val.as_string, astate.working, sizeof(working.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &working, "working"));
        Sched_TryYield();
    });

    struct attr num_sites = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_transport_count)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_sites, "num_sites"));

    uint32_t count;
    kh_foreach(s_transport_count, key, count, {

        struct attr uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = key
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid, "uid"));

        struct attr count_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = count
        };
        CHK_TRUE_RET(Attr_Write(stream, &count_attr, "count"));
        Sched_TryYield();
    });
    return true;
}

bool G_Automation_LoadState(struct SDL_RWops *stream)
{
    s_snapshot.valid = false;
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_ents = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_ents; i++) {

        uint32_t uid;
        struct automation_state *astate;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uid = attr.val.as_int;

        /* The entity should have already been loaded from the scripting state */
        khiter_t k = kh_get(state, s_entity_state_table, uid);
        CHK_TRUE_RET(k != kh_end(s_entity_state_table));
        astate = &kh_value(s_entity_state_table, k);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        astate->state = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        astate->transient_ticks = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        astate->automatic_transport = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        astate->transport_target = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        astate->automatic_work = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        pf_strlcpy(astate->pinned, attr.val.as_string, sizeof(astate->pinned));

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        astate->home_district = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        pf_strlcpy(astate->working, attr.val.as_string, sizeof(astate->working));
        Sched_TryYield();
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_sites = attr.val.as_int;

    for(int i = 0; i < num_sites; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t count = attr.val.as_int;

        khiter_t k = kh_get(count, s_transport_count, uid);
        if(k == kh_end(s_transport_count)) {
            int status;
            k = kh_put(count, s_transport_count, uid, &status);
            CHK_TRUE_RET(status != -1);
        }
        kh_val(s_transport_count, k) = count;
        Sched_TryYield();
    }
    return true;
}

