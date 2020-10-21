/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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
#include "game_private.h"
#include "public/game.h"
#include "../event.h"
#include "../entity.h"
#include "../cursor.h"
#include "../lib/public/mpool.h"
#include "../lib/public/khash.h"
#include "../lib/public/string_intern.h"

#include <stddef.h>
#include <assert.h>

#define UID_NONE            (~((uint32_t)0))
#define REACQUIRE_RADIUS    (25.0)

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

enum harvester_state{
    STATE_NOT_HARVESTING,
    STATE_MOVING_TO_RESOURCE,
    STATE_HARVESTING,
    STATE_MOVING_TO_STORAGE,
};

struct hstate{
    enum harvester_state state;
    uint32_t target_uid;
    kh_int_t *gather_speeds;    /* How much of each resource the entity gets each cycle */
    kh_int_t *max_carry;        /* The maximum amount of each resource the entity can carry */
    kh_int_t *curr_carry;       /* The amount of each resource the entity currently holds */
};

typedef char buff_t[512];

MPOOL_TYPE(buff, buff_t)
MPOOL_PROTOTYPES(static, buff, buff_t)
MPOOL_IMPL(static, buff, buff_t)

#undef kmalloc
#undef kcalloc
#undef krealloc
#undef kfree

#define kmalloc  malloc
#define kcalloc  calloc
#define krealloc realloc
#define kfree    free

KHASH_MAP_INIT_INT(state, struct hstate)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static mp_buff_t         s_mpool;
static khash_t(stridx)  *s_stridx;
static mp_strbuff_t      s_stringpool;
static khash_t(state)   *s_entity_state_table;
static bool              s_gather_on_lclick = false;
static const struct map *s_map;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void *pmalloc(size_t size)
{
    mp_ref_t ref = mp_buff_alloc(&s_mpool);
    if(ref == 0)
        return NULL;
    return mp_buff_entry(&s_mpool, ref);
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
    if(size <= sizeof(buff_t))
        return ptr;
    return NULL;
}

static void pfree(void *ptr)
{
    if(!ptr)
        return;
    mp_ref_t ref = mp_buff_ref(&s_mpool, ptr);
    mp_buff_free(&s_mpool, ref);
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

static bool hstate_init(struct hstate *hs)
{
    hs->gather_speeds = kh_init(int);
    hs->max_carry = kh_init(int);
    hs->curr_carry = kh_init(int);

    if(!hs->gather_speeds || !hs->max_carry || !hs->curr_carry) {
    
        kh_destroy(int, hs->gather_speeds);
        kh_destroy(int, hs->max_carry);
        kh_destroy(int, hs->curr_carry);
        return false;
    }

    hs->target_uid = UID_NONE;
    hs->state = STATE_NOT_HARVESTING;
    return true;
}

static void hstate_destroy(struct hstate *hs)
{
    kh_destroy(int, hs->gather_speeds);
    kh_destroy(int, hs->max_carry);
    kh_destroy(int, hs->curr_carry);
}

static bool hstate_set_key(khash_t(int) *table, const char *name, int val)
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
    if(status == -1)
        return false;

    assert(status == 1);
    kh_value(table, k) = val;
    return true;
}

static bool hstate_get_key(khash_t(int) *table, const char *name, int *out)
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

static void on_harvest_anim_finished(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    const struct entity *ent = G_EntityForUID(uid);

    struct hstate *hs = hstate_get(uid);
    assert(hs);

    struct entity *target = G_EntityForUID(hs->target_uid);
    //TODO: Get the Resource Here
}

static void on_motion_begin(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    const struct entity *ent = G_EntityForUID(uid);

    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished);

    struct hstate *hs = hstate_get(uid);
    assert(hs);
    assert(hs->state == STATE_HARVESTING);

    hs->state = STATE_NOT_HARVESTING;
    E_Entity_Notify(EVENT_HARVEST_END, uid, NULL, ES_ENGINE);
}

static void finish_harvesting(struct hstate *hs, uint32_t uid)
{
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished);
    E_Entity_Unregister(EVENT_MOTION_START, uid, on_motion_begin);

    hs->state = STATE_NOT_HARVESTING;
    hs->target_uid = UID_NONE;
    E_Entity_Notify(EVENT_HARVEST_END, uid, NULL, ES_ENGINE);
}

static void on_motion_end(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    struct entity *ent = G_EntityForUID(uid);

    struct hstate *hs = hstate_get(uid);
    assert(hs);

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_motion_end);

    if(!G_Move_Still(ent)) {
        hs->state = STATE_NOT_HARVESTING;
        hs->target_uid = UID_NONE;
        return; /* harvester received a new destination */
    }

    assert(hs->target_uid != UID_NONE);
    struct entity *target = G_EntityForUID(hs->target_uid);

    struct obb obb;
    if(target) {
        Entity_CurrentOBB(target, &obb, false);
    }

    if(!target
    || !M_NavObjAdjacentToStatic(s_map, ent, &obb)) {
        hs->state = STATE_NOT_HARVESTING;
        hs->target_uid = UID_NONE;
        return; /* harvester could not reach the resource */
    }

    E_Entity_Notify(EVENT_HARVEST_BEGIN, uid, NULL, ES_ENGINE);
    hs->state = STATE_HARVESTING; 
    E_Entity_Register(EVENT_MOTION_START, uid, on_motion_begin, (void*)((uintptr_t)uid), G_RUNNING);
    E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_harvest_anim_finished, 
        (void*)((uintptr_t)uid), G_RUNNING);
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);
    bool targeting = G_MouseInTargetMode();

    s_gather_on_lclick = false;
    Cursor_SetRTSPointer(CURSOR_POINTER);

    if(G_MouseOverMinimap())
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    if((mouse_event->button == SDL_BUTTON_RIGHT) && targeting)
        return;

    if((mouse_event->button == SDL_BUTTON_LEFT) && !targeting)
        return;

    struct entity *target = G_Sel_GetHovered();
    if(!target || !(target->flags & ENTITY_FLAG_RESOURCE))
        return;

    enum selection_type sel_type;
    const vec_pentity_t *sel = G_Sel_Get(&sel_type);
    size_t ngather = 0;

    for(int i = 0; i < vec_size(sel); i++) {

        struct entity *curr = vec_AT(sel, i);
        if(!(curr->flags & ENTITY_FLAG_HARVESTER))
            continue;

        struct hstate *hs = hstate_get(curr->uid);
        assert(hs);

        finish_harvesting(hs, curr->uid);
        G_Harvester_Gather(curr, target);
        ngather++;
    }

    if(ngather) {
        Entity_Ping(target);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Harvester_Init(const struct map *map)
{
    mp_buff_init(&s_mpool);

    if(!mp_buff_reserve(&s_mpool, 4096 * 3))
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
    mp_buff_destroy(&s_mpool);
fail_mpool:
    return false;
}

void G_Harvester_Shutdown(void)
{
    s_map = NULL;
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);

    si_shutdown(&s_stringpool, s_stridx);
    kh_destroy(state, s_entity_state_table);
    mp_buff_destroy(&s_mpool);
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
    assert(hs);
    hstate_destroy(hs);
    hstate_remove(uid);
}

bool G_Harvester_SetGatherSpeed(uint32_t uid, const char *rname, int speed)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return hstate_set_key(hs->gather_speeds, rname, speed);
}

int G_Harvester_GetGatherSpeed(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_GATHER_SPEED;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hstate_get_key(hs->gather_speeds, rname, &ret);
    return ret;
}

bool G_Harvester_SetMaxCarry(uint32_t uid, const char *rname, int max)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return hstate_set_key(hs->max_carry, rname, max);
}

int G_Harvester_GetMaxCarry(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_MAX_CARRY;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hstate_get_key(hs->max_carry, rname, &ret);
    return ret;
}

bool G_Harvester_SetCurrCarry(uint32_t uid, const char *rname, int curr)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return hstate_set_key(hs->curr_carry, rname, curr);
}

int G_Harvester_GetCurrCarry(uint32_t uid, const char *rname)
{
    int ret = 0;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hstate_get_key(hs->curr_carry, rname, &ret);
    return ret;
}

void G_Harvester_SetGatherOnLeftClick(void)
{
    s_gather_on_lclick  = true;
    Cursor_SetRTSPointer(CURSOR_TARGET);
}

bool G_Harvester_Gather(struct entity *harvester, struct entity *resource)
{
    struct hstate *hs = hstate_get(harvester->uid);
    assert(hs);

    if(!(resource->flags & ENTITY_FLAG_RESOURCE))
        return false;

    E_Entity_Register(EVENT_MOTION_END, harvester->uid, on_motion_end, 
        (void*)((uintptr_t)harvester->uid), G_RUNNING);
    G_Move_SetSurroundEntity(harvester, resource);

    hs->state = STATE_MOVING_TO_RESOURCE;
    hs->target_uid = resource->uid;
    E_Entity_Notify(EVENT_HARVEST_TARGET_ACQUIRED, harvester->uid, resource, ES_ENGINE);
    return true;
}

bool G_Harvester_InTargetMode(void)
{
    return s_gather_on_lclick;
}

bool G_Harvester_HasRightClickAction(void)
{
    struct entity *hovered = G_Sel_GetHovered();
    if(!hovered)
        return false;

    enum selection_type sel_type;
    const vec_pentity_t *sel = G_Sel_Get(&sel_type);

    if(vec_size(sel) == 0)
        return false;

    const struct entity *first = vec_AT(sel, 0);
    if(first->flags & ENTITY_FLAG_HARVESTER && hovered->flags & ENTITY_FLAG_RESOURCE)
        return true;

    return false;
}

