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
#include "game_private.h"
#include "../entity.h"
#include "../collision.h"
#include "../map/public/map.h"
#include "../lib/public/khash.h"
#include "../lib/public/string_intern.h"

struct rstate{
    const char *name;
    const char *cursor;
    int         amount;
    vec2_t      blocking_pos;
    int         blocking_radius;
};

KHASH_MAP_INIT_INT(state, struct rstate)
KHASH_SET_INIT_STR(name)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(stridx)  *s_stridx;
static mp_strbuff_t      s_stringpool;
static khash_t(state)   *s_entity_state_table;
static const struct map *s_map;
/* The set of all resources that exist (or have existed) in the current session */
static khash_t(name)    *s_all_names;

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

static int compare_keys(const void *a, const void *b)
{
    char *stra = *(char**)a;
    char *strb = *(char**)b;
    return strcmp(stra, strb);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Resource_Init(const struct map *map)
{
    if(!(s_entity_state_table = kh_init(state)))
        goto fail_table;
    if(!si_init(&s_stringpool, &s_stridx, 512))
        goto fail_strintern;
    if(!(s_all_names = kh_init(name)))
        goto fail_name_set;

    s_map = map;
    return true;

fail_name_set:
    si_shutdown(&s_stringpool, s_stridx);
fail_strintern:
    kh_destroy(state, s_entity_state_table);
fail_table:
    return false;
}

void G_Resource_Shutdown(void)
{
    kh_destroy(name, s_all_names);
    si_shutdown(&s_stringpool, s_stridx);
    kh_destroy(state, s_entity_state_table);
}

bool G_Resource_AddEntity(const struct entity *ent)
{
    struct rstate rs = (struct rstate) {
        .name = "",
        .cursor = "",
        .amount = 0,
        .blocking_pos = G_Pos_GetXZ(ent->uid),
        .blocking_radius = ent->selection_radius,
    };

    if(!rstate_set(ent->uid, rs))
        return false;

    if(!(ent->flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersIncref(rs.blocking_pos, rs.blocking_radius, s_map);
    }
    return true;
}

void G_Resource_RemoveEntity(struct entity *ent)
{
    struct rstate *rs = rstate_get(ent->uid);
    if(!rs)
        return;

    if(!(ent->flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersDecref(rs->blocking_pos, rs->blocking_radius, s_map);
    }

    rstate_remove(ent->uid);
}

void G_Resource_UpdateBounds(const struct entity *ent)
{
    struct rstate *rs = rstate_get(ent->uid);
    if(!rs)
        return;

    if(!(ent->flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersDecref(rs->blocking_pos, rs->blocking_radius, s_map);
        rs->blocking_pos = G_Pos_GetXZ(ent->uid);
        M_NavBlockersIncref(rs->blocking_pos, rs->blocking_radius, s_map);
    }
}

void G_Resource_UpdateSelectionRadius(const struct entity *ent, float radius)
{
    struct rstate *rs = rstate_get(ent->uid);
    if(!rs)
        return;

    if(!(ent->flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersDecref(rs->blocking_pos, rs->blocking_radius, s_map);
        rs->blocking_radius = radius;
        M_NavBlockersIncref(rs->blocking_pos, rs->blocking_radius, s_map);
    }
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

bool G_Resource_SetName(uint32_t uid, const char *name)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);

    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return false;

    rs->name = key;
    kh_put(name, s_all_names, key, &(int){0});
    return true;
}

const char *G_Resource_GetCursor(uint32_t uid)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);
    return rs->cursor;
}

bool G_Resource_SetCursor(uint32_t uid, const char *cursor)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);

    const char *key = si_intern(cursor, &s_stringpool, s_stridx);
    if(!key)
        return false;

    rs->cursor = key;
    return true;
}

int G_Resource_GetAllNames(size_t maxout, const char *out[static maxout])
{
    size_t ret = 0;

    for(khiter_t k = kh_begin(s_all_names); k != kh_end(s_all_names); k++) {
        if(!kh_exist(s_all_names, k))
            continue;
        if(ret == maxout)
            break;
        out[ret++] = kh_key(s_all_names, k);
    }

    qsort(out, ret, sizeof(char*), compare_keys);
    return ret;
}

