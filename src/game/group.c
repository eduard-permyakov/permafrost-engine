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
#define MEM_FILE_SUB MEM_SUB_GAME_GROUP

#include "group.h"
#include "formation.h"
#include "public/game.h"
#include "../main.h"
#include "../sched.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"

#include <assert.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_GROUP)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_GROUP)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_GROUP)

#define MIN(a, b)           ((a) < (b) ? (a) : (b))

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

KHASH_MAP_INIT_INT(gid, int)
KHASH_MAP_INIT_INT(members, vec_entity_t)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(gid)     *s_ent_group_map;
static khash_t(members) *s_groups;
static int               s_next_group_id;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool entities_equal(uint32_t *a, uint32_t *b)
{
    return ((*a) == (*b));
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Group_Init(void)
{
    if((s_ent_group_map = kh_init(gid)) == NULL)
        goto fail_ent_group_map;
    if((s_groups = kh_init(members)) == NULL)
        goto fail_groups;
    s_next_group_id = 1;
    return true;

fail_groups:
    kh_destroy(gid, s_ent_group_map);
    s_ent_group_map = NULL;
fail_ent_group_map:
    return false;
}

void G_Group_Shutdown(void)
{
    if(s_groups) {
        vec_entity_t curr;
        kh_foreach_value(s_groups, curr, {
            vec_entity_destroy(&curr);
        });
        kh_destroy(members, s_groups);
        s_groups = NULL;
    }
    if(s_ent_group_map) {
        kh_destroy(gid, s_ent_group_map);
        s_ent_group_map = NULL;
    }
}

int G_Group_Lock(const uint32_t *uids, size_t nuids)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_groups)
        return 0;
    nuids = MIN(nuids, MAX_FORMATION_UNITS);

    vec_entity_t members;
    vec_entity_init(&members);

    for(size_t i = 0; i < nuids; i++) {
        uint32_t uid = uids[i];
        if(!G_EntityExists(uid))
            continue;
        if(vec_entity_indexof(&members, uid, entities_equal) != -1)
            continue;
        vec_entity_push(&members, uid);
    }

    if(vec_size(&members) == 0) {
        vec_entity_destroy(&members);
        return 0;
    }

    int gid = s_next_group_id++;
    for(int i = 0; i < vec_size(&members); i++) {

        uint32_t uid = vec_AT(&members, i);
        G_Group_RemoveEntity(uid);

        int ret;
        khiter_t k = kh_put(gid, s_ent_group_map, uid, &ret);
        assert(ret != -1);
        kh_value(s_ent_group_map, k) = gid;
    }

    int ret;
    khiter_t k = kh_put(members, s_groups, gid, &ret);
    assert(ret != -1);
    kh_value(s_groups, k) = members;
    return gid;
}

void G_Group_Unlock(int group_id)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_groups)
        return;
    khiter_t k = kh_get(members, s_groups, group_id);
    if(k == kh_end(s_groups))
        return;

    vec_entity_t *members = &kh_value(s_groups, k);
    for(int i = 0; i < vec_size(members); i++) {
        khiter_t m = kh_get(gid, s_ent_group_map, vec_AT(members, i));
        assert(m != kh_end(s_ent_group_map));
        kh_del(gid, s_ent_group_map, m);
    }
    vec_entity_destroy(members);
    kh_del(members, s_groups, k);
}

int G_Group_ForEnt(uint32_t uid)
{
    if(!s_ent_group_map)
        return 0;
    khiter_t k = kh_get(gid, s_ent_group_map, uid);
    if(k == kh_end(s_ent_group_map))
        return 0;
    return kh_value(s_ent_group_map, k);
}

int G_Group_ForSet(const uint32_t *uids, size_t nuids)
{
    if(nuids == 0)
        return 0;

    int gid = G_Group_ForEnt(uids[0]);
    if(gid == 0)
        return 0;

    for(size_t i = 1; i < nuids; i++) {
        if(G_Group_ForEnt(uids[i]) != gid)
            return 0;
    }
    return gid;
}

const vec_entity_t *G_Group_Members(int group_id)
{
    if(!s_groups)
        return NULL;
    khiter_t k = kh_get(members, s_groups, group_id);
    if(k == kh_end(s_groups))
        return NULL;
    return &kh_value(s_groups, k);
}

void G_Group_RemoveEntity(uint32_t uid)
{
    if(!s_ent_group_map)
        return;
    khiter_t k = kh_get(gid, s_ent_group_map, uid);
    if(k == kh_end(s_ent_group_map))
        return;

    int gid = kh_value(s_ent_group_map, k);
    kh_del(gid, s_ent_group_map, k);

    khiter_t m = kh_get(members, s_groups, gid);
    assert(m != kh_end(s_groups));

    vec_entity_t *members = &kh_value(s_groups, m);
    int idx = vec_entity_indexof(members, uid, entities_equal);
    assert(idx != -1);
    vec_entity_del(members, idx);

    if(vec_size(members) == 0) {
        vec_entity_destroy(members);
        kh_del(members, s_groups, m);
    }
}

bool G_Group_SaveState(struct SDL_RWops *stream)
{
    struct attr next_group_id = (struct attr){
        .type = TYPE_INT,
        .val.as_int = s_next_group_id
    };
    CHK_TRUE_RET(Attr_Write(stream, &next_group_id, "next_group_id"));

    struct attr num_groups = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_groups)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_groups, "num_groups"));
    Sched_TryYield();

    int gid;
    vec_entity_t curr;
    kh_foreach(s_groups, gid, curr, {

        struct attr group_id = (struct attr){
            .type = TYPE_INT,
            .val.as_int = gid
        };
        CHK_TRUE_RET(Attr_Write(stream, &group_id, "group_id"));

        struct attr num_members = (struct attr){
            .type = TYPE_INT,
            .val.as_int = vec_size(&curr)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_members, "num_members"));

        for(int i = 0; i < vec_size(&curr); i++) {
            struct attr member = (struct attr){
                .type = TYPE_INT,
                .val.as_int = vec_AT(&curr, i)
            };
            CHK_TRUE_RET(Attr_Write(stream, &member, "member"));
            Sched_TryYield();
        }
    });
    return true;
}

bool G_Group_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    s_next_group_id = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_groups = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_groups; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const int gid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const size_t num_members = attr.val.as_int;

        vec_entity_t members;
        vec_entity_init(&members);

        for(int j = 0; j < num_members; j++) {

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            CHK_TRUE_RET(G_EntityExists(attr.val.as_int));

            uint32_t uid = attr.val.as_int;
            vec_entity_push(&members, uid);

            int ret;
            khiter_t k = kh_put(gid, s_ent_group_map, uid, &ret);
            CHK_TRUE_RET(ret != -1);
            kh_value(s_ent_group_map, k) = gid;
            Sched_TryYield();
        }

        int ret;
        khiter_t k = kh_put(members, s_groups, gid, &ret);
        CHK_TRUE_RET(ret != -1);
        kh_value(s_groups, k) = members;
    }
    return true;
}

