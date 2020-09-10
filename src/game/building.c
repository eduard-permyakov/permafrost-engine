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

#include "building.h"
#include "game_private.h"
#include "public/game.h"
#include "../event.h"
#include "../collision.h"
#include "../entity.h"
#include "../map/public/map.h"
#include "../lib/public/khash.h"

#include <assert.h>

enum buildstate{
    BUILDING_STATE_PLACEMENT,
    BUILDING_STATE_MARKED,
    BUILDING_STATE_FOUNDED,
    BUILDING_STATE_COMPLETED,
};

KHASH_MAP_INIT_INT(state, enum buildstate)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map     *s_map;
static khash_t(state)       *s_entity_state_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static enum buildstate *buildstate_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;

    return &kh_value(s_entity_state_table, k);
}

static void buildstate_set(const struct entity *ent, enum buildstate bs)
{
    int ret;
    assert(ent->flags & ENTITY_FLAG_BUILDING);

    khiter_t k = kh_put(state, s_entity_state_table, ent->uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_state_table, k) = bs;
}

static void buildstate_remove(const struct entity *ent)
{
    assert(ent->flags & ENTITY_FLAG_BUILDING);

    khiter_t k = kh_get(state, s_entity_state_table, ent->uid);
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
}

static void on_render_3d(void *user, void *event)
{
    const struct camera *cam = G_GetActiveCamera();

    uint32_t key;
    enum buildstate curr;

    kh_foreach(s_entity_state_table, key, curr, {

        if(curr != BUILDING_STATE_PLACEMENT)
            continue;

        const struct entity *ent = G_EntityForUID(key);
        struct obb obb;
        Entity_CurrentOBB(ent, &obb, true);

        M_NavRenderBuildableTiles(s_map, cam, &obb);
    });
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Building_Init(const struct map *map)
{
    if(NULL == (s_entity_state_table = kh_init(state)))
        return false;

    E_Global_Register(EVENT_RENDER_3D_PRE, on_render_3d, NULL, G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    s_map = map;
    return true;
}

void G_Building_Shutdown(void)
{
    s_map = NULL;
    E_Global_Unregister(EVENT_RENDER_3D_PRE, on_render_3d);
    kh_destroy(state, s_entity_state_table);
}

void G_Building_AddEntity(struct entity *ent)
{
    assert(buildstate_get(ent->uid) == NULL);
    assert(ent->flags & ENTITY_FLAG_BUILDING);

    buildstate_set(ent, BUILDING_STATE_PLACEMENT);
    ent->flags |= ENTITY_FLAG_TRANSLUCENT;
    ent->flags |= ENTITY_FLAG_STATIC;
}

void G_Building_RemoveEntity(const struct entity *ent)
{
    if(!(ent->flags & ENTITY_FLAG_BUILDING))
        return;
    buildstate_remove(ent);
}

bool G_Building_Mark(const struct entity *ent)
{
    enum buildstate *bs = buildstate_get(ent->uid);
    assert(bs);

    if(*bs != BUILDING_STATE_PLACEMENT)
        return false;

    *bs = BUILDING_STATE_MARKED;
    return true;
}

bool G_Building_Found(const struct entity *ent)
{
    return true;
}

