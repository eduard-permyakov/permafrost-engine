/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2020 Eduard Permyakov 
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

#include "event.h"
#include "main.h"
#include "perf.h"
#include "sched.h"
#include "lib/public/khash.h"
#include "lib/public/vec.h"
#include "lib/public/queue.h"
#include "game/public/game.h"

#include <assert.h>


enum handler_type{
    HANDLER_TYPE_ENGINE,
    HANDLER_TYPE_SCRIPT,
};

struct handler_desc{
    enum handler_type type;
    union {
        handler_t       as_function;
        script_opaque_t as_script_callable;
    }handler;
    void          *user_arg;
    int            simmask;    /* Specifies during which simulation states the handler gets invoked */
};

struct event{
    enum eventtype     type; 
    void              *arg;
    enum event_source  source;
    uint32_t           receiver_id;
};

/* Used in the place of the entity ID for key generation for global events,
 * which are not associated with any entity. This is the maximum 32-bit 
 * entity ID, we will assume entity IDs will never reach this high.
 */
#define GLOBAL_ID (~((uint32_t)0))

VEC_TYPE(hd, struct handler_desc)
VEC_IMPL(static inline, hd, struct handler_desc)

KHASH_MAP_INIT_INT64(handler_desc, vec_hd_t)

QUEUE_TYPE(event, struct event)
QUEUE_IMPL(static, event, struct event)

#define STR(_event) [_event - EVENT_UPDATE_START] = #_event

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const char *s_event_str_table[] = {
    STR(EVENT_UPDATE_START),
    STR(EVENT_UPDATE_END),
    STR(EVENT_UPDATE_UI),
    STR(EVENT_RENDER_3D_PRE),
    STR(EVENT_RENDER_3D_POST),
    STR(EVENT_RENDER_UI),
    STR(EVENT_RENDER_FINISH),
    STR(EVENT_SELECTED_TILE_CHANGED),
    STR(EVENT_NEW_GAME),
    STR(EVENT_UNIT_SELECTION_CHANGED),
    STR(EVENT_60HZ_TICK),
    STR(EVENT_30HZ_TICK),
    STR(EVENT_20HZ_TICK),
    STR(EVENT_15HZ_TICK),
    STR(EVENT_10HZ_TICK),
    STR(EVENT_1HZ_TICK),
    STR(EVENT_ANIM_FINISHED),
    STR(EVENT_ANIM_CYCLE_FINISHED),
    STR(EVENT_MOVE_ISSUED),
    STR(EVENT_MOTION_START),
    STR(EVENT_MOTION_END),
    STR(EVENT_ATTACK_START),
    STR(EVENT_ENTITY_DEATH),
    STR(EVENT_ATTACK_END),
    STR(EVENT_GAME_SIMSTATE_CHANGED),
    STR(EVENT_SESSION_LOADED),
    STR(EVENT_SESSION_POPPED),
    STR(EVENT_SESSION_FAIL_LOAD),
    STR(EVENT_SCRIPT_TASK_EXCEPTION),
    STR(EVENT_SCRIPT_TASK_FINISHED),
    STR(EVENT_BUILD_BEGIN),
    STR(EVENT_BUILD_END),
    STR(EVENT_BUILD_FAIL_FOUND),
    STR(EVENT_BUILD_TARGET_ACQUIRED),
    STR(EVENT_BUILDING_COMPLETED),
    STR(EVENT_ENTITY_DIED),
    STR(EVENT_ENTITY_STOP),
    STR(EVENT_HARVEST_BEGIN),
    STR(EVENT_HARVEST_END),
    STR(EVENT_HARVEST_TARGET_ACQUIRED),
    STR(EVENT_TRANSPORT_TARGET_ACQUIRED),
    STR(EVENT_STORAGE_TARGET_ACQUIRED),
    STR(EVENT_STORAGE_SITE_AMOUNT_CHANGED),
    STR(EVENT_RESOURCE_DROPPED_OFF),
    STR(EVENT_RESOURCE_PICKED_UP),
    STR(EVENT_RESOURCE_EXHAUSTED),
    STR(EVENT_RESOURCE_AMOUNT_CHANGED),
    STR(EVENT_ENTERED_REGION),
    STR(EVENT_EXITED_REGION),
    STR(EVENT_UPDATE_FACTION),
    STR(EVENT_PROJECTILE_DISAPPEAR),
    STR(EVENT_PROJECTILE_HIT),
    STR(EVENT_ENTITY_DISAPPEARED),
};

static khash_t(handler_desc) *s_event_handler_table;
static queue(event)           s_event_queues[2];
static int                    s_front_queue_idx = 0;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static inline bool handlers_equal(const struct handler_desc *a, const struct handler_desc *b)
{
    if(a->type != b->type)
        return false;

    if(a->type == HANDLER_TYPE_SCRIPT)
        return S_ObjectsEqual(a->handler.as_script_callable, b->handler.as_script_callable);
    else
        return a->handler.as_function == b->handler.as_function;
}

static uint64_t e_key(uint32_t ent_id, enum eventtype event)
{
    return (((uint64_t)ent_id) << 32) | (uint64_t)event;
}

static bool e_register_handler(uint64_t key, struct handler_desc *desc)
{
    khiter_t k;
    k = kh_get(handler_desc, s_event_handler_table, key);

    if(k == kh_end(s_event_handler_table)) {

        vec_hd_t newv;
        vec_hd_init(&newv);
        vec_hd_push(&newv, *desc);

        int ret;
        k = kh_put(handler_desc, s_event_handler_table, key, &ret);
        assert(ret == 1 || ret == 2);
        kh_value(s_event_handler_table, k) = newv;

    }else{
    
        vec_hd_t vec = kh_value(s_event_handler_table, k);

        int idx = vec_hd_indexof(&vec, *desc, handlers_equal);
        if(idx != -1)
            return false; /* Don't allow registering duplicate handlers for the same event */

        vec_hd_push(&vec, *desc);
        kh_value(s_event_handler_table, k) = vec;
    }

    return true;
}

static bool e_unregister_handler(uint64_t key, struct handler_desc *desc)
{
    khiter_t k;
    k = kh_get(handler_desc, s_event_handler_table, key);

    if(k == kh_end(s_event_handler_table))
        return false;

    vec_hd_t vec = kh_value(s_event_handler_table, k);

    int idx = vec_hd_indexof(&vec, *desc, handlers_equal);
    if(idx == -1)
        return false;
    struct handler_desc to_del = vec_AT(&vec, idx);

    if(to_del.type == HANDLER_TYPE_SCRIPT) {

        S_Release(to_del.handler.as_script_callable);
        S_Release(to_del.user_arg); 
    }

    vec_hd_del(&vec, idx);
    kh_value(s_event_handler_table, k) = vec;

    return true;
}

static void e_invoke(const struct handler_desc *hd, struct event event)
{
    if(hd->type == HANDLER_TYPE_ENGINE) {

        hd->handler.as_function(hd->user_arg, event.arg);

    }else if(hd->type == HANDLER_TYPE_SCRIPT) {

        script_opaque_t script_arg = (event.source == ES_SCRIPT) 
            ? S_UnwrapIfWeakref(event.arg)
            : S_WrapEngineEventArg(event.type, event.arg);
        assert(script_arg);
        script_opaque_t user_arg = S_UnwrapIfWeakref(hd->user_arg);

        S_RunEventHandler(hd->handler.as_script_callable, user_arg, script_arg);

        S_Release(script_arg);
        S_Release(user_arg);
    }
}

static void e_handle_event(struct event event, bool immediate)
{
    Sched_HandleEvent(event.type, event.arg, event.source, immediate);

    uint64_t key = e_key(event.receiver_id, event.type);
    enum simstate ss = G_GetSimState();
    
    /* The execution of an event handler can cause one or more event handlers 
     * to be unregistered. We want to provide a guarantee that once an event 
     * handler is unregistered, it will never be executed. So, keep fetching 
     * the handlers vector from the table after every execution, in case it's
     * been changed by the prior handler call.
     */

    vec_hd_t execd_handlers;
    vec_hd_init(&execd_handlers);
    bool ran; 

    do{
        ran = false;
        khiter_t k = kh_get(handler_desc, s_event_handler_table, key);
        if(k == kh_end(s_event_handler_table))
            break; 

        vec_hd_t vec = kh_value(s_event_handler_table, k);
        for(int i = 0; i < vec_size(&vec); i++) {
        
            struct handler_desc *elem = &vec_AT(&vec, i);
            int idx = vec_hd_indexof(&execd_handlers, *elem, handlers_equal); 
            if(idx != -1)
                continue;
            /* memoize any handlers that we've already ran */
            vec_hd_push(&execd_handlers, *elem);

            if(!immediate && ((elem->simmask & ss) == 0))
                continue;

            e_invoke(elem, event);
            ran = true;
            break;
        }
    
    }while(ran);

    vec_hd_destroy(&execd_handlers);

    if(event.source == ES_SCRIPT)
        S_Release(event.arg);
}

static void notify_entities_update_start(void)
{
    uint64_t key;
    vec_hd_t curr;
    (void)curr;

    kh_foreach(s_event_handler_table, key, curr, {

        if((key & 0xffffffff) != EVENT_UPDATE_START)
            continue;

        uint32_t uid = key >> 32;
        e_handle_event( (struct event){EVENT_UPDATE_START, NULL, ES_ENGINE, uid}, false);
    });
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool E_Init(void)
{
    s_event_handler_table = kh_init(handler_desc);
    if(!s_event_handler_table)
        goto fail_table;

    if(!queue_event_init(&s_event_queues[0], 2048))
        goto fail_front_queue;
    if(!queue_event_init(&s_event_queues[1], 2048))
        goto fail_back_queue;

    return true;
        
fail_back_queue:
    queue_event_destroy(&s_event_queues[0]);
fail_front_queue:
    kh_destroy(handler_desc, s_event_handler_table);
fail_table:
    return false;
}

void E_Shutdown(void)
{
    khiter_t k;
    for (k = kh_begin(s_event_handler_table); k != kh_end(s_event_handler_table); ++k) {

        if(!kh_exist(s_event_handler_table, k))
            continue; 

        vec_hd_t vec = kh_value(s_event_handler_table, k);
        vec_hd_destroy(&vec);
    }

    kh_destroy(handler_desc, s_event_handler_table);
    queue_event_destroy(&s_event_queues[1]);
    queue_event_destroy(&s_event_queues[0]);
}

void E_ServiceQueue(void)
{
    PERF_ENTER();

    queue_event_t *queue = &s_event_queues[s_front_queue_idx];
    s_front_queue_idx = (s_front_queue_idx + 1) % 2;

    e_handle_event( (struct event){EVENT_UPDATE_START, NULL, ES_ENGINE, GLOBAL_ID}, false);
    notify_entities_update_start();

    struct event event;
    while(queue_event_pop(queue, &event)) {
    
        e_handle_event(event, false);
        /* event arg already released */
    }

    e_handle_event( (struct event){EVENT_UPDATE_END, NULL, ES_ENGINE, GLOBAL_ID}, false);

    PERF_RETURN_VOID();
}

void E_ClearPendingEvents(void)
{
    queue_event_clear(&s_event_queues[s_front_queue_idx]);
}

void E_FlushEventQueue(void)
{
    e_handle_event( (struct event){EVENT_RENDER_FINISH, NULL, ES_ENGINE, GLOBAL_ID}, true);

    while(E_EventsQueued()) {

        queue_event_t *queue = &s_event_queues[s_front_queue_idx];
        s_front_queue_idx = (s_front_queue_idx + 1) % 2;

        e_handle_event( (struct event){EVENT_UPDATE_START,  NULL, ES_ENGINE, GLOBAL_ID}, true);

        struct event event;
        while(queue_event_pop(queue, &event)) {
            e_handle_event(event, true);
        }
        e_handle_event( (struct event){EVENT_UPDATE_END, NULL, ES_ENGINE, GLOBAL_ID}, true);
        e_handle_event( (struct event){EVENT_RENDER_FINISH, NULL, ES_ENGINE, GLOBAL_ID}, true);
    }
}

bool E_EventsQueued(void)
{
    return (queue_size(s_event_queues[0]) > 0) 
        || (queue_size(s_event_queues[1]) > 0);
}

void E_DeleteScriptHandlers(void)
{
    uint64_t keys_to_del[kh_size(s_event_handler_table)];
    size_t ntodel = 0;

    uint64_t key;
    vec_hd_t curr;

    kh_foreach(s_event_handler_table, key, curr, {

        /* iterate backwards to delete while iterating */
        for(int i = vec_size(&curr)-1; i >= 0; i--) {

            struct handler_desc hd = vec_AT(&curr, i);
            if(hd.type == HANDLER_TYPE_ENGINE)
                continue;

            S_Release(hd.handler.as_script_callable);
            S_Release(hd.user_arg); 
            vec_hd_del(&curr, i);
        }

        khiter_t k = kh_get(handler_desc, s_event_handler_table, key);
        assert(k != kh_end(s_event_handler_table));
        kh_value(s_event_handler_table, k) = curr;

        if(vec_size(&curr) == 0) {
            keys_to_del[ntodel++] = key;
            vec_hd_destroy(&curr);
        }
    });
    
    for(int i = 0; i < ntodel; i++) {

        khiter_t k = kh_get(handler_desc, s_event_handler_table, keys_to_del[i]);
        assert(k != kh_end(s_event_handler_table));
        kh_del(handler_desc, s_event_handler_table, k);
    }
}

size_t E_GetScriptHandlers(size_t max_out, struct script_handler *out)
{
    size_t ret = 0;
    uint64_t key;
    vec_hd_t curr;

    kh_foreach(s_event_handler_table, key, curr, {

        for(int i = 0; i < vec_size(&curr); i++) {

            (void)key;
            struct handler_desc hd = vec_AT(&curr, i);
            if(hd.type == HANDLER_TYPE_ENGINE)
                continue;

            if(ret == max_out)
                break;

            assert(hd.handler.as_script_callable && hd.user_arg);
            out[ret] = (struct script_handler){
                .event = key & ~((uint32_t)0),
                .id = key >> 32,
                .simmask = hd.simmask,
                .handler = hd.handler.as_script_callable,
                .arg = (script_opaque_t)hd.user_arg
            };
            ret++;
        }
    });
    return ret;
}

/*
 * Global Events
 */

void E_Global_Notify(enum eventtype event, void *event_arg, enum event_source source)
{
    struct event e = (struct event){event, event_arg, source, GLOBAL_ID};
    queue_event_push(&s_event_queues[s_front_queue_idx], &e);
}

bool E_Global_Register(enum eventtype event, handler_t handler, void *user_arg, int simmask)
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_ENGINE;
    hd.handler.as_function = handler;
    hd.user_arg = user_arg;
    hd.simmask = simmask;

    return e_register_handler(e_key(GLOBAL_ID, event), &hd);
}

bool E_Global_Unregister(enum eventtype event, handler_t handler)
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_ENGINE;
    hd.handler.as_function = handler;

    return e_unregister_handler(e_key(GLOBAL_ID, event), &hd);
}

bool E_Global_ScriptRegister(enum eventtype event, script_opaque_t handler, 
                             script_opaque_t user_arg, int simmask)
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_SCRIPT;
    hd.handler.as_script_callable = handler;
    hd.user_arg = user_arg;
    hd.simmask = simmask;

    return e_register_handler(e_key(GLOBAL_ID, event), &hd);
}

bool E_Global_ScriptUnregister(enum eventtype event, script_opaque_t handler)
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_SCRIPT;
    hd.handler.as_script_callable = handler;

    return e_unregister_handler(e_key(GLOBAL_ID, event), &hd);
}

void E_Global_NotifyImmediate(enum eventtype event, void *event_arg, enum event_source source)
{
    struct event e = (struct event){event, event_arg, source, GLOBAL_ID};
    e_handle_event(e, true);
}

/*
 * Entity Events
 */

bool E_Entity_Register(enum eventtype event, uint32_t ent_uid, handler_t handler, 
                       void *user_arg, int simmask)
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_ENGINE;
    hd.handler.as_function = handler;
    hd.user_arg = user_arg;
    hd.simmask = simmask;

    return e_register_handler(e_key(ent_uid, event), &hd);
}

bool E_Entity_Unregister(enum eventtype event, uint32_t ent_uid, handler_t handler)
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_ENGINE;
    hd.handler.as_function = handler;

    return e_unregister_handler(e_key(ent_uid, event), &hd);
}

bool E_Entity_ScriptRegister(enum eventtype event, uint32_t ent_uid, 
                             script_opaque_t handler, script_opaque_t user_arg, int simmask)
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_SCRIPT;
    hd.handler.as_script_callable = handler;
    hd.user_arg = user_arg;
    hd.simmask = simmask;

    return e_register_handler(e_key(ent_uid, event), &hd);
}

bool E_Entity_ScriptUnregister(enum eventtype event, uint32_t ent_uid, 
                               script_opaque_t handler)
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_SCRIPT;
    hd.handler.as_script_callable = handler;

    return e_unregister_handler(e_key(ent_uid, event), &hd);
}

void E_Entity_Notify(enum eventtype event, uint32_t ent_uid, void *event_arg, 
                     enum event_source source)
{
    struct event e = (struct event){event, event_arg, source, ent_uid};
    queue_event_push(&s_event_queues[s_front_queue_idx], &e);
}

void E_Entity_NotifyImmediate(enum eventtype event, uint32_t ent_uid, void *event_arg, 
                              enum event_source source)
{
    struct event e = (struct event){event, event_arg, source, ent_uid};
    e_handle_event(e, true);
}

const char *E_EngineEventString(enum eventtype event)
{
    if(event <= (int)SDL_LASTEVENT)
        return NULL;
    if(event - EVENT_UPDATE_START >= sizeof(s_event_str_table)/sizeof(const char *))
        return NULL;
    return s_event_str_table[event - EVENT_UPDATE_START];
}

