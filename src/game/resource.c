/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2023 Eduard Permyakov 
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
#include "public/game.h"
#include "storage_site.h"
#include "../sched.h"
#include "../event.h"
#include "../entity.h"
#include "../phys/public/collision.h"
#include "../map/public/map.h"
#include "../lib/public/khash.h"
#include "../lib/public/string_intern.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/string_intern.h"


#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

KHASH_MAP_INIT_STR(int, int)

enum resource_state{
    STATE_NORMAL,
    STATE_REPLENISHING,
};

struct rstate{
    const char *name;
    const char *cursor;
    int         amount;
    int         restored_amount;
    vec2_t      blocking_pos;
    float       blocking_radius;
    bool        replenishable;
    kh_int_t   *replenish_resources;
    bool        is_storage_site;
    bool        ss_do_not_take_land;
    bool        ss_do_not_take_water;
    enum resource_state state;
};

KHASH_MAP_INIT_INT(state, struct rstate)
KHASH_MAP_INIT_STR(icon, const char*)
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
static khash_t(icon)    *s_icon_table;

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
    if(!(s_icon_table = kh_init(icon)))
        goto fail_icon_table;

    s_map = map;
    return true;

fail_icon_table:
    kh_destroy(name, s_all_names);
fail_name_set:
    si_shutdown(&s_stringpool, s_stridx);
fail_strintern:
    kh_destroy(state, s_entity_state_table);
fail_table:
    return false;
}

void G_Resource_Shutdown(void)
{
    kh_destroy(icon, s_icon_table);
    kh_destroy(name, s_all_names);
    si_shutdown(&s_stringpool, s_stridx);
    kh_destroy(state, s_entity_state_table);
}

bool G_Resource_AddEntity(uint32_t uid)
{
    struct rstate rs = (struct rstate){
        .name = "",
        .cursor = "",
        .amount = 0,
        .restored_amount = 0,
        .blocking_pos = G_Pos_GetXZ(uid),
        .blocking_radius = G_GetSelectionRadius(uid),
        .replenishable = false,
        .replenish_resources = kh_init(int),
        .is_storage_site = false,
        .ss_do_not_take_land = false,
        .ss_do_not_take_water = false,
        .state = STATE_NORMAL
    };

    if(!rstate_set(uid, rs))
        return false;

    uint32_t flags = G_FlagsGet(uid);
    if(!(flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersIncref(rs.blocking_pos, rs.blocking_radius, G_GetFactionID(uid), flags, s_map);
    }
    return true;
}

void G_Resource_RemoveEntity(uint32_t uid)
{
    struct rstate *rs = rstate_get(uid);
    if(!rs)
        return;

    uint32_t flags = G_FlagsGet(uid);
    if(!(flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersDecref(rs->blocking_pos, rs->blocking_radius, G_GetFactionID(uid), 
            flags, s_map);
    }

    kh_destroy(int, rs->replenish_resources);
    rstate_remove(uid);
}

void G_Resource_UpdateBounds(uint32_t uid)
{
    struct rstate *rs = rstate_get(uid);
    if(!rs)
        return;

    uint32_t flags = G_FlagsGet(uid);
    if(!(flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersDecref(rs->blocking_pos, rs->blocking_radius, G_GetFactionID(uid), 
            flags, s_map);
        rs->blocking_pos = G_Pos_GetXZ(uid);
        M_NavBlockersIncref(rs->blocking_pos, rs->blocking_radius, G_GetFactionID(uid), 
            flags, s_map);
    }
}

void G_Resource_UpdateFactionID(uint32_t uid, int oldfac, int newfac)
{
    struct rstate *rs = rstate_get(uid);
    if(!rs)
        return;

    uint32_t flags = G_FlagsGet(uid);
    if(!(flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersDecref(rs->blocking_pos, rs->blocking_radius, oldfac, flags, s_map);
        M_NavBlockersIncref(rs->blocking_pos, rs->blocking_radius, newfac, flags, s_map);
    }
}

void G_Resource_UpdateSelectionRadius(uint32_t uid, float radius)
{
    struct rstate *rs = rstate_get(uid);
    if(!rs)
        return;

    uint32_t flags = G_FlagsGet(uid);
    if(!(flags & ENTITY_FLAG_BUILDING)) {
        M_NavBlockersDecref(rs->blocking_pos, rs->blocking_radius, G_GetFactionID(uid), 
            flags, s_map);
        rs->blocking_radius = radius;
        M_NavBlockersIncref(rs->blocking_pos, rs->blocking_radius, G_GetFactionID(uid), 
            flags, s_map);
    }
}

bool G_Resource_GetReplenishable(uint32_t uid)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);
    return rs->replenishable;
}

void G_Resource_SetReplenishable(uint32_t uid, bool set)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);
    rs->replenishable = set;
}

bool G_Resource_SetReplenishAmount(uint32_t uid, const char *rname, int amount)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);

    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return false;

    int status;
    khiter_t k = kh_put(int, rs->replenish_resources, key, &status);
    if(status == -1)
        return false;

    kh_val(rs->replenish_resources, k) = amount;
    return true;
}

int G_Resource_GetReplenishAmount(uint32_t uid, const char *rname)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);

    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return 0;

    khiter_t k = kh_get(int, rs->replenish_resources, key);
    if(k == kh_end(rs->replenish_resources))
        return 0;

    return kh_val(rs->replenish_resources, k);
}

void G_Resource_SetReplenishing(uint32_t uid)
{
    uint32_t flags = G_FlagsGet(uid);

    struct rstate *rs = rstate_get(uid);
    assert(rs);
    rs->state = STATE_REPLENISHING;
    rs->is_storage_site = !!(flags & ENTITY_FLAG_STORAGE_SITE);
    if(rs->is_storage_site) {
        rs->ss_do_not_take_land = G_StorageSite_GetDoNotTakeLand(uid);
        rs->ss_do_not_take_water = G_StorageSite_GetDoNotTakeWater(uid);
    }

    if(!rs->is_storage_site) {

        flags |= ENTITY_FLAG_STORAGE_SITE;
        G_StorageSite_AddEntity(uid);
        G_FlagsSet(uid, flags);
        G_StorageSite_SetDoNotTakeLand(uid, true);
        G_StorageSite_SetDoNotTakeWater(uid, true);

        const char *rname;
        int amount;
        kh_foreach(rs->replenish_resources, rname, amount, {
            G_StorageSite_SetCapacity(uid, rname, amount);
            G_StorageSite_SetDesired(uid, rname, amount);
        });
    }else{
        G_StorageSite_SetUseAlt(uid, true);
        G_StorageSite_SetDoNotTakeLand(uid, true);
        G_StorageSite_SetDoNotTakeWater(uid, true);

        const char *rname;
        int amount;
        kh_foreach(rs->replenish_resources, rname, amount, {
            G_StorageSite_SetAltCapacity(uid, rname, amount);
            G_StorageSite_SetAltDesired(uid, rname, amount);
        });
    }
}

void G_Resource_SetReplenished(uint32_t uid)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);
    rs->state = STATE_NORMAL;

    uint32_t flags = G_FlagsGet(uid);

    if(rs->is_storage_site) {
        G_StorageSite_ClearAlt(uid);
        G_StorageSite_SetUseAlt(uid, false);
        G_StorageSite_SetDoNotTakeLand(uid, rs->ss_do_not_take_land);
        G_StorageSite_SetDoNotTakeWater(uid, rs->ss_do_not_take_water);
    }else{
        G_StorageSite_RemoveEntity(uid);
        flags &= ~ENTITY_FLAG_STORAGE_SITE;
        G_FlagsSet(uid, flags);
    }
    G_Resource_SetAmount(uid, rs->restored_amount);
}

bool G_Resource_IsReplenishing(uint32_t uid)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);
    return (rs->state == STATE_REPLENISHING);
}

int G_Resource_GetRestoredAmount(uint32_t uid)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);
    return rs->restored_amount;
}

void G_Resource_SetRestoredAmount(uint32_t uid, int amount)
{
    struct rstate *rs = rstate_get(uid);
    assert(rs);
    rs->restored_amount = amount;
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

void G_Resource_SetIcon(const char *name, const char *path)
{
    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return;

    const char *value = si_intern(path, &s_stringpool, s_stridx);
    if(!path)
        return;

    khiter_t k = kh_put(icon, s_icon_table, key, &(int){0});
    kh_val(s_icon_table, k) = value;
}

const char *G_Resource_GetIcon(const char *name)
{
    khiter_t k = kh_get(icon, s_icon_table, name);
    if(k == kh_end(s_icon_table))
        return NULL;
    return kh_val(s_icon_table, k);
}

int G_Resource_GetAllNames(size_t maxout, const char *out[])
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
    Sched_TryYield();

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

        struct attr restored_amount = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.restored_amount
        };
        CHK_TRUE_RET(Attr_Write(stream, &restored_amount, "restored_amount"));

        struct attr replenishable = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.replenishable
        };
        CHK_TRUE_RET(Attr_Write(stream, &replenishable, "replenishable"));

        struct attr num_replenish_resources = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr.replenish_resources)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_replenish_resources, "num_replenish_resources"));

        const char *key;
        int value;
        kh_foreach(curr.replenish_resources, key, value, {

            struct attr resource_name = (struct attr){
                .type = TYPE_STRING,
            };
            pf_strlcpy(resource_name.val.as_string, key, sizeof(resource_name.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &resource_name, "resource_name"));

            struct attr resource_amount = (struct attr){
                .type = TYPE_INT,
                .val.as_int = value
            };
            CHK_TRUE_RET(Attr_Write(stream, &resource_amount, "resource_amount"));
        });

        struct attr is_storage_site = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.is_storage_site
        };
        CHK_TRUE_RET(Attr_Write(stream, &is_storage_site, "is_storage_site"));

        struct attr ss_do_not_take_land = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.ss_do_not_take_land
        };
        CHK_TRUE_RET(Attr_Write(stream, &ss_do_not_take_land, "ss_do_not_take_land"));

        struct attr ss_do_not_take_water = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.ss_do_not_take_water
        };
        CHK_TRUE_RET(Attr_Write(stream, &ss_do_not_take_water, "ss_do_not_take_water"));

        struct attr resource_state = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.state
        };
        CHK_TRUE_RET(Attr_Write(stream, &resource_state, "resource_state"));

        Sched_TryYield();
    });

    struct attr num_names = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_all_names)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_names, "num_names"));
    Sched_TryYield();

    for(khiter_t k = kh_begin(s_all_names); k != kh_end(s_all_names); k++) {

        if(!kh_exist(s_all_names, k))
            continue;

        const char *key = kh_key(s_all_names, k);
        struct attr name = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(name.val.as_string, key, sizeof(name.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &name, "name"));
        Sched_TryYield();
    }

    struct attr nicons = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_icon_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &nicons, "nicons"));
    Sched_TryYield();

    for(khiter_t k = kh_begin(s_icon_table); k != kh_end(s_icon_table); k++) {

        if(!kh_exist(s_icon_table, k))
            continue;

        const char *key = kh_key(s_icon_table, k);
        struct attr name = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(name.val.as_string, key, sizeof(name.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &name, "name"));

        const char *val = kh_val(s_icon_table, k);
        struct attr path = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(path.val.as_string, val, sizeof(path.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &path, "icon"));

        Sched_TryYield();
    }

    return true;
}

bool G_Resource_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_ents = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_ents; i++) {

        uint32_t uid;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uid = attr.val.as_int;

        struct rstate *rstate = rstate_get(uid);
        CHK_TRUE_RET(rstate);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        G_Resource_SetName(uid, attr.val.as_string);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        G_Resource_SetCursor(uid, attr.val.as_string);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        G_Resource_SetAmount(uid, attr.val.as_int);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        G_Resource_SetRestoredAmount(uid, attr.val.as_int);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        G_Resource_SetReplenishable(uid, attr.val.as_bool);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const size_t num_replenish_resources = attr.val.as_int;

        for(int i = 0; i < num_replenish_resources; i++) {

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_STRING);
            const char *key = attr.val.as_string;

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            int amount = attr.val.as_int;

            G_Resource_SetReplenishAmount(uid, key, amount);
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        rstate->is_storage_site = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        rstate->ss_do_not_take_land = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        rstate->ss_do_not_take_water = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        rstate->state = attr.val.as_int;

        Sched_TryYield();
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_names = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_names; i++) {
    
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);

        const char *key = si_intern(attr.val.as_string, &s_stringpool, s_stridx);
        kh_put(name, s_all_names, key, &(int){0});
        Sched_TryYield();
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_icons = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_icons; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        const char *key = si_intern(attr.val.as_string, &s_stringpool, s_stridx);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        const char *val = si_intern(attr.val.as_string, &s_stringpool, s_stridx);

        G_Resource_SetIcon(key, val);
    }

    return true;
}

