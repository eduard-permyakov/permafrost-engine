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
#define MEM_FILE_SUB MEM_SUB_GAME_COMMAND

#include "command.h"
#include "builder.h"
#include "building.h"
#include "harvester.h"
#include "game_private.h"
#include "public/game.h"
#include "../sched/public/sched.h"
#include "../lib/public/khash.h"
#include "../lib/public/vec.h"
#include "../lib/public/attr.h"

#include <assert.h>
#include <string.h>

#include "../perf/public/mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_COMMAND)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_COMMAND)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_COMMAND)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

VEC_TYPE(cmd, struct command)
VEC_IMPL(static inline, cmd, struct command)

KHASH_MAP_INIT_INT(queue, vec_cmd_t)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(queue) *s_queues;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static vec_cmd_t *queue_get(uint32_t uid)
{
    khiter_t k = kh_get(queue, s_queues, uid);
    if(k == kh_end(s_queues))
        return NULL;
    return &kh_value(s_queues, k);
}

static vec_cmd_t *queue_get_or_create(uint32_t uid)
{
    vec_cmd_t *ret = queue_get(uid);
    if(ret)
        return ret;

    int status;
    khiter_t k = kh_put(queue, s_queues, uid, &status);
    if(status == -1)
        return NULL;

    ret = &kh_value(s_queues, k);
    vec_cmd_init(ret);
    return ret;
}

static void queue_remove(uint32_t uid)
{
    khiter_t k = kh_get(queue, s_queues, uid);
    if(k == kh_end(s_queues))
        return;
    vec_cmd_destroy(&kh_value(s_queues, k));
    kh_del(queue, s_queues, k);
}

static void queue_erase(vec_cmd_t *queue, int idx)
{
    assert(idx >= 0 && idx < vec_size(queue));
    memmove(&vec_AT(queue, idx), &vec_AT(queue, idx + 1),
        (vec_size(queue) - idx - 1) * sizeof(struct command));
    queue->size--;
}

static bool command_valid(uint32_t uid, const struct command *cmd)
{
    switch(cmd->kind) {
    case COMMAND_BUILD:
        if(!(G_FlagsGet(uid) & ENTITY_FLAG_BUILDER))
            return false;
        if(!G_EntityExists(cmd->target) || G_EntityIsZombie(cmd->target))
            return false;
        if(!(G_FlagsGet(cmd->target) & ENTITY_FLAG_BUILDING))
            return false;
        return G_Building_IsMarked(cmd->target)
            && (!G_Building_IsCompleted(cmd->target) || G_Building_NeedsRepair(cmd->target));
    default:
        return false;
    }
}

static bool command_start(uint32_t uid, const struct command *cmd)
{
    switch(cmd->kind) {
    case COMMAND_BUILD:
        return G_Builder_Order(uid, cmd->target);
    default:
        return false;
    }
}

/* Sites not yet founded come first, so that the ground for all of them is
 * laid before any of them is raised */
static int next_index(uint32_t uid, const vec_cmd_t *queue)
{
    for(int i = 0; i < vec_size(queue); i++) {
        const struct command *curr = &vec_AT(queue, i);
        if(curr->kind != COMMAND_BUILD || !command_valid(uid, curr))
            continue;
        if(!G_Building_IsFounded(curr->target))
            return i;
    }
    return 0;
}

static bool unit_working(uint32_t uid)
{
    uint32_t flags = G_FlagsGet(uid);
    if((flags & ENTITY_FLAG_BUILDER) && G_Builder_Target(uid) != NULL_UID)
        return true;
    if(!(flags & ENTITY_FLAG_HARVESTER))
        return false;

    /* A builder brings in the materials of its site before it raises it */
    uint32_t dest = G_Harvester_TransportTarget(uid);
    return (dest != NULL_UID)
        && G_EntityExists(dest)
        && !G_EntityIsZombie(dest)
        && (G_FlagsGet(dest) & ENTITY_FLAG_BUILDING)
        && !G_Building_IsSupplied(dest);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Command_Init(void)
{
    if(NULL == (s_queues = kh_init(queue)))
        return false;
    return true;
}

void G_Command_Shutdown(void)
{
    vec_cmd_t curr;
    kh_foreach_value(s_queues, curr, {
        vec_cmd_destroy(&curr);
    });
    kh_destroy(queue, s_queues);
    s_queues = NULL;
}

void G_Command_RemoveEntity(uint32_t uid)
{
    queue_remove(uid);
}

bool G_Command_Push(uint32_t uid, struct command cmd)
{
    if(!G_EntityExists(uid) || G_EntityIsZombie(uid))
        return false;
    if(!command_valid(uid, &cmd))
        return false;

    vec_cmd_t *queue = queue_get_or_create(uid);
    if(!queue || !vec_cmd_push(queue, cmd))
        return false;

    if(!unit_working(uid)) {
        G_Command_Next(uid);
    }
    return true;
}

bool G_Command_PushFront(uint32_t uid, struct command cmd)
{
    if(!G_EntityExists(uid) || G_EntityIsZombie(uid))
        return false;
    if(!command_valid(uid, &cmd))
        return false;

    vec_cmd_t *queue = queue_get_or_create(uid);
    if(!queue || !vec_cmd_push(queue, cmd))
        return false;

    memmove(&vec_AT(queue, 1), &vec_AT(queue, 0), (vec_size(queue) - 1) * sizeof(struct command));
    vec_AT(queue, 0) = cmd;
    return true;
}

void G_Command_Clear(uint32_t uid)
{
    queue_remove(uid);
}

bool G_Command_Next(uint32_t uid)
{
    /* Starting a command may change any unit's queue, so it is looked up anew */
    vec_cmd_t *queue;
    while((queue = queue_get(uid)) && vec_size(queue) > 0) {

        int idx = next_index(uid, queue);
        struct command cmd = vec_AT(queue, idx);
        queue_erase(queue, idx);

        if(command_valid(uid, &cmd) && command_start(uid, &cmd))
            return true;
    }
    queue_remove(uid);
    return false;
}

const struct command *G_Command_Get(uint32_t uid, size_t *out_count)
{
    vec_cmd_t *queue = queue_get(uid);
    if(!queue || vec_size(queue) == 0) {
        *out_count = 0;
        return NULL;
    }
    *out_count = vec_size(queue);
    return &vec_AT(queue, 0);
}

bool G_Command_SaveState(struct SDL_RWops *stream)
{
    struct attr num_queues = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_queues)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_queues, "num_queues"));
    Sched_TryYield();

    uint32_t uid;
    vec_cmd_t curr;

    kh_foreach(s_queues, uid, curr, {

        struct attr quid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &quid, "queue_uid"));

        struct attr num_commands = (struct attr){
            .type = TYPE_INT,
            .val.as_int = vec_size(&curr)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_commands, "num_commands"));

        for(int i = 0; i < vec_size(&curr); i++) {

            struct attr kind = (struct attr){
                .type = TYPE_INT,
                .val.as_int = vec_AT(&curr, i).kind
            };
            CHK_TRUE_RET(Attr_Write(stream, &kind, "command_kind"));

            struct attr target = (struct attr){
                .type = TYPE_INT,
                .val.as_int = vec_AT(&curr, i).target
            };
            CHK_TRUE_RET(Attr_Write(stream, &target, "command_target"));
        }
        Sched_TryYield();
    });

    return true;
}

bool G_Command_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_queues = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_queues; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const int num_commands = attr.val.as_int;

        vec_cmd_t *queue = queue_get_or_create(uid);
        CHK_TRUE_RET(queue);

        for(int j = 0; j < num_commands; j++) {

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            int kind = attr.val.as_int;

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            uint32_t target = attr.val.as_int;

            CHK_TRUE_RET(kind == COMMAND_BUILD);
            CHK_TRUE_RET(vec_cmd_push(queue, (struct command){kind, target}));
        }
        Sched_TryYield();
    }
    return true;
}
