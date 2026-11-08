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
#include "duty.h"
#include "movement.h"
#include "harvester.h"
#include "builder.h"
#include "combat.h"
#include "position.h"
#include "storage_site.h"
#include "provision.h"
#include "posts.h"
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
/* A Low request waits behind the others at most this long */
#define LOW_PATIENCE_S               (60.0f)
/* A carrier is not sent for less than this while more could be had, until
 * the request has waited MIN_LOAD_WAIT_S */
#define MIN_LOAD                     (5)
/* A top-up's walk, for a carrier of the least load; a bigger one walks as much farther */
#define TOP_UP_WALK                  (300.0f)
/* What a keep stands for, in minutes, against the drawers' minutes of stock */
#define KEEP_MINUTES                 (5.0f)
#define MIN_LOAD_WAIT_S              (30.0f)
/* The most carts one faction's carrying between districts is planned over,
 * the loads of one request and the lanes kept for the scripts to read */
#define MAX_FLEET_CARTS              (64)
#define MAX_PLAN_LOADS               (32)
#define MAX_PLAN_SOURCES             (32)
/* A haul this long waits for a full load */
#define FULL_LOAD_TRAVEL             (400.0f)
#define MAX_RESOURCES                (64)
#define MAX_DEMANDS                  (256)
#define MAX_WORK_SITES               (512)
/* How far a worker will go for work it finds by itself */
#define WORK_RADIUS                  (600.0f)
/* Each gatherer already bound for a node makes it read this many world
 * units further from the hub, so that the workers fill the nodes nearest
 * the hub before any goes further afield, without all crowding the nearest
 * one, and a node takes no more than fit about its rim */
#define CROWD_SLACK                  (40.0f)
/* How much further a worker goes for a food the less of it is in store, and how
 * much behind another his own must be for him to leave it */
#define STOCK_MINUTE_SLACK           (60.0f)
#define MEMBER_BAND                  (120.0f)
#define NO_DRAW_MINUTES              (30.0f)
#define NODE_GAP                     (1.0f)
#define PINNED_LEN                   (32)
/* What every good counts for before anything asks for it, so that idle
 * workers still gather into stock */
#define BASE_NEED                    (10)
/* How long a good stays above its target before its gatherers are let go */
#define STOCKED_HOLD_TICKS           (600)
/* Nodes this close together fill up as one patch */
#define PATCH_RADIUS                 (60.0f)
#define MAX_CLASSES                  (4)
#define MAX_CLASS_MEMBERS            (8)
/* Stock of a good beyond what is asked for, past which gathering more of it waits */
#define STOCK_TARGET                 (150)
/* A good short by this much more takes one more gatherer */
#define GOOD_PER_GATHERER            (15)
/* A gatherer's output is measured over about this long */
#define RATE_WINDOW_S                (120.0f)
/* What one gatherer is taken to bring in a minute until enough is measured */
#define GATHERER_RATE                (8.0f)
/* A worker's load, pace and swings per unit, for what a node yields a minute */
#define GATHER_LOAD                  (5)
#define WORKER_SPEED                 (20.0f)
#define GATHER_S_PER_UNIT            (4.0f)
#define WORKER_RADIUS                (3.0f)
/* How far the measured pace may bend the reckoned one */
#define RATE_FIX_MIN                 (0.5f)
#define RATE_FIX_MAX                 (2.0f)
#define MIN_RATE_SAMPLE_S            (30.0f)
/* How far the movement may move an idle walk's destination to where it can be reached */
#define IDLE_DEST_SLACK              (16.0f)
/* Once a second */
#define RATE_SAMPLE_TICKS            (20)
/* Stock past the spare target by at least this much takes no gatherers for
 * a rate either, so that a good used slower than asked for does not pile up */
#define RATE_STOCK_MARGIN            (50)
/* A good's gatherer count follows what it wants a step at a time, so that a
 * shortfall does not draw a crowd that is let go all at once when it is met */
#define ALLOWED_STEP_TICKS           (400)
#define ALLOWED_STEP_UP              (2)
/* Gatherers leave off to build a site that has fewer builders than this */
#define SITE_BUILDERS                (3)
/* A road tile takes one builder beyond its founder */
#define ROAD_SITE_BUILDERS           (2)
/* How many workers of no district build at once, unless the realm sets it */
#define CREW_SIZE                    (4)
/* How far off a node lies, in these units, that halves a good's draw */
#define BREADTH_SCALE                (100.0f)
#define MAX_TRACE_STEPS              (24)
#define MAX_WORK_EVENTS              (6)
/* How soon a work event told again is taken as the same one, 10 s */
#define EVENT_REPEAT_TICKS           (200)
#define MAX_VACANCIES                (256)
/* How long a workshop's own district has to fill its posts before a worker of
 * no district at the end of its work takes one */
#define HOMED_FIRST_TICKS            (100)

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
    /* Finding its own work, it takes posts in workshops */
    bool              automatic_posts;
};

/* A site's want of one resource */
struct job_request{
    uint32_t    site;
    const char *rname;
    int         shortfall;
    int         tier;
    /* Minutes of stock left, less the wait: the least first within a tier */
    float       key;
};

/* Where a request stands on the carriers' ladder: a drawer about to stop or a
 * site with a builder waiting, a drawer or keep under what it works from, a
 * drawer being topped up toward its capacity */
enum request_tier{
    TIER_STARVING = 1,
    TIER_BUFFERING,
    TIER_TOPPING,
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
    int                 tier;
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
    /* How long its stock lasts the realm's draw of it */
    float       stock_minutes;
};

/* A site to be built, and how many builders are on it or on their way */
struct work_site{
    uint32_t uid;
    int      faction;
    int      builders;
    /* Waiting on materials that nothing can bring it now */
    bool     starved;
    /* Road work, staffed only once no building in reach wants builders */
    bool     road;
};

/* The realm's say over how its automation goes about the work */
enum sched_option{
    /* The most of a district's own workers, or of a faction's workers of no
     * district, set to carrying, and to building, while there is other work */
    OPT_CARRY_SHARE,
    OPT_BUILD_SHARE,
    /* How far from its hub a worker goes to gather */
    OPT_REACH,
    /* How strongly a nearer node draws a worker over a more wanted good */
    OPT_BREADTH,
    /* Whether workers left to themselves take posts in workshops */
    OPT_MAN_WORKSHOPS,
    /* Whether gatherers leave off at a drop-off to build */
    OPT_PULL_TO_BUILD,
    /* Whether a worker with nothing nearer goes to the nearest node however far */
    OPT_ROAM_FAR,
    /* The least seconds between two workers of a pool taken off their work for
     * something more urgent, and the least a worker keeps at a task before it is */
    OPT_REBALANCE_S,
    OPT_TENURE_S,
    /* The share of a good's target its stock must stand above to count as stocked */
    OPT_STOCK_BAND,
    NUM_SCHED_OPTIONS
};

/* Goods the scheduler treats as one, such as every food: a class is scored
 * once and its gatherers go to the nearest node of any of its members */
struct good_class{
    char name[PINNED_LEN];
    int  n;
    char members[MAX_CLASS_MEMBERS][PINNED_LEN];
};

struct sched_option_desc{
    const char *name;
    float       def;
    float       min;
    float       max;
};

/* One pool of workers: a district's own, or a faction's of no district */
struct pool_census{
    int workers;
    int carriers;
    int builders;
};

enum work_share{
    SHARE_CARRY,
    SHARE_BUILD
};

KHASH_MAP_INIT_INT(pool, struct pool_census)

/* What every worker looking for a job this tick weighs, gathered once for all of them */
struct work_snapshot{
    bool               valid;
    uint32_t           tick;
    /* Gatherers bound for each node, counting those handed one this pass */
    struct kh_count_s *aimed;
    vec_request_t      requests;
    size_t             ndemands;
    struct good_demand demands[MAX_DEMANDS];
    size_t             nsites;
    struct work_site   sites[MAX_WORK_SITES];
    /* Each faction's workers of no district building or bound for a site */
    int                crew[MAX_FACTIONS];
    struct kh_pool_s  *pools;
};
VEC_TYPE(source, struct job_source)
VEC_IMPL(static inline, source, struct job_source)

/* A worker's last decision about its work */
struct work_trace{
    uint32_t         tick;
    size_t           nsteps;
    struct work_step steps[MAX_TRACE_STEPS];
};

/* What befell a worker at its work since: a meal, a hold-up, a post left;
 * kept apart from the decision, which they do not change */
struct work_events{
    size_t           head;
    size_t           count;
    uint32_t         ticks[MAX_WORK_EVENTS];
    struct work_step steps[MAX_WORK_EVENTS];
};

/* A faction's cart as the plan sees it: when it is next free, and where */
struct fleet_cart{
    uint32_t uid;
    float    speed;
    float    free_s;
    vec2_t   pos;
    bool     waiting;
};

struct cart_fleet{
    int               ncarts;
    struct fleet_cart carts[MAX_FLEET_CARTS];
};

/* A load of a request laid on a cart, and what the cart was before it */
struct plan_load{
    int    cart;
    int    src_district;
    int    amount;
    float  eta;
    float  prev_free_s;
    vec2_t prev_pos;
    bool   now;
    /* Whether the cart would bring it after the request needs it */
    bool   late;
};

/* How the carts would bring what a request wants from other districts */
struct carry_plan{
    bool             valid;
    int              nloads;
    /* Wanted and to be had, with no cart able to bring it */
    int              unplanned;
    int              unplanned_district;
    struct plan_load loads[MAX_PLAN_LOADS];
};

/* A worker that could be sent to a post */
struct post_worker{
    uint32_t uid;
    int      faction;
    int      home_district;
    vec2_t   pos;
    /* Standing idle rather than gathering */
    bool     idle;
    bool     taken;
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
    /* The gatherer count let gather it now, and the tick it last stepped; not saved */
    int      allowed;
    uint32_t allowed_tick;
    /* Since when its stock has stood above its target, or 0; not saved */
    uint32_t stocked_since;
};

VEC_TYPE(priority, struct good_priority)
VEC_IMPL(static inline, priority, struct good_priority)

VEC_TYPE(district, int)
VEC_IMPL(static inline, district, int)

KHASH_MAP_INIT_INT(state, struct automation_state)
KHASH_MAP_INIT_INT(count, uint32_t);
KHASH_MAP_INIT_INT64(pending, struct pending)
KHASH_MAP_INIT_INT(trace, struct work_trace)
KHASH_MAP_INIT_INT(events, struct work_events)

static void on_order_issued(void *user, void *event);
static bool waiting_carrier(uint32_t uid, struct automation_state *astate);
static float option(int faction, enum sched_option opt);
static bool node_of_good(uint32_t uid, void *arg);
static void on_player_order_issued(void *user, void *event);
static bool try_assign_transport_job(uint32_t uid, struct automation_state *astate, bool spare);
static bool try_take_post(uint32_t uid, struct automation_state *astate);
static enum duty_by by_of(const struct automation_state *astate);
static bool leaves_gathering(uint32_t uid, const char *rname, uint32_t node);
static bool wanted_elsewhere(uint32_t uid, struct automation_state *astate, struct work_snapshot *snap);
static bool try_assign_work(uint32_t uid, struct automation_state *astate, bool spare);
static bool stocked(int surplus, int shortfall, int target);
static int gatherers_wanted(int surplus, int shortfall, int target, int for_rate);
static int gatherers_for_rate(int faction, const char *rname);
static struct good_priority *priority_get(int faction, int district, const char *rname, bool create);
static float good_weight(int faction, int district, const char *rname);
static float node_weight(int faction, uint32_t node, const char *rname);
static float top_weight(int faction, const char *rname);
static uint32_t gather_node(uint32_t uid, const struct automation_state *astate, const char *rname,
                            struct work_snapshot *snap);
static bool could_serve(uint32_t site, const struct job_request *req);
static int stock_target(int faction, const char *rname);
static vec2_t work_origin(uint32_t uid, const struct automation_state *astate);
static bool in_home_territory(int faction, int home, uint32_t node);
static void reconsider_work(uint32_t uid, struct automation_state *astate);
static struct work_snapshot *snapshot_get(void);
static struct good_demand *demand_get(struct work_snapshot *snap, int faction, const char *rname);
static struct good_demand *good_demand_get(struct work_snapshot *snap, int faction, const char *rname);
static const struct good_class *class_named(const char *name);
static const struct good_class *class_of(const char *rname);
static uint32_t node_for(uint32_t uid, const struct automation_state *astate, const char *name,
                         struct work_snapshot *snap, const char **out_good);
static void trace_note(uint32_t uid, const char *kind, const char *verdict, const char *why, 
                       const char *rname, uint32_t target);
static float distance(vec2_t a, vec2_t b);
static bool districts_equal(const int *a, const int *b);

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
static khash_t(events) *s_events;
/* Settings of the realms and their districts, few enough to look up in turn */
static vec_priority_t  s_priorities;
/* Districts whose stores only carts carry out of; not saved, the scripts set them again */
static vec_district_t  s_kept_home;
/* Districts whose workers carry their goods out whether or not there are carts */
static vec_district_t  s_carried_out;
static int             s_crew_size[MAX_FACTIONS];
static const struct sched_option_desc s_option_descs[NUM_SCHED_OPTIONS] = {
    [OPT_CARRY_SHARE]   = {"carry_share",   0.25f,   0.0f,    1.0f},
    [OPT_BUILD_SHARE]   = {"build_share",   0.25f,   0.0f,    1.0f},
    [OPT_REACH]         = {"reach",         600.0f,  100.0f,  3000.0f},
    [OPT_BREADTH]       = {"breadth",       1.0f,    0.0f,    4.0f},
    [OPT_MAN_WORKSHOPS] = {"man_workshops", 1.0f,    0.0f,    1.0f},
    [OPT_PULL_TO_BUILD] = {"pull_to_build", 1.0f,    0.0f,    1.0f},
    [OPT_ROAM_FAR]      = {"roam_far",      0.0f,    0.0f,    1.0f},
    [OPT_REBALANCE_S]   = {"rebalance_s",   5.0f,    1.0f,    30.0f},
    [OPT_TENURE_S]      = {"tenure_s",      60.0f,   0.0f,    300.0f},
    [OPT_STOCK_BAND]    = {"stock_band",    0.2f,    0.0f,    1.0f},
};
static float           s_options[MAX_FACTIONS][NUM_SCHED_OPTIONS];
static struct cart_fleet s_fleets[MAX_FACTIONS];
/* The lanes of the last judging pass and of the one being made */
static struct carry_lane s_lanes[MAX_CARRY_LANES];
static size_t            s_nlanes;
static struct carry_lane s_lanes_next[MAX_CARRY_LANES];
static size_t            s_nlanes_next;
static struct good_class s_classes[MAX_CLASSES];
static int             s_nclasses;
/* When each pool last took a worker off its work for something more urgent */
static khash_t(count)  *s_reassigned;

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

/* What happens to a worker at its work, rather than the choice of it */
static bool trace_is_event(const char *kind, const char *why)
{
    static const char *events[] = {"meal", "duty", "walled", "crowded", "stalled"};
    for(int i = 0; i < ARR_SIZE(events); i++) {
        if(!strcmp(kind, events[i]))
            return true;
    }
    return !strcmp(kind, "post") && strcmp(why, "workshop") && strcmp(why, "manned");
}

/* Notes of nothing happening, which would only crowd out what did */
static bool trace_is_noise(const char *kind, const char *verdict, const char *why)
{
    return (!strcmp(kind, "meal") && !strcmp(verdict, "skipped"))
        || (!strcmp(kind, "duty") && !strcmp(why, "resumed"));
}

static bool same_str(const char *a, const char *b)
{
    if(!a || !b)
        return a == b;
    return !strcmp(a, b);
}

static void event_push(uint32_t uid, struct work_step step)
{
    if(!(G_FlagsGet(uid) & (ENTITY_FLAG_HARVESTER | ENTITY_FLAG_BUILDER)))
        return;
    int status;
    khiter_t k = kh_put(events, s_events, uid, &status);
    if(status == -1)
        return;
    struct work_events *ev = &kh_value(s_events, k);
    if(status != 0) {
        ev->head = 0;
        ev->count = 0;
    }
    /* The same thing told again soon after is the one event */
    if(ev->count > 0) {
        int last = (ev->head + MAX_WORK_EVENTS - 1) % MAX_WORK_EVENTS;
        const struct work_step *prev = &ev->steps[last];
        if(s_tick - ev->ticks[last] < EVENT_REPEAT_TICKS
        && same_str(prev->kind, step.kind) && same_str(prev->why, step.why) && prev->target == step.target
        && !strcmp(prev->rname, step.rname)) {
            ev->ticks[last] = s_tick;
            return;
        }
    }
    ev->ticks[ev->head] = s_tick;
    ev->steps[ev->head] = step;
    ev->head = (ev->head + 1) % MAX_WORK_EVENTS;
    ev->count = MIN(ev->count + 1, MAX_WORK_EVENTS);
}

static void trace_note(uint32_t uid, const char *kind, const char *verdict, const char *why, 
                       const char *rname, uint32_t target)
{
    if(trace_is_noise(kind, verdict, why))
        return;
    struct work_step step = (struct work_step){
        .kind = kind,
        .verdict = verdict,
        .why = why,
        .target = target
    };
    pf_strlcpy(step.rname, rname ? rname : "", sizeof(step.rname));
    if(trace_is_event(kind, why)) {
        event_push(uid, step);
        return;
    }
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

/* Who the automation gives a worker's duty for: its city, or the automation itself */
static enum duty_by by_of(const struct automation_state *astate)
{
    return astate->home_district ? DUTY_BY_CITY : DUTY_BY_AUTO;
}

static bool idle(uint32_t uid)
{
    /* At a workshop's post, or on its way to one */
    if(G_Posts_Holds(uid))
        return false;
    /* A supplier standing by for its charge's next request */
    if(G_Duty_Waiting(uid))
        return false;
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
    if(k != kh_end(s_transport_count) && kh_val(s_transport_count, k) > 0) {
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
            astate->state = STATE_ACTIVE;
            continue;
        }
        trace_note(uid, "meal", due ? "failed" : "skipped", due ? "no_food" : "not_due", NULL, NULL_UID);

        /* The duty it was at comes before any new work, and holds it while it cannot */
        if(G_Duty_Resume(uid)) {
            trace_note(uid, "duty", "taken", "resumed", NULL, NULL_UID);
            astate->state = STATE_ACTIVE;
            continue;
        }
        if(G_Duty_Kind(uid) != DUTY_NONE) {
            astate->state = STATE_IDLE;
            continue;
        }

        if(try_take_post(uid, astate)) {
            astate->state = STATE_ACTIVE;
            continue;
        }

        /* Carrying and building take their share of the workers first, and
         * any with nothing else to do after */
        if(try_assign_transport_job(uid, astate, false) || try_assign_work(uid, astate, false)
        || try_assign_transport_job(uid, astate, true) || try_assign_work(uid, astate, true)) {
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

static bool districts_equal(const int *a, const int *b)
{
    return *a == *b;
}

static float distance(vec2_t a, vec2_t b)
{
    vec2_t delta;
    PFM_Vec2_Sub(&a, &b, &delta);
    return PFM_Vec2_Len(&delta);
}

/* A larder, or a drawer still above its low mark, asks for a load, not a unit */
static bool worth_asking(uint32_t site, const char *rname, int want, int shortfall)
{
    enum ss_chip chip = G_StorageSite_GetChip(site, rname);
    int curr = G_StorageSite_GetCurr(site, rname);
    bool larder = (chip == SS_CHIP_COLLECT || chip == SS_CHIP_RELAY) && G_StorageSite_GetKeep(site, rname) > 0;
    bool buffering = G_StorageSite_GetDraw(site, rname) > 0.0f && curr >= G_StorageSite_LowMark(site, rname);
    if(!larder && !buffering)
        return true;
    if(shortfall >= MIN(MIN_LOAD, MAX(1, want / 2)))
        return true;
    return curr == 0;
}

/* What the faction's stores could give of the good between them */
static int faction_spare(const vec_entity_t *sites, int faction, const char *rname)
{
    int ret = 0;
    for(int i = 0; i < vec_size(sites); i++) {
        uint32_t site = vec_AT(sites, i);
        if(G_EntityIsZombie(site) || !is_representative(site) || G_GetFactionID(site) != faction)
            continue;
        if(G_StorageSite_GetUseAlt(site))
            continue;
        ret += G_StorageSite_Surplus(site, rname);
    }
    return ret;
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
        int priority = G_StorageSite_GetPriority(site);

        bool done[MAX_RESOURCES] = {0};
        for(int j = 0; j < nnames; j++) {

            if(done[j])
                continue;
            /* Goods eaten in one another's stead are asked for as one, by the
             * first member, the richest, of which the realm can spare a load */
            const char *rname = names[j];
            const char *members[MAX_RESOURCES];
            size_t nmembers = G_StorageSite_DrawMembers(site, rname, ARR_SIZE(members), members);
            int en_route = pool_en_route(site, rname);
            if(nmembers > 1) {
                int best = -1;
                bool chosen = false;
                en_route = 0;
                for(size_t m = 0; m < nmembers; m++) {
                    for(int n = j + 1; n < nnames; n++) {
                        if(!strcmp(names[n], members[m])) {
                            done[n] = true;
                        }
                    }
                    en_route += pool_en_route(site, members[m]);
                    if(chosen)
                        continue;
                    int spare = faction_spare(sites, G_GetFactionID(site), members[m]);
                    if(spare >= MIN_LOAD) {
                        rname = members[m];
                        chosen = true;
                    }else if(spare > best) {
                        best = spare;
                        rname = members[m];
                    }
                }
            }

            int want = G_StorageSite_Request(site, rname);
            int curr = G_StorageSite_GetCurr(site, rname);
            int have = curr + en_route;
            int shortfall = want - have;
            bool drawing = G_StorageSite_GetDraw(site, rname) > 0.0f;
            int tier;
            float key;
            if(shortfall > 0) {
                if(!worth_asking(site, rname, want, shortfall))
                    continue;
                if(G_StorageSite_GetUseAlt(site)) {
                    tier = (G_StorageSite_Builders(site) > 0) ? TIER_STARVING : TIER_BUFFERING;
                    key = 0.0f;
                }else if(drawing) {
                    tier = (curr < G_StorageSite_LowMark(site, rname)) ? TIER_STARVING : TIER_BUFFERING;
                    key = G_StorageSite_MinutesOfStock(site, rname);
                }else{
                    tier = TIER_BUFFERING;
                    key = KEEP_MINUTES * curr / (float)want;
                }
            }else if(drawing && G_StorageSite_GetCapacity(site, rname) - have >= MIN_LOAD) {
                /* Fed, a drawer is topped up toward its capacity, the emptiest
                 * first, by the load: a unit or two is not worth a trip */
                tier = TIER_TOPPING;
                shortfall = G_StorageSite_GetCapacity(site, rname) - have;
                key = G_StorageSite_MinutesOfStock(site, rname);
            }else{
                continue;
            }
            struct pending *pend = pending_get(site, rname, true);
            if(!pend)
                continue;
            pend->touched = true;
            float waited_s = (s_tick - pend->since_tick) * TICK_MS / 1000.0f;

            /* A Low request lets the others go first, but not for ever */
            if(priority >= SS_PRIORITY_URGENT) {
                tier = MAX(tier - 1, TIER_STARVING);
            }else if(priority == SS_PRIORITY_LOW && waited_s < LOW_PATIENCE_S) {
                tier++;
            }
            pend->tier = tier;

            vec_request_push(out, (struct job_request){
                .site = site,
                .rname = rname,
                .shortfall = shortfall,
                .tier = tier,
                .key = key - waited_s / 60.0f
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
    if(ra->tier != rb->tier) return ra->tier - rb->tier;
    if(ra->key < rb->key) return -1;
    if(ra->key > rb->key) return 1;
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
        /* A pool holds its goods in common: nothing is carried within it */
        if(site == exclude || pool_representative(site) == pool_representative(exclude))
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

/* Whether goods made at the site could be brought to the request: not to one
 * no carrier can get to, nor to one cut off from all but its own district */
static bool could_serve(uint32_t site, const struct job_request *req)
{
    struct pending *pend = pending_get(req->site, req->rname, false);
    switch(pend ? pend->reason : REQUEST_OK) {
    case REQUEST_UNREACHABLE:   return false;
    case REQUEST_NOT_CONNECTED: return G_District_Of(site) == G_District_Of(req->site);
    default:                    return true;
    }
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

/* Whether a node of the good lies within the gatherers' reach of the site, on its island */
static bool gatherable_near(uint32_t site, const char *rname)
{
    uint32_t nodes[256];
    vec2_t pos = G_Pos_GetXZ(site);
    size_t nnodes = G_Pos_EntsInCircleWithPred(pos, option(G_GetFactionID(site), OPT_REACH), nodes,
        ARR_SIZE(nodes), node_of_good, (void*)rname);
    const struct map *map = G_GetPrevTickMap();
    for(int i = 0; i < nnodes; i++) {
        if(M_NavLocationsReachable(map, NAV_LAYER_GROUND_1X1, pos, G_Pos_GetXZ(nodes[i])))
            return true;
    }
    return false;
}

/* Each faction's carts, with when and where each comes free of what it is doing */
static void fleets_build(void)
{
    memset(s_fleets, 0, sizeof(s_fleets));
    uint32_t uid;
    struct automation_state *astate;
    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {
        if(!astate->automatic_transport)
            continue;
        uint32_t flags = G_FlagsGet(uid);
        if(!(flags & ENTITY_FLAG_HARVESTER) || (flags & (ENTITY_FLAG_BUILDER | ENTITY_FLAG_ZOMBIE)))
            continue;
        int faction = G_GetFactionID(uid);
        if(faction < 0 || faction >= MAX_FACTIONS || s_fleets[faction].ncarts == MAX_FLEET_CARTS)
            continue;
        float speed = 0.0f;
        if(!G_Move_GetEffectiveSpeed(uid, &speed) || speed <= 0.0f)
            continue;

        vec2_t pos = G_Pos_GetXZ(uid);
        float walk = 0.0f;
        uint32_t src = NULL_UID, dest = NULL_UID;
        const char *rname = NULL;
        int state = 0;
        bool job = G_Harvester_JobInfo(uid, &src, &dest, &rname, &state) && G_EntityExists(dest)
                && (G_FlagsGet(dest) & ENTITY_FLAG_STORAGE_SITE);
        if(job) {
            vec2_t end = G_Pos_GetXZ(dest);
            if(!G_Harvester_GetCurrTotalCarry(uid) && G_EntityExists(src)) {
                vec2_t via = G_Pos_GetXZ(src);
                walk = distance(pos, via) + distance(via, end);
            }else{
                walk = distance(pos, end);
            }
            pos = end;
        }
        s_fleets[faction].carts[s_fleets[faction].ncarts++] = (struct fleet_cart){
            .uid = uid,
            .speed = speed,
            .free_s = walk / speed,
            .pos = pos,
            .waiting = !job && waiting_carrier(uid, astate),
        };
    });
}

static bool cart_can_serve(const struct fleet_cart *cart, const struct job_request *req, uint32_t src,
                           int src_district, int dest_district)
{
    if(!carrier_can_take(cart->uid, req->rname))
        return false;
    struct automation_state *astate = astate_get(cart->uid);
    int home = astate ? astate->home_district : 0;
    if(home && (home != dest_district || home != src_district))
        return false;
    if(G_StorageSite_GetDoNotTake(G_FlagsGet(cart->uid), src))
        return false;
    return G_Harvester_CanReachSite(cart->uid, nearest_member(src, cart->pos))
        && G_Harvester_CanReachSite(cart->uid, nearest_member(req->site, G_Pos_GetXZ(src)));
}

/* How many seconds from now the request needs the goods beyond 'held': at
 * once with builders waiting on them, when what it holds runs out for a store
 * being drawn from, and never for one only laying up stock */
static float need_by_s(const struct job_request *req, int held)
{
    if(G_StorageSite_GetUseAlt(req->site))
        return (G_StorageSite_Builders(req->site) > 0) ? 0.0f : INFINITY;
    float per_min = G_StorageSite_GetDraw(req->site, req->rname);
    if(per_min <= 0.0f)
        return INFINITY;
    return held * 60.0f / per_min;
}

/* Lays what the request wants from other districts, as far as they can spare
 * it, on the carts: each load, nearest source first, goes to the cart that
 * can deliver it soonest, and that cart is then taken up until it has */
static void plan_carts(struct cart_fleet *fleet, const struct job_request *req, const vec_source_t *sources,
                       int dest_district, struct carry_plan *out)
{
    out->valid = true;
    out->nloads = 0;
    out->unplanned = 0;
    out->unplanned_district = 0;

    vec2_t dest_pos = G_Pos_GetXZ(req->site);
    int left = req->shortfall;
    uint32_t sites[MAX_PLAN_SOURCES];
    int spare[MAX_PLAN_SOURCES];
    int nsites = 0;
    for(int i = 0; i < vec_size(sources); i++) {
        const struct job_source *src = &vec_AT((vec_source_t*)sources, i);
        if(G_District_Of(src->site) == dest_district) {
            left -= src->surplus;
        }else if(nsites < MAX_PLAN_SOURCES) {
            sites[nsites] = src->site;
            spare[nsites++] = src->surplus;
        }
    }

    int held = MAX(0, G_StorageSite_Request(req->site, req->rname) - req->shortfall) + (req->shortfall - MAX(left, 0));
    bool refused[MAX_FLEET_CARTS];
    int last_source = -1;
    while(left > 0 && out->nloads < MAX_PLAN_LOADS) {

        int source = -1;
        float nearest = INFINITY;
        for(int i = 0; i < nsites; i++) {
            float d = distance(G_Pos_GetXZ(sites[i]), dest_pos);
            if(spare[i] > 0 && d < nearest) {
                nearest = d;
                source = i;
            }
        }
        if(source == -1)
            break;
        vec2_t spos = G_Pos_GetXZ(sites[source]);
        int src_district = G_District_Of(sites[source]);
        if(source != last_source) {
            memset(refused, 0, sizeof(refused));
            last_source = source;
        }

        int best = -1;
        float best_eta = INFINITY;
        for(int c = 0; c < fleet->ncarts; c++) {
            const struct fleet_cart *cart = &fleet->carts[c];
            if(refused[c] || G_Harvester_GetMaxCarry(cart->uid, req->rname) <= 0)
                continue;
            float eta = cart->free_s + (distance(cart->pos, spos) + distance(spos, dest_pos)) / cart->speed;
            if(eta >= best_eta)
                continue;
            if(!cart_can_serve(cart, req, sites[source], src_district, dest_district)) {
                refused[c] = true;
                continue;
            }
            best = c;
            best_eta = eta;
        }
        if(best == -1) {
            out->unplanned_district = src_district;
            break;
        }

        struct fleet_cart *cart = &fleet->carts[best];
        int amount = MIN(left, MIN(spare[source], G_Harvester_GetMaxCarry(cart->uid, req->rname)));
        out->loads[out->nloads++] = (struct plan_load){
            .cart = best,
            .src_district = src_district,
            .amount = amount,
            .eta = best_eta,
            .prev_free_s = cart->free_s,
            .prev_pos = cart->pos,
            .now = cart->waiting && cart->free_s == 0.0f,
            .late = best_eta > need_by_s(req, held),
        };
        cart->free_s = best_eta;
        cart->pos = dest_pos;
        held += amount;
        left -= amount;
        spare[source] -= amount;
    }

    int to_be_had = 0;
    for(int i = 0; i < nsites; i++) {
        to_be_had += spare[i];
        if(spare[i] > 0 && !out->unplanned_district) {
            out->unplanned_district = G_District_Of(sites[i]);
        }
    }
    out->unplanned = MAX(0, MIN(left, to_be_had));
}

/* The latest load the carts would bring too late that a worker delivering at 'eta' beats, or -1 */
static int plan_late_load(const struct carry_plan *plan, float eta)
{
    for(int i = plan->nloads - 1; i >= 0; i--) {
        const struct plan_load *load = &plan->loads[i];
        if(load->late && !load->now && eta < load->eta)
            return i;
    }
    return -1;
}

/* Whether a worker delivering at 'eta' brings what the carts cannot, or cannot in time */
static bool plan_leaves_load(const struct cart_fleet *fleet, const struct carry_plan *plan, float eta)
{
    if(fleet->ncarts == 0)
        return true;
    if(!plan->valid)
        return false;
    return plan->unplanned > 0 || plan_late_load(plan, eta) != -1;
}

/* A worker delivering at 'eta' has taken 'amount' on: the carts are let off those loads */
static void plan_take(struct cart_fleet *fleet, struct carry_plan *plan, float eta, int amount)
{
    int from_open = MIN(plan->unplanned, amount);
    plan->unplanned -= from_open;
    amount -= from_open;
    int idx;
    while(amount > 0 && (idx = plan_late_load(plan, eta)) != -1) {
        struct plan_load *load = &plan->loads[idx];
        int taken = MIN(load->amount, amount);
        load->amount -= taken;
        amount -= taken;
        if(load->amount > 0)
            break;
        if(idx == plan->nloads - 1) {
            fleet->carts[load->cart].free_s = load->prev_free_s;
            fleet->carts[load->cart].pos = load->prev_pos;
        }
        memmove(load, load + 1, (plan->nloads - idx - 1) * sizeof(struct plan_load));
        plan->nloads--;
    }
}

static struct carry_lane *lane_get(int faction, int src, int dest, const char *rname)
{
    for(size_t i = 0; i < s_nlanes_next; i++) {
        struct carry_lane *lane = &s_lanes_next[i];
        if(lane->faction == faction && lane->src_district == src && lane->dest_district == dest
        && !strcmp(lane->rname, rname))
            return lane;
    }
    if(s_nlanes_next == MAX_CARRY_LANES)
        return NULL;
    struct carry_lane *lane = &s_lanes_next[s_nlanes_next++];
    *lane = (struct carry_lane){ .faction = faction, .src_district = src, .dest_district = dest };
    pf_strlcpy(lane->rname, rname, sizeof(lane->rname));
    return lane;
}

/* Adds what the plan left of a request to its lanes */
static void lanes_note(int faction, int dest, const char *rname, const struct carry_plan *plan)
{
    for(int i = 0; i < plan->nloads; i++) {
        const struct plan_load *load = &plan->loads[i];
        struct carry_lane *lane = lane_get(faction, load->src_district, dest, rname);
        if(lane) {
            lane->by_carts += load->amount;
            lane->last_eta_s = MAX(lane->last_eta_s, load->eta);
        }
    }
    if(plan->unplanned > 0) {
        struct carry_lane *lane = lane_get(faction, plan->unplanned_district, dest, rname);
        if(lane) {
            lane->uncovered += plan->unplanned;
        }
    }
}

/* Adds the loads workers are on their way with, between districts, to their lanes */
static void lanes_note_workers(void)
{
    uint32_t uid;
    kh_foreach_key(s_entity_state_table, uid, {
        uint32_t flags = G_FlagsGet(uid);
        if(!(flags & ENTITY_FLAG_BUILDER) || !(flags & ENTITY_FLAG_HARVESTER) || (flags & ENTITY_FLAG_ZOMBIE))
            continue;
        int amount = G_Harvester_JobAmount(uid);
        if(amount <= 0)
            continue;
        uint32_t src = NULL_UID, dest = NULL_UID;
        const char *rname = NULL;
        int state = 0;
        if(!G_Harvester_JobInfo(uid, &src, &dest, &rname, &state) || !G_EntityExists(src) || !G_EntityExists(dest))
            continue;
        int from = G_District_Of(src), to = G_District_Of(dest);
        if(from == to)
            continue;
        struct carry_lane *lane = lane_get(G_GetFactionID(uid), from, to, rname);
        if(lane) {
            lane->by_workers += amount;
        }
    });
}

/* Gives the waiting carriers the most pressing requests: each request takes 
 * the carrier and source that get its goods to it soonest. Only a pass over 
 * all the waiting carriers can 'judge' why a request goes unserved. */
static void assign_jobs(vec_entity_t *carriers, bool judge)
{
    /* A judging pass says why requests wait even with every carrier at work */
    if(vec_size(carriers) == 0 && !judge)
        return;
    PERF_ENTER();

    fleets_build();

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

        struct cart_fleet *fleet = &s_fleets[faction];
        struct carry_plan plan = {0};
        if(req->tier < TIER_TOPPING) {
            plan_carts(fleet, req, &sources, G_District_Of(req->site), &plan);
        }

        bool any_idle = false;
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
            bool best_planned = false;
            float best_eta = INFINITY;

            for(int c = 0; c < vec_size(carriers); c++) {

                uint32_t carrier = vec_AT(carriers, c);
                if(G_GetFactionID(carrier) != faction)
                    continue;
                any_idle = true;
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
                    /* A top-up is a full load's walk, and no long one for a small carrier */
                    bool topping = (req->tier >= TIER_TOPPING);
                    if(topping && travel > TOP_UP_WALK * max_carry / MIN_LOAD) {
                        any_small = true;
                        continue;
                    }
                    if(src->surplus < min_load_for(travel, max_carry, req->shortfall, topping ? 0.0f : waited_s)) {
                        any_small = any_small || src->surplus > 0;
                        continue;
                    }
                    if(home && G_District_Of(src->site) != home) {
                        any_district_bar = true;
                        continue;
                    }
                    int src_district = G_District_Of(src->site);
                    bool planned = false;
                    float worker_eta = INFINITY;
                    if((flags & ENTITY_FLAG_BUILDER) && src_district != dest_district) {
                        enum carry_out_mode mode = G_Automation_GetCarryOutMode(src_district);
                        if(mode == CARRY_OUT_OFF) {
                            any_district_bar = true;
                            continue;
                        }
                        float speed = 0.0f;
                        G_Move_GetEffectiveSpeed(carrier, &speed);
                        worker_eta = (speed > 0.0f) ? travel / speed : INFINITY;
                        if(mode == CARRY_OUT_AUTO && !plan_leaves_load(fleet, &plan, worker_eta)) {
                            any_small = true;
                            continue;
                        }
                        planned = (mode == CARRY_OUT_AUTO);
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
                        best_planned = planned;
                        best_eta = worker_eta;
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
            struct automation_state *cstate = astate_get(carrier);
            if(cstate) {
                G_Duty_TagNext(carrier, by_of(cstate));
            }
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
            if(best_planned) {
                plan_take(fleet, &plan, best_eta, amount);
            }

            /* A request waits from its last load, so that one left unserved 
             * comes to outweigh the ones being served */
            struct pending *pend = pending_get(req->site, req->rname, false);
            if(pend) {
                pend->since_tick = s_tick;
            }
        }

        if(judge && plan.valid) {
            lanes_note(faction, dest_district, req->rname, &plan);
        }
        if(!judge || req->tier >= TIER_TOPPING)
            continue;

        /* Waiting for a load worth the trip, or for one on its way, is not being blocked */
        if(req->shortfall > 0 && !any_reachable && !any_small 
        && !pool_bound_for(req->site, req->rname)) {
            bool none = (vec_size(&sources) == 0);
            /* A good gathered on the site's own island comes from its nodes */
            if((none || any_district_bar) && gatherable_near(req->site, req->rname)) {
                struct pending *pend = pending_get(req->site, req->rname, false);
                if(pend) {
                    pend->reason = REQUEST_OK;
                }
            }else if(none) {
                note_blocked(req, no_source_reason(&sites, req));
            }else if(any_district_bar) {
                note_blocked(req, REQUEST_NOT_CONNECTED);
            }else if(any_idle) {
                /* With every carrier busy it only waits its turn */
                note_blocked(req, any_carrier ? REQUEST_UNREACHABLE : REQUEST_NO_CARRIER);
            }
        }else{
            struct pending *pend = pending_get(req->site, req->rname, false);
            if(pend) {
                pend->reason = REQUEST_OK;
            }
        }
    }

    if(judge) {
        lanes_note_workers();
        memcpy(s_lanes, s_lanes_next, s_nlanes_next * sizeof(struct carry_lane));
        s_nlanes = s_nlanes_next;
        s_nlanes_next = 0;
    }

    vec_source_destroy(&sources);
    vec_request_destroy(&requests);
    vec_entity_destroy(&sites);
    PERF_RETURN_VOID();
}

static bool build_pinned(const struct automation_state *astate)
{
    return !strcmp(astate->pinned, BUILD_PIN);
}

static bool posts_pinned(const struct automation_state *astate)
{
    return !strcmp(astate->pinned, POSTS_PIN);
}

/* A worker of no district that builds as the realm's construction crew: not on the
 * player's order, nor set to building by the player
 */
static bool in_crew(const struct automation_state *astate)
{
    return astate->home_district == 0 && !build_pinned(astate);
}

/* The site a builder works on, or fetches the materials for */
static uint32_t crew_site(uint32_t uid)
{
    uint32_t ret = G_Builder_Target(uid);
    if(ret == NULL_UID && (G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER)) {
        ret = G_Harvester_SupplyingSite(uid);
    }
    return ret;
}

static void count_crews(int out[MAX_FACTIONS])
{
    memset(out, 0, sizeof(int) * MAX_FACTIONS);
    uint32_t uid;
    struct automation_state *astate;
    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {
        if(!in_crew(astate) || !(G_FlagsGet(uid) & ENTITY_FLAG_BUILDER))
            continue;
        int faction = G_GetFactionID(uid);
        if(crew_site(uid) != NULL_UID && faction >= 0 && faction < MAX_FACTIONS) {
            out[faction]++;
        }
    });
}

static bool crew_full(const struct work_snapshot *snap, int faction)
{
    if(faction < 0 || faction >= MAX_FACTIONS)
        return true;
    return snap->crew[faction] >= s_crew_size[faction];
}

static float option(int faction, enum sched_option opt)
{
    if(faction < 0 || faction >= MAX_FACTIONS)
        return s_option_descs[opt].def;
    return s_options[faction][opt];
}

static int option_index(const char *name)
{
    for(int i = 0; i < NUM_SCHED_OPTIONS; i++) {
        if(!strcmp(s_option_descs[i].name, name))
            return i;
    }
    return -1;
}

static void reset_options(void)
{
    for(int i = 0; i < MAX_FACTIONS; i++) {
        for(int j = 0; j < NUM_SCHED_OPTIONS; j++) {
            s_options[i][j] = s_option_descs[j].def;
        }
    }
}

static uint32_t pool_key(int faction, int district)
{
    return ((uint32_t)faction << 24) | ((uint32_t)district & 0xffffff);
}

static bool pool_member(uint32_t uid)
{
    uint32_t flags = G_FlagsGet(uid);
    return (flags & ENTITY_FLAG_BUILDER) && (flags & ENTITY_FLAG_HARVESTER) && !(flags & ENTITY_FLAG_ZOMBIE);
}

static void count_pools(khash_t(pool) *pools)
{
    kh_clear(pool, pools);
    uint32_t uid;
    struct automation_state *astate;
    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {
        if(!pool_member(uid))
            continue;
        int status;
        khiter_t k = kh_put(pool, pools, pool_key(G_GetFactionID(uid), astate->home_district), &status);
        if(status == -1)
            continue;
        if(status != 0) {
            kh_value(pools, k) = (struct pool_census){0};
        }
        struct pool_census *pool = &kh_value(pools, k);
        pool->workers++;
        enum duty_kind kind = G_Duty_Kind(uid);
        if(kind == DUTY_BUILD) {
            pool->builders++;
        }else if(kind == DUTY_SUPPLY) {
            pool->carriers++;
        }
    });
}

static struct pool_census *pool_get(struct work_snapshot *snap, uint32_t uid, const struct automation_state *astate)
{
    if(!snap->pools)
        return NULL;
    khiter_t k = kh_get(pool, snap->pools, pool_key(G_GetFactionID(uid), astate->home_district));
    if(k == kh_end(snap->pools))
        return NULL;
    return &kh_value(snap->pools, k);
}

/* Whether the worker's pool has its share of carriers, or of builders; one
 * set to the work by the player is not held to it */
static bool share_full(struct work_snapshot *snap, uint32_t uid, const struct automation_state *astate,
                       enum work_share share)
{
    if(!astate->automatic_work || build_pinned(astate))
        return false;
    const struct pool_census *pool = pool_get(snap, uid, astate);
    if(!pool)
        return false;
    float frac = option(G_GetFactionID(uid), (share == SHARE_CARRY) ? OPT_CARRY_SHARE : OPT_BUILD_SHARE);
    int cap = MAX(1, (int)ceilf(pool->workers * frac));
    return ((share == SHARE_CARRY) ? pool->carriers : pool->builders) >= cap;
}

static void share_taken(struct work_snapshot *snap, uint32_t uid, const struct automation_state *astate,
                        enum work_share share)
{
    struct pool_census *pool = pool_get(snap, uid, astate);
    if(!pool)
        return;
    if(share == SHARE_CARRY) {
        pool->carriers++;
    }else{
        pool->builders++;
    }
}

static bool waiting_carrier(uint32_t uid, struct automation_state *astate)
{
    if(!(G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER))
        return false;
    if(G_FlagsGet(uid) & ENTITY_FLAG_GARRISONED)
        return false;
    if(G_Posts_Holds(uid))
        return false;
    if(!astate->automatic_transport || build_pinned(astate))
        return false;
    if(G_Harvester_GetCurrTotalCarry(uid))
        return false;
    if(G_Provision_WantsFood(uid))
        return false;
    return true;
}

struct site_search{
    int faction;
    /* Only sites in this district, or in any with 0 */
    int district;
    /* Only sites with fewer builders than this, or any with 0 */
    int max_builders;
    /* Only sites that have their materials and can be worked on */
    bool supplied;
    /* Only sites not yet founded */
    bool unfounded;
    /* Only road work, or only the rest */
    bool road;
    /* At any distance */
    bool far;
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

/* Work on a road tile, the only site units walk through, unless the player marked it urgent */
static bool road_work(uint32_t uid)
{
    if((G_FlagsGet(uid) & ENTITY_FLAG_COLLISION) || !G_Building_GetPathable(uid))
        return false;
    return !(G_FlagsGet(uid) & ENTITY_FLAG_STORAGE_SITE) || G_StorageSite_GetPriority(uid) != SS_PRIORITY_URGENT;
}

/* The nearest of this tick's unfinished sites within reach that the search admits */
static struct work_site *nearest_site(struct work_snapshot *snap, vec2_t pos, 
                                      const struct site_search *ss)
{
    struct work_site *ret = NULL;
    float best = ss->far ? INFINITY : WORK_RADIUS;
    for(int i = 0; i < snap->nsites; i++) {
        struct work_site *curr = &snap->sites[i];
        if(curr->faction != ss->faction)
            continue;
        if(curr->road != ss->road)
            continue;
        int max_builders = ss->max_builders;
        if(curr->road && (max_builders == 0 || max_builders > ROAD_SITE_BUILDERS)) {
            max_builders = ROAD_SITE_BUILDERS;
        }
        if(max_builders && curr->builders >= max_builders)
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
        if(ss->unfounded && G_Building_IsFounded(curr->uid))
            continue;
        if(ss->district && G_District_Of(curr->uid) != ss->district)
            continue;
        best = dist;
        ret = curr;
    }
    return ret;
}

/* A building out of everybody's reach, whether waiting to be worked on or for
 * its founder, with no builder at all: one has to walk out to it */
static struct work_site *far_site(struct work_snapshot *snap, vec2_t pos, int faction, int district)
{
    struct site_search arg = (struct site_search){
        .faction = faction,
        .district = district,
        .max_builders = 1,
        .supplied = true,
        .far = true
    };
    struct work_site *ret = nearest_site(snap, pos, &arg);
    if(ret)
        return ret;
    arg = (struct site_search){
        .faction = faction,
        .district = district,
        .max_builders = 1,
        .unfounded = true,
        .far = true
    };
    return nearest_site(snap, pos, &arg);
}

/* The site a builder takes: a building before road work, and one of either
 * that can be worked on before one that waits for its founder
 */
static struct work_site *pick_site(struct work_snapshot *snap, vec2_t pos, int faction, int district)
{
    for(int road = 0; road < 2; road++) {
        struct site_search arg = (struct site_search){
            .faction = faction,
            .district = district,
            .max_builders = SITE_BUILDERS,
            .supplied = true,
            .road = road
        };
        struct work_site *ret = nearest_site(snap, pos, &arg);
        if(ret)
            return ret;
        arg = (struct site_search){ .faction = faction, .district = district, .max_builders = 1, .road = road };
        if((ret = nearest_site(snap, pos, &arg)))
            return ret;
    }
    return far_site(snap, pos, faction, district);
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

/* The gatherer count stepped toward what the good wants: up two at a time
 * when it is well short, else one, and down one */
static int gatherers_allowed(int faction, const char *rname, int want)
{
    struct good_priority *realm = priority_get(faction, 0, rname, true);
    if(!realm)
        return want;
    if(realm->allowed_tick == 0 || s_tick < realm->allowed_tick) {
        realm->allowed = want;
        realm->allowed_tick = s_tick;
    }else if(s_tick - realm->allowed_tick >= ALLOWED_STEP_TICKS) {
        realm->allowed_tick = s_tick;
        if(want > realm->allowed) {
            realm->allowed += MIN(want - realm->allowed, ALLOWED_STEP_UP);
        }else if(want < realm->allowed) {
            realm->allowed--;
        }
    }
    return realm->allowed;
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

static float node_weight(int faction, uint32_t node, const char *rname)
{
    return good_weight(faction, G_District_OfNode(faction, node, NULL), rname);
}

/* The most the good is favoured anywhere in the realm */
static float top_weight(int faction, const char *rname)
{
    float ret = good_weight(faction, 0, rname);
    for(int i = 0; i < vec_size(&s_priorities); i++) {
        const struct good_priority *curr = &vec_AT(&s_priorities, i);
        if(curr->district != 0 && curr->weight > ret && !strcmp(curr->rname, rname)) {
            ret = curr->weight;
        }
    }
    return ret;
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
    uint32_t node = G_Harvester_GatherNode(uid);
    bool node_out = astate->home_district && G_EntityExists(node) && !G_EntityIsZombie(node)
                 && !in_home_territory(G_GetFactionID(uid), astate->home_district, node);
    bool off_home = node_out || (astate->home_district && G_District_Of(uid) != astate->home_district);
    if(!off_good && !off_home)
        return;
    G_Duty_End(uid, "reconsidered");
    G_Harvester_Stop(uid);
    try_assign_work(uid, astate, false);
}

static void note_aimed(struct work_snapshot *snap, uint32_t node)
{
    int status;
    khiter_t k = kh_put(count, snap->aimed, node, &status);
    if(status == -1)
        return;
    kh_value(snap->aimed, k) = (status == 0) ? kh_value(snap->aimed, k) + 1 : 1;
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
            .starved = site_starved(uid, &sites),
            .road = road_work(uid)
        };
    }
    vec_entity_destroy(&sites);

    count_crews(s_snapshot.crew);
    if(!s_snapshot.pools) {
        s_snapshot.pools = kh_init(pool);
    }
    count_pools(s_snapshot.pools);

    if(!s_snapshot.aimed) {
        s_snapshot.aimed = kh_init(count);
    }
    kh_clear(count, s_snapshot.aimed);
    uint32_t targets[2048];
    size_t ntargets = G_Harvester_AimedTargets(ARR_SIZE(targets), targets);
    for(int i = 0; i < ntargets; i++) {
        note_aimed(&s_snapshot, targets[i]);
    }

    s_snapshot.ndemands = 0;
    s_snapshot.tick = s_tick;
    s_snapshot.valid = true;
    PERF_RETURN(&s_snapshot);
}

static int aimed_at(const struct work_snapshot *snap, uint32_t node)
{
    khiter_t k = kh_get(count, snap->aimed, node);
    return (k == kh_end(snap->aimed)) ? 0 : kh_value(snap->aimed, k);
}

/* The gatherers bound for the patch of nodes about a point, so that a patch
 * fills before the next is worked */
static int aimed_near(const struct work_snapshot *snap, vec2_t at)
{
    int ret = 0;
    uint32_t node;
    uint32_t count;
    kh_foreach(snap->aimed, node, count, {
        if(!G_EntityExists(node))
            continue;
        if(distance(G_Pos_GetXZ(node), at) <= PATCH_RADIUS) {
            ret += count;
        }
    });
    return ret;
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

/* Whether the point lies within the home district's own territory */
static bool in_home_territory(int faction, int home, uint32_t node)
{
    float cost;
    return G_District_OfNode(faction, node, &cost) == home && cost <= G_DISTRICT_TERRITORY_COST;
}

/* The node of the good to gather at: the nearest to the origin with room
 * about it, the gatherers already bound for one weighing against it, for
 * one that roams, only in a district that wants the good, and for one with
 * a home, only within its home's territory */
static uint32_t best_resource(uint32_t uid, vec2_t origin, const char *rname, float radius,
                              bool roams, int home, struct work_snapshot *snap)
{
    uint32_t nodes[512];
    float scores[512];
    size_t nnodes = G_Harvester_ResourcesNear(uid, origin, rname, radius, ARR_SIZE(nodes), nodes);
    float worker_radius = G_GetSelectionRadius(uid);
    int faction = G_GetFactionID(uid);

    for(int i = 0; i < nnodes; i++) {
        if(roams && node_weight(faction, nodes[i], rname) <= 0.0f) {
            scores[i] = INFINITY;
            continue;
        }
        if(home && !in_home_territory(faction, home, nodes[i])) {
            scores[i] = INFINITY;
            continue;
        }
        int aimed = aimed_at(snap, nodes[i]);
        if(aimed >= node_capacity(nodes[i], worker_radius)) {
            scores[i] = INFINITY;
            continue;
        }
        vec2_t at = G_Pos_GetXZ(nodes[i]);
        scores[i] = distance(at, origin) + CROWD_SLACK * aimed_near(snap, at);
    }
    for(int tries = 0; tries < nnodes; tries++) {
        int best = -1;
        for(int i = 0; i < nnodes; i++) {
            if(scores[i] < INFINITY && (best == -1 || scores[i] < scores[best]))
                best = i;
        }
        if(best == -1)
            break;
        if(G_Harvester_ResourceReachable(uid, nodes[best])) {
            struct sval setting;
            if(Settings_Get("pf.debug.log_perf_csv", &setting) == SS_OKAY && setting.as_bool) {
                fprintf(stdout, "[auto-node] %u,%u,%.1f,%d,%d,%d\n", uid, nodes[best], scores[best],
                    aimed_at(snap, nodes[best]), node_capacity(nodes[best], worker_radius), (int)nnodes);
            }
            return nodes[best];
        }
        scores[best] = INFINITY;
    }
    return NULL_UID;
}

/* Whether one of the hub cluster's buildings takes the good into its stores */
static bool hub_takes(int hub, const char *rname)
{
    uint32_t members[64];
    size_t nmembers = G_Cluster_Members(hub, ARR_SIZE(members), members);
    for(int i = 0; i < nmembers; i++) {
        if(G_StorageSite_Accepts(members[i], rname) && G_StorageSite_GetCapacity(members[i], rname) > 0)
            return true;
    }
    return false;
}

/* Where a worker's gathering is reckoned from, so that the nodes nearest the
 * hub are worked first wherever the worker stands: its home's hub, else the
 * hub of the district it is in where that takes the good, else itself */
static vec2_t gather_origin(uint32_t uid, const struct automation_state *astate, const char *rname)
{
    vec2_t pos = G_Pos_GetXZ(uid);
    int hub = astate->home_district ? astate->home_district : G_District_Of(uid);
    if(hub == 0)
        return pos;
    uint32_t anchor = G_Cluster_Anchor(hub);
    if(anchor == NULL_UID || !G_EntityExists(anchor))
        return pos;
    if(astate->home_district == 0 && !hub_takes(hub, rname))
        return pos;
    return G_Pos_GetXZ(anchor);
}

/* The node a worker would gather the good at: the best within the realm's
 * reach of its hub, in a district that wants it for one that roams, else for
 * one of no district where the realm lets it roam far, the nearest however far */
static uint32_t gather_node(uint32_t uid, const struct automation_state *astate, const char *rname,
                            struct work_snapshot *snap)
{
    int faction = G_GetFactionID(uid);
    vec2_t origin = gather_origin(uid, astate, rname);
    bool roams = (astate->home_district == 0 && strcmp(astate->pinned, rname));
    uint32_t ret = best_resource(uid, origin, rname, option(faction, OPT_REACH), roams,
        astate->home_district, snap);
    if(ret == NULL_UID && astate->home_district == 0 && option(faction, OPT_ROAM_FAR) != 0.0f) {
        ret = G_Harvester_NearestResource(uid, origin, rname, 0.0f);
    }
    return ret;
}

static struct good_demand *demand_find(struct work_snapshot *snap, int faction, const char *rname)
{
    for(int i = 0; i < snap->ndemands; i++) {
        struct good_demand *curr = &snap->demands[i];
        if(curr->faction == faction && !strcmp(curr->rname, rname))
            return curr;
    }
    return NULL;
}

/* What the faction's stores could still take of the good */
static int stores_room(int faction, const char *rname)
{
    /* A class has room while any of its goods has */
    const struct good_class *cls = class_named(rname);
    if(cls) {
        int ret = 0;
        for(int i = 0; i < cls->n; i++) {
            ret = MAX(ret, stores_room(faction, cls->members[i]));
        }
        return ret;
    }
    return G_StorageSite_GetFactionCapacity(faction, rname) - G_StorageSite_GetFactionStored(faction, rname);
}

static bool node_of_good(uint32_t uid, void *arg)
{
    const char *rname = arg;
    uint32_t flags = G_FlagsGet(uid);
    if(!(flags & ENTITY_FLAG_RESOURCE) || (flags & ENTITY_FLAG_ZOMBIE))
        return false;
    if(G_Resource_GetDormant(uid) || G_Resource_GetAmount(uid) <= 0)
        return false;
    return !strcmp(G_Resource_GetName(uid), rname);
}

/* What one gatherer brings in of the good a minute from the node: the walk
 * to the store and back with a load, and the swings for it */
static float node_rate(uint32_t node, int faction)
{
    float dist = G_Resource_DropDistance(node, faction);
    if(dist >= INFINITY)
        return 0.0f;
    float trip_s = 2.0f * dist / WORKER_SPEED + GATHER_LOAD * GATHER_S_PER_UNIT;
    return GATHER_LOAD * 60.0f / trip_s;
}

/* Of goods a site eats in one another's stead, the one its draw is gathered
 * as: the one the realm's stores have most to spare of, else the one with a
 * node nearest the site */
static const char *gathered_member(int faction, uint32_t site, size_t nmembers, const char **members,
                                   const vec_entity_t *sites)
{
    const char *ret = members[0];
    int best = -1;
    for(size_t m = 0; m < nmembers; m++) {
        int spare = faction_spare(sites, faction, members[m]);
        if(spare > best) {
            best = spare;
            ret = members[m];
        }
    }
    if(best > 0)
        return ret;
    float nearest = INFINITY;
    vec2_t at = G_Pos_GetXZ(site);
    for(size_t m = 0; m < nmembers; m++) {
        uint32_t node = G_Pos_NearestWithPred(at, node_of_good, (void*)members[m], option(faction, OPT_REACH));
        if(node == NULL_UID)
            continue;
        float dist = distance(G_Pos_GetXZ(node), at);
        if(dist < nearest) {
            nearest = dist;
            ret = members[m];
        }
    }
    return ret;
}

/* What the faction's buildings eat of the good between them, a minute */
static float realm_draw(int faction, const char *rname, const vec_entity_t *sites)
{
    float ret = 0.0f;
    for(int i = 0; i < vec_size(sites); i++) {
        uint32_t site = vec_AT(sites, i);
        if(G_EntityIsZombie(site) || !is_representative(site) || G_GetFactionID(site) != faction)
            continue;
        float rate = G_StorageSite_GetDraw(site, rname);
        if(rate <= 0.0f)
            continue;
        const char *members[MAX_RESOURCES];
        size_t nmembers = G_StorageSite_DrawMembers(site, rname, ARR_SIZE(members), members);
        if(nmembers > 1 && strcmp(gathered_member(faction, site, nmembers, members, sites), rname))
            continue;
        ret += rate;
    }
    return ret;
}

/* What the gatherers bring in against what their nodes were reckoned to
 * yield, so that the reckoning learns from the realm's own pace */
static float rate_fix(int faction, const char *rname, const struct work_snapshot *snap)
{
    const struct good_priority *realm = priority_get(faction, 0, rname, false);
    if(!realm || realm->gatherer_s < MIN_RATE_SAMPLE_S || realm->gathered <= 0.0f)
        return 1.0f;
    float predicted = 0.0f;
    int aimed = 0;
    uint32_t node;
    uint32_t count;
    kh_foreach(snap->aimed, node, count, {
        if(!G_EntityExists(node) || G_EntityIsZombie(node) || strcmp(G_Resource_GetName(node), rname))
            continue;
        predicted += count * node_rate(node, faction);
        aimed += count;
    });
    if(aimed == 0 || predicted <= 0.0f)
        return 1.0f;
    float fix = gatherer_rate(realm) / (predicted / aimed);
    return MAX(RATE_FIX_MIN, MIN(fix, RATE_FIX_MAX));
}

/* How many gatherers the realm's draw of the good takes: the nodes nearest
 * its stores first, each up to its rim, a far patch asking more of them */
static int gatherers_for_draw(int faction, const char *rname, const struct work_snapshot *snap)
{
    vec_entity_t sites;
    vec_entity_init(&sites);
    G_StorageSite_GetAll(&sites);
    float draw = realm_draw(faction, rname, &sites);
    if(draw <= 0.0f) {
        vec_entity_destroy(&sites);
        return 0;
    }
    float fix = rate_fix(faction, rname, snap);

    uint32_t nodes[512];
    float rates[512];
    size_t nnodes = 0;
    float reach = option(faction, OPT_REACH);
    for(int i = 0; i < vec_size(&sites) && nnodes < ARR_SIZE(nodes); i++) {
        uint32_t site = vec_AT(&sites, i);
        if(G_EntityIsZombie(site) || G_GetFactionID(site) != faction || G_StorageSite_GetUseAlt(site))
            continue;
        if(!G_StorageSite_Accepts(site, rname) || G_StorageSite_GetCapacity(site, rname) <= 0)
            continue;
        uint32_t found[512];
        size_t nfound = G_Pos_EntsInCircleWithPred(G_Pos_GetXZ(site), reach, found, ARR_SIZE(found),
            node_of_good, (void*)rname);
        for(size_t j = 0; j < nfound && nnodes < ARR_SIZE(nodes); j++) {
            bool seen = false;
            for(size_t k = 0; k < nnodes; k++) {
                if(nodes[k] == found[j]) {
                    seen = true;
                    break;
                }
            }
            if(seen)
                continue;
            nodes[nnodes] = found[j];
            rates[nnodes] = node_rate(found[j], faction) * fix;
            nnodes++;
        }
    }
    vec_entity_destroy(&sites);

    int workers = 0;
    float left = draw;
    for(size_t tries = 0; tries < nnodes && left > 0.0f; tries++) {
        int best = -1;
        for(size_t i = 0; i < nnodes; i++) {
            if(rates[i] > 0.0f && (best == -1 || rates[i] > rates[best]))
                best = i;
        }
        if(best == -1)
            break;
        int rim = node_capacity(nodes[best], WORKER_RADIUS);
        int n = MIN(rim, (int)ceilf(left / rates[best]));
        workers += n;
        left -= n * rates[best];
        rates[best] = 0.0f;
    }
    if(left > 0.0f) {
        workers += (int)ceilf(left / GATHERER_RATE);
    }
    return workers;
}

/* A good counts as stocked only once its stock has stood a while above its
 * target and the band over it, so that a stock at the line does not send
 * its gatherers back and forth */
static bool stocked_held(int faction, const char *rname, int surplus, int shortfall, int target)
{
    int band = (int)(target * option(faction, OPT_STOCK_BAND));
    bool raw = stocked(surplus, shortfall, target + band);
    struct good_priority *realm = priority_get(faction, 0, rname, true);
    if(!realm)
        return raw;
    if(!raw) {
        realm->stocked_since = 0;
        return false;
    }
    if(realm->stocked_since == 0) {
        realm->stocked_since = s_tick;
    }
    return s_tick - realm->stocked_since >= STOCKED_HOLD_TICKS;
}

/* How long the good in store, with those eaten in its stead, lasts the realm's
 * draw of them: bushes and game feed the one tavern, and only wheat the mill */
static float stock_minutes(int faction, const char *rname)
{
    vec_entity_t sites;
    vec_entity_init(&sites);
    G_StorageSite_GetAll(&sites);
    const char *seen[MAX_RESOURCES] = {rname};
    size_t nseen = 1;
    float draw = 0.0f;
    for(int i = 0; i < vec_size(&sites); i++) {
        uint32_t site = vec_AT(&sites, i);
        if(G_EntityIsZombie(site) || !is_representative(site) || G_GetFactionID(site) != faction)
            continue;
        float rate = G_StorageSite_GetDraw(site, rname);
        if(rate <= 0.0f)
            continue;
        draw += rate;
        const char *members[MAX_RESOURCES];
        size_t nmembers = G_StorageSite_DrawMembers(site, rname, ARR_SIZE(members), members);
        for(size_t m = 0; m < nmembers; m++) {
            bool known = false;
            for(size_t k = 0; k < nseen; k++) {
                known = known || !strcmp(seen[k], members[m]);
            }
            if(!known && nseen < ARR_SIZE(seen)) {
                seen[nseen++] = members[m];
            }
        }
    }
    vec_entity_destroy(&sites);
    if(draw <= 0.0f)
        return NO_DRAW_MINUTES;
    int stored = 0;
    for(size_t k = 0; k < nseen; k++) {
        stored += MAX(G_StorageSite_GetFactionStored(faction, seen[k]), 0);
    }
    return stored / MAX(draw, 1.0f);
}

static struct good_demand *good_demand_get(struct work_snapshot *snap, int faction, const char *rname)
{
    struct good_demand *found = demand_find(snap, faction, rname);
    if(found)
        return found;
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
        .wanted = gatherers_allowed(faction, rname,
            gatherers_wanted(surplus, shortfall, target,
                MAX(gatherers_for_rate(faction, rname), gatherers_for_draw(faction, rname, snap)))),
        .surplus = surplus,
        .shortfall = shortfall,
        .stocked = stocked_held(faction, rname, surplus, shortfall, target),
        .stock_minutes = stock_minutes(faction, rname)
    };
    pf_strlcpy(ret->rname, rname, sizeof(ret->rname));
    return ret;
}

/* The demand of a class is its members' together: one base need, and
 * stocked only when every member is */
static struct good_demand *class_demand_get(struct work_snapshot *snap, int faction, const struct good_class *cls)
{
    struct good_demand *found = demand_find(snap, faction, cls->name);
    if(found)
        return found;
    struct good_demand sum = (struct good_demand){ .faction = faction, .stocked = true };
    int nmembers = 0;
    for(int i = 0; i < cls->n; i++) {
        struct good_demand *curr = good_demand_get(snap, faction, cls->members[i]);
        if(!curr)
            continue;
        sum.target += curr->target;
        sum.need += curr->need;
        sum.workers += curr->workers;
        sum.wanted += curr->wanted;
        sum.surplus += curr->surplus;
        sum.shortfall += curr->shortfall;
        sum.stocked = sum.stocked && curr->stocked;
        nmembers++;
    }
    if(nmembers == 0 || snap->ndemands == MAX_DEMANDS)
        return NULL;
    sum.need -= (nmembers - 1) * BASE_NEED;
    struct good_demand *ret = &snap->demands[snap->ndemands++];
    *ret = sum;
    pf_strlcpy(ret->rname, cls->name, sizeof(ret->rname));
    return ret;
}

/* The demand of a good, or of the class it belongs to */
static struct good_demand *demand_get(struct work_snapshot *snap, int faction, const char *rname)
{
    const struct good_class *cls = class_named(rname);
    if(!cls) {
        cls = class_of(rname);
    }
    if(cls)
        return class_demand_get(snap, faction, cls);
    return good_demand_get(snap, faction, rname);
}

static const struct good_class *class_named(const char *name)
{
    for(int i = 0; i < s_nclasses; i++) {
        if(!strcmp(s_classes[i].name, name))
            return &s_classes[i];
    }
    return NULL;
}

static const struct good_class *class_of(const char *rname)
{
    for(int i = 0; i < s_nclasses; i++) {
        for(int j = 0; j < s_classes[i].n; j++) {
            if(!strcmp(s_classes[i].members[j], rname))
                return &s_classes[i];
        }
    }
    return NULL;
}

/* The goods a worker could gather with the members of each class folded into
 * the class, in the place of the first of them; a pinned good stays its own */
static size_t fold_classes(const char *candidates[], size_t n)
{
    size_t ret = 0;
    for(int i = 0; i < n; i++) {
        const struct good_class *cls = class_of(candidates[i]);
        const char *name = cls ? cls->name : candidates[i];
        bool seen = false;
        for(int j = 0; j < ret; j++) {
            if(!strcmp(candidates[j], name)) {
                seen = true;
                break;
            }
        }
        if(!seen) {
            candidates[ret++] = name;
        }
    }
    return ret;
}

static float gather_speed_for(uint32_t uid, const char *name)
{
    const struct good_class *cls = class_named(name);
    if(!cls)
        return G_Harvester_GetGatherSpeed(uid, name);
    float ret = 0.0f;
    for(int i = 0; i < cls->n; i++) {
        ret = MAX(ret, G_Harvester_GetGatherSpeed(uid, cls->members[i]));
    }
    return ret;
}

/* The weight of a good, or the highest of a class's members' */
static float weight_for(int faction, int district, bool roams, const char *name)
{
    const struct good_class *cls = class_named(name);
    if(!cls)
        return roams ? top_weight(faction, name) : good_weight(faction, district, name);
    float ret = -1.0f;
    for(int i = 0; i < cls->n; i++) {
        float curr = roams ? top_weight(faction, cls->members[i]) : good_weight(faction, district, cls->members[i]);
        ret = MAX(ret, curr);
    }
    return ret;
}

/* How far the worker would go for a member of a class, the crowd about its node
 * and the minutes of it in store weighing against it; INFINITY for none to gather */
static float member_cost(uint32_t uid, const struct automation_state *astate, const char *rname,
                         struct work_snapshot *snap, uint32_t *out_node)
{
    int faction = G_GetFactionID(uid);
    if(G_Harvester_GetGatherSpeed(uid, rname) <= 0.0f)
        return INFINITY;
    struct good_demand *raw = good_demand_get(snap, faction, rname);
    if(!raw || raw->stocked)
        return INFINITY;
    uint32_t node = gather_node(uid, astate, rname, snap);
    if(node == NULL_UID)
        return INFINITY;
    *out_node = node;
    vec2_t origin = gather_origin(uid, astate, rname);
    return distance(G_Pos_GetXZ(node), origin) + CROWD_SLACK * aimed_at(snap, node)
         + STOCK_MINUTE_SLACK * raw->stock_minutes;
}

/* The nearest node of any member of the class the worker can gather and that
 * is not stocked, the crowds about the nodes and the stock of the good weighing
 * against them */
static uint32_t class_node(uint32_t uid, const struct automation_state *astate, const struct good_class *cls,
                           struct work_snapshot *snap, const char **out_good)
{
    uint32_t ret = NULL_UID;
    float best = INFINITY;
    for(int i = 0; i < cls->n; i++) {
        uint32_t node = NULL_UID;
        float cost = member_cost(uid, astate, cls->members[i], snap, &node);
        if(cost < best) {
            best = cost;
            ret = node;
            *out_good = cls->members[i];
        }
    }
    return ret;
}

/* Whether another member of the worker's class is well behind his own: fruit
 * piling up at the nearest bushes does nothing for a mill out of wheat */
static bool member_wanted_more(uint32_t uid, const struct automation_state *astate, struct work_snapshot *snap,
                               const char *rname)
{
    const struct good_class *cls = class_of(rname);
    if(!cls)
        return false;
    uint32_t node = NULL_UID;
    float own = member_cost(uid, astate, rname, snap, &node);
    for(int i = 0; i < cls->n; i++) {
        if(!strcmp(cls->members[i], rname))
            continue;
        float cost = member_cost(uid, astate, cls->members[i], snap, &node);
        if(cost < INFINITY && cost + MEMBER_BAND < own)
            return true;
    }
    return false;
}

static uint32_t node_for(uint32_t uid, const struct automation_state *astate, const char *name,
                         struct work_snapshot *snap, const char **out_good)
{
    const struct good_class *cls = class_named(name);
    if(cls)
        return class_node(uid, astate, cls, snap, out_good);
    *out_good = name;
    return gather_node(uid, astate, name, snap);
}

/* Why the nearest unfinished site, if any, took no builder */
static void note_passed_site(uint32_t uid, struct work_snapshot *snap, vec2_t pos, int faction, int district)
{
    struct work_site *nearest = NULL;
    float best = 0.0f;
    for(int i = 0; i < snap->nsites; i++) {
        struct work_site *curr = &snap->sites[i];
        if(curr->faction != faction || !unfinished_site(curr->uid))
            continue;
        if(district && G_District_Of(curr->uid) != district)
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

/* Gives the worker building, else gathering; a spare one, with no other work,
 * builds past its pool's share and does not gather */
static bool try_assign_work(uint32_t uid, struct automation_state *astate, bool spare)
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

    bool crew = in_crew(astate);
    if((flags & ENTITY_FLAG_BUILDER) && astate->pinned[0] && !build_pinned(astate)) {
        trace_note(uid, "build", "skipped", "pinned", astate->pinned, NULL_UID);
    }else if((flags & ENTITY_FLAG_BUILDER) && crew && crew_full(snap, faction)) {
        trace_note(uid, "build", "skipped", "crew_full", NULL, NULL_UID);
    }else if((flags & ENTITY_FLAG_BUILDER) && !spare && share_full(snap, uid, astate, SHARE_BUILD)) {
        trace_note(uid, "build", "skipped", "share", NULL, NULL_UID);
    }else if(flags & ENTITY_FLAG_BUILDER) {
        /* Builders spread over the sites that can be worked on; one waiting on its 
         * materials gets one to found it, and the rest gather and carry them */
        struct work_site *site = pick_site(snap, pos, faction, astate->home_district);
        if(site) {
            G_Duty_TagNext(uid, by_of(astate));
        }
        if(site && G_Builder_Build(uid, site->uid)) {
            trace_note(uid, "build", "taken", "site", NULL, site->uid);
            site->builders++;
            if(crew) {
                snap->crew[faction]++;
            }
            share_taken(snap, uid, astate, SHARE_BUILD);
            return true;
        }
        if(site) {
            trace_note(uid, "build", "failed", "unreachable", NULL, site->uid);
        }else{
            note_passed_site(uid, snap, pos, faction, astate->home_district);
        }
    }

    if(!(flags & ENTITY_FLAG_HARVESTER))
        return false;
    /* A load left in hand is brought in before anything else */
    if(G_Harvester_GetCurrTotalCarry(uid)) {
        bool ret = G_Harvester_BringInLoad(uid, NULL_UID, false);
        trace_note(uid, "load", ret ? "taken" : "failed", "in_hand", NULL, NULL_UID);
        return ret;
    }
    if(build_pinned(astate)) {
        trace_note(uid, "gather", "skipped", "build_pin", NULL, NULL_UID);
        return false;
    }
    if(posts_pinned(astate)) {
        trace_note(uid, "gather", "skipped", "posts_pin", NULL, NULL_UID);
        return false;
    }

    const char *candidates[MAX_RESOURCES];
    size_t ncandidates;
    if(astate->pinned[0]) {
        candidates[0] = astate->pinned;
        ncandidates = 1;
    }else{
        ncandidates = G_Harvester_GetGatherable(uid, ARR_SIZE(candidates), candidates);
        ncandidates = fold_classes(candidates, ncandidates);
    }
    if(ncandidates == 0)
        return false;

    /* Most wanted per worker already at it, weighted by how fast this one
     * gathers it, so that a specialist keeps to his trade, and by how much 
     * the realm or the worker's district favours it. One of no district goes
     * by the district of the node it would gather at */
    int district = work_district(uid, astate);
    float scores[MAX_RESOURCES];
    struct good_demand *demands[MAX_RESOURCES];
    const char *why[MAX_RESOURCES];
    uint32_t nodes[MAX_RESOURCES];
    bool searched[MAX_RESOURCES];
    /* The good of the node found, which for a class is one of its members' */
    const char *goods[MAX_RESOURCES];
    for(int i = 0; i < ncandidates; i++) {
        demands[i] = demand_get(snap, faction, candidates[i]);
        bool pinned = !strcmp(astate->pinned, candidates[i]);
        bool roams = (astate->home_district == 0 && !pinned);
        float weight = weight_for(faction, district, roams, candidates[i]);
        nodes[i] = NULL_UID;
        searched[i] = false;
        goods[i] = candidates[i];
        if(roams && demands[i] && demands[i]->workers < (int)ceilf(demands[i]->wanted * MAX(weight, 1.0f))) {
            nodes[i] = node_for(uid, astate, candidates[i], snap, &goods[i]);
            searched[i] = true;
            weight = (nodes[i] != NULL_UID) ? node_weight(faction, nodes[i], goods[i])
                                            : good_weight(faction, district, goods[i]);
        }
        if(!pinned && weight <= 0.0f) {
            scores[i] = -1.0f;
            why[i] = "not_wanted";
            continue;
        }
        int wanted = demands[i] ? (int)ceilf(demands[i]->wanted * MAX(weight, 1.0f)) : 0;
        /* With no store left to take a load, nobody goes for one; a spare
         * worker, with nothing else to do, goes on past the count wanted
         * while the stores have room for more */
        bool room = demands[i] && stores_room(faction, candidates[i]) >= GATHER_LOAD;
        if(!demands[i] || !room) {
            scores[i] = -1.0f;
            why[i] = demands[i] ? "full" : "enough";
            continue;
        }
        if(!pinned && !spare && demands[i]->workers >= wanted) {
            scores[i] = -1.0f;
            why[i] = demands[i]->stocked ? "stocked" : "enough";
            continue;
        }
        scores[i] = weight * demands[i]->need * gather_speed_for(uid, candidates[i]) 
                  / (1.0f + demands[i]->workers);
        why[i] = "lower";

        /* The nearer work comes first, a good further out once that is taken */
        if(!searched[i]) {
            nodes[i] = node_for(uid, astate, candidates[i], snap, &goods[i]);
            searched[i] = true;
        }
        if(nodes[i] == NULL_UID) {
            scores[i] = -1.0f;
            why[i] = "no_node";
            continue;
        }
        float dist = distance(G_Pos_GetXZ(nodes[i]), gather_origin(uid, astate, goods[i]));
        scores[i] /= 1.0f + option(faction, OPT_BREADTH) * dist / BREADTH_SCALE;
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
        bool pinned = !strcmp(astate->pinned, candidates[best]);
        bool roams = (astate->home_district == 0 && !pinned);
        uint32_t node = searched[best] ? nodes[best] : node_for(uid, astate, candidates[best], snap, &goods[best]);
        if(node != NULL_UID && roams && node_weight(faction, node, goods[best]) <= 0.0f) {
            why[best] = "not_wanted";
            scores[best] = -1.0f;
            continue;
        }
        if(node != NULL_UID) {
            G_Duty_TagNext(uid, by_of(astate));
        }
        if(node != NULL_UID && G_Harvester_Gather(uid, node)) {
            note_aimed(snap, node);
            pf_strlcpy(astate->working, goods[best], sizeof(astate->working));
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
    if(build_pinned(astate))
        return "build_pin";
    return "hungry";
}

/* Gives the worker a carrying job; a spare one, with no other work, carries
 * past its pool's share */
static bool try_assign_transport_job(uint32_t uid, struct automation_state *astate, bool spare)
{
    if(!waiting_carrier(uid, astate)) {
        trace_note(uid, "carry", "skipped", not_waiting_why(uid, astate), NULL, NULL_UID);
        return false;
    }
    struct work_snapshot *snap = snapshot_get();
    if(!spare && share_full(snap, uid, astate, SHARE_CARRY)) {
        trace_note(uid, "carry", "skipped", "share", NULL, NULL_UID);
        return false;
    }

    vec_entity_t carriers;
    vec_entity_init(&carriers);
    vec_entity_push(&carriers, uid);
    assign_jobs(&carriers, false);
    bool assigned = (vec_size(&carriers) == 0);
    vec_entity_destroy(&carriers);

    if(assigned) {
        share_taken(snap, uid, astate, SHARE_CARRY);
        trace_carry_taken(uid);
    }else{
        trace_note(uid, "carry", "skipped", "nothing", NULL, NULL_UID);
    }
    return assigned;
}

static void assign_transport_jobs(void)
{
    vec_entity_t carriers, laden, over;
    vec_entity_init(&carriers);
    vec_entity_init(&laden);
    vec_entity_init(&over);

    uint32_t uid;
    struct automation_state *astate;
    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {
        if(astate->state != STATE_IDLE || G_Duty_Kind(uid) != DUTY_NONE)
            continue;
        /* A carrier whose drop-off fell through brings the load in itself */
        if(astate->automatic_transport && (G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER)
        && G_Harvester_GetCurrTotalCarry(uid)) {
            vec_entity_push(&laden, uid);
            continue;
        }
        if(!waiting_carrier(uid, astate))
            continue;
        if(pool_member(uid)) {
            struct work_snapshot *snap = snapshot_get();
            /* The share is a floor: those over it carry what is left waiting */
            if(share_full(snap, uid, astate, SHARE_CARRY)) {
                vec_entity_push(&over, uid);
                continue;
            }
            share_taken(snap, uid, astate, SHARE_CARRY);
        }
        vec_entity_push(&carriers, uid);
    });

    for(int i = 0; i < vec_size(&laden); i++) {
        uint32_t uid = vec_AT(&laden, i);
        bool ret = G_Harvester_BringInLoad(uid, NULL_UID, false);
        trace_note(uid, "load", ret ? "taken" : "failed", "in_hand", NULL, NULL_UID);
    }

    vec_entity_t waiting;
    vec_entity_init(&waiting);
    vec_entity_copy(&waiting, &carriers);
    for(int i = 0; i < vec_size(&over); i++) {
        vec_entity_push(&waiting, vec_AT(&over, i));
    }
    assign_jobs(&carriers, true);
    assign_jobs(&over, false);

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
    vec_entity_destroy(&over);
    vec_entity_destroy(&carriers);
}

/* A worker of its own choosing, empty-handed and fed: one idle, or one gathering
 * by itself rather than carrying or building */
static bool post_worker(uint32_t uid, const struct automation_state *astate, struct post_worker *out)
{
    uint32_t flags = G_FlagsGet(uid);
    if(!astate->automatic_work || !astate->automatic_posts || (astate->pinned[0] && !posts_pinned(astate))
    || !(flags & ENTITY_FLAG_HARVESTER) || option(G_GetFactionID(uid), OPT_MAN_WORKSHOPS) == 0.0f)
        return false;
    if(flags & (ENTITY_FLAG_GARRISONED | ENTITY_FLAG_ZOMBIE))
        return false;
    if((flags & ENTITY_FLAG_GARRISON) && G_Garrison_IsIngredient(uid))
        return false;
    if(G_Posts_Holds(uid) || G_Provision_WantsFood(uid) || G_Provision_Fetching(uid)
    || G_Harvester_GetCurrTotalCarry(uid))
        return false;
    enum duty_kind kind = G_Duty_Kind(uid);
    if(kind != DUTY_NONE && (kind != DUTY_GATHER || G_Duty_ByPlayer(uid)))
        return false;
    bool gathering = (astate->state == STATE_ACTIVE && astate->working[0]
        && astate->transport_target == NULL_UID
        && (!(flags & ENTITY_FLAG_BUILDER) || G_Builder_Idle(uid)));
    /* One at the end of its work is as free as one idle: its post comes before any job */
    bool free = (astate->state == STATE_IDLE || astate->state == STATE_STOPPING);
    if(!free && !gathering)
        return false;
    *out = (struct post_worker){
        .uid = uid,
        .faction = G_GetFactionID(uid),
        .home_district = astate->home_district,
        .pos = G_Pos_GetXZ(uid),
        .idle = free
    };
    return true;
}

/* The worker the post goes to: the one it waits for, else one homed in the
 * workshop's district before one of no district within reach, one idle before
 * one gathering, and the nearest of those */
static int pick_post_worker(const struct post_vacancy *vac, const struct post_worker *workers, size_t nworkers)
{
    vec2_t at = G_Pos_GetXZ(vac->workshop);
    int ret = -1, best_tier = 0;
    float best_dist = 0.0f;
    for(int i = 0; i < nworkers; i++) {
        const struct post_worker *curr = &workers[i];
        if(curr->taken || curr->faction != vac->faction || curr->uid == vac->shunned)
            continue;
        if(vac->awaited != NULL_UID) {
            if(curr->uid == vac->awaited)
                return i;
            continue;
        }
        bool homed = (vac->district != 0 && curr->home_district == vac->district);
        if(!homed && curr->home_district != 0)
            continue;
        float dist = distance(curr->pos, at);
        if(!homed && dist > WORK_RADIUS)
            continue;
        int tier = (homed ? 0 : 2) + (curr->idle ? 0 : 1);
        if(ret == -1 || tier < best_tier || (tier == best_tier && dist < best_dist)) {
            ret = i;
            best_tier = tier;
            best_dist = dist;
        }
    }
    return ret;
}

static bool take_post(uint32_t uid, struct automation_state *astate, uint32_t workshop)
{
    G_Duty_End(uid, "post");
    G_Duty_TagNext(uid, by_of(astate));
    if(!G_Posts_Take(workshop, uid))
        return false;
    astate->working[0] = '\0';
    if(trace_get(uid, astate->state != STATE_STOPPING)) {
        trace_note(uid, "post", "taken", "workshop", NULL, workshop);
    }
    return true;
}

/* A worker at the end of its work takes an open post of its own district's
 * workshops, or the one kept for it while it ate; one of no district takes
 * one that the district's own workers have left open a while */
static bool try_take_post(uint32_t uid, struct automation_state *astate)
{
    struct post_worker worker;
    if(!post_worker(uid, astate, &worker))
        return false;
    struct post_vacancy vacs[MAX_VACANCIES];
    size_t nvacs = G_Posts_Vacancies(ARR_SIZE(vacs), vacs);
    for(int i = 0; i < nvacs; i++) {
        if(vacs[i].awaited == NULL_UID && worker.home_district == 0
        && vacs[i].waited_ticks < HOMED_FIRST_TICKS)
            continue;
        if(pick_post_worker(&vacs[i], &worker, 1) == -1)
            continue;
        if(take_post(uid, astate, vacs[i].workshop))
            return true;
    }
    return false;
}

/* Fills the open posts, the most urgent workshops first, looking over the
 * workers only when there is one */
static void assign_posts(void)
{
    struct post_vacancy vacs[MAX_VACANCIES];
    size_t nvacs = G_Posts_Vacancies(ARR_SIZE(vacs), vacs);
    if(nvacs == 0)
        return;

    struct post_worker *workers = PF_MALLOC(kh_size(s_entity_state_table) * sizeof(struct post_worker));
    if(!workers)
        return;
    size_t nworkers = 0;
    uint32_t uid;
    struct automation_state *astate;
    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {
        if(post_worker(uid, astate, &workers[nworkers])) {
            nworkers++;
        }
    });

    for(int i = 0; i < nvacs && nworkers > 0; i++) {
        int idx = pick_post_worker(&vacs[i], workers, nworkers);
        if(idx == -1)
            continue;
        struct automation_state *astate = astate_get(workers[idx].uid);
        if(astate && take_post(workers[idx].uid, astate, vacs[i].workshop)) {
            workers[idx].taken = true;
        }
    }
    PF_FREE(workers);
}

static void assign_work(void)
{
    vec_entity_t workers;
    vec_entity_init(&workers);

    uint32_t uid;
    struct automation_state *astate;
    vec_entity_t held;
    vec_entity_init(&held);
    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {
        if(astate->state != STATE_IDLE)
            continue;
        if(G_Duty_Kind(uid) != DUTY_NONE) {
            vec_entity_push(&held, uid);
            continue;
        }
        if(!astate->automatic_work)
            continue;
        vec_entity_push(&workers, uid);
    });
    /* One left idle with a duty it could not take up tries again, until it is given up */
    for(int i = 0; i < vec_size(&held); i++) {
        uint32_t curr = vec_AT(&held, i);
        if(G_Duty_Resume(curr) || G_Duty_Kind(curr) != DUTY_NONE)
            continue;
        /* Given up, it settles like any other: the idle events tell of it */
        struct automation_state *cstate = astate_get(curr);
        if(cstate && cstate->state == STATE_IDLE) {
            cstate->transient_ticks = 0;
            cstate->state = STATE_STOPPING;
        }
    }
    vec_entity_destroy(&held);

    vec_entity_t spare, unassigned;
    vec_entity_init(&spare);
    vec_entity_init(&unassigned);
    for(int i = 0; i < vec_size(&workers); i++) {
        uint32_t uid = vec_AT(&workers, i);
        struct automation_state *astate = astate_get(uid);
        /* A carrier handed a job in this pass is still counted idle until the next tick */
        if(!astate || astate->state != STATE_IDLE || !idle(uid))
            continue;
        if(try_assign_work(uid, astate, false))
            continue;
        vec_entity_push(&unassigned, uid);
        if(waiting_carrier(uid, astate)) {
            vec_entity_push(&spare, uid);
        }
    }

    /* Those with nothing else to do carry and build past their pools' shares */
    assign_jobs(&spare, false);
    for(int i = 0; i < vec_size(&unassigned); i++) {
        uint32_t uid = vec_AT(&unassigned, i);
        struct automation_state *astate = astate_get(uid);
        if(!astate)
            continue;
        if(G_Harvester_TransportTarget(uid) != NULL_UID) {
            trace_carry_taken(uid);
            continue;
        }
        if(!try_assign_work(uid, astate, true)) {
            trace_note(uid, "idle", "taken", "no_work", NULL, NULL_UID);
        }
    }
    vec_entity_destroy(&unassigned);
    vec_entity_destroy(&spare);
    vec_entity_destroy(&workers);
}

static uint64_t tick_us(uint64_t since)
{
    return (SDL_GetPerformanceCounter() - since) * 1000000 / SDL_GetPerformanceFrequency();
}

static void on_20hz_tick(void *user, void *event)
{
    s_tick++;
    uint64_t t0 = SDL_GetPerformanceCounter();
    PERF_PUSH("automation::recompute_idle");
    recompute_idle();
    PERF_POP();
    PERF_PUSH("automation::settle_stopping");
    settle_stopping();
    PERF_POP();
    uint64_t t1 = SDL_GetPerformanceCounter();
    if(s_tick % RATE_SAMPLE_TICKS == 0) {
        sample_gather_rates(RATE_SAMPLE_TICKS / 20.0f);
    }
    uint64_t t2 = SDL_GetPerformanceCounter(), t3 = t2;
    if(s_tick % JOB_PASS_TICKS == 0) {
        PERF_PUSH("automation::assign_posts");
        assign_posts();
        PERF_POP();
        PERF_PUSH("automation::assign_transport_jobs");
        assign_transport_jobs();
        PERF_POP();
        t3 = SDL_GetPerformanceCounter();
        PERF_PUSH("automation::assign_work");
        assign_work();
        PERF_POP();
    }
    struct sval setting;
    if(Settings_Get("pf.debug.log_perf_csv", &setting) == SS_OKAY && setting.as_bool
    && tick_us(t0) > 5000) {
        fprintf(stdout, "[auto-prof] %u,%lu,%lu,%lu,%lu\n", s_tick,
            (unsigned long)((t1 - t0) * 1000000 / SDL_GetPerformanceFrequency()),
            (unsigned long)((t2 - t1) * 1000000 / SDL_GetPerformanceFrequency()),
            (unsigned long)((t3 - t2) * 1000000 / SDL_GetPerformanceFrequency()),
            (unsigned long)tick_us(t3));
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

/* A worker the player gives an order to is his to manage from then on: the
 * automation and its city leave it be */
static void on_player_order_issued(void *user, void *event)
{
    G_Automation_MakePlayerManaged((uintptr_t)event);
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
        .automatic_posts = true,
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
    if(astate->transport_target != NULL_UID) {
        try_decrement_assigned_transporters(astate->transport_target);
    }
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_order_issued);
    astate_remove(uid);

    k = kh_get(trace, s_traces, uid);
    if(k != kh_end(s_traces)) {
        kh_del(trace, s_traces, k);
    }
    k = kh_get(events, s_events, uid);
    if(k != kh_end(s_events)) {
        kh_del(events, s_events, k);
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
    if((s_events = kh_init(events)) == NULL)
        goto fail_events;
    if((s_reassigned = kh_init(count)) == NULL)
        goto fail_reassigned;
    s_tick = 0;
    vec_request_init(&s_snapshot.requests);
    vec_priority_init(&s_priorities);
    vec_district_init(&s_kept_home);
    vec_district_init(&s_carried_out);
    for(int i = 0; i < MAX_FACTIONS; i++) {
        s_crew_size[i] = CREW_SIZE;
    }
    reset_options();
    s_snapshot.valid = false;

    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
    E_Global_Register(EVENT_UPDATE_UI, on_update_ui, NULL, G_RUNNING);
    E_Global_Register(EVENT_ORDER_ISSUED, on_order_issued, NULL, G_RUNNING);
    E_Global_Register(EVENT_PLAYER_ORDER_ISSUED, on_player_order_issued, NULL, G_RUNNING);
    return true;

fail_reassigned:
    kh_destroy(events, s_events);
fail_events:
    kh_destroy(trace, s_traces);
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

size_t G_Automation_GetEvents(uint32_t uid, size_t maxout, struct work_step out[], int out_age_ms[])
{
    khiter_t k = kh_get(events, s_events, uid);
    if(k == kh_end(s_events))
        return 0;
    const struct work_events *ev = &kh_value(s_events, k);
    size_t ret = MIN(ev->count, maxout);
    for(int i = 0; i < ret; i++) {
        size_t idx = (ev->head + MAX_WORK_EVENTS - 1 - i) % MAX_WORK_EVENTS;
        out[i] = ev->steps[idx];
        out_age_ms[i] = (s_tick - ev->ticks[idx]) * TICK_MS;
    }
    return ret;
}

size_t G_Automation_CarryLanes(int faction, size_t maxout, struct carry_lane out[])
{
    size_t ret = 0;
    for(size_t i = 0; i < s_nlanes && ret < maxout; i++) {
        if(s_lanes[i].faction == faction) {
            out[ret++] = s_lanes[i];
        }
    }
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
    E_Global_Unregister(EVENT_PLAYER_ORDER_ISSUED, on_player_order_issued);
    E_Global_Unregister(EVENT_UPDATE_UI, on_update_ui);
    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
    vec_request_destroy(&s_snapshot.requests);
    vec_priority_destroy(&s_priorities);
    vec_district_destroy(&s_kept_home);
    vec_district_destroy(&s_carried_out);
    vec_entity_destroy(&s_stopping);
    kh_destroy(count, s_reassigned);
    kh_destroy(events, s_events);
    kh_destroy(trace, s_traces);
    kh_destroy(pending, s_pending);
    kh_destroy(count, s_transport_count);
    kh_destroy(state, s_entity_state_table);
}

uint32_t G_Automation_PostWorker(uint32_t workshop)
{
    struct post_vacancy vac = (struct post_vacancy){
        .workshop = workshop,
        .faction = G_GetFactionID(workshop),
        .district = G_District_Of(workshop),
        .awaited = NULL_UID,
        .shunned = NULL_UID
    };
    struct post_worker *workers = PF_MALLOC(kh_size(s_entity_state_table) * sizeof(struct post_worker));
    if(!workers)
        return NULL_UID;
    size_t nworkers = 0;
    uint32_t uid;
    struct automation_state *astate;
    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {
        if(post_worker(uid, astate, &workers[nworkers])) {
            nworkers++;
        }
    });
    int idx = pick_post_worker(&vac, workers, nworkers);
    uint32_t ret = (idx == -1) ? NULL_UID : workers[idx].uid;
    PF_FREE(workers);
    return ret;
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
    /* Sent to a post in the same pass that saw it idle */
    if(G_Posts_Holds(uid))
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

enum duty_by G_Automation_ManagedBy(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate || G_Automation_PlayerManaged(uid))
        return DUTY_BY_PLAYER;
    return by_of(astate);
}

void G_Automation_MakePlayerManaged(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate || !(G_FlagsGet(uid) & ENTITY_FLAG_BUILDER))
        return;
    astate->automatic_work = false;
    astate->home_district = 0;
    if(astate->automatic_transport) {
        G_Automation_SetAutomaticTransport(uid, false);
    }
    G_Duty_SetBy(uid, DUTY_BY_PLAYER);
}

void G_Automation_JoinCity(uint32_t uid, int district)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return;
    astate->automatic_work = true;
    if(!astate->automatic_transport) {
        G_Automation_SetAutomaticTransport(uid, true);
    }
    G_Automation_SetHomeDistrict(uid, district);
    G_Duty_SetBy(uid, DUTY_BY_CITY);
}

bool G_Automation_SetOption(int faction, const char *name, float value)
{
    int opt = option_index(name);
    if(opt < 0 || faction < 0 || faction >= MAX_FACTIONS)
        return false;
    const struct sched_option_desc *desc = &s_option_descs[opt];
    s_options[faction][opt] = MIN(MAX(value, desc->min), desc->max);
    s_snapshot.valid = false;
    return true;
}

bool G_Automation_GetOption(int faction, const char *name, float *out)
{
    int opt = option_index(name);
    if(opt < 0 || faction < 0 || faction >= MAX_FACTIONS)
        return false;
    *out = s_options[faction][opt];
    return true;
}

bool G_Automation_PlayerManaged(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate || !(G_FlagsGet(uid) & ENTITY_FLAG_BUILDER))
        return false;
    return !astate->automatic_work && !astate->automatic_transport && astate->home_district == 0;
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

void G_Automation_SetAutomaticPosts(uint32_t uid, bool on)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return;
    astate->automatic_posts = on;
}

bool G_Automation_GetAutomaticPosts(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return false;
    return astate->automatic_posts;
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

uint32_t G_Automation_BestResource(uint32_t uid, const char *rname)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate || !astate->automatic_work)
        return NULL_UID;
    struct work_snapshot *snap = snapshot_get();
    bool roams = (astate->home_district == 0 && strcmp(astate->pinned, rname));
    /* One on the player's order keeps to the patch it was sent to */
    vec2_t origin = gather_origin(uid, astate, rname);
    uint32_t node = best_resource(uid, origin, rname, option(G_GetFactionID(uid), OPT_REACH), roams,
        astate->home_district, snap);
    if(node != NULL_UID) {
        note_aimed(snap, node);
    }
    return node;
}

/* Whether the pool may take another worker off its work yet, and when it has */
static bool pool_may_reassign(uint32_t uid, const struct automation_state *astate)
{
    int faction = G_GetFactionID(uid);
    uint32_t every = (uint32_t)(option(faction, OPT_REBALANCE_S) * 20.0f);
    khiter_t k = kh_get(count, s_reassigned, pool_key(faction, astate->home_district));
    if(k == kh_end(s_reassigned))
        return true;
    return s_tick - kh_value(s_reassigned, k) >= every;
}

static void note_reassigned(uint32_t uid, const struct automation_state *astate)
{
    int status;
    khiter_t k = kh_put(count, s_reassigned, pool_key(G_GetFactionID(uid), astate->home_district), &status);
    if(status != -1) {
        kh_value(s_reassigned, k) = s_tick;
    }
}

/* Whether the worker has kept at its duty long enough to be moved */
static bool tenure_up(uint32_t uid)
{
    int faction = G_GetFactionID(uid);
    return G_Duty_AgeTicks(uid) >= (uint32_t)(option(faction, OPT_TENURE_S) * 20.0f);
}

/* Whether anything wants the worker more than the good it is at: a request
 * short of carriers it could serve, or a good short of gatherers it could
 * gather; a gatherer of a stocked good with nothing else to do keeps at it */
static bool more_urgent_work(uint32_t uid, struct automation_state *astate, struct work_snapshot *snap,
                             const char *rname)
{
    int faction = G_GetFactionID(uid);
    if(astate->automatic_transport && !share_full(snap, uid, astate, SHARE_CARRY)) {
        for(int i = 0; i < vec_size(&snap->requests); i++) {
            const struct job_request *req = &vec_AT(&snap->requests, i);
            if(req->shortfall <= 0 || G_GetFactionID(req->site) != faction)
                continue;
            if(astate->home_district && G_District_Of(req->site) != astate->home_district)
                continue;
            return true;
        }
    }
    const char *candidates[MAX_RESOURCES];
    size_t ncandidates = G_Harvester_GetGatherable(uid, ARR_SIZE(candidates), candidates);
    ncandidates = fold_classes(candidates, ncandidates);
    const struct good_class *own = class_of(rname);
    for(int i = 0; i < ncandidates; i++) {
        if(!strcmp(candidates[i], rname) || (own && !strcmp(candidates[i], own->name)))
            continue;
        struct good_demand *demand = demand_get(snap, faction, candidates[i]);
        if(!demand || demand->stocked || demand->workers >= demand->wanted)
            continue;
        const char *good;
        if(node_for(uid, astate, candidates[i], snap, &good) != NULL_UID)
            return true;
    }
    return false;
}

/* Whether a good the worker could gather is shorter of hands for its count
 * than its own would be without him, or has none while his has others. No
 * good stands unserved while another keeps every gatherer; equal shares leave
 * him where he is */
static bool hands_wanted_more(uint32_t uid, struct automation_state *astate, struct work_snapshot *snap,
                              const char *rname, const struct good_demand *own)
{
    int faction = G_GetFactionID(uid);
    int district = work_district(uid, astate);
    bool roams = (astate->home_district == 0);
    const char *candidates[MAX_RESOURCES];
    size_t ncandidates = G_Harvester_GetGatherable(uid, ARR_SIZE(candidates), candidates);
    ncandidates = fold_classes(candidates, ncandidates);
    const struct good_class *own_class = class_of(rname);
    int own_wanted = MAX(own->wanted, 1);
    for(int i = 0; i < ncandidates; i++) {
        if(!strcmp(candidates[i], rname) || (own_class && !strcmp(candidates[i], own_class->name)))
            continue;
        struct good_demand *demand = demand_get(snap, faction, candidates[i]);
        if(!demand || demand->stocked || demand->wanted <= demand->workers)
            continue;
        bool unserved = (demand->workers == 0 && own->workers >= 2);
        if(!unserved && (demand->workers + 1) * own_wanted >= own->workers * demand->wanted)
            continue;
        if(weight_for(faction, district, roams, candidates[i]) <= 0.0f)
            continue;
        if(stores_room(faction, candidates[i]) < GATHER_LOAD)
            continue;
        const char *good;
        if(node_for(uid, astate, candidates[i], snap, &good) != NULL_UID)
            return true;
    }
    return false;
}

static bool leaves_gathering(uint32_t uid, const char *rname, uint32_t node)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate || !astate->automatic_work)
        return false;
    /* A field worker or one resumed on its duty did not choose the good here,
     * but it is the automation's to move all the same */
    struct duty duty;
    bool kept = G_Duty_Get(uid, &duty) && duty.kind == DUTY_GATHER
             && duty.by != DUTY_BY_PLAYER && !strcmp(duty.good, rname);
    if((strcmp(astate->working, rname) && !kept) || !strcmp(astate->pinned, rname))
        return false;
    if(build_pinned(astate))
        return true;
    int faction = G_GetFactionID(uid);
    struct work_snapshot *snap = snapshot_get();
    struct good_demand *demand = demand_get(snap, faction, rname);
    /* The few that keep up its rate stay on, and so do the rest while nothing
     * more urgent wants them; one goes at a time, after a while at it */
    if(demand && demand->stocked && demand->workers > demand->wanted) {
        if(!tenure_up(uid) || !pool_may_reassign(uid, astate) || !more_urgent_work(uid, astate, snap, rname))
            return false;
        note_reassigned(uid, astate);
        demand->workers--;
        return true;
    }
    if(good_weight(faction, work_district(uid, astate), rname) <= 0.0f)
        return true;
    /* One that roams drops off far from where it gathers */
    if(astate->home_district == 0 && G_EntityExists(node) && !G_EntityIsZombie(node)
    && node_weight(faction, node, rname) <= 0.0f)
        return true;
    if(astate->pinned[0] || !(G_FlagsGet(uid) & ENTITY_FLAG_BUILDER))
        return false;
    if(wanted_elsewhere(uid, astate, snap)) {
        note_reassigned(uid, astate);
        if(demand) {
            demand->workers--;
        }
        return true;
    }
    if(demand && tenure_up(uid) && pool_may_reassign(uid, astate)
    && (hands_wanted_more(uid, astate, snap, rname, demand) || member_wanted_more(uid, astate, snap, rname))) {
        note_reassigned(uid, astate);
        demand->workers--;
        return true;
    }
    if(in_crew(astate) && crew_full(snap, faction))
        return false;
    if(share_full(snap, uid, astate, SHARE_BUILD))
        return false;
    if(option(faction, OPT_PULL_TO_BUILD) == 0.0f)
        return false;
    /* As with carrying, the first builder of a pool that has none does not wait */
    const struct pool_census *pool = pool_get(snap, uid, astate);
    if((!tenure_up(uid) && !(pool && pool->builders == 0)) || !pool_may_reassign(uid, astate))
        return false;
    /* Only for a site that can be worked, or founded: one waiting on its materials needs gatherers more */
    vec2_t pos = work_origin(uid, astate);
    for(int road = 0; road < 2; road++) {
        struct site_search arg = (struct site_search){
            .faction = faction,
            .district = astate->home_district,
            .max_builders = SITE_BUILDERS,
            .supplied = true,
            .road = road
        };
        if(nearest_site(snap, pos, &arg)) {
            note_reassigned(uid, astate);
            return true;
        }
        arg = (struct site_search){
            .faction = faction,
            .district = astate->home_district,
            .max_builders = 1,
            .unfounded = true,
            .road = road
        };
        if(nearest_site(snap, pos, &arg)) {
            note_reassigned(uid, astate);
            return true;
        }
    }
    if(far_site(snap, pos, faction, astate->home_district)) {
        note_reassigned(uid, astate);
        return true;
    }
    return false;
}

/* Whether a request can be served and has room for another carrier */
static bool carry_wanted(uint32_t uid, struct automation_state *astate, struct work_snapshot *snap)
{
    if(!astate->automatic_transport || share_full(snap, uid, astate, SHARE_CARRY))
        return false;
    vec_entity_t sites;
    vec_entity_init(&sites);
    G_StorageSite_GetAll(&sites);
    vec_source_t sources;
    vec_source_init(&sources);
    bool ret = false;
    int faction = G_GetFactionID(uid);
    for(int i = 0; !ret && i < vec_size(&snap->requests); i++) {
        const struct job_request *req = &vec_AT(&snap->requests, i);
        if(req->shortfall <= 0 || G_GetFactionID(req->site) != faction)
            continue;
        if(astate->home_district && G_District_Of(req->site) != astate->home_district)
            continue;
        vec_source_reset(&sources);
        collect_sources(&sources, &sites, req->rname, faction, req->site);
        ret = (vec_size(&sources) > 0);
    }
    vec_source_destroy(&sources);
    vec_entity_destroy(&sites);
    return ret;
}

static bool post_open_for(uint32_t uid, const struct automation_state *astate)
{
    struct post_worker worker;
    if(!post_worker(uid, astate, &worker))
        return false;
    struct post_vacancy vacs[MAX_VACANCIES];
    size_t nvacs = G_Posts_Vacancies(ARR_SIZE(vacs), vacs);
    for(int i = 0; i < nvacs; i++) {
        if(vacs[i].awaited == NULL_UID && worker.home_district == 0
        && vacs[i].waited_ticks < HOMED_FIRST_TICKS)
            continue;
        if(pick_post_worker(&vacs[i], &worker, 1) != -1)
            return true;
    }
    return false;
}

/* Whether a free gatherer goes from a good still short to carry or man a post.
 * The first carrier of a pool that has none does not wait out its tenure */
static bool wanted_elsewhere(uint32_t uid, struct automation_state *astate, struct work_snapshot *snap)
{
    if(!pool_may_reassign(uid, astate))
        return false;
    const struct pool_census *pool = pool_get(snap, uid, astate);
    bool up = tenure_up(uid);
    if((up || (pool && pool->carriers == 0)) && carry_wanted(uid, astate, snap))
        return true;
    return up && post_open_for(uid, astate);
}

/* Whether a gatherer of its own choosing leaves the good at its drop-off; its
 * duty ends with it, so that it is not taken up again */
bool G_Automation_SetGoodClass(const char *name, const char *members[], size_t n)
{
    if(n == 0 || n > MAX_CLASS_MEMBERS)
        return false;
    struct good_class *cls = NULL;
    for(int i = 0; i < s_nclasses; i++) {
        if(!strcmp(s_classes[i].name, name)) {
            cls = &s_classes[i];
        }
    }
    if(!cls) {
        if(s_nclasses == MAX_CLASSES)
            return false;
        cls = &s_classes[s_nclasses++];
    }
    pf_strlcpy(cls->name, name, sizeof(cls->name));
    cls->n = n;
    for(int i = 0; i < n; i++) {
        pf_strlcpy(cls->members[i], members[i], sizeof(cls->members[i]));
    }
    s_snapshot.valid = false;
    return true;
}

uint32_t G_Automation_GatherNode(uint32_t uid, const char *rname)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return NULL_UID;
    return gather_node(uid, astate, rname, snapshot_get());
}

bool G_Automation_LeavesGathering(uint32_t uid, const char *rname, uint32_t node)
{
    if(!leaves_gathering(uid, rname, node))
        return false;
    if(G_Duty_Kind(uid) == DUTY_GATHER && !G_Duty_ByPlayer(uid)) {
        G_Duty_End(uid, "leaves");
    }
    return true;
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

void G_Automation_OpenRequests(int faction, int *out_open, int *out_carriers)
{
    int open = 0, carriers = 0;
    for(khint_t k = kh_begin(s_pending); k != kh_end(s_pending); k++) {
        /* A fed drawer being topped up is not waiting on anyone */
        if(!kh_exist(s_pending, k) || !kh_value(s_pending, k).touched
        || kh_value(s_pending, k).tier > TIER_BUFFERING)
            continue;
        uint32_t site = (uint32_t)(kh_key(s_pending, k) >> 32);
        if(G_EntityExists(site) && !G_EntityIsZombie(site) && G_GetFactionID(site) == faction) {
            open++;
        }
    }
    uint32_t uid;
    struct automation_state *astate;
    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {
        if(!astate->automatic_transport || G_GetFactionID(uid) != faction)
            continue;
        uint32_t flags = G_FlagsGet(uid);
        if(!(flags & ENTITY_FLAG_HARVESTER) || (flags & (ENTITY_FLAG_GARRISONED | ENTITY_FLAG_ZOMBIE)))
            continue;
        carriers++;
    });
    *out_open = open;
    *out_carriers = carriers;
}

bool G_Automation_OutputStocked(uint32_t site, const char *rname, int limit)
{
    int faction = G_GetFactionID(site);
    uint32_t pool = pool_representative(site);
    struct work_snapshot *snap = snapshot_get();
    for(int i = 0; i < vec_size(&snap->requests); i++) {
        const struct job_request *req = &vec_AT(&snap->requests, i);
        if(req->site == pool || G_GetFactionID(req->site) != faction || strcmp(req->rname, rname))
            continue;
        if(could_serve(site, req))
            return false;
    }

    vec_entity_t sites;
    vec_entity_init(&sites);
    G_StorageSite_GetAll(&sites);
    int district = G_District_Of(site);
    int spare = 0;
    for(int i = 0; i < vec_size(&sites); i++) {
        uint32_t curr = vec_AT(&sites, i);
        if(G_EntityIsZombie(curr) || !is_representative(curr) || G_GetFactionID(curr) != faction)
            continue;
        if(curr != pool && (district == 0 || G_District_Of(curr) != district))
            continue;
        spare += G_StorageSite_Surplus(curr, rname);
    }
    vec_entity_destroy(&sites);
    return spare >= limit;
}

void G_Automation_TraceFollowOn(uint32_t uid, const char *kind, const char *why, 
                                const char *rname, uint32_t target)
{
    if(!trace_is_event(kind, why) && !trace_get(uid, true))
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

static void district_set_member(vec_district_t *set, int district, bool member)
{
    int idx = vec_district_indexof(set, district, districts_equal);
    if(!member && idx != -1) {
        vec_district_del(set, idx);
    }
    if(member && idx == -1) {
        vec_district_push(set, district);
    }
}

void G_Automation_SetCarryOutMode(int district, enum carry_out_mode mode)
{
    district_set_member(&s_kept_home, district, mode == CARRY_OUT_OFF);
    district_set_member(&s_carried_out, district, mode == CARRY_OUT_ON);
    s_snapshot.valid = false;
}

enum carry_out_mode G_Automation_GetCarryOutMode(int district)
{
    if(vec_district_indexof(&s_kept_home, district, districts_equal) != -1)
        return CARRY_OUT_OFF;
    if(vec_district_indexof(&s_carried_out, district, districts_equal) != -1)
        return CARRY_OUT_ON;
    return CARRY_OUT_AUTO;
}

void G_Automation_SetWorkersCarryOut(int district, bool on)
{
    G_Automation_SetCarryOutMode(district, on ? CARRY_OUT_AUTO : CARRY_OUT_OFF);
}

bool G_Automation_GetWorkersCarryOut(int district)
{
    return G_Automation_GetCarryOutMode(district) != CARRY_OUT_OFF;
}

void G_Automation_SetCrewSize(int faction, int size)
{
    if(faction < 0 || faction >= MAX_FACTIONS)
        return;
    s_crew_size[faction] = MAX(size, 0);
    s_snapshot.valid = false;
}

int G_Automation_GetCrewSize(int faction)
{
    if(faction < 0 || faction >= MAX_FACTIONS)
        return 0;
    return s_crew_size[faction];
}

int G_Automation_CrewBusy(int faction)
{
    if(faction < 0 || faction >= MAX_FACTIONS)
        return 0;
    int crews[MAX_FACTIONS];
    count_crews(crews);
    return crews[faction];
}

bool G_Automation_AwaitsBuilders(uint32_t site, int *out_busy, int *out_size)
{
    int faction = G_GetFactionID(site);
    *out_busy = G_Automation_CrewBusy(faction);
    *out_size = G_Automation_GetCrewSize(faction);
    if(!unfinished_site(site) || G_Builder_CountOn(site) > 0)
        return false;
    if(G_Building_IsFounded(site) && !G_Building_IsSupplied(site))
        return false;
    return (*out_busy >= *out_size);
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

    struct attr num_crews = (struct attr){
        .type = TYPE_INT,
        .val.as_int = MAX_FACTIONS
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_crews, "num_crew_sizes"));

    for(int i = 0; i < MAX_FACTIONS; i++) {
        struct attr size = (struct attr){
            .type = TYPE_INT,
            .val.as_int = s_crew_size[i]
        };
        CHK_TRUE_RET(Attr_Write(stream, &size, "crew_size"));
    }

    struct attr num_options = (struct attr){
        .type = TYPE_INT,
        .val.as_int = NUM_SCHED_OPTIONS
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_options, "num_sched_options"));

    for(int i = 0; i < NUM_SCHED_OPTIONS; i++) {
        struct attr name = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(name.val.as_string, s_option_descs[i].name, sizeof(name.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &name, "sched_option"));

        for(int j = 0; j < MAX_FACTIONS; j++) {
            struct attr value = (struct attr){
                .type = TYPE_FLOAT,
                .val.as_float = s_options[j][i]
            };
            CHK_TRUE_RET(Attr_Write(stream, &value, "value"));
        }
    }
    return true;
}

bool G_Automation_LoadPriorities(struct SDL_RWops *stream)
{
    struct attr attr;
    vec_priority_reset(&s_priorities);
    vec_district_reset(&s_kept_home);
    vec_district_reset(&s_carried_out);
    s_snapshot.valid = false;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_priorities = attr.val.as_int;

    for(int i = 0; i < num_priorities; i++) {

        struct good_priority prio = (struct good_priority){0};

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

    for(int i = 0; i < MAX_FACTIONS; i++) {
        s_crew_size[i] = CREW_SIZE;
    }
    if(Attr_NextKeyIs(stream, "num_crew_sizes")) {
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const size_t num_crews = attr.val.as_int;

        for(int i = 0; i < num_crews; i++) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            if(i < MAX_FACTIONS) {
                s_crew_size[i] = MAX(attr.val.as_int, 0);
            }
        }
    }

    reset_options();
    if(Attr_NextKeyIs(stream, "num_sched_options")) {
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const size_t num_options = attr.val.as_int;

        for(int i = 0; i < num_options; i++) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_STRING);
            int opt = option_index(attr.val.as_string);

            for(int j = 0; j < MAX_FACTIONS; j++) {
                CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
                CHK_TRUE_RET(attr.type == TYPE_FLOAT);
                if(opt >= 0) {
                    s_options[j][opt] = attr.val.as_float;
                }
            }
        }
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

        struct attr automatic_posts = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = astate.automatic_posts
        };
        CHK_TRUE_RET(Attr_Write(stream, &automatic_posts, "automatic_posts"));
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

        /* Saves from before the player's orders made a worker his own kept it
         * on the order for a while after; it is now his */
        if(Attr_NextKeyIs(stream, "ordered")) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_BOOL);
            if(attr.val.as_bool) {
                astate->automatic_work = false;
                astate->automatic_transport = false;
                astate->home_district = 0;
            }
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
        }
        if(Attr_NextKeyIs(stream, "automatic_posts")) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_BOOL);
            astate->automatic_posts = attr.val.as_bool;
        }
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

