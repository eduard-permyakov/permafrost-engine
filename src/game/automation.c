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
#include "../event.h"
#include "../entity.h"
#include "../lib/public/khash.h"

#include <assert.h>

#define TRANSIENT_STATE_TICKS   (2) 

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
};

KHASH_MAP_INIT_INT(state, struct automation_state)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(state) *s_entity_state_table;

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

static void on_20hz_tick(void *user, void *event)
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
                E_Global_Notify(EVENT_UNIT_BECAME_IDLE, (void*)((uintptr_t)uid), ES_ENGINE);
            }
            break;
        }
        default: assert(0);
        }
    });
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Automation_AddEntity(uint32_t uid)
{
    struct automation_state state = (struct automation_state){
        .state = STATE_IDLE,
        .transient_ticks = 0,
        .automatic_transport = false
    };
    return astate_set(uid, state);
}

void G_Automation_RemoveEntity(uint32_t uid)
{
    struct automation_state *astate = astate_get(uid);
    if(!astate)
        return;
    astate_remove(uid);
}

bool G_Automation_Init(void)
{
    if((s_entity_state_table = kh_init(state)) == NULL)
        return false;
    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
    return true;
}

void G_Automation_Shutdown(void)
{
    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
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

