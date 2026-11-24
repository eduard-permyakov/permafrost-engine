/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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

#define MEM_FILE_SYS MEM_SYS_EVENT
#define MEM_FILE_SUB MEM_SUB_EVENT_EVENT

#include "public/event.h"
#include "../main.h"
#include "../perf/public/perf.h"
#include "../settings.h"
#include "../sched/public/sched.h"
#include "../perf/public/mem.h"
#include "../lib/public/khash.h"
#include "../lib/public/vec.h"
#include "../lib/public/queue.h"
#include "../lib/public/lockfree.h"
#include "../game/public/game.h"

#include <assert.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_EVENT, MEM_SUB_EVENT_EVENT)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_EVENT, MEM_SUB_EVENT_EVENT)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_EVENT, MEM_SUB_EVENT_EVENT)


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
    uint32_t       register_tick;
    const char    *fname;      /* Handler symbol name, for perf-marker attribution (engine handlers only) */
};

struct event{
    enum eventtype     type; 
    void              *arg;
    enum event_source  source;
    uint32_t           receiver_id;
    uint32_t           tick;
};

/* Used in the place of the entity ID for key generation for global events,
 * which are not associated with any entity. This is the maximum 32-bit 
 * entity ID, we will assume entity IDs will never reach this high.
 */
#define GLOBAL_ID (~((uint32_t)0))

VEC_TYPE(hd, struct handler_desc)
VEC_IMPL(static inline, hd, struct handler_desc)

KHASH_MAP_INIT_INT64(handler_desc, vec_hd_t)
KHASH_MAP_INIT_INT(usub, int)
KHASH_MAP_INIT_INT64(skey, int)

/* Records the script thread hands to their handlers between two yields, when
 * it does so alongside the main thread
 */
#define SCRIPT_DISPATCH_BATCH   (8)

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
    STR(EVENT_5HZ_TICK),
    STR(EVENT_1HZ_TICK),
    STR(EVENT_HALFHZ_TICK),
    STR(EVENT_ANIM_FINISHED),
    STR(EVENT_ANIM_CYCLE_FINISHED),
    STR(EVENT_MOVE_ISSUED),
    STR(EVENT_MOTION_START),
    STR(EVENT_MOTION_END),
    STR(EVENT_ATTACK_START),
    STR(EVENT_ENTITY_DEATH),
    STR(EVENT_ENTITY_DEATH_IMMEDIATE),
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
    STR(EVENT_BUILDING_CONSTRUCTED),
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
    STR(EVENT_ORDER_ISSUED),
    STR(EVENT_MOVABLE_ENTITY_UNBLOCK),
    STR(EVENT_MOVABLE_ENTITY_BLOCK),
    STR(EVENT_BUILDING_PLACED),
    STR(EVENT_BUILDING_REMOVED),
    STR(EVENT_RALLY_POINT_SET),
    STR(EVENT_UNIT_BECAME_IDLE),
    STR(EVENT_UNIT_BECAME_ACTIVE),
    STR(EVENT_UNIT_IDLE_PENDING),
    STR(EVENT_CLUSTER_CHANGED),
    STR(EVENT_RESOURCES_SPILLED),
    STR(EVENT_REQUEST_BLOCKED),
    STR(EVENT_PROVISION_CHANGED),
    STR(EVENT_BUILDING_MARKED),
    STR(EVENT_PLAYER_ORDER_ISSUED),
    STR(EVENT_HUNT_HIT),
    STR(EVENT_ORDER_HOLD_BEGIN),
    STR(EVENT_ORDER_HOLD_END),
    STR(EVENT_POST_MANNED),
    STR(EVENT_DUTY_CHANGED),
    STR(EVENT_UI_FRAME),
    STR(EVENT_SCRIPT_DISPATCH),
};

static khash_t(handler_desc) *s_event_handler_table;
/* The handlers registered by scripts. Only the script thread touches them, as
 * they hold script objects. */
static khash_t(handler_desc) *s_script_handler_table;
/* Refcount of per-entity EVENT_UPDATE_START registrations, so the per-frame
 * notify iterates the live subscribers instead of scanning the whole handler
 * table.
 */
static khash_t(usub)         *s_update_start_subs;
/* How many script handlers each entity has for each event. The script thread
 * keeps it as handlers come and go; the main thread asks it, under the lock,
 * in place of the script thread's own table.
 */
static khash_t(skey)         *s_script_keys;
static SDL_SpinLock           s_script_keys_lock;
/* Event records the script thread took and has yet to hand to their handlers,
 * oldest first: its own
 */
static struct script_record  *s_script_pending;
/* Whether it holds any, for the main thread to see */
static int                    s_script_held;
/* Set as the frame's queue is served: the script thread's once-a-frame upkeep is
 * due, on frames where the game ran, not while a session loads
 */
static int                    s_frame_upkeep_due;
static queue(event)           s_event_queues[2];
static int                    s_front_queue_idx = 0;
/* What the script side has ever listened to, by event: sticky bits, so that
 * the dispatching thread decides what to send it without reading its tables */
#define SCRIPT_EVENT_SPACE  (0x30000)
static uint8_t                s_script_global_subs[SCRIPT_EVENT_SPACE / 8];
static uint8_t                s_script_entity_subs[SCRIPT_EVENT_SPACE / 8];
static uint8_t                s_script_task_subs[SCRIPT_EVENT_SPACE / 8];
/* Events for the script side waiting for its next turn, newest first */
static struct script_record  *s_script_inbox;
/* Scratch values for e_handle_event, to avoid allocation churn */
static vec_hd_t               s_dispatch_execd;
static vec_hd_t               s_dispatch_curr;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static inline bool e_handlers_equal(const struct handler_desc *a, const struct handler_desc *b)
{
    if(a->type != b->type)
        return false;

    if(a->type == HANDLER_TYPE_SCRIPT) {
        return S_ObjectsEqual(a->handler.as_script_callable, b->handler.as_script_callable);
    }else {
        return a->handler.as_function == b->handler.as_function;
    }
}

static inline bool e_ptrs_equal(const struct handler_desc *a, const struct handler_desc *b)
{
    if(a->type != b->type)
        return false;

    if(a->type == HANDLER_TYPE_SCRIPT) {
        return a->handler.as_script_callable == b->handler.as_script_callable;
    }else {
        return a->handler.as_function == b->handler.as_function;
    }
}

static uint64_t e_key(uint32_t ent_id, enum eventtype event)
{
    return (((uint64_t)ent_id) << 32) | (uint64_t)event;
}

static bool e_zombie_can_receive(enum eventtype type)
{
    return (type == EVENT_ENTITY_DEATH)
        || (type == EVENT_ENTITY_DISAPPEARED);
}

static bool e_is_timer_event(enum eventtype type)
{
    if(type == EVENT_60HZ_TICK
    || type == EVENT_30HZ_TICK
    || type == EVENT_20HZ_TICK
    || type == EVENT_15HZ_TICK
    || type == EVENT_10HZ_TICK
    || type == EVENT_5HZ_TICK
    || type == EVENT_1HZ_TICK)
        return true;
    return false;
}

static void e_script_keys_add(uint64_t key, int delta)
{
    SDL_AtomicLock(&s_script_keys_lock);
    khiter_t k = kh_get(skey, s_script_keys, key);
    if(k == kh_end(s_script_keys)) {
        int ret;
        k = kh_put(skey, s_script_keys, key, &ret);
        if(ret != -1)
            kh_value(s_script_keys, k) = 0;
    }
    if(k != kh_end(s_script_keys)) {
        kh_value(s_script_keys, k) += delta;
        if(kh_value(s_script_keys, k) <= 0)
            kh_del(skey, s_script_keys, k);
    }
    SDL_AtomicUnlock(&s_script_keys_lock);
}

static void e_update_start_subs_add(uint64_t key, int delta)
{
    if((uint32_t)(key & 0xffffffff) != EVENT_UPDATE_START)
        return;
    uint32_t uid = key >> 32;
    if(uid == GLOBAL_ID)
        return;
    /* The main thread walks the index every frame */
    S_View_RequireWorld();

    khiter_t k = kh_get(usub, s_update_start_subs, uid);
    if(k == kh_end(s_update_start_subs)) {
        int ret;
        k = kh_put(usub, s_update_start_subs, uid, &ret);
        assert(ret != -1);
        kh_value(s_update_start_subs, k) = 0;
    }
    kh_value(s_update_start_subs, k) += delta;
    if(kh_value(s_update_start_subs, k) <= 0)
        kh_del(usub, s_update_start_subs, k);
}

static khash_t(handler_desc) *e_table(enum handler_type type)
{
    return (type == HANDLER_TYPE_SCRIPT) ? s_script_handler_table : s_event_handler_table;
}

static bool e_register_handler(uint64_t key, struct handler_desc *desc)
{
    khash_t(handler_desc) *table = e_table(desc->type);
    khiter_t k;
    k = kh_get(handler_desc, table, key);

    if(k == kh_end(table)) {

        vec_hd_t newv;
        vec_hd_init(&newv);
        vec_hd_push(&newv, *desc);

        int ret;
        k = kh_put(handler_desc, table, key, &ret);
        assert(ret == 1 || ret == 2);
        kh_value(table, k) = newv;

    }else{
    
        vec_hd_t vec = kh_value(table, k);

        int idx = vec_hd_indexof(&vec, *desc, e_handlers_equal);
        if(idx != -1)
            return false; /* Don't allow registering duplicate handlers for the same event */

        vec_hd_push(&vec, *desc);
        kh_value(table, k) = vec;
    }

    if(desc->type == HANDLER_TYPE_SCRIPT)
        e_script_keys_add(key, +1);
    e_update_start_subs_add(key, +1);
    return true;
}

static bool e_unregister_handler(uint64_t key, const struct handler_desc *desc)
{
    khash_t(handler_desc) *table = e_table(desc->type);
    khiter_t k;
    k = kh_get(handler_desc, table, key);

    if(k == kh_end(table))
        return false;

    vec_hd_t vec = kh_value(table, k);

    int idx = vec_hd_indexof(&vec, *desc, e_handlers_equal);
    if(idx == -1)
        return false;
    struct handler_desc to_del = vec_AT(&vec, idx);

    if(to_del.type == HANDLER_TYPE_SCRIPT) {

        S_Release(to_del.handler.as_script_callable);
        S_Release(to_del.user_arg); 
    }

    vec_hd_del(&vec, idx);
    kh_value(table, k) = vec;

    if(to_del.type == HANDLER_TYPE_SCRIPT)
        e_script_keys_add(key, -1);
    e_update_start_subs_add(key, -1);
    return true;
}

static void e_invoke(const struct handler_desc hd, struct event event, 
                     const struct script_event_arg *sarg)
{
    if(hd.type == HANDLER_TYPE_ENGINE) {

        PERF_PUSH(hd.fname ? hd.fname : "handler");
        hd.handler.as_function(hd.user_arg, event.arg);
        PERF_POP();
        return;
    }

    /* Remove the handler if the user arg goes out of scope. Since Python
     * doesn't actually guarantee destructors getting called, it's possible
     * that we miss an 'unregister'. Take care of that here.
     */
    if(S_WeakrefDied(hd.user_arg)) {
        uint64_t key = e_key(event.receiver_id, event.type);
        e_unregister_handler(key, &hd);
        return;
    }

    /* Marker names must come from a bounded set; event names qualify,
     * script callable names do not.
     */
    const char *evtname = E_EngineEventString(event.type);
    PERF_PUSH("script handler");
    PERF_PUSH(evtname ? evtname : "sdl event");

    script_opaque_t script_arg = (event.source == ES_SCRIPT)
        ? S_UnwrapIfWeakref(event.arg)
        : S_WrapEngineEventArg(event.type, sarg);
    assert(script_arg);
    script_opaque_t user_arg = S_UnwrapIfWeakref(hd.user_arg);

    S_RunEventHandler(hd.handler.as_script_callable, user_arg, script_arg);

    S_Release(script_arg);
    S_Release(user_arg);

    PERF_POP();
    PERF_POP();
}

static int s_immediate_depth = 0;

/* Runs the event's handlers in one table, in the order they registered */
static void e_run_handlers(khash_t(handler_desc) *table, struct event event, bool immediate,
                           const struct script_event_arg *sarg)
{
    uint64_t key = e_key(event.receiver_id, event.type);
    enum simstate ss = G_PeekSimState();
    
    /* The execution of an event handler can cause one or more event handlers 
     * to be unregistered. We want to provide a guarantee that once an event 
     * handler is unregistered, it will never be executed. So, keep fetching 
     * the handlers vector from the table after every execution, in case it's
     * been changed by the prior handler call.
     */

    vec_hd_t execd_local, curr_local;
    vec_hd_t *execd_handlers, *curr;
    if(immediate || table == s_script_handler_table) {
        vec_hd_init(&execd_local);
        vec_hd_init(&curr_local);
        execd_handlers = &execd_local;
        curr = &curr_local;
    }else{
        execd_handlers = &s_dispatch_execd;
        curr = &s_dispatch_curr;
        vec_hd_reset(execd_handlers);
        vec_hd_reset(curr);
    }
    bool ran;

    do{
        ran = false;
        khiter_t k = kh_get(handler_desc, table, key);
        if(k == kh_end(table))
            break;

        vec_hd_reset(curr);
        vec_hd_copy(curr, &kh_value(table, k));

        for(int i = 0; i < vec_size(curr); i++) {

            /* Since any of the executed handlers may have subsequently
             * been deleted, the pointers to scripting objects in the
             * executed handlers must be treated as dangling references
             * and must not be dereferenced
             */
            struct handler_desc elem = vec_AT(curr, i);
            int idx = vec_hd_indexof(execd_handlers, elem, e_ptrs_equal);
            if(idx != -1)
                continue;
            /* memoize any handlers that we've already ran */
            vec_hd_push(execd_handlers, elem);

            if(!immediate && ((elem.simmask & ss) == 0))
                continue;
            if(event.tick != elem.register_tick && SDL_TICKS_PASSED(elem.register_tick, event.tick))
                continue;

            s_immediate_depth += immediate;
            e_invoke(elem, event, sarg);
            s_immediate_depth -= immediate;
            ran = true;
            break;
        }

    }while(ran);

    if(immediate || table == s_script_handler_table) {
        vec_hd_destroy(&execd_local);
        vec_hd_destroy(&curr_local);
    }
}

struct script_dispatch{
    struct event            event;
    bool                    immediate;
    bool                    to_zombie;  /* the receiver takes no handlers */
    struct script_event_arg sarg;
};

struct script_record{
    struct script_dispatch  sd;
    struct script_record   *next;
    /* Set for a call kept in the events' order in place of an event */
    void                  (*call)(void *arg);
    void                   *arg;
};

/* The script tasks waiting on the event wake first, then the handlers run,
 * as on the engine's side */
static void e_dispatch_script(void *arg)
{
    struct script_dispatch *sd = arg;
    void *task_arg = (sd->event.source == ES_SCRIPT) ? sd->event.arg : (void*)&sd->sarg;
    /* From a view slice, a task waiting on the event is left for the next
     * world window: only a view task may run alongside the main thread
     */
    Sched_HandleScriptEvent(sd->event.type, task_arg, sd->event.source, 
        sd->immediate && !Sched_InViewSlice());
    if(!sd->to_zombie)
        e_run_handlers(s_script_handler_table, sd->event, sd->immediate, &sd->sarg);
}

static void e_sub_set(uint8_t *bits, int event)
{
    if(event < 0 || event >= SCRIPT_EVENT_SPACE)
        return;
    LF_FOR(&bits[event / 8], (uint8_t)(1u << (event % 8)), LF_RLX);
}

static bool e_sub_get(const uint8_t *bits, int event)
{
    if(event < 0 || event >= SCRIPT_EVENT_SPACE)
        return true;
    return LF_LOAD(&bits[event / 8], LF_RLX) & (1u << (event % 8));
}

/* Whether the script side may want the event: it has listened to the event
 * before, and for an entity's, the entity has a script object */
static bool e_script_interested(struct event event, bool to_zombie)
{
    if(e_sub_get(s_script_task_subs, event.type))
        return true;
    if(to_zombie)
        return false;
    if(event.receiver_id == GLOBAL_ID)
        return e_sub_get(s_script_global_subs, event.type);
    return e_sub_get(s_script_entity_subs, event.type) 
        && S_ScriptTypeID(event.receiver_id) != 0;
}

/* Events whose engine code goes on to read what the script handlers did;
 * input, as a script's key handler may set what the engine's next click does;
 * and the UI and render events, whose handlers draw into this frame's UI */
/* The tasks that draw on the render events run at once only while the game
 * is paused, where nothing else would run them. While it runs they are
 * woken with the frame's other events, by the script tick at the latest.
 */
static bool e_script_render_now(enum eventtype type)
{
    if(type != EVENT_RENDER_3D_PRE && type != EVENT_RENDER_3D_POST
    && type != EVENT_RENDER_UI && type != EVENT_RENDER_FINISH)
        return false;
    return G_PeekSimState() != G_RUNNING;
}

static bool e_script_needs_now(enum eventtype type)
{
    return ((int)type < (int)SDL_LASTEVENT)
        || (type == EVENT_RESOURCE_EXHAUSTED)
        || (type == EVENT_UNIT_IDLE_PENDING)
        || (type == EVENT_POST_MANNED)
        || (type == EVENT_UPDATE_UI)
        || e_script_render_now(type);
}

static void e_script_record_free(struct script_record *rec)
{
    if(rec->call) {
        PF_FREE(rec->arg);
        PF_FREE(rec);
        return;
    }
    S_EventArgRelease(&rec->sd.sarg);
    if(rec->sd.event.source == ES_SCRIPT)
        S_Release(rec->sd.event.arg);
    PF_FREE(rec);
}

/* Takes the waiting events in the order they were sent */
static struct script_record *e_script_take(void)
{
    struct script_record *head = LF_XCHG(&s_script_inbox, NULL, LF_ACQ);
    struct script_record *prev = NULL;
    while(head) {
        struct script_record *next = head->next;
        head->next = prev;
        prev = head;
        head = next;
    }
    return prev;
}

static void e_script_record_run(struct script_record *rec)
{
    if(rec->call) {
        rec->call(rec->arg);
        return;
    }
    e_dispatch_script(&rec->sd);
}

/* The next record to hand to its handlers. The task that handles them
 * alongside the main thread and the drain the main thread waits for take
 * from the one list, so the records keep their order whoever takes them.
 */
static struct script_record *e_script_next(void)
{
    if(!s_script_pending)
        s_script_pending = e_script_take();
    struct script_record *rec = s_script_pending;
    if(rec)
        s_script_pending = rec->next;
    LF_STORE(&s_script_held, s_script_pending != NULL, LF_REL);
    return rec;
}

static void e_script_drain(void *unused)
{
    (void)unused;
    struct script_record *rec;
    while((rec = e_script_next())) {
        e_script_record_run(rec);
        e_script_record_free(rec);
    }
}

static bool e_script_send(struct event event, bool immediate, bool to_zombie)
{
    struct script_record *rec = PF_MALLOC(sizeof(struct script_record));
    if(!rec)
        return false;
    rec->call = NULL;
    rec->arg = NULL;
    rec->sd = (struct script_dispatch){event, immediate, to_zombie};
    /* A script argument's reference passes to the record */
    S_EventArgEncode(event.type, event.source, event.arg, &rec->sd.sarg);

    struct script_record *head = LF_LOAD(&s_script_inbox, LF_RLX);
    do{
        rec->next = head;
    }while(!LF_CASW(&s_script_inbox, &head, rec, LF_REL, LF_RLX));
    return true;
}

static void e_handle_event(struct event event, bool immediate)
{
    if((G_GetSimState() != G_RUNNING) && e_is_timer_event(event.type))
        return;

    Sched_HandleEvent(event.type, event.arg, event.source, immediate);

    bool to_zombie = (event.receiver_id != GLOBAL_ID)
                  && G_EntityIsZombie(event.receiver_id) 
                  && !e_zombie_can_receive(event.type);
    if(!to_zombie)
        e_run_handlers(s_event_handler_table, event, immediate, NULL);

    /* The script side runs after the engine's, on the script thread, with the
     * argument copied out of whatever the engine pointed it at. Raised on the
     * script thread, the event reaches it at once; otherwise it waits for the
     * script thread's next turn, unless the engine needs the result now.
     */
    bool handed = false;
    /* The scripts' part of the UI update is their windows' frame, which the
     * script thread draws by itself
     */
    bool script_frame = (event.type == EVENT_UPDATE_UI) && S_Thread_Running() && !S_Thread_OnScript();
    if(!script_frame && e_script_interested(event, to_zombie)) {
        if(S_Thread_OnScript() || !S_Thread_Running()) {
            struct script_dispatch sd = {event, immediate, to_zombie};
            S_EventArgEncode(event.type, event.source, event.arg, &sd.sarg);
            e_dispatch_script(&sd);
            S_EventArgRelease(&sd.sarg);
        }else{
            handed = e_script_send(event, immediate, to_zombie);
            if(e_script_needs_now(event.type))
                S_Thread_Call(e_script_drain, NULL);
        }
    }

    if(event.source == ES_SCRIPT && !handed)
        S_Release(event.arg);
}

static void e_notify_entities_update_start(uint32_t ticks, bool immediate)
{
    /* Snapshot the subscriber set: a handler may register or unregister
     * UPDATE_START handlers mid-dispatch, mutating the index.
     */
    STALLOC(uint32_t, uids, kh_size(s_update_start_subs));
    size_t nuids = 0;
    uint32_t uid;
    kh_foreach_key(s_update_start_subs, uid, {
        uids[nuids++] = uid;
    });

    for(size_t i = 0; i < nuids; i++) {
        e_handle_event( (struct event){EVENT_UPDATE_START, NULL, ES_ENGINE, uids[i], ticks}, immediate);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool E_Init(void)
{
    s_event_handler_table = kh_init(handler_desc);
    if(!s_event_handler_table)
        goto fail_table;

    s_script_handler_table = kh_init(handler_desc);
    if(!s_script_handler_table)
        goto fail_script_table;

    s_update_start_subs = kh_init(usub);
    s_script_keys = kh_init(skey);
    if(!s_update_start_subs)
        goto fail_front_queue;

    if(!queue_event_init(&s_event_queues[0], 2048))
        goto fail_front_queue;
    if(!queue_event_init(&s_event_queues[1], 2048))
        goto fail_back_queue;

    vec_hd_init(&s_dispatch_execd);
    vec_hd_init(&s_dispatch_curr);

    return true;
        
fail_back_queue:
    queue_event_destroy(&s_event_queues[0]);
fail_front_queue:
    kh_destroy(usub, s_update_start_subs);
    kh_destroy(skey, s_script_keys);
    kh_destroy(handler_desc, s_script_handler_table);
fail_script_table:
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

    for (k = kh_begin(s_script_handler_table); k != kh_end(s_script_handler_table); ++k) {

        if(!kh_exist(s_script_handler_table, k))
            continue; 

        vec_hd_t vec = kh_value(s_script_handler_table, k);
        vec_hd_destroy(&vec);
    }

    kh_destroy(usub, s_update_start_subs);
    kh_destroy(skey, s_script_keys);
    kh_destroy(handler_desc, s_script_handler_table);
    kh_destroy(handler_desc, s_event_handler_table);
    queue_event_destroy(&s_event_queues[1]);
    queue_event_destroy(&s_event_queues[0]);

    vec_hd_destroy(&s_dispatch_execd);
    vec_hd_destroy(&s_dispatch_curr);
}

static uint64_t s_service_ticks[E_TIME_NBUCKETS];

static enum e_time_bucket e_bucket(const struct event *event)
{
    if(event->source == ES_SCRIPT)
        return E_TIME_SCRIPT;
    switch(event->type) {
    case EVENT_60HZ_TICK:           return E_TIME_60HZ;
    case EVENT_30HZ_TICK:           return E_TIME_30HZ;
    case EVENT_20HZ_TICK:           return E_TIME_20HZ;
    case EVENT_10HZ_TICK:           return E_TIME_10HZ;
    case EVENT_5HZ_TICK:            return E_TIME_5HZ;
    case EVENT_1HZ_TICK:
    case EVENT_HALFHZ_TICK:         return E_TIME_1HZ;
    case EVENT_ANIM_FINISHED:
    case EVENT_ANIM_CYCLE_FINISHED: return E_TIME_ANIM;
    default: break;
    }
    if((int)event->type < (int)SDL_LASTEVENT)
        return E_TIME_SDL;
    return E_TIME_OTHER;
}

void E_ServiceQueue(void)
{
    PERF_ENTER();
    LF_STORE(&s_frame_upkeep_due, 1, LF_REL);
    uint32_t ticks = G_ClockTicks();

    queue_event_t *queue = &s_event_queues[s_front_queue_idx];
    s_front_queue_idx = (s_front_queue_idx + 1) % 2;

    uint64_t t0 = SDL_GetPerformanceCounter();
    e_handle_event( (struct event){EVENT_UPDATE_START, NULL, ES_ENGINE, GLOBAL_ID, ticks}, false);
    uint64_t t1 = SDL_GetPerformanceCounter();
    s_service_ticks[E_TIME_UPDATE_START] += t1 - t0;

    PERF_PUSH("entities update start scan");
    e_notify_entities_update_start(ticks, false);
    PERF_POP();
    uint64_t t2 = SDL_GetPerformanceCounter();
    s_service_ticks[E_TIME_ENT_UPDATE_START] += t2 - t1;

    PERF_PUSH("event queue drain");
    struct event event;
    while(queue_event_pop(queue, &event)) {

        enum e_time_bucket bucket = e_bucket(&event);
        uint64_t e0 = SDL_GetPerformanceCounter();
        e_handle_event(event, false);
        s_service_ticks[bucket] += SDL_GetPerformanceCounter() - e0;
        /* event arg already released */
    }
    PERF_POP();

    uint64_t t3 = SDL_GetPerformanceCounter();
    e_handle_event( (struct event){EVENT_UPDATE_END, NULL, ES_ENGINE, GLOBAL_ID, ticks}, false);
    s_service_ticks[E_TIME_UPDATE_END] += SDL_GetPerformanceCounter() - t3;

    PERF_RETURN_VOID();
}

void E_LastServiceTimes(uint64_t out_us[E_TIME_NBUCKETS])
{
    double us = 1e6 / SDL_GetPerformanceFrequency();
    for(int i = 0; i < E_TIME_NBUCKETS; i++) {
        out_us[i] = s_service_ticks[i] * us;
        s_service_ticks[i] = 0;
    }
}

void E_ServiceScriptEvents(void)
{
    if(LF_LOAD(&s_script_inbox, LF_ACQ) || LF_LOAD(&s_script_held, LF_ACQ))
        S_Thread_Call(e_script_drain, NULL);
}

void E_ScriptCallLater(void (*fn)(void *arg), void *arg)
{
    struct script_record *rec = PF_CALLOC(1, sizeof(struct script_record));
    if(!rec || !S_Thread_Running()) {
        PF_FREE(rec);
        fn(arg);
        PF_FREE(arg);
        return;
    }
    rec->call = fn;
    rec->arg = arg;
    struct script_record *head = LF_LOAD(&s_script_inbox, LF_RLX);
    do{
        rec->next = head;
    }while(!LF_CASW(&s_script_inbox, &head, rec, LF_REL, LF_RLX));
}

bool E_TakeFrameUpkeep(void)
{
    return LF_XCHG(&s_frame_upkeep_due, 0, LF_ACQ) != 0;
}

bool E_ScriptEventsWaiting(void)
{
    return LF_LOAD(&s_script_inbox, LF_ACQ) != NULL;
}

void E_PostScriptEvents(void)
{
    struct sval sync = {.as_bool = false};
    Settings_Get("pf.debug.script_events_sync", &sync);

    uint64_t t0 = SDL_GetPerformanceCounter();
    if(sync.as_bool || !S_Thread_Running()) {
        E_ServiceScriptEvents();
    }else if(LF_LOAD(&s_script_inbox, LF_ACQ)) {
        S_Thread_Poke(S_POKE_EVENTS);
    }
    s_service_ticks[E_TIME_SCRIPT] += SDL_GetPerformanceCounter() - t0;
}

void E_DispatchScriptEvents(void)
{
    assert(S_Thread_OnScript());
    struct script_record *rec;
    int nhandled = 0;
    while(!S_Task_Ending() && (rec = e_script_next())) {
        e_script_record_run(rec);
        e_script_record_free(rec);
        /* Alongside the main thread, a few at a time: a call it makes waits
         * for the slice to end
         */
        if(++nhandled % SCRIPT_DISPATCH_BATCH == 0 && Sched_InViewSlice())
            S_Task_YieldSlice();
    }
}

void E_NoteScriptAwait(int event)
{
    e_sub_set(s_script_task_subs, event);
}

static void e_script_clear(void *unused)
{
    (void)unused;
    struct script_record *rec;
    while((rec = e_script_next())) {
        e_script_record_free(rec);
    }
}

void E_ClearPendingEvents(void)
{
    /* The records it holds are the script thread's to free */
    S_Thread_Call(e_script_clear, NULL);

    for(int q = 0; q < 2; q++) {
        queue_event_t *queue = &s_event_queues[q];
        struct event event;
        while(queue_event_pop(queue, &event)) {
            if(event.source == ES_SCRIPT)
                S_Release(event.arg);
        }
    }
}

void E_FlushEventQueue(void)
{
    while(E_EventsQueued()) {

        E_ServiceScriptEvents();

        queue_event_t *queue = &s_event_queues[s_front_queue_idx];
        s_front_queue_idx = (s_front_queue_idx + 1) % 2;

        struct event event;
        while(queue_event_pop(queue, &event)) {
            /* Skip over SDL events during flushing - the pointers
             * may already be stale.
             */
            if((int)event.type <= (int)SDL_LASTEVENT)
                continue;
            e_handle_event(event, true);
        }
    }
}

bool E_Entity_HasHandler(enum eventtype event, uint32_t ent_uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(handler_desc, s_event_handler_table, e_key(ent_uid, event));
    if(k != kh_end(s_event_handler_table) && vec_size(&kh_value(s_event_handler_table, k)) > 0)
        return true;
    SDL_AtomicLock(&s_script_keys_lock);
    bool ret = kh_get(skey, s_script_keys, e_key(ent_uid, event)) != kh_end(s_script_keys);
    SDL_AtomicUnlock(&s_script_keys_lock);
    return ret;
}

bool E_QueuedThisFrame(enum eventtype event)
{
    ASSERT_IN_MAIN_THREAD();

    queue_event_t *queue = &s_event_queues[(s_front_queue_idx + 1) % 2];
    size_t left = queue_size(*queue);
    for(int i = queue->itail; left > 0;) {
        struct event *curr = &queue->mem[i];
        if(curr->type == event)
            return true;
        i--;
        left--;
        if(i < 0) {
            i = queue->capacity - 1; /* Wrap around */
        }
    }
    return false;
}

bool E_InImmediateDispatch(void)
{
    return (s_immediate_depth > 0);
}

bool E_EventsQueued(void)
{
    return (queue_size(s_event_queues[0]) > 0) 
        || (queue_size(s_event_queues[1]) > 0)
        || (LF_LOAD(&s_script_inbox, LF_ACQ) != NULL)
        || LF_LOAD(&s_script_held, LF_ACQ);
}

void E_DeleteScriptHandlers(void)
{
    uint64_t key;
    vec_hd_t curr;

    kh_foreach(s_script_handler_table, key, curr, {

        for(int i = 0; i < vec_size(&curr); i++) {

            struct handler_desc hd = vec_AT(&curr, i);
            S_Release(hd.handler.as_script_callable);
            S_Release(hd.user_arg); 
            e_update_start_subs_add(key, -1);
        }
        vec_hd_destroy(&curr);
    });
    kh_clear(handler_desc, s_script_handler_table);
    SDL_AtomicLock(&s_script_keys_lock);
    kh_clear(skey, s_script_keys);
    SDL_AtomicUnlock(&s_script_keys_lock);
}

size_t E_GetScriptHandlers(size_t max_out, struct script_handler *out)
{
    size_t ret = 0;
    uint64_t key;
    vec_hd_t curr;

    kh_foreach(s_script_handler_table, key, curr, {

        for(int i = 0; i < vec_size(&curr); i++) {

            struct handler_desc hd = vec_AT(&curr, i);
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
    struct event e = (struct event){event, event_arg, source, GLOBAL_ID, G_ClockTicks()};
    queue_event_push(&s_event_queues[s_front_queue_idx], &e);
}

bool E_Global_RegisterNamed(enum eventtype event, handler_t handler, void *user_arg,
                            int simmask, const char *fname)
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_ENGINE;
    hd.handler.as_function = handler;
    hd.user_arg = user_arg;
    hd.simmask = simmask;
    hd.register_tick = G_ClockTicks();
    hd.fname = fname;

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
    e_sub_set(s_script_global_subs, event);
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_SCRIPT;
    hd.handler.as_script_callable = handler;
    hd.user_arg = user_arg;
    hd.simmask = simmask;
    hd.register_tick = G_ClockTicks();
    hd.fname = NULL;

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
    struct event e = (struct event){event, event_arg, source, GLOBAL_ID, G_ClockTicks()};
    e_handle_event(e, true);
}

void E_Global_NotifyScriptNow(enum eventtype event)
{
    assert(S_Thread_OnScript() || !S_Thread_Running());
    struct script_dispatch sd = {
        .event = (struct event){event, NULL, ES_ENGINE, GLOBAL_ID, G_ClockTicks()},
        .immediate = true,
        .to_zombie = false,
    };
    S_EventArgEncode(event, ES_ENGINE, NULL, &sd.sarg);
    /* From a view slice, the tasks waiting on the event are left for the next
     * world window: only a view task may run alongside the main thread. The
     * handlers run here and now, under the slice's own guard.
     */
    Sched_HandleScriptEvent(event, &sd.sarg, ES_ENGINE, !Sched_InViewSlice());
    e_run_handlers(s_script_handler_table, sd.event, true, &sd.sarg);
    S_EventArgRelease(&sd.sarg);
}

/*
 * Entity Events
 */

bool E_Entity_RegisterNamed(enum eventtype event, uint32_t ent_uid, handler_t handler,
                            void *user_arg, int simmask, const char *fname)
{
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_ENGINE;
    hd.handler.as_function = handler;
    hd.user_arg = user_arg;
    hd.simmask = simmask;
    hd.register_tick = G_ClockTicks();
    hd.fname = fname;

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
    e_sub_set(s_script_entity_subs, event);
    struct handler_desc hd;
    hd.type = HANDLER_TYPE_SCRIPT;
    hd.handler.as_script_callable = handler;
    hd.user_arg = user_arg;
    hd.simmask = simmask;
    hd.register_tick = G_ClockTicks();
    hd.fname = NULL;

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
    struct event e = (struct event){event, event_arg, source, ent_uid, G_ClockTicks()};
    queue_event_push(&s_event_queues[s_front_queue_idx], &e);
}

void E_Entity_NotifyImmediate(enum eventtype event, uint32_t ent_uid, void *event_arg, 
                              enum event_source source)
{
    struct event e = (struct event){event, event_arg, source, ent_uid, G_ClockTicks()};
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

