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

#include "builder.h"
#include "game_private.h"
#include "movement.h"
#include "position.h"
#include "public/game.h"
#include "../entity.h"
#include "../perf.h"
#include "../event.h"
#include "../collision.h"
#include "../map/public/map.h"
#include "../lib/public/khash.h"

#include <stdint.h>
#include <assert.h>

#define UID_NONE  (~((uint32_t)0))

struct builderstate{
    enum{
        STATE_NOT_BUILDING,
        STATE_MOVING_TO_TARGET,
        STATE_BUILDING,
    }state;
    int build_speed;
    uint32_t target_uid;
};

KHASH_MAP_INIT_INT(state, struct builderstate)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(state) *s_entity_state_table;
static struct map     *s_map;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct builderstate *builderstate_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;

    return &kh_value(s_entity_state_table, k);
}

static void builderstate_set(const struct entity *ent, struct builderstate bs)
{
    int ret;
    assert(ent->flags & ENTITY_FLAG_BUILDER);

    khiter_t k = kh_put(state, s_entity_state_table, ent->uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_state_table, k) = bs;
}

static void builderstate_remove(const struct entity *ent)
{
    assert(ent->flags & ENTITY_FLAG_BUILDER);

    khiter_t k = kh_get(state, s_entity_state_table, ent->uid);
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
}

static void on_30hz_tick(void *user, void *event)
{
    PERF_ENTER();

    uint32_t key;
    struct builderstate curr;

    struct entity *ent;
    (void)ent;

    kh_foreach(s_entity_state_table, key, curr, {

        ent = G_EntityForUID(key);
        switch(curr.state) {
        case STATE_NOT_BUILDING:
            /* no-op */
            break;
        case STATE_MOVING_TO_TARGET:
            break;
        case STATE_BUILDING:
            break;
        default: assert(0);
        }
    });

    PERF_RETURN_VOID();
}

static void on_motion_end(void *user, void *event)
{
    uint32_t uid = (uintptr_t)user;
    const struct entity *ent = G_EntityForUID(uid);

    struct builderstate *bs = builderstate_get(uid);
    assert(bs);

    E_Entity_Unregister(EVENT_MOTION_END, uid, on_motion_end);

    if(!G_Move_Still(ent)) {
        bs->state = STATE_NOT_BUILDING;
        bs->target_uid = UID_NONE;
        return; /* builder received a new destination */
    }

    assert(bs->target_uid != UID_NONE);
    const struct entity *target = G_EntityForUID(bs->target_uid);

    if(!target
    || !M_NavObjsAdjacent(s_map, ent, target)) {
        bs->state = STATE_NOT_BUILDING;
        bs->target_uid = UID_NONE;
        return; /* builder could not reach the building */
    }

    //TODO: try to "found" the building here... (?)
    bs->state = STATE_BUILDING; 
    E_Entity_Notify(EVENT_BUILD_BEGIN, uid, NULL, ES_ENGINE);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Builder_Init(struct map *map)
{
    if(NULL == (s_entity_state_table = kh_init(state)))
        return false;

    E_Global_Register(EVENT_30HZ_TICK, on_30hz_tick, NULL, G_RUNNING);

    s_map = map;
    return true;
}

void G_Builder_Shutdown(void)
{
    s_map = NULL;
    E_Global_Unregister(EVENT_30HZ_TICK, on_30hz_tick);
    kh_destroy(state, s_entity_state_table);
}

bool G_Builder_Build(struct entity *builder, struct entity *building)
{
    struct builderstate *bs = builderstate_get(builder->uid);
    assert(bs);

    if(!(building->flags & ENTITY_FLAG_BUILDING))
        return false;

    G_Move_SetSurroundEntity(builder, building);
    E_Entity_Register(EVENT_MOTION_END, builder->uid, on_motion_end, (void*)((uintptr_t)builder->uid), G_RUNNING);

    bs->state = STATE_MOVING_TO_TARGET;
    bs->target_uid = building->uid;
    return true;
}

void G_Builder_AddEntity(struct entity *ent)
{
    assert(builderstate_get(ent->uid) == NULL);
    assert(ent->flags & ENTITY_FLAG_BUILDER);

    builderstate_set(ent, (struct builderstate){
        .state = STATE_NOT_BUILDING,
        .build_speed = 0.0,
        .target_uid = UID_NONE,
    });
}

void G_Builder_RemoveEntity(const struct entity *ent)
{
    if(!(ent->flags & ENTITY_FLAG_BUILDER))
        return;
    E_Entity_Unregister(EVENT_MOTION_END, ent->uid, on_motion_end);
    builderstate_remove(ent);
}

void G_Builder_SetBuildSpeed(const struct entity *ent, int speed)
{
    struct builderstate *bs = builderstate_get(ent->uid);
    assert(bs);
    bs->build_speed = speed;
}

int G_Builder_GetBuildSpeed(const struct entity *ent)
{
    struct builderstate *bs = builderstate_get(ent->uid);
    assert(bs);
    return bs->build_speed;
}

