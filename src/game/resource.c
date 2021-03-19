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
#include "../event.h"
#include "../entity.h"
#include "../collision.h"
#include "../map/public/map.h"
#include "../lib/public/khash.h"
#include "../lib/public/string_intern.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_string.h"


#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)


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
        .blocking_radius = G_GetSelectionRadius(ent->uid),
    };

    if(!rstate_set(ent->uid, rs))
        return false;

    if(!(ent->flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersIncref(rs.blocking_pos, rs.blocking_radius, G_GetFactionID(ent->uid), s_map);
    }
    return true;
}

void G_Resource_RemoveEntity(struct entity *ent)
{
    struct rstate *rs = rstate_get(ent->uid);
    if(!rs)
        return;

    if(!(ent->flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersDecref(rs->blocking_pos, rs->blocking_radius, G_GetFactionID(ent->uid), s_map);
    }

    rstate_remove(ent->uid);
}

void G_Resource_UpdateBounds(const struct entity *ent)
{
    struct rstate *rs = rstate_get(ent->uid);
    if(!rs)
        return;

    if(!(ent->flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersDecref(rs->blocking_pos, rs->blocking_radius, G_GetFactionID(ent->uid), s_map);
        rs->blocking_pos = G_Pos_GetXZ(ent->uid);
        M_NavBlockersIncref(rs->blocking_pos, rs->blocking_radius, G_GetFactionID(ent->uid), s_map);
    }
}

void G_Resource_UpdateFactionID(const struct entity *ent, int oldfac, int newfac)
{
    struct rstate *rs = rstate_get(ent->uid);
    if(!rs)
        return;

    if(!(ent->flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersDecref(rs->blocking_pos, rs->blocking_radius, oldfac, s_map);
        M_NavBlockersIncref(rs->blocking_pos, rs->blocking_radius, newfac, s_map);
    }
}

void G_Resource_UpdateSelectionRadius(const struct entity *ent, float radius)
{
    struct rstate *rs = rstate_get(ent->uid);
    if(!rs)
        return;

    if(!(ent->flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersDecref(rs->blocking_pos, rs->blocking_radius, G_GetFactionID(ent->uid), s_map);
        rs->blocking_radius = radius;
        M_NavBlockersIncref(rs->blocking_pos, rs->blocking_radius, G_GetFactionID(ent->uid), s_map);
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
    if(rs->amount != amount) {
        E_Entity_Notify(EVENT_RESOURCE_AMOUNT_CHANGED, uid, NULL, ES_ENGINE);
    }
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

bool G_Resource_SaveState(struct SDL_RWops *stream)
{
    struct attr num_ents = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_entity_state_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_ents, "num_ents"));

    uint32_t key;
    struct rstate curr;

    kh_foreach(s_entity_state_table, key, curr, {

        struct attr uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = key
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid, "uid"));

        struct attr name = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(name.val.as_string, curr.name, sizeof(name.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &name, "name"));

        struct attr cursor = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(cursor.val.as_string, curr.cursor, sizeof(cursor.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &cursor, "cursor"));

        struct attr amount = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.amount
        };
        CHK_TRUE_RET(Attr_Write(stream, &amount, "amount"));
    });

    struct attr num_names = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_all_names)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_names, "num_names"));

    for(khiter_t k = kh_begin(s_all_names); k != kh_end(s_all_names); k++) {

        if(!kh_exist(s_all_names, k))
            continue;

        const char *key = kh_key(s_all_names, k);
        struct attr name = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(name.val.as_string, key, sizeof(name.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &name, "name"));
    }

    return true;
}

bool G_Resource_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_ents = attr.val.as_int;

    for(int i = 0; i < num_ents; i++) {

        uint32_t uid;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        G_Resource_SetName(uid, attr.val.as_string);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        G_Resource_SetCursor(uid, attr.val.as_string);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        G_Resource_SetAmount(uid, attr.val.as_int);
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_names = attr.val.as_int;

    for(int i = 0; i < num_names; i++) {
    
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);

        const char *key = si_intern(attr.val.as_string, &s_stringpool, s_stridx);
        kh_put(name, s_all_names, key, &(int){0});
    }

    return true;
}

