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

#include "resource.h"
#include "../lib/public/khash.h"
#include "../lib/public/string_intern.h"

struct rstate{
    const char *name;
    int         amount;
};

KHASH_MAP_INIT_INT(state, struct rstate)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(stridx) *s_stridx;
static mp_strbuff_t     s_stringpool;
static khash_t(state)  *s_entity_state_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct rstate *rstate_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;

    return &kh_value(s_entity_state_table, k);
}

static bool rstate_set(uint32_t uid, struct rstate rs)
{
    int status;
    khiter_t k = kh_put(state, s_entity_state_table, uid, &status);
    if(status == -1 || status == 0)
        return false;
    kh_value(s_entity_state_table, k) = rs;
    return true;
}

static void rstate_remove(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Resource_Init(void)
{
    if(!(s_entity_state_table = kh_init(state)))
        goto fail_table;
    if(!si_init(&s_stringpool, &s_stridx, 512))
        goto fail_strintern;

    return true;

fail_strintern:
    kh_destroy(state, s_entity_state_table);
fail_table:
    return false;
}

void G_Resource_Shutdown(void)
{
    si_shutdown(&s_stringpool, s_stridx);
    kh_destroy(state, s_entity_state_table);
}

bool G_Resource_AddEntity(uint32_t uid, const char *name, int init_amount)
{
    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return false;

    struct rstate rs = (struct rstate) {
        .name = key,
        .amount = init_amount
    };
    if(!rstate_set(uid, rs))
        return false;
    return true;
}

void G_Resource_RemoveEntity(uint32_t uid)
{
    rstate_remove(uid);
}

int G_Resource_GetAmount(uint32_t uid)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);
    return rs->amount;
}

void G_Resource_SetAmount(uint32_t uid, int amount)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);
    rs->amount = amount;
}

const char *G_Resource_GetName(uint32_t uid)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);
    return rs->name;
}

