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

#include "automation.h"
#include "movement.h"
#include "harvester.h"
#include "builder.h"
#include "combat.h"
#include "position.h"
#include "storage_site.h"
#include "harvester.h"
#include "../event.h"
#include "../entity.h"
#include "../settings.h"
#include "../camera.h"
#include "../lib/public/khash.h"
#include "../lib/public/stalloc.h"
#include "../lib/public/pf_string.h"

#include <assert.h>
#include <stdlib.h>
#include <assert.h>

#define TRANSIENT_STATE_TICKS        (2) 
#define TRANSPORT_UNIT_COST_DISTANCE (150)
#define ARR_SIZE(a)                  (sizeof(a)/sizeof((a)[0]))

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
};

struct cost_mapping{
    uint32_t site;
    int      cost;
    float    distance;
    int      num_assigned;
};

KHASH_MAP_INIT_INT(state, struct automation_state)
KHASH_MAP_INIT_INT(count, uint32_t);

static void on_order_issued(void *user, void *event);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(state) *s_entity_state_table;
/* Maps storage sites to the number of automated transporters servicing it */
static khash_t(count) *s_transport_count;

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

static bool idle(uint32_t uid)
{
    uint32_t flags = G_FlagsGet(uid);
    if(flags & ENTITY_FLAG_GARRISONED)
        return true;
    if((flags & ENTITY_FLAG_MOVABLE) && !G_Move_Still(uid))
        return false;
    if((flags & ENTITY_FLAG_HARVESTER) && !G_Harvester_Idle(uid))
        return false;
    if((flags & ENTITY_FLAG_BUILDER) && !G_Builder_Idle(uid))
        return false;
    if((flags & ENTITY_FLAG_COMBATABLE) && !G_Combat_Idle(uid))
        return false;
    return true;
}

static bool transporter_compatible_for_resource(uint32_t worker, uint32_t site, const char *rname)
{
    if(!G_StorageSite_Desires(site, rname))
        return false;
    if(G_Harvester_GetDoNotTransport(worker, rname))
        return false;
    if(G_Harvester_GetMaxCarry(worker, rname) == 0)
        return false;
    return true;
}

static int transport_job_cost(uint32_t worker, uint32_t site, int *out_num_assigned, float *out_dist)
{
    /* The job 'cost' takes into account both the distance from
     * the target site, and the number of automated workers 
     * currently 'servicing' that site, in order to stike a 
     * balance between 'fairness' and redundant traveling due 
     * to far-off assignments.
     */
    vec2_t worker_pos = G_Pos_GetXZ(worker);
    vec2_t site_pos = G_Pos_GetXZ(site);

    vec2_t delta;
    PFM_Vec2_Sub(&site_pos, &worker_pos, &delta);
    float len = PFM_Vec2_Len(&delta);
    *out_dist = len;

    int num_assigned = 0;
    khiter_t k = kh_get(count, s_transport_count, site);
    if(k != kh_end(s_transport_count)) {
        num_assigned = kh_value(s_transport_count, k);
    }
    *out_num_assigned = num_assigned;

    int distance_cost = ((int)len) / TRANSPORT_UNIT_COST_DISTANCE;
    int fairness_cost = num_assigned;
    return (distance_cost + fairness_cost);
}

static int compare_jobs(const void *a, const void *b)
{
    struct cost_mapping mappinga = *(const struct cost_mapping*)a;
    struct cost_mapping mappingb = *(const struct cost_mapping*)b;

    if (mappinga.cost < mappingb.cost) return -1;
    if (mappinga.cost > mappingb.cost) return 1;

    /* When costs are the same, resort to sorting by number of
     * assigned workers.
     */
    if(mappinga.num_assigned < mappingb.num_assigned) return -1;
    if(mappinga.num_assigned > mappingb.num_assigned) return 1;

    /* Lastly, resort to distance. 
     */
    if(mappinga.distance < mappingb.distance) return -1;
    if(mappinga.distance > mappingb.distance) return 1;

    return 0;
}

static uint32_t target_site_for_resource(uint32_t uid, const char *rname)
{
    uint32_t ret = NULL_UID;

    vec_entity_t sites;
    vec_entity_init(&sites);
    G_StorageSite_GetAll(&sites);

    const size_t nsites = vec_size(&sites);
    STALLOC(struct cost_mapping, costs, nsites);

    /* Prune sites which are not compatible */
    for(int i = nsites - 1; i >= 0; i--) {
        uint32_t site = vec_AT(&sites, i);
        if(!transporter_compatible_for_resource(uid, site, rname)) {
            vec_entity_del(&sites, i);
        }
    }

    size_t left = vec_size(&sites);
    for(int i = 0; i < left; i++) {

        uint32_t site = vec_AT(&sites, i);
        int num_assigned;
        float distance;
        int cost = transport_job_cost(uid, site, &num_assigned, &distance);

        costs[i] = (struct cost_mapping){
            .site = site,
            .cost = cost,
            .num_assigned = num_assigned,
            .distance = distance,
        };
    }
    qsort(costs, left, sizeof(struct cost_mapping), compare_jobs);
    if(left > 0) {
        ret = costs[0].site;
    }

    STFREE(costs);
    vec_entity_destroy(&sites);
    return ret;
}

static uint32_t target_site(uint32_t uid)
{
    const char *transportable[64];
    size_t ntransportable = G_Harvester_GetTransportPrio(uid, ARR_SIZE(transportable), transportable);

    for(int i = 0; i < ntransportable; i++) {
        uint32_t target = target_site_for_resource(uid, transportable[i]);
        if(target != NULL_UID)
            return target;
    }
    return NULL_UID;
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

static void decrement_assigned_transporters(uint32_t site)
{
    khiter_t k = kh_get(count, s_transport_count, site);
    assert(k != kh_end(s_transport_count));
    kh_val(s_transport_count, k)--;
}

static int get_assigned_transporters(uint32_t site)
{
    khiter_t k = kh_get(count, s_transport_count, site);
    if(k == kh_end(s_transport_count))
        return 0;
    return kh_val(s_transport_count, k);
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
                astate->state = STATE_IDLE;
                if(astate->transport_target != NULL_UID) {
                    decrement_assigned_transporters(astate->transport_target);
                    astate->transport_target = NULL_UID;
                }
                E_Global_Notify(EVENT_UNIT_BECAME_IDLE, (void*)((uintptr_t)uid), ES_ENGINE);
            }
            break;
        }
        default: assert(0);
        }
    });
}

static void assign_transport_jobs(void)
{
    uint32_t uid;
    struct automation_state *astate;

    kh_foreach_val_ptr(s_entity_state_table, uid, astate, {

        if(astate->state != STATE_IDLE)
            continue;

        if(!(G_FlagsGet(uid) & ENTITY_FLAG_HARVESTER))
            continue;
        
        if(!astate->automatic_transport)
            continue;

        uint32_t site = target_site(uid);
        if(site == NULL_UID)
            continue;

        increment_assigned_transporters(site);
        astate->transport_target = site;
        G_Harvester_Transport(uid, site);
    });
}

static void on_20hz_tick(void *user, void *event)
{
    recompute_idle();
    assign_transport_jobs();
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

    if(!astate->automatic_transport)
        return;

    uint32_t target = G_Harvester_TransportTarget(uid);
    if(astate->transport_target != target) {
        decrement_assigned_transporters(astate->transport_target);
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
    };
    return astate_set(uid, state);
}

void G_Automation_RemoveEntity(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return;
    E_Entity_Unregister(EVENT_ORDER_ISSUED, uid, on_order_issued);
    astate_remove(uid);
}

bool G_Automation_Init(void)
{
    if((s_entity_state_table = kh_init(state)) == NULL)
        goto fail_entity_state_table;
    if((s_transport_count = kh_init(count)) == NULL)
        goto fail_transport_count_table;

    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
    E_Global_Register(EVENT_UPDATE_UI, on_update_ui, NULL, G_RUNNING);
    E_Global_Register(EVENT_ORDER_ISSUED, on_order_issued, NULL, G_RUNNING);
    return true;

fail_transport_count_table:
    kh_destroy(state, s_entity_state_table);
fail_entity_state_table:
    return false;
}

void G_Automation_Shutdown(void)
{
    E_Global_Unregister(EVENT_ORDER_ISSUED, on_order_issued);
    E_Global_Unregister(EVENT_UPDATE_UI, on_update_ui);
    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
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
            decrement_assigned_transporters(astate->transport_target);
            astate->transport_target = NULL_UID;
        }
    }
    astate->automatic_transport = on;
}

bool G_Automation_GetAutomaticTransport(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return false;
    return astate->automatic_transport;
}

