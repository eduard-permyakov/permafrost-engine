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

#include "garrison.h"
#include "public/game.h"
#include "../lib/public/khash.h"
#include "../lib/public/vec.h"

#include <assert.h>

struct garrison_state{
    int capacity_consumed;
};

struct garrisonable_state{
    int          capacity;
    int          current;
    vec_entity_t garrisoned;
};

KHASH_MAP_INIT_INT(garrison, struct garrison_state)
KHASH_MAP_INIT_INT(garrisonable, struct garrisonable_state)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map      *s_map;
static khash_t(garrison)     *s_garrison_state_table;
static khash_t(garrisonable) *s_garrisonable_state_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/* gu - garrison unit 
 * gb - garrisonable building
 */

static struct garrison_state *gu_state_get(uint32_t uid)
{
    khiter_t k = kh_get(garrison, s_garrison_state_table, uid);
    if(k == kh_end(s_garrison_state_table))
        return NULL;

    return &kh_value(s_garrison_state_table, k);
}

static bool gu_state_set(uint32_t uid, struct garrison_state gus)
{
    int status;
    khiter_t k = kh_put(garrison, s_garrison_state_table, uid, &status);
    if(status == -1 || status == 0)
        return false;
    kh_value(s_garrison_state_table, k) = gus;
    return true;
}

static void gu_state_remove(uint32_t uid)
{
    khiter_t k = kh_get(garrison, s_garrison_state_table, uid);
    if(k != kh_end(s_garrison_state_table))
        kh_del(garrison, s_garrison_state_table, k);
}

static struct garrisonable_state *gb_state_get(uint32_t uid)
{
    khiter_t k = kh_get(garrisonable, s_garrisonable_state_table, uid);
    if(k == kh_end(s_garrisonable_state_table))
        return NULL;

    return &kh_value(s_garrisonable_state_table, k);
}

static bool gb_state_set(uint32_t uid, struct garrisonable_state gus)
{
    int status;
    khiter_t k = kh_put(garrisonable, s_garrisonable_state_table, uid, &status);
    if(status == -1 || status == 0)
        return false;
    kh_value(s_garrisonable_state_table, k) = gus;
    return true;
}

static void gb_state_remove(uint32_t uid)
{
    khiter_t k = kh_get(garrisonable, s_garrisonable_state_table, uid);
    if(k != kh_end(s_garrisonable_state_table))
        kh_del(garrisonable, s_garrisonable_state_table, k);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Garrison_Init(const struct map *map)
{
    if((s_garrison_state_table = kh_init(garrison)) == NULL)
        goto fail_garrison;
    if((s_garrisonable_state_table = kh_init(garrisonable)) == NULL)
        goto fail_garrisonable;

    s_map = map;
    return true;

fail_garrisonable:
    kh_destroy(garrison, s_garrison_state_table);
fail_garrison:
    return false;
}

void G_Garrison_Shutdown(void)
{
    kh_destroy(garrisonable, s_garrisonable_state_table);
    kh_destroy(garrison, s_garrison_state_table);
}

bool G_Garrison_AddGarrison(uint32_t uid)
{
    struct garrison_state gus;
    gus.capacity_consumed = 1;
    return gu_state_set(uid, gus);
}

void G_Garrison_RemoveGarrison(uint32_t uid)
{
    gu_state_remove(uid);
}

bool G_Garrison_AddGarrisonable(uint32_t uid)
{
    struct garrisonable_state gbs;
    gbs.capacity = 0;
    gbs.current = 0;
    vec_entity_init(&gbs.garrisoned);
    return gb_state_set(uid, gbs);
}

void G_Garrison_RemoveGarrisonable(uint32_t uid)
{
    gb_state_remove(uid);
}

void G_Garrison_SetCapacityConsumed(uint32_t uid, int capacity)
{
    struct garrison_state *gus = gu_state_get(uid);
    assert(gus);
    gus->capacity_consumed = capacity;
}

void G_Garrison_SetGarrisonableCapacity(uint32_t uid, int capacity)
{
    struct garrisonable_state *gbs = gb_state_get(uid);
    assert(gbs);
    gbs->capacity = capacity;
}

bool G_Garrison_Enter(uint32_t garrisonable, uint32_t unit)
{
    return true;
}

bool G_Garrison_Evict(uint32_t garrisonable, uint32_t unit)
{
    return true;
}

