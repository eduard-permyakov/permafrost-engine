/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
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

#define MEM_FILE_SYS MEM_SYS_GAME
#define MEM_FILE_SUB MEM_SUB_GAME_POPULATION

#include "population.h"
#include "game_private.h"
#include "../entity.h"
#include "../event.h"
#include "../main.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"

#include <assert.h>
#include <string.h>

#include "../lib/public/mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_POPULATION)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_POPULATION)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_POPULATION)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

struct limit_state{
    int  amount;
    bool active;
};

KHASH_MAP_INIT_INT(limit, struct limit_state)
KHASH_SET_INIT_INT(pop)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static int             s_pop_counts[MAX_FACTIONS];
static int             s_pop_limits[MAX_FACTIONS];

/* The set of UIDs currently contributing to a faction's population count.
 * Tracked explicitly so we can be idempotent in the face of duplicate Add/Remove
 * calls (e.g. when zombification fires after removal). 
 */
static khash_t(pop)   *s_contributors;
/* UID -> per-entity limit amount and active flag. */
static khash_t(limit) *s_limit_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct limit_state *limit_get(uint32_t uid)
{
    khiter_t k = kh_get(limit, s_limit_table, uid);
    if(k == kh_end(s_limit_table))
        return NULL;
    return &kh_value(s_limit_table, k);
}

static void on_building_constructed(void *user, void *event)
{
    uint32_t uid = (uintptr_t)event;
    struct limit_state *ls = limit_get(uid);
    if(!ls || ls->active)
        return;

    ls->active = true;
    int fac = G_GetFactionID(uid);
    s_pop_limits[fac] += ls->amount;
}

static bool diplomacy_is(int fac_a, int fac_b, enum diplomacy_state want)
{
    enum diplomacy_state ds;
    if(!G_GetDiplomacyState(fac_a, fac_b, &ds))
        return false;
    return ds == want;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Population_Init(void)
{
    memset(s_pop_counts, 0, sizeof(s_pop_counts));
    memset(s_pop_limits, 0, sizeof(s_pop_limits));

    if((s_contributors = kh_init(pop)) == NULL)
        goto fail_contributors;
    if((s_limit_table = kh_init(limit)) == NULL)
        goto fail_limit;

    E_Global_Register(EVENT_BUILDING_CONSTRUCTED, on_building_constructed, NULL,
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);

    return true;

fail_limit:
    kh_destroy(pop, s_contributors);
    s_contributors = NULL;
fail_contributors:
    return false;
}

void G_Population_Shutdown(void)
{
    E_Global_Unregister(EVENT_BUILDING_CONSTRUCTED, on_building_constructed);

    if(s_limit_table) {
        kh_destroy(limit, s_limit_table);
        s_limit_table = NULL;
    }
    if(s_contributors) {
        kh_destroy(pop, s_contributors);
        s_contributors = NULL;
    }
}

void G_Population_ClearState(void)
{
    G_Population_Shutdown();
    G_Population_Init();
}

void G_Population_AddContributor(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    int ret;
    kh_put(pop, s_contributors, uid, &ret);
    if(ret == 0)
        return; /* already a contributor */

    int fac = G_GetFactionID(uid);
    s_pop_counts[fac]++;
}

void G_Population_RemoveContributor(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(pop, s_contributors, uid);
    if(k == kh_end(s_contributors))
        return;
    kh_del(pop, s_contributors, k);

    int fac = G_GetFactionID(uid);
    s_pop_counts[fac]--;
}

void G_Population_AddLimitContributor(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    if(limit_get(uid))
        return;

    int ret;
    khiter_t k = kh_put(limit, s_limit_table, uid, &ret);
    assert(ret != -1);
    kh_value(s_limit_table, k) = (struct limit_state){.amount = 0, .active = false};
}

void G_Population_RemoveLimitContributor(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(limit, s_limit_table, uid);
    if(k == kh_end(s_limit_table))
        return;

    struct limit_state ls = kh_value(s_limit_table, k);
    if(ls.active) {
        int fac = G_GetFactionID(uid);
        s_pop_limits[fac] -= ls.amount;
    }
    kh_del(limit, s_limit_table, k);
}

void G_Population_UpdateFaction(uint32_t uid, int oldfac, int newfac)
{
    ASSERT_IN_MAIN_THREAD();

    if(oldfac == newfac)
        return;

    khiter_t k = kh_get(pop, s_contributors, uid);
    if(k != kh_end(s_contributors)) {
        s_pop_counts[oldfac]--;
        s_pop_counts[newfac]++;
    }

    struct limit_state *ls = limit_get(uid);
    if(ls && ls->active) {
        s_pop_limits[oldfac] -= ls->amount;
        s_pop_limits[newfac] += ls->amount;
    }
}

void G_Population_SetEntityLimit(uint32_t uid, int amount)
{
    ASSERT_IN_MAIN_THREAD();

    struct limit_state *ls = limit_get(uid);
    if(!ls)
        return;

    if(ls->active) {
        int fac = G_GetFactionID(uid);
        s_pop_limits[fac] += (amount - ls->amount);
    }
    ls->amount = amount;
}

int G_Population_GetEntityLimit(uint32_t uid)
{
    struct limit_state *ls = limit_get(uid);
    return ls ? ls->amount : 0;
}

int G_Population_Get(int faction_id)
{
    if(faction_id < 0 || faction_id >= MAX_FACTIONS)
        return 0;
    return s_pop_counts[faction_id];
}

int G_Population_GetLimit(int faction_id)
{
    if(faction_id < 0 || faction_id >= MAX_FACTIONS)
        return 0;
    return s_pop_limits[faction_id];
}

int G_Population_GetAllied(int faction_id)
{
    int ret = 0;
    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(i == faction_id)
            continue;
        if(!diplomacy_is(i, faction_id, DIPLOMACY_STATE_PEACE))
            continue;
        ret += s_pop_counts[i];
    }
    return ret;
}

int G_Population_GetEnemy(int faction_id)
{
    int ret = 0;
    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(i == faction_id)
            continue;
        if(!diplomacy_is(i, faction_id, DIPLOMACY_STATE_WAR))
            continue;
        ret += s_pop_counts[i];
    }
    return ret;
}

int G_Population_GetPlayer(void)
{
    uint16_t mask = G_GetPlayerControlledFactions();
    int ret = 0;
    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(!(mask & (0x1 << i)))
            continue;
        ret += s_pop_counts[i];
    }
    return ret;
}

int G_Population_GetPlayerLimit(void)
{
    uint16_t mask = G_GetPlayerControlledFactions();
    int ret = 0;
    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(!(mask & (0x1 << i)))
            continue;
        ret += s_pop_limits[i];
    }
    return ret;
}

bool G_Population_SaveState(struct SDL_RWops *stream)
{
    struct attr attr;

    attr = (struct attr){.type = TYPE_INT, .val.as_int = kh_size(s_contributors)};
    CHK_TRUE_RET(Attr_Write(stream, &attr, "num_contributors"));

    uint32_t uid;
    kh_foreach_key(s_contributors, uid, {
        attr = (struct attr){.type = TYPE_INT, .val.as_int = uid};
        CHK_TRUE_RET(Attr_Write(stream, &attr, "contributor_uid"));
    });

    attr = (struct attr){.type = TYPE_INT, .val.as_int = kh_size(s_limit_table)};
    CHK_TRUE_RET(Attr_Write(stream, &attr, "num_limit_contributors"));

    struct limit_state ls;
    kh_foreach(s_limit_table, uid, ls, {
        attr = (struct attr){.type = TYPE_INT, .val.as_int = uid};
        CHK_TRUE_RET(Attr_Write(stream, &attr, "limit_uid"));
        attr = (struct attr){.type = TYPE_INT, .val.as_int = ls.amount};
        CHK_TRUE_RET(Attr_Write(stream, &attr, "limit_amount"));
        attr = (struct attr){.type = TYPE_BOOL, .val.as_bool = ls.active};
        CHK_TRUE_RET(Attr_Write(stream, &attr, "limit_active"));
    });

    return true;
}

bool G_Population_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    int ncontribs = attr.val.as_int;

    for(int i = 0; i < ncontribs; i++) {
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        G_Population_AddContributor((uint32_t)attr.val.as_int);
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    int nlimits = attr.val.as_int;

    for(int i = 0; i < nlimits; i++) {
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t uid = (uint32_t)attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int amount = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        bool active = attr.val.as_bool;

        G_Population_AddLimitContributor(uid);
        G_Population_SetEntityLimit(uid, amount);
        if(active) {
            struct limit_state *ls = limit_get(uid);
            if(ls && !ls->active) {
                ls->active = true;
                s_pop_limits[G_GetFactionID(uid)] += amount;
            }
        }
    }

    return true;
}

