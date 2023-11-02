/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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

#include "combat.h"
#include "game_private.h"
#include "movement.h"
#include "building.h"
#include "fog_of_war.h"
#include "position.h"
#include "public/game.h"
#include "../ui.h"
#include "../event.h"
#include "../entity.h"
#include "../main.h"
#include "../perf.h"
#include "../settings.h"
#include "../sched.h"
#include "../task.h"
#include "../asset_load.h"
#include "../phys/public/collision.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../phys/public/phys.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/mem.h"
#include "../lib/public/queue.h"

#include <assert.h>
#include <float.h>
#include <SDL.h>


#define TARGET_ACQUISITION_RANGE    (50.0f)
#define PROJECTILE_DEFAULT_SPEED    (100.0f)
#define EPSILON                     (1.0f/1024)
#define DEFAULT_ATTACK_PERIOD       (4.0f/3.0f)
#define MAX(a, b)                   ((a) > (b) ? (a) : (b))
#define MIN(a, b)                   ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)                 (sizeof(a)/sizeof(a[0]))
#define X_BINS_PER_CHUNK            (8)
#define Z_BINS_PER_CHUNK            (8)
#define MAX_COMBAT_TASKS            (64)

#define CHK_TRUE_RET(_pred)         \
    do{                             \
        if(!(_pred))                \
            return false;           \
    }while(0)

KHASH_MAP_INIT_INT(aabb, struct aabb)

/*
 *                    Start
 *                      |
 *                      V
 *(enter atk range)+--[STATE_NOT_IN_COMBAT]<---------------------------+
 **attack begins*  |    |(enter acquire but not attack range)          |
 *                 |    V                                              |
 *                 |  [STATE_MOVING_TO_TARGET]<-+                      |
 *                 |    |(enter attack range)   |(leave attack range)  |(target dies)
 *                 |    |*attack begins*        |*attack ends*         |*attack ends*
 *                 |    |              +--------+----------------------+
 *                 |    V              |
 *                 +->[STATE_TURNING_TO_TARGET]<--+<---------------------+
 *                      |(target alive)           |                      |
 *                      |                         |                      |
 *                      V                         |                      |
 *                    [STATE_CAN_ATTACK]          |                      |
 *                      |                         |                      |
 *                      |(facing target)          |                      |
 *                      |                         |                      |
 *                 +----+                         |                      |
 *                 |    |                         |                      |
 *                 |    |(animated)               |                      |
 *                 |    V                         |(anim cycle finishes) |(attack time elapsed)
 *   (not animated)|  [STATE_ATTACK_ANIM_PLAYING]-+                      |
 *                 |                                                     |
 *                 +->[STATE_ATTACKING]----------------------------------+
 * 
 * From any of the states, an entity can move to the [STATE_DEATH_ANIM_PLAYING] 
 * state upon receiving a fatal hit. At the next EVENT_ANIM_CYCLE_FINISHED
 * event, the entity is reaped.
 */

struct combatstats{
    int   max_hp;
    int   base_dmg;         /* The base damage per hit */
    float base_armour_pc;   /* Percentage of damage blocked. Valid range: [0.0 - 1.0] */
    float attack_range;
};

struct combatstate{
    struct combatstats stats;
    int                current_hp;
    enum combat_stance stance;
    enum{
        STATE_NOT_IN_COMBAT,
        STATE_MOVING_TO_TARGET,
        STATE_MOVING_TO_TARGET_LOCKED,
        STATE_CAN_ATTACK,
        STATE_ATTACK_ANIM_PLAYING,
        STATE_DEATH_ANIM_PLAYING,
        STATE_ATTACKING,
        STATE_TURNING_TO_TARGET,
    }state;
    bool               sticky;
    uint32_t           target_uid;
    /* If the target gained a target while moving, save and restore
     * its' intial move command once it finishes combat. */
    bool               move_cmd_interrupted;
    bool               move_cmd_attacking;
    vec2_t             move_cmd_xz;
    uint32_t           attack_start_tick;
    /* only used by ranged entities */
    struct proj_desc   pd;
};

struct combat_work_in{
    uint32_t ent_uid;
};

enum combat_action{
    COMBAT_ACTION_NONE,
    COMBAT_ACTION_TARGET_ENEMY,
    COMBAT_ACTION_STOP_COMBAT,
    COMBAT_ACTION_TURN_TO_TARGET,
    COMBAT_ACTION_STOP_MOVEMENT,
    COMBAT_ACTION_MOVE_IN_RANGE,
    COMBAT_ACTION_MOVE_IN_RANGE_IF_STILL,
    COMBAT_ACTION_ANIMATED_ATTACK,
    COMBAT_ACTION_ATTACK_IF_STILL,
    COMBAT_ACTION_TRYHIT
};

struct combat_work_out{
    uint32_t           ent_uid;
    struct combatstate next_state;
    bool               notify_attack_end;
    enum combat_action action;
    struct attr        action_args[2];
};

struct combat_task_arg{
    size_t begin_idx;
    size_t end_idx;
};

/* The substet of the gamestate necessary
 * for deriving the next combat state for 
 * each entity.
 */
struct combat_gamestate{
    uint16_t               factions;
    uint16_t               player_factions;
    bool                   fog_enabled;
    khash_t(id)           *flags;
    khash_t(pos)          *positions;
    qt_ent_t              *postree;
    void                  *transforms;
    khash_t(range)        *sel_radiuses;
    khash_t(id)           *faction_ids;
    enum diplomacy_state (*diptable)[MAX_FACTIONS];
    void                  *buildstate;
    khash_t(aabb)         *aabbs;
    uint32_t              *fog_state;
};

struct combat_work{
    struct memstack         mem;
    struct combat_gamestate gamestate;
    struct combat_work_in  *in;
    struct combat_work_out *out;
    size_t                  nwork;
    size_t                  ntasks;
    uint32_t                tids[MAX_COMBAT_TASKS];
    struct future           futures[MAX_COMBAT_TASKS];
};

enum combat_cmd_type{
    COMBAT_CMD_ADD,
    COMBAT_CMD_REMOVE,
    COMBAT_CMD_ADD_REF,
    COMBAT_CMD_REMOVE_REF,
    COMBAT_CMD_UPDATE_REF,
    COMBAT_CMD_TRYHIT,
    COMBAT_CMD_ATTACK_UNIT,
    COMBAT_CMD_STOP_ATTACK,
    COMBAT_CMD_SET_STANCE,
    COMBAT_CMD_SET_CURRENT_HP,
    COMBAT_CMD_SET_BASE_ARMOUR,
    COMBAT_CMD_SET_BASE_DAMAGE,
    COMBAT_CMD_SET_MAX_HP,
    COMBAT_CMD_SET_RANGE,
    COMBAT_CMD_CLEAR_SAVED_MOVE_CMD,
    COMBAT_CMD_ADD_TIME_DELTA,
    COMBAT_CMD_SET_PROJ_DESC,
    COMBAT_CMD_PROJ_HIT
};

struct combat_cmd{
    enum combat_cmd_type type;
    struct attr          args[4];
};

KHASH_MAP_INIT_INT(state, struct combatstate)

QUEUE_TYPE(cmd, struct combat_cmd)
QUEUE_IMPL(static, cmd, struct combat_cmd)

static void combat_push_cmd(struct combat_cmd cmd);
static void on_attack_anim_finish(void *user, void *event);
static void on_death_anim_finish(void *user, void *event);
static void do_stop_attack(uint32_t uid);
static bool entity_dead(uint32_t uid);
static struct combat_cmd *snoop_most_recent_command(enum combat_cmd_type type, void *arg,
                                                    bool (*pred)(void*, struct combat_cmd*));
static bool uids_match(void *arg, struct combat_cmd *cmd);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const char *s_name_for_state[] = {
    [STATE_NOT_IN_COMBAT]           = "NOT_IN_COMBAT",
    [STATE_MOVING_TO_TARGET]        = "MOVING_TO_TARGET",
    [STATE_MOVING_TO_TARGET_LOCKED] = "MOVING_TO_TARGET_LOCKED",
    [STATE_CAN_ATTACK]              = "STATE_CAN_ATTACK",
    [STATE_ATTACK_ANIM_PLAYING]     = "ATTACK_ANIM_PLAYING",
    [STATE_DEATH_ANIM_PLAYING]      = "DEATH_ANIM_PLAYING",
    [STATE_ATTACKING]               = "ATTACKING",
    [STATE_TURNING_TO_TARGET]       = "TURNING_TO_TARGET",
};

static khash_t(state)    *s_entity_state_table;
/* For saving/restoring state */
static vec_entity_t       s_dying_ents;
static const struct map  *s_map;
/* How many units of a faction currently currently occupy that bin.
 * For quickly finding that there are no enemy units nearby */
static uint16_t          *s_fac_refcnts[MAX_FACTIONS];

static struct combat_work s_combat_work;
static queue_cmd_t        s_combat_commands;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/* The returned pointer is guaranteed to be valid to write to for
 * so long as we don't add anything to the table. At that point, there
 * is a case that a 'realloc' might take place. */
static struct combatstate *combatstate_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;

    return &kh_value(s_entity_state_table, k);
}

static void combatstate_set(uint32_t uid, const struct combatstate *cs)
{
    int ret;
    khiter_t k = kh_put(state, s_entity_state_table, uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_state_table, k) = *cs;
}

static void combatstate_remove(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    assert(k != kh_end(s_entity_state_table));
    kh_del(state, s_entity_state_table, k);
}

static bool entities_equal(uint32_t *a, uint32_t *b)
{
    return ((*a) == (*b));
}

static void combat_dying_remove(uint32_t uid)
{
    int idx = vec_entity_indexof(&s_dying_ents, uid, entities_equal);
    if(idx == -1)
        return;
    vec_entity_del(&s_dying_ents, idx);
}

static struct proj_desc combat_default_proj(void)
{
    return (struct proj_desc){
        .basedir = pf_strdup("assets/models/bow_arrow"),
        .pfobj = pf_strdup("arrow.pfobj"),
        .scale = (vec3_t){12.0, 12.0, 12.0},
        .speed = PROJECTILE_DEFAULT_SPEED,
    };
}

static bool enemies(uint32_t a, uint32_t b)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    int faction_a = G_GetFactionIDFrom(gs->faction_ids, a);
    int faction_b = G_GetFactionIDFrom(gs->faction_ids, b);
    if(faction_a == faction_b)
        return false;

    enum diplomacy_state ds;
    bool result = G_GetDiplomacyStateFrom(gs->diptable, faction_a, faction_b, &ds);

    assert(result);
    return (ds == DIPLOMACY_STATE_WAR);
}

static bool enemies_in_bin(int faction_id, struct map_resolution binres, struct tile_desc td)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    uint16_t facs = gs->factions;

    size_t x = td.chunk_c * X_BINS_PER_CHUNK + td.tile_c;
    size_t z = td.chunk_r * Z_BINS_PER_CHUNK + td.tile_r;
    size_t idx = x * (binres.chunk_w * binres.tile_w) + z;

    for(int i = 0; facs; facs >>= 1, i++) {

        if(!(facs & 0x1))
            continue;
        if(i == faction_id)
            continue;

        enum diplomacy_state ds;
        G_GetDiplomacyStateFrom(gs->diptable, faction_id, i, &ds);
        if(ds != DIPLOMACY_STATE_WAR)
            continue;

        if(s_fac_refcnts[i][idx] > 0)
            return true;
    }
    return false;
}

static bool maybe_enemy_near(uint32_t uid)
{
    PERF_ENTER();
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    vec2_t pos = G_Pos_GetXZFrom(gs->positions, uid);
    int binlen = MAX(
        (float)(X_COORDS_PER_TILE * TILES_PER_CHUNK_WIDTH)  / X_BINS_PER_CHUNK,
        (float)(Z_COORDS_PER_TILE * TILES_PER_CHUNK_HEIGHT) / Z_BINS_PER_CHUNK
    );
    int binrange = ceil(TARGET_ACQUISITION_RANGE / binlen);

    struct map_resolution mapres;
    M_GetResolution(s_map, &mapres);

    struct map_resolution binres = (struct map_resolution){
        mapres.chunk_w, mapres.chunk_h,
        X_BINS_PER_CHUNK, Z_BINS_PER_CHUNK
    };

    struct tile_desc td;
    bool found = M_Tile_DescForPoint2D(binres, M_GetPos(s_map), pos, &td);
    assert(found);

    size_t binx = td.chunk_c * X_COORDS_PER_TILE + td.tile_c;
    size_t binz = td.chunk_r * Z_COORDS_PER_TILE + td.tile_r;

    for(int dr = -binrange; dr <= binrange; dr++) {
    for(int dc = -binrange; dc <= binrange; dc++) {

        struct tile_desc bin = td;
        if(!M_Tile_RelativeDesc(binres, &bin, dc, dr))
            continue;
        int faction_id = G_GetFactionIDFrom(gs->faction_ids, uid);
        if(enemies_in_bin(faction_id, binres, bin))
            PERF_RETURN(true);
    }}
    PERF_RETURN(false);
}

static void entity_move_in_range(uint32_t uid, uint32_t target)
{
    ASSERT_IN_MAIN_THREAD();

    struct combat_gamestate *gs = &s_combat_work.gamestate;
    const struct combatstate *cs = combatstate_get(uid);
    assert(cs->stance != COMBAT_STANCE_HOLD_POSITION);

    if(cs->stats.attack_range == 0.0f) {
        G_Move_SetSurroundEntity(uid, target);
    }else{
        if(M_NavLocationsReachable(s_map, Entity_NavLayer(uid), 
            G_Pos_GetXZFrom(gs->positions, uid), G_Pos_GetXZFrom(gs->positions, target))) {

            G_Move_SetSurroundEntity(uid, target);
        }else{
            G_Move_SetEnterRange(uid, target, cs->stats.attack_range);
        }
    }
}

static void current_obb_from_gamestate(uint32_t uid, struct obb *out)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;

    vec3_t pos = G_Pos_GetFrom(gs->positions, uid);
    vec3_t scale = Entity_GetScaleFrom(gs->transforms, uid);
    quat_t rot = Entity_GetRotFrom(gs->transforms, uid);

    mat4x4_t model;
    Entity_ModelMatrixFrom(pos, rot, scale, &model);

    khiter_t k = kh_get(aabb, gs->aabbs, uid);
    assert(k != kh_end(gs->aabbs));
    struct aabb *aabb = &kh_value(gs->aabbs, k);

    Entity_CurrentOBBFrom(aabb, model, scale, out);
}

static bool entities_adjacent(uint32_t ent, uint32_t target)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    uint32_t flags = G_FlagsGetFrom(gs->flags, target);

    if(flags & ENTITY_FLAG_MOVABLE) {

        vec2_t ent_pos = G_Pos_GetXZFrom(gs->positions, ent);
        vec2_t target_pos = G_Pos_GetXZFrom(gs->positions, target);

        float ent_radius = G_GetSelectionRadiusFrom(gs->sel_radiuses, ent);
        float target_radius = G_GetSelectionRadiusFrom(gs->sel_radiuses, target);

        return M_NavObjAdjacentToDynamicWith(s_map, ent_pos, ent_radius, 
            target_pos, target_radius);
    }else{

        struct obb obb;
        current_obb_from_gamestate(target, &obb);

        vec2_t ent_pos = G_Pos_GetXZFrom(gs->positions, ent);
        float ent_radius = G_GetSelectionRadiusFrom(gs->sel_radiuses, ent);

        return M_NavObjAdjacentToStaticWith(s_map, ent_pos, ent_radius, &obb);
    }
}

static bool entity_can_attack_melee(uint32_t uid, uint32_t target)
{
    return entities_adjacent(uid, target);
}

static bool entity_can_attack(uint32_t uid, uint32_t target)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    const struct combatstate *cs = combatstate_get(uid);
    if(cs->stats.attack_range == 0.0f) {
        return entity_can_attack_melee(uid, target);
    }

    vec2_t xz_src = G_Pos_GetXZFrom(gs->positions, uid);
    vec2_t xz_dst = G_Pos_GetXZFrom(gs->positions, target);

    vec2_t delta;
    PFM_Vec2_Sub(&xz_src, &xz_dst, &delta);
    return (PFM_Vec2_Len(&delta) <= cs->stats.attack_range);
}

static bool valid_enemy(uint32_t curr, void *arg)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    uint32_t ent = (uintptr_t)arg;
    uint32_t curr_flags = G_FlagsGetFrom(gs->flags, curr);

    if(curr == ent)
        return false;
    if(!(curr_flags & ENTITY_FLAG_COMBATABLE))
        return false;
    if((curr_flags & ENTITY_FLAG_BUILDING) 
    && !G_Building_IsFoundedFrom(gs->buildstate, curr))
        return false;
    if(!enemies(ent, curr))
        return false;

    struct combatstate *cs = combatstate_get(curr);
    assert(cs);
    if(cs->state == STATE_DEATH_ANIM_PLAYING)
        return false;

    struct obb obb;
    current_obb_from_gamestate(curr, &obb);

    uint16_t pmask = gs->player_factions;
    if(!G_Fog_ObjVisibleFrom(gs->fog_state, gs->fog_enabled, pmask, &obb))
        return false;

    return true;
}

static quat_t quat_from_vec(vec2_t dir)
{
    assert(PFM_Vec2_Len(&dir) > EPSILON);

    float angle_rad = atan2(dir.z, dir.x) - M_PI/2.0f;
    return (quat_t) {
        0.0f, 
        1.0f * sin(angle_rad / 2.0f),
        0.0f,
        cos(angle_rad / 2.0f)
    };
}

static quat_t entity_turn_dir(uint32_t uid, uint32_t target)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    vec2_t ent_pos_xz = G_Pos_GetXZFrom(gs->positions, uid);
    vec2_t tar_pos_xz = G_Pos_GetXZFrom(gs->positions, target);

    vec2_t ent_to_target;
    PFM_Vec2_Sub(&tar_pos_xz, &ent_pos_xz, &ent_to_target);

    if(PFM_Vec2_Len(&ent_to_target) < EPSILON) {
        return Entity_GetRotFrom(gs->transforms, uid);
    }

    PFM_Vec2_Normal(&ent_to_target, &ent_to_target);
    quat_t curr = Entity_GetRotFrom(gs->transforms, uid);
    return quat_from_vec(ent_to_target);
}

static void entity_turn_to_target(uint32_t uid, uint32_t target)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    struct combatstate *cs = combatstate_get(uid);
    assert(cs);

    if(!cs->move_cmd_interrupted 
    && G_Move_GetDest(uid, &cs->move_cmd_xz, &cs->move_cmd_attacking)) {
        cs->move_cmd_interrupted = true; 
    }
    G_Move_Stop(uid);

    uint32_t flags = G_FlagsGetFrom(gs->flags, uid);
    if(!(flags & ENTITY_FLAG_MOVABLE)) {
        cs->state = STATE_CAN_ATTACK;
    }else{
        quat_t rot = entity_turn_dir(uid, target);
        G_Move_SetChangeDirection(uid, rot);
        cs->state = STATE_TURNING_TO_TARGET;
    }
}

static void on_disappear_finish(void *arg)
{
    uint32_t self = (uintptr_t)arg;
    uint32_t flags = G_FlagsGet(self);
    flags |= ENTITY_FLAG_INVISIBLE;
    G_FlagsSet(self, flags);
    E_Entity_Notify(EVENT_ENTITY_DISAPPEARED, self, NULL, ES_ENGINE);
}

static void entity_die(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    G_Move_Stop(uid);

    uint32_t flags = G_FlagsGet(uid);
    if(flags & ENTITY_FLAG_SELECTABLE) {
        G_Sel_Remove(uid);
        flags &= ~ENTITY_FLAG_SELECTABLE;
        G_FlagsSet(uid, flags);
    }

    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_attack_anim_finish);
    E_Global_Notify(EVENT_ENTITY_DIED, (void*)((uintptr_t)uid), ES_ENGINE);
    E_Entity_Notify(EVENT_ENTITY_DEATH, uid, NULL, ES_ENGINE);
    vec_entity_push(&s_dying_ents, uid);

    if(flags & ENTITY_FLAG_ANIMATED && !(flags & ENTITY_FLAG_BUILDING)) {

        struct combatstate *cs = combatstate_get(uid);
        cs->state = STATE_DEATH_ANIM_PLAYING;
        E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_death_anim_finish, 
            (void*)((uintptr_t)uid), G_RUNNING);

    }else{

        khash_t(id) *flags = s_combat_work.gamestate.flags;
        khiter_t k = kh_get(id, flags, uid);
        assert(k != kh_end(flags));
        kh_value(flags, k) = kh_value(flags, k) | ENTITY_FLAG_ZOMBIE;

        G_Zombiefy(uid, false);
        Entity_DisappearAnimated(uid, s_map, on_disappear_finish, (void*)((uintptr_t)uid));
    }
}

static void entity_melee_attack(uint32_t uid, uint32_t target)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    struct combatstate *target_cs = combatstate_get(cs->target_uid);

    float dmg = cs->stats.base_dmg  * (1.0f - target_cs->stats.base_armour_pc);
    target_cs->current_hp = MAX(0, target_cs->current_hp - dmg);

    if(target_cs->current_hp == 0 && target_cs->stats.max_hp > 0) {
        entity_die(target);
    }
}

static void entity_ranged_attack(uint32_t uid, uint32_t target)
{
    ASSERT_IN_MAIN_THREAD();

    struct combat_gamestate *gs = &s_combat_work.gamestate;
    struct combatstate *cs = combatstate_get(uid);
    assert(cs);

    vec3_t ent_pos = Entity_CenterPos(uid);
    vec3_t target_pos = Entity_CenterPos(target);
    float ent_dmg = cs->stats.base_dmg;

    vec3_t vel;
    if(!P_Projectile_VelocityForTarget(ent_pos, target_pos, cs->pd.speed, &vel)) {
        /* We resort to just shooting nothing when we can't hit our target. This case 
         * should never be hit so long as the initial velocity is high enough */
        return;
    }

    P_Projectile_Add(ent_pos, vel, uid, G_GetFactionIDFrom(gs->faction_ids, uid), 
        ent_dmg, PROJ_ONLY_HIT_COMBATABLE | PROJ_ONLY_HIT_ENEMIES, cs->pd);
}

static void on_death_anim_finish(void *user, void *event)
{
    uint32_t self = (uintptr_t)user;

    khash_t(id) *flags = s_combat_work.gamestate.flags;
    khiter_t k = kh_get(id, flags, self);
    assert(k != kh_end(flags));
    kh_value(flags, k) = kh_value(flags, k) | ENTITY_FLAG_ZOMBIE;

    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, self, on_death_anim_finish);
    G_Zombiefy(self, true);
}

static void do_add_entity(uint32_t uid, enum combat_stance initial)
{
    ASSERT_IN_MAIN_THREAD();

    assert(combatstate_get(uid) == NULL);
    assert(G_FlagsGet(uid) & ENTITY_FLAG_COMBATABLE);

    struct combatstate new_cs = (struct combatstate) {
        .stats = {0},
        .current_hp = 0,
        .stance = initial,
        .state = STATE_NOT_IN_COMBAT,
        .sticky = false,
        .move_cmd_interrupted = false,
        .pd = combat_default_proj(),
    };
    combatstate_set(uid, &new_cs);
}

static void do_remove_entity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    if(!cs)
        return;

    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_attack_anim_finish);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_death_anim_finish);

    if(cs->state == STATE_ATTACK_ANIM_PLAYING
    || cs->state == STATE_CAN_ATTACK) {
        E_Entity_Notify(EVENT_ATTACK_END, uid, NULL, ES_ENGINE);
    }

    PF_FREE(cs->pd.basedir);
    PF_FREE(cs->pd.pfobj);

    combat_dying_remove(uid);
    combatstate_remove(uid);
}

static void do_tryhit(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    if(entity_dead(uid))
        return; /* Our unit already got 'killed' */

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    if(cs->state == STATE_DEATH_ANIM_PLAYING
    || cs->state == STATE_NOT_IN_COMBAT)
        return;

    cs->state = STATE_CAN_ATTACK;
    if(entity_dead(cs->target_uid)) {
        return; /* Our target already got 'killed' */
    }

    struct combatstate *target_cs = combatstate_get(cs->target_uid);
    quat_t target_dir = entity_turn_dir(uid, cs->target_uid);
    quat_t ent_rot = Entity_GetRot(uid);
    float angle_diff = PFM_Quat_PitchDiff(&ent_rot, &target_dir);

    /* Ranged units fire their shot regardless */
    if(cs->stats.attack_range > 0.0f) {
        entity_ranged_attack(uid, cs->target_uid);
        return;
    }

    if(RAD_TO_DEG(fabs(angle_diff)) > 5.0f) {

        E_Entity_Notify(EVENT_ATTACK_END, uid, NULL, ES_ENGINE);
        entity_turn_to_target(uid, cs->target_uid);
        return;
    }

    if(!entity_can_attack(uid, cs->target_uid)) {
        return; /* Target slipped out of range */
    }

    entity_melee_attack(uid, cs->target_uid);
}

static void do_proj_tryhit(struct proj_hit *hit)
{
    if(entity_dead(hit->ent_uid))
        return;

    struct combatstate *cs = combatstate_get(hit->ent_uid);
    float dmg = hit->cookie * (1.0f - cs->stats.base_armour_pc);
    cs->current_hp = MAX(0, cs->current_hp - dmg);

    if(cs->current_hp == 0 && cs->stats.max_hp > 0) {
        entity_die(hit->ent_uid);
    }
}

static void do_set_stance(uint32_t uid, enum combat_stance stance)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);

    if(stance == cs->stance)
        return;

    if(stance == COMBAT_STANCE_NO_ENGAGEMENT) {
        do_stop_attack(uid);
    }

    if(stance == COMBAT_STANCE_HOLD_POSITION 
    && (cs->state == STATE_MOVING_TO_TARGET || cs->state == STATE_MOVING_TO_TARGET_LOCKED)) {

        G_Move_Stop(uid);
        cs->state = STATE_NOT_IN_COMBAT;
        cs->move_cmd_interrupted = false;
    }

    cs->stance = stance;
}

static void do_clear_saved_move_cmd(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    if(cs) {
        cs->move_cmd_interrupted = false;
    }
}

static void do_attack_unit(uint32_t uid, uint32_t target)
{
    ASSERT_IN_MAIN_THREAD();

    struct combat_gamestate *gs = &s_combat_work.gamestate;
    struct combatstate *cs = combatstate_get(uid);
    assert(cs);

    do_stop_attack(uid);
    cs->stance = COMBAT_STANCE_AGGRESSIVE;

    uint32_t flags = G_FlagsGetFrom(gs->flags, uid);
    if(flags & ENTITY_FLAG_MOVABLE) {
    
        cs->sticky = true;
        cs->target_uid = target;
        cs->state = STATE_MOVING_TO_TARGET_LOCKED;
        cs->move_cmd_interrupted = false;

        entity_move_in_range(uid, target);

    }else if(entity_can_attack(uid, target)) {
    
        cs->sticky = true;
        cs->target_uid = target;
        cs->state = STATE_CAN_ATTACK;
    }
}

static void do_stop_attack(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    if(!cs)
        return;

    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_attack_anim_finish);

    if(cs->state == STATE_ATTACK_ANIM_PLAYING
    || cs->state == STATE_CAN_ATTACK) {

        E_Entity_Notify(EVENT_ATTACK_END, uid, NULL, ES_ENGINE);
    }

    cs->state = STATE_NOT_IN_COMBAT;

    struct combat_cmd *cmd = snoop_most_recent_command(COMBAT_CMD_CLEAR_SAVED_MOVE_CMD,
        (void*)(uintptr_t)uid, uids_match);

    if(!cmd && cs->move_cmd_interrupted) {
        assert(cs->stance != COMBAT_STANCE_HOLD_POSITION);
        G_Move_SetDest(uid, cs->move_cmd_xz, cs->move_cmd_attacking);
        cs->move_cmd_interrupted = false;
    }
}

static void do_add_time_delta(uint32_t delta)
{
    ASSERT_IN_MAIN_THREAD();

    uint32_t key;
    kh_foreach(s_entity_state_table, key, (struct combatstate){0}, {

        struct combatstate *curr = combatstate_get(key);
        if(curr->state != STATE_ATTACKING)
            continue;
        curr->attack_start_tick += delta;
    });
}

static void do_add_ref(int faction_id, vec2_t pos)
{
    ASSERT_IN_MAIN_THREAD();

    struct map_resolution mapres;
    M_GetResolution(s_map, &mapres);

    struct map_resolution binres = (struct map_resolution){
        mapres.chunk_w, mapres.chunk_h,
        X_BINS_PER_CHUNK, Z_BINS_PER_CHUNK
    };

    struct tile_desc td;
    if(!M_Tile_DescForPoint2D(binres, M_GetPos(s_map), pos, &td))
        return;

    size_t x = td.chunk_c * X_BINS_PER_CHUNK + td.tile_c;
    size_t z = td.chunk_r * Z_BINS_PER_CHUNK + td.tile_r;
    size_t idx = x * (binres.chunk_w * binres.tile_w) + z;

    assert(s_fac_refcnts[faction_id][idx] < UINT16_MAX);
    s_fac_refcnts[faction_id][idx]++;
}

static void do_remove_ref(int faction_id, vec2_t pos)
{
    ASSERT_IN_MAIN_THREAD();

    struct map_resolution mapres;
    M_GetResolution(s_map, &mapres);

    struct map_resolution binres = (struct map_resolution){
        mapres.chunk_w, mapres.chunk_h,
        X_BINS_PER_CHUNK, Z_BINS_PER_CHUNK
    };

    struct tile_desc td;
    if(!M_Tile_DescForPoint2D(binres, M_GetPos(s_map), pos, &td))
        return;

    size_t x = td.chunk_c * X_BINS_PER_CHUNK + td.tile_c;
    size_t z = td.chunk_r * Z_BINS_PER_CHUNK + td.tile_r;
    size_t idx = x * (binres.chunk_w * binres.tile_w) + z;

    assert(s_fac_refcnts[faction_id][idx] > 0);
    s_fac_refcnts[faction_id][idx]--;
}

static void do_update_ref(int oldfac, int newfac, vec2_t pos)
{
    ASSERT_IN_MAIN_THREAD();

    G_Combat_RemoveRef(oldfac, pos);
    G_Combat_AddRef(newfac, pos);
}

static void do_set_base_armour(uint32_t uid, float armour_pc)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    cs->stats.base_armour_pc = armour_pc;
}

static void do_set_base_damage(uint32_t uid, int dmg)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    cs->stats.base_dmg = dmg;
}

static void do_set_current_hp(uint32_t uid, int hp)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    cs->current_hp = MIN(hp, cs->stats.max_hp);
}

static void do_set_max_hp(uint32_t uid, int hp)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    cs->stats.max_hp = hp;
}

static void do_set_range(uint32_t uid, float range)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    cs->stats.attack_range = range;
}

static void do_set_proj_desc(uint32_t uid, const struct proj_desc *pd)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);

    PF_FREE(cs->pd.basedir);
    PF_FREE(cs->pd.pfobj);

    cs->pd = (struct proj_desc) {
        pf_strdup(pd->basedir),
        pf_strdup(pd->pfobj),
        pd->scale,
        pd->speed,
    };
}

static void on_attack_anim_finish(void *user, void *event)
{
    uint32_t self = (uintptr_t)user;
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, self, on_attack_anim_finish);
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_TRYHIT,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = self
        }
    });
}

static bool entity_dead(uint32_t uid)
{
    struct combatstate *cs = combatstate_get(uid);
    if(!cs || (cs->state == STATE_DEATH_ANIM_PLAYING)
    || (G_FlagsGetFrom(s_combat_work.gamestate.flags, uid) & ENTITY_FLAG_ZOMBIE))
        return true;

    return false;
}

static void entity_target_enemy(uint32_t uid, uint32_t enemy)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    struct combatstate *cs = combatstate_get(uid);
    assert(cs);

    if(entity_can_attack(uid, enemy)) {

        assert(cs->stance == COMBAT_STANCE_AGGRESSIVE 
            || cs->stance == COMBAT_STANCE_HOLD_POSITION);

        cs->target_uid = enemy;
        entity_turn_to_target(uid, enemy);
        return;
    }

    uint32_t flags = G_FlagsGetFrom(gs->flags, uid);
    if(cs->stance == COMBAT_STANCE_AGGRESSIVE && (flags & ENTITY_FLAG_MOVABLE)) {

        cs->target_uid = enemy;
        cs->state = STATE_MOVING_TO_TARGET;

        if(!cs->move_cmd_interrupted 
        && G_Move_GetDest(uid, &cs->move_cmd_xz, &cs->move_cmd_attacking)) {
            cs->move_cmd_interrupted = true; 
        }
        G_Move_SetSeekEnemies(uid);
    }
}

static void entity_stop_combat(uint32_t uid)
{
    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    cs->state = STATE_NOT_IN_COMBAT; 

    uint32_t flags = G_FlagsGet(uid);
    if(!(flags & ENTITY_FLAG_MOVABLE))
        return;

    struct combat_cmd *cmd = snoop_most_recent_command(COMBAT_CMD_SET_RANGE,
        (void*)(uintptr_t)uid, uids_match);

    if(!cmd && cs->move_cmd_interrupted) {
        G_Move_SetDest(uid, cs->move_cmd_xz, cs->move_cmd_attacking);
        cs->move_cmd_interrupted = false;
    }else {
        G_Move_Stop(uid);
    }
}

uint32_t closest_eligible_entity(uint32_t uid)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    vec2_t pos = G_Pos_GetXZFrom(gs->positions, uid);
    uint32_t ents[128];
    size_t nents = G_Pos_EntsInCircleWithPredFrom(
        gs->postree, pos, TARGET_ACQUISITION_RANGE, ents, 
        ARR_SIZE(ents), valid_enemy, (void*)((uintptr_t)uid));

    if(!nents)
        return NULL_UID;

    float min_dist = INFINITY;
    uint32_t ret = NULL_UID;

    for(int i = 0; i < nents; i++) {
    
        if(entities_adjacent(uid, ents[i]))
            return ents[i];

        vec2_t enemy_pos = G_Pos_GetXZFrom(gs->positions, ents[i]);
        vec2_t delta;
        PFM_Vec2_Sub(&pos, &enemy_pos, &delta);
        float dist = PFM_Vec2_Len(&delta);

        if(dist < min_dist) {
            min_dist = dist;
            ret = ents[i];
        }
    }
    return ret;
}

static void entity_compute_update(uint32_t uid, struct combat_work_out *out)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    uint32_t flags = G_FlagsGetFrom(gs->flags, uid);

    const struct combatstate *old = combatstate_get(uid);
    struct combatstate *curr = &out->next_state;
    *curr = *old;

    out->action = COMBAT_ACTION_NONE;
    out->notify_attack_end = false;
    out->ent_uid = uid;

    switch(curr->state) {
    case STATE_NOT_IN_COMBAT: 
    {
        if(curr->stance == COMBAT_STANCE_NO_ENGAGEMENT)
            break;

        if(flags & ENTITY_FLAG_BUILDING 
        && !G_Building_IsCompletedFrom(gs->buildstate, uid))
            break;

        if(curr->stats.base_dmg == 0)
            break;

        if(!maybe_enemy_near(uid))
            break;

        uint32_t enemy = closest_eligible_entity(uid);
        if(enemy == NULL_UID)
            break;

        out->action = COMBAT_ACTION_TARGET_ENEMY;
        out->action_args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        };
        out->action_args[1] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = enemy
        };
        break;
    }
    case STATE_MOVING_TO_TARGET:
    {
        assert(flags & ENTITY_FLAG_MOVABLE);

        /* Handle the case where our target dies before we reach it */
        uint32_t enemy = closest_eligible_entity(uid);
        if(enemy == NULL_UID) {

            out->action = COMBAT_ACTION_STOP_COMBAT;
            out->action_args[0] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            break;
        }

        /* And the case where a different target becomes even closer */
        if(enemy != curr->target_uid) {
            curr->target_uid = enemy;
        }

        /* Check if we're within attacking range of our target */
        if(entity_can_attack(uid, enemy)) {

            out->action = COMBAT_ACTION_TURN_TO_TARGET;
            out->action_args[0] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            out->action_args[1] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = enemy
            };
        }
        break;
    }
    case STATE_MOVING_TO_TARGET_LOCKED:
    {
        assert(flags & ENTITY_FLAG_MOVABLE);

        if(entity_dead(curr->target_uid)) {

            curr->state = STATE_NOT_IN_COMBAT;
            curr->sticky = false;

            out->action = COMBAT_ACTION_STOP_MOVEMENT;
            out->action_args[0] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            break;
        }

        struct obb obb;
        current_obb_from_gamestate(curr->target_uid, &obb);

        uint16_t pmask = gs->player_factions;
        if(!G_Fog_ObjVisibleFrom(gs->fog_state, gs->fog_enabled, pmask, &obb)) {
        
            curr->state = STATE_NOT_IN_COMBAT;
            curr->sticky = false;

            out->action = COMBAT_ACTION_STOP_MOVEMENT;
            out->action_args[0] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            break;
        }

        /* Check if we're within attacking range of our target */
        if(entity_can_attack(uid, curr->target_uid)) {

            out->action = COMBAT_ACTION_TURN_TO_TARGET;
            out->action_args[0] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            out->action_args[1] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = curr->target_uid
            };
            break;
        }

        /* We approached the target, but it slipped away from us. Re-engage. */
        out->action = COMBAT_ACTION_MOVE_IN_RANGE_IF_STILL;
        out->action_args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        };
        out->action_args[1] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr->target_uid
        };
        break;
    }
    case STATE_CAN_ATTACK:
    {
        /* Our target could have 'died' or gotten out of combat range - check this first. */
        if(entity_dead(curr->target_uid) || !entity_can_attack(uid, curr->target_uid)) {

            if(curr->sticky) {

                if(!entity_dead(curr->target_uid)) {

                    curr->state = STATE_MOVING_TO_TARGET_LOCKED;
                    out->notify_attack_end = true;
                    out->action = COMBAT_ACTION_MOVE_IN_RANGE;
                    out->action_args[0] = (struct attr){
                        .type = TYPE_INT,
                        .val.as_int = uid
                    };
                    out->action_args[1] = (struct attr){
                        .type = TYPE_INT,
                        .val.as_int = curr->target_uid
                    };
                    break;
                }else{
                    curr->sticky = false;
                }
            }

            /* Check if there's another suitable target */
            uint32_t enemy = closest_eligible_entity(uid);
            if(enemy == NULL_UID) {
                out->notify_attack_end = true;
                out->action = COMBAT_ACTION_STOP_COMBAT;
                out->action_args[0] = (struct attr){
                    .type = TYPE_INT,
                    .val.as_int = uid
                };
                break;
            }

            if(curr->stance == COMBAT_STANCE_HOLD_POSITION && !entity_can_attack(uid, enemy)) {

                out->notify_attack_end = true;
                out->action = COMBAT_ACTION_STOP_COMBAT;
                out->action_args[0] = (struct attr){
                    .type = TYPE_INT,
                    .val.as_int = uid
                };
                break;
            }

            out->notify_attack_end = true;
            out->action = COMBAT_ACTION_TARGET_ENEMY;
            out->action_args[0] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            out->action_args[1] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = enemy
            };
            break;
        }

        /* Perform combat simulation between entities with targets within range */
        if(flags & ENTITY_FLAG_ANIMATED) {
            curr->state = STATE_ATTACK_ANIM_PLAYING;
            out->action = COMBAT_ACTION_ANIMATED_ATTACK;
            out->action_args[0] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
        }else{
            curr->state = STATE_ATTACKING;
            curr->attack_start_tick = SDL_GetTicks();
        }

        break;
    }
    case STATE_TURNING_TO_TARGET: {

        if(entity_dead(curr->target_uid) || !entity_can_attack(uid, curr->target_uid)) {

            out->action = COMBAT_ACTION_STOP_COMBAT;
            out->action_args[0] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            break;
        }

        out->action = COMBAT_ACTION_ATTACK_IF_STILL;
        out->action_args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        };
        break;
    }
    case STATE_ATTACKING: {

        uint32_t ticks = SDL_GetTicks();
        uint32_t period = DEFAULT_ATTACK_PERIOD * 1000.0f;
        if(!SDL_TICKS_PASSED(ticks, curr->attack_start_tick + period))
            break;

        out->action = COMBAT_ACTION_TRYHIT;
        out->action_args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        };
        break;
    }
    case STATE_ATTACK_ANIM_PLAYING:
    case STATE_DEATH_ANIM_PLAYING:
        /* No-op */
        break;
    default: 
        assert(0);
    };
}

static void entity_apply_update(struct combat_work_out *out)
{
    uint32_t uid = out->ent_uid;
    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    *cs = out->next_state;

    if(out->notify_attack_end) {
        E_Entity_Notify(EVENT_ATTACK_END, uid, NULL, ES_ENGINE);
    }

    switch(out->action) {
    case COMBAT_ACTION_NONE:
        break;
    case COMBAT_ACTION_TARGET_ENEMY: {
        uint32_t uid = out->action_args[0].val.as_int;
        uint32_t target = out->action_args[1].val.as_int;
        entity_target_enemy(uid, target);
        break;
    }
    case COMBAT_ACTION_STOP_COMBAT: {
        uint32_t uid = out->action_args[0].val.as_int;
        entity_stop_combat(uid);
        break;
    }
    case COMBAT_ACTION_TURN_TO_TARGET: {
        uint32_t uid = out->action_args[0].val.as_int;
        uint32_t target = out->action_args[1].val.as_int;
        entity_turn_to_target(uid, target);
        break;
    }
    case COMBAT_ACTION_STOP_MOVEMENT: {
        uint32_t uid = out->action_args[0].val.as_int;
        G_Move_Stop(uid);
        break;
    }
    case COMBAT_ACTION_MOVE_IN_RANGE: {
        uint32_t uid = out->action_args[0].val.as_int;
        uint32_t target = out->action_args[1].val.as_int;
        entity_move_in_range(uid, target);
        break;
    }
    case COMBAT_ACTION_MOVE_IN_RANGE_IF_STILL: {
        uint32_t uid = out->action_args[0].val.as_int;
        uint32_t target = out->action_args[1].val.as_int;
        if(G_Move_Still(uid)) {
            entity_move_in_range(uid, target);
        }
        break;
    }
    case COMBAT_ACTION_ANIMATED_ATTACK: {
        uint32_t uid = out->action_args[0].val.as_int;
        E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_attack_anim_finish, 
            (void*)((uintptr_t)uid), G_RUNNING);
        break;
    }
    case COMBAT_ACTION_ATTACK_IF_STILL: {
        uint32_t old_uid = uid;
        uint32_t uid = out->action_args[0].val.as_int;
        assert(old_uid == uid);
        if(G_Move_Still(uid)) {
            cs->state = STATE_CAN_ATTACK;
            E_Entity_Notify(EVENT_ATTACK_START, uid, NULL, ES_ENGINE);
        }
        break;
    }
    case COMBAT_ACTION_TRYHIT: {
        uint32_t uid = out->action_args[0].val.as_int;
        do_tryhit(uid);
        break;
    }
    default:
        assert(0);
    }
}

static void combat_push_cmd(struct combat_cmd cmd)
{
    queue_cmd_push(&s_combat_commands, &cmd);
}

static bool uids_match(void *arg, struct combat_cmd *cmd)
{
    uint32_t desired_uid = (uintptr_t)arg;
    uint32_t actual_uid = cmd->args[0].val.as_int;
    return (desired_uid == actual_uid);
}

static struct combat_cmd *snoop_most_recent_command(enum combat_cmd_type type, void *arg,
                                                    bool (*pred)(void*, struct combat_cmd*))
{
    ASSERT_IN_MAIN_THREAD();

    if(queue_size(s_combat_commands) == 0)
        return NULL;

    size_t left = queue_size(s_combat_commands);
    for(int i = s_combat_commands.itail; left > 0;) {
        struct combat_cmd *curr = &s_combat_commands.mem[i];
        if(curr->type == type)
            if(pred(arg, curr))
                return curr;
        i--;
        left--;
        if(i < 0) {
            i = s_combat_commands.capacity - 1; /* Wrap around */
        }
    }
    return NULL;
}

static void combat_process_cmds(void)
{
    struct combat_cmd cmd;
    while(queue_cmd_pop(&s_combat_commands, &cmd)) {
        switch(cmd.type) {
        case COMBAT_CMD_ADD: {
            uint32_t uid = cmd.args[0].val.as_int;
            enum combat_stance initial = cmd.args[1].val.as_int;
            do_add_entity(uid, initial);
            break;
        }
        case COMBAT_CMD_REMOVE: {
            uint32_t uid = cmd.args[0].val.as_int;
            do_remove_entity(uid);
            break;
        }
        case COMBAT_CMD_TRYHIT: {
            uint32_t uid = cmd.args[0].val.as_int;
            do_tryhit(uid);
            break;
        }
        case COMBAT_CMD_SET_STANCE: {
            uint32_t uid = cmd.args[0].val.as_int;
            enum combat_stance stance = cmd.args[1].val.as_int;
            do_set_stance(uid, stance);
            break;
        }
        case COMBAT_CMD_CLEAR_SAVED_MOVE_CMD: {
            uint32_t uid = cmd.args[0].val.as_int;
            do_clear_saved_move_cmd(uid);
            break;
        }
        case COMBAT_CMD_ATTACK_UNIT: {
            uint32_t uid = cmd.args[0].val.as_int;
            uint32_t target = cmd.args[1].val.as_int;
            do_attack_unit(uid, target);
            break;
        }
        case COMBAT_CMD_STOP_ATTACK: {
            uint32_t uid = cmd.args[0].val.as_int;
            do_stop_attack(uid);
            break;
        }
        case COMBAT_CMD_ADD_TIME_DELTA: {
            uint32_t delta = cmd.args[0].val.as_int;
            do_add_time_delta(delta);
            break;
        }
        case COMBAT_CMD_ADD_REF: {
            int faction_id = cmd.args[0].val.as_int;
            vec2_t pos = cmd.args[1].val.as_vec2;
            do_add_ref(faction_id, pos);
            break;
        }
        case COMBAT_CMD_REMOVE_REF: {
            int faction_id = cmd.args[0].val.as_int;
            vec2_t pos = cmd.args[1].val.as_vec2;
            do_remove_ref(faction_id, pos);
            break;
        }
        case COMBAT_CMD_UPDATE_REF: {
            int oldfac = cmd.args[0].val.as_int;
            int newfac = cmd.args[1].val.as_int;
            vec2_t pos = cmd.args[2].val.as_vec2;
            do_update_ref(oldfac, newfac, pos);
            break;
        }
        case COMBAT_CMD_SET_BASE_ARMOUR: {
            uint32_t uid = cmd.args[0].val.as_int;
            float armour_pc = cmd.args[1].val.as_float;
            do_set_base_armour(uid, armour_pc);
            break;
        }
        case COMBAT_CMD_SET_BASE_DAMAGE: {
            uint32_t uid = cmd.args[0].val.as_int;
            int dmg = cmd.args[1].val.as_int;
            do_set_base_damage(uid, dmg);
            break;
        }
        case COMBAT_CMD_SET_CURRENT_HP: {
            uint32_t uid = cmd.args[0].val.as_int;
            int hp = cmd.args[1].val.as_int;
            do_set_current_hp(uid, hp);
            break;
        }
        case COMBAT_CMD_SET_MAX_HP: {
            uint32_t uid = cmd.args[0].val.as_int;
            int hp = cmd.args[1].val.as_int;
            do_set_max_hp(uid, hp);
            break;
        }
        case COMBAT_CMD_SET_RANGE: {
            uint32_t uid = cmd.args[0].val.as_int;
            float range  = cmd.args[1].val.as_float;
            do_set_range(uid, range);
            break;
        }
        case COMBAT_CMD_SET_PROJ_DESC: {
            uint32_t uid = cmd.args[0].val.as_int;
            struct proj_desc *pd = cmd.args[1].val.as_pointer;
            do_set_proj_desc(uid, pd);
            PF_FREE(pd->basedir);
            PF_FREE(pd->pfobj);
            PF_FREE(pd);
            break;
        }
        case COMBAT_CMD_PROJ_HIT: {
            struct proj_hit *hit = cmd.args[0].val.as_pointer;
            do_proj_tryhit(hit);
            PF_FREE(hit);
            break;
        }
        default:
            assert(0);
        }
    }
}

static void combat_work(int begin_idx, int end_idx)
{
    for(int i = begin_idx; i <= end_idx; i++) {
    
        struct combat_work_in *in = &s_combat_work.in[i];
        struct combat_work_out *out = &s_combat_work.out[i];
        entity_compute_update(in->ent_uid, out);
    }
}

static struct result combat_task(void *arg)
{
    struct combat_task_arg *combat_arg = arg;
    size_t ncomputed = 0;

    for(int i = combat_arg->begin_idx; i <= combat_arg->end_idx; i++) {

        combat_work(i, i);
        ncomputed++;

        if(ncomputed % 64 == 0)
            Task_Yield();
    }
    return NULL_RESULT;
}

static void combat_complete_work(void)
{
    for(int i = 0; i < s_combat_work.ntasks; i++) {
        while(!Sched_FutureIsReady(&s_combat_work.futures[i])) {
            Sched_RunSync(s_combat_work.tids[i]);
        }
    }
}

static khash_t(aabb) *combat_copy_aabbs(void)
{
    PERF_ENTER();
    khash_t(aabb) *aabbs = kh_init(aabb);
    if(!aabbs)
        PERF_RETURN(NULL);

    const khash_t(entity) *ents = G_GetAllEntsSet();

    uint32_t uid;
    kh_foreach_key(ents, uid, {
        int ret;
        khiter_t k = kh_put(aabb, aabbs, uid, &ret);
        assert(ret != -1);
        kh_value(aabbs, k) = AL_EntityGet(uid)->identity_aabb;
    });
    PERF_RETURN(aabbs);
}

static void combat_copy_gamestate(void)
{
    PERF_ENTER();
    s_combat_work.gamestate.factions = G_GetFactions(NULL, NULL, NULL);
    s_combat_work.gamestate.player_factions = G_GetPlayerControlledFactions();
    s_combat_work.gamestate.fog_enabled = G_Fog_Enabled();
    s_combat_work.gamestate.flags = G_FlagsCopyTable();
    s_combat_work.gamestate.positions = G_Pos_CopyTable();
    s_combat_work.gamestate.postree = G_Pos_CopyQuadTree();
    s_combat_work.gamestate.transforms = Entity_CopyTransforms();
    s_combat_work.gamestate.sel_radiuses = G_SelectionRadiusCopyTable();
    s_combat_work.gamestate.faction_ids = G_FactionIDCopyTable();
    s_combat_work.gamestate.diptable = G_CopyDiplomacyTable();
    s_combat_work.gamestate.buildstate = G_Building_CopyState();
    s_combat_work.gamestate.aabbs = combat_copy_aabbs();
    s_combat_work.gamestate.fog_state = G_Fog_CopyState();
    PERF_RETURN_VOID();
}

static void combat_release_gamestate(void)
{
    PERF_ENTER();
    if(s_combat_work.gamestate.flags) {
        kh_destroy(id, s_combat_work.gamestate.flags);
        s_combat_work.gamestate.flags = NULL;
    }
    if(s_combat_work.gamestate.positions) {
        kh_destroy(pos, s_combat_work.gamestate.positions);
        s_combat_work.gamestate.positions = NULL;
    }
    if(s_combat_work.gamestate.postree) {
        G_Pos_DestroyQuadTree(s_combat_work.gamestate.postree);
        s_combat_work.gamestate.postree = NULL;
    }
    if(s_combat_work.gamestate.transforms) {
        kh_destroy(trans, s_combat_work.gamestate.transforms);
        s_combat_work.gamestate.transforms = NULL;
    }
    if(s_combat_work.gamestate.sel_radiuses) {
        kh_destroy(range, s_combat_work.gamestate.sel_radiuses);
        s_combat_work.gamestate.sel_radiuses = NULL;
    }
    if(s_combat_work.gamestate.faction_ids) {
        kh_destroy(id, s_combat_work.gamestate.faction_ids);
        s_combat_work.gamestate.faction_ids = NULL;
    }
    if(s_combat_work.gamestate.diptable) {
        PF_FREE(s_combat_work.gamestate.diptable);
        s_combat_work.gamestate.diptable = NULL;
    }
    if(s_combat_work.gamestate.buildstate) {
        PF_FREE(s_combat_work.gamestate.buildstate);
        s_combat_work.gamestate.buildstate = NULL;
    }
    if(s_combat_work.gamestate.aabbs) {
        PF_FREE(s_combat_work.gamestate.aabbs);
        s_combat_work.gamestate.aabbs = NULL;
    }
    if(s_combat_work.gamestate.fog_state) {
        PF_FREE(s_combat_work.gamestate.fog_state);
        s_combat_work.gamestate.fog_state = NULL;
    }
    PERF_RETURN_VOID();
}

static void combat_update_gamestate(void)
{
    combat_release_gamestate();
    combat_copy_gamestate();
}

static void combat_prepare_work(void)
{
    size_t nents = kh_size(s_entity_state_table);
    s_combat_work.in = stalloc(&s_combat_work.mem, nents * sizeof(struct combat_work_in));
    s_combat_work.out = stalloc(&s_combat_work.mem, nents * sizeof(struct combat_work_out));
}

static void combat_finish_work(void)
{
    PERF_ENTER();

    if(s_combat_work.nwork == 0)
        PERF_RETURN_VOID();

    combat_complete_work();    

    PERF_PUSH("apply updates");
    for(int i = 0; i < s_combat_work.nwork; i++) {
        struct combat_work_out *out = &s_combat_work.out[i];
        entity_apply_update(out);
    }
    PERF_POP();

    stalloc_clear(&s_combat_work.mem);
    s_combat_work.in = NULL;
    s_combat_work.out = NULL;
    s_combat_work.nwork = 0;
    s_combat_work.ntasks = 0;

    PERF_RETURN_VOID();
}

static void combat_push_work(struct combat_work_in in)
{
    s_combat_work.in[s_combat_work.nwork++] = in;
}

static void combat_submit_work(void)
{
    if(s_combat_work.nwork == 0)
        return;

    size_t ntasks = SDL_GetCPUCount();
    if(s_combat_work.nwork < 64)
        ntasks = 1;
    ntasks = MIN(ntasks, MAX_COMBAT_TASKS);

    for(int i = 0; i < ntasks; i++) {

        struct combat_task_arg *arg = stalloc(&s_combat_work.mem, sizeof(struct combat_task_arg));
        size_t nitems = ceil((float)s_combat_work.nwork / ntasks);

        arg->begin_idx = nitems * i;
        arg->end_idx = MIN(nitems * (i + 1) - 1, s_combat_work.nwork-1);

        SDL_AtomicSet(&s_combat_work.futures[s_combat_work.ntasks].status, FUTURE_INCOMPLETE);
        s_combat_work.tids[s_combat_work.ntasks] = Sched_Create(4, combat_task, arg, 
            &s_combat_work.futures[s_combat_work.ntasks], TASK_BIG_STACK);

        if(s_combat_work.tids[s_combat_work.ntasks] == NULL_TID) {
            combat_work(arg->begin_idx, arg->end_idx);
        }else{
            s_combat_work.ntasks++;
        }
    }
}

static void on_20hz_tick(void *user, void *event)
{
    PERF_PUSH("combat::on_20hz_tick");

    combat_finish_work();
    combat_process_cmds();
    combat_release_gamestate();

    combat_prepare_work();
    combat_copy_gamestate();

    uint32_t uid;
    kh_foreach_key(s_entity_state_table, uid, {
        combat_push_work((struct combat_work_in){uid});
    });
    combat_submit_work();

    PERF_POP();
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    bool targeting = G_Move_InTargetMode();
    bool right = (mouse_event->button == SDL_BUTTON_RIGHT);
    bool left = (mouse_event->button == SDL_BUTTON_LEFT);

    if(G_MouseOverMinimap())
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    if(right && targeting)
        return;

    if(left && !targeting)
        return;

    if(right && (G_CurrContextualAction() != CTX_ACTION_ATTACK))
        return;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);
    size_t nattacking = 0;

    if(vec_size(sel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return;

    uint32_t first = vec_AT(sel, 0);
    uint32_t target = G_Sel_GetHovered();

    if((target == NULL_UID) 
    || !(G_FlagsGet(target) & ENTITY_FLAG_COMBATABLE) || !enemies(first, target))
        return;

    for(int i = 0; i < vec_size(sel); i++) {

        uint32_t curr = vec_AT(sel, i);
        uint32_t flags = G_FlagsGet(curr);

        if(!(flags & ENTITY_FLAG_COMBATABLE))
            continue;

        G_Combat_AttackUnit(curr, target);
        nattacking++;
    }

    if(nattacking) {
        Entity_Ping(target);
    }
}

static void combat_render_targets(void)
{
    int winw, winh;
    Engine_WinDrawableSize(&winw, &winh);
    const struct camera *cam = G_GetActiveCamera();
    struct combat_gamestate *gs = &s_combat_work.gamestate;

    uint32_t key;
    struct combatstate curr;

    kh_foreach(s_entity_state_table, key, curr, {

        if(!G_EntityExists(key))
            continue;

        vec2_t ent_pos = G_Pos_GetXZFrom(gs->positions, key);
        mat4x4_t ident;
        PFM_Mat4x4_Identity(&ident);

        const float radius = TARGET_ACQUISITION_RANGE;
        const float width = 0.25f;
        vec3_t red = (vec3_t){1.0f, 0.0f, 0.0f};
        vec3_t blue = (vec3_t){0.0f, 0.0f, 1.0f};

        switch(curr.state) {
        case STATE_MOVING_TO_TARGET:
        case STATE_MOVING_TO_TARGET_LOCKED:
        case STATE_CAN_ATTACK: {

            if(entity_dead(curr.target_uid))
                continue;
        
            vec2_t delta;
            vec2_t target_pos = G_Pos_GetXZFrom(gs->positions, curr.target_uid);
            PFM_Vec2_Sub(&target_pos, &ent_pos, &delta);

            float t = PFM_Vec2_Len(&delta);
            PFM_Vec2_Normal(&delta, &delta);
            vec3_t dir = (vec3_t){delta.x, 0.0f, delta.z};

            vec3_t raised_pos = (vec3_t){
                ent_pos.x,
                M_HeightAtPoint(G_GetPrevTickMap(), (vec2_t){ent_pos.x, ent_pos.z}) + 5.0f, 
                ent_pos.z 
            };

            R_PushCmd((struct rcmd){
                .func = R_GL_DrawRay,
                .nargs = 5,
                .args = {
                    R_PushArg(&raised_pos, sizeof(raised_pos)),
                    R_PushArg(&dir, sizeof(dir)),
                    R_PushArg(&ident, sizeof(ident)),
                    R_PushArg(&red, sizeof(red)),
                    R_PushArg(&t, sizeof(t)),
                },
            });
            break;
        }
        default:
            break;
        }

        R_PushCmd((struct rcmd){
            .func = R_GL_DrawSelectionCircle,
            .nargs = 5,
            .args = {
                R_PushArg(&ent_pos, sizeof(ent_pos)),
                R_PushArg(&radius, sizeof(radius)),
                R_PushArg(&width, sizeof(width)),
                R_PushArg(&blue, sizeof(blue)),
                (void*)G_GetPrevTickMap(),
            },
        });

        vec2_t ss_pos = Entity_TopScreenPos(key, winw, winh);
        struct rect bounds = (struct rect){ss_pos.x - 75, ss_pos.y + 5, 600, 16};
        struct rgba color = (struct rgba){255, 0, 0, 255};
        UI_DrawText(s_name_for_state[curr.state], bounds, color);
    });
}

static void combat_render_ranges(void)
{
    uint32_t key;
    struct combatstate curr;
    struct combat_gamestate *gs = &s_combat_work.gamestate;

    kh_foreach(s_entity_state_table, key, curr, {

        if(!G_EntityExists(key))
            continue;

        if(curr.stats.attack_range == 0.0f)
            continue;

        vec2_t ent_pos = G_Pos_GetXZFrom(gs->positions, key);
        mat4x4_t ident;
        PFM_Mat4x4_Identity(&ident);

        const float radius = curr.stats.attack_range;
        const float width = 0.25f;
        vec3_t red = (vec3_t){1.0f, 0.0f, 0.0f};

        R_PushCmd((struct rcmd){
            .func = R_GL_DrawSelectionCircle,
            .nargs = 5,
            .args = {
                R_PushArg(&ent_pos, sizeof(ent_pos)),
                R_PushArg(&radius, sizeof(radius)),
                R_PushArg(&width, sizeof(width)),
                R_PushArg(&red, sizeof(red)),
                (void*)G_GetPrevTickMap(),
            },
        });
    });
}

static void on_render_3d(void *user, void *event)
{
    struct sval setting;
    ss_e status;
    (void)status;

    status = Settings_Get("pf.debug.show_combat_targets", &setting);
    assert(status == SS_OKAY);

    if(setting.as_bool) {
        combat_render_targets();
    }

    status = Settings_Get("pf.debug.show_combat_ranges", &setting);
    assert(status == SS_OKAY);

    if(setting.as_bool) {
        combat_render_ranges();
    }
}

static void on_proj_hit(void *user, void *event)
{
    struct proj_hit *hit = event;
    struct proj_hit *copy = malloc(sizeof(struct proj_hit));
    if(!copy)
        return;
    *copy = *hit;

    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_PROJ_HIT,
        .args[0] = {
            .type = TYPE_POINTER,
            .val.as_pointer = copy
        }
    });
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Combat_Init(const struct map *map)
{
    if(NULL == (s_entity_state_table = kh_init(state)))
        return false;

    memset(&s_combat_work, 0, sizeof(s_combat_work));
    if(!stalloc_init(&s_combat_work.mem))
        goto fail_stack;

    if(!queue_cmd_init(&s_combat_commands, 256))
        goto fail_queue;

    struct map_resolution res;
    M_GetResolution(map, &res);

    for(int i = 0; i < MAX_FACTIONS; i++) {
        s_fac_refcnts[i] = calloc((res.chunk_w * X_BINS_PER_CHUNK) 
                                * (res.chunk_h * Z_BINS_PER_CHUNK) 
                                * sizeof(uint16_t), 1);
        if(!s_fac_refcnts[i])
            goto fail_refcnts;
    }

    vec_entity_init(&s_dying_ents);
    E_Global_Register(EVENT_30HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, G_ALL);
    E_Global_Register(EVENT_PROJECTILE_HIT, on_proj_hit, NULL, G_RUNNING);
    s_map = map;
    combat_copy_gamestate();
    return true;

fail_refcnts:
    for(int i = 0; i < MAX_FACTIONS; i++)
        PF_FREE(s_fac_refcnts[i]);
    queue_cmd_destroy(&s_combat_commands);
fail_queue:
    stalloc_destroy(&s_combat_work.mem);
fail_stack:
    kh_destroy(state, s_entity_state_table);
    return false;
}

void G_Combat_Shutdown(void)
{
    combat_complete_work();
    s_map = NULL;

    E_Global_Unregister(EVENT_30HZ_TICK, on_20hz_tick);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    E_Global_Unregister(EVENT_PROJECTILE_HIT, on_proj_hit);

    uint32_t uid;
    kh_foreach_key(s_entity_state_table, uid, {
        E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_attack_anim_finish);
        E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_death_anim_finish);
    });

    combat_release_gamestate();
    vec_entity_destroy(&s_dying_ents);
    for(int i = 0; i < MAX_FACTIONS; i++) {
        PF_FREE(s_fac_refcnts[i]);
    }
    queue_cmd_destroy(&s_combat_commands);
    stalloc_destroy(&s_combat_work.mem);
    kh_destroy(state, s_entity_state_table);
}

bool G_Combat_HasWork(void)
{
    return (queue_size(s_combat_commands) > 0);
}

void G_Combat_FlushWork(void)
{
    combat_finish_work();
    combat_process_cmds();
}

void G_Combat_AddEntity(uint32_t uid, enum combat_stance initial)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_ADD,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_INT,
            .val.as_int = initial
        }
    });
}

void G_Combat_RemoveEntity(uint32_t uid)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_REMOVE,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        }
    });
}

void G_Combat_SetStance(uint32_t uid, enum combat_stance stance)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_STANCE,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_INT,
            .val.as_int = stance
        }
    });
}

enum combat_stance G_Combat_GetStance(uint32_t uid)
{
    struct combat_cmd *cmd = snoop_most_recent_command(COMBAT_CMD_SET_STANCE,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->args[1].val.as_int;
    }
    cmd = snoop_most_recent_command(COMBAT_CMD_ADD,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->args[1].val.as_int;
    }

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    return cs->stance;
}

void G_Combat_ClearSavedMoveCmd(uint32_t uid)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_CLEAR_SAVED_MOVE_CMD,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        }
    });
}

int G_Combat_CurrContextualAction(void)
{
    uint32_t hovered = G_Sel_GetHovered();
    if(!G_EntityExists(hovered))
        return CTX_ACTION_NONE;

    if(M_MouseOverMinimap(s_map))
        return CTX_ACTION_NONE;

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    if(S_UI_MouseOverWindow(mouse_x, mouse_y))
        return CTX_ACTION_NONE;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);

    if(vec_size(sel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return CTX_ACTION_NONE;

    uint32_t first = vec_AT(sel, 0);
    uint32_t flags = G_FlagsGet(first);

    if(!(flags & ENTITY_FLAG_COMBATABLE))
        return CTX_ACTION_NONE;

    if(G_Combat_GetBaseDamage(first) == 0)
        return CTX_ACTION_NONE;

    if(G_GetFactionID(first) == G_GetFactionID(hovered))
        return CTX_ACTION_NONE;

    if((G_FlagsGet(hovered) & ENTITY_FLAG_MARKER) || (G_FlagsGet(hovered) & ENTITY_FLAG_ZOMBIE))
        return CTX_ACTION_NONE;

    bool can_target = (G_FlagsGet(hovered) & ENTITY_FLAG_MOVABLE) 
                  && !(G_FlagsGet(hovered) & ENTITY_FLAG_RESOURCE);
    if(!(G_FlagsGet(hovered) & ENTITY_FLAG_COMBATABLE) && !can_target)
        return CTX_ACTION_NONE;

    if(!(G_FlagsGet(hovered) & ENTITY_FLAG_COMBATABLE) && can_target)
        return CTX_ACTION_NO_ATTACK;

    if(enemies(hovered, first)) {
        return CTX_ACTION_ATTACK;
    }else{
        return CTX_ACTION_NO_ATTACK;
    }
}

void G_Combat_AttackUnit(uint32_t uid, uint32_t target)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_ATTACK_UNIT,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_INT,
            .val.as_int = target
        }
    });
}

void G_Combat_StopAttack(uint32_t uid)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_STOP_ATTACK,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        }
    });
}

void G_Combat_AddTimeDelta(uint32_t delta)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_ADD_TIME_DELTA,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = delta
        }
    });
}

void G_Combat_AddRef(int faction_id, vec2_t pos)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_ADD_REF,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = faction_id
        },
        .args[1] = {
            .type = TYPE_VEC2,
            .val.as_vec2 = pos
        }
    });
}

void G_Combat_RemoveRef(int faction_id, vec2_t pos)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_REMOVE_REF,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = faction_id
        },
        .args[1] = {
            .type = TYPE_INT,
            .val.as_vec2 = pos
        }
    });
}

void G_Combat_UpdateRef(int oldfac, int newfac, vec2_t pos)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_UPDATE_REF,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = oldfac
        },
        .args[1] = {
            .type = TYPE_INT,
            .val.as_int = newfac
        },
        .args[2] = {
            .type = TYPE_VEC2,
            .val.as_vec2 = pos
        }
    });
}

bool G_Combat_IsDying(uint32_t uid)
{
    struct combatstate *cs = combatstate_get(uid);
    if(!cs)
        return false;
    return (cs->state == STATE_DEATH_ANIM_PLAYING);
}

int G_Combat_GetCurrentHP(uint32_t uid)
{
    struct combat_cmd *cmd;
    cmd = snoop_most_recent_command(COMBAT_CMD_SET_CURRENT_HP,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->args[1].val.as_int;
    }
    cmd = snoop_most_recent_command(COMBAT_CMD_ADD,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return 0;
    }

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    return cs->current_hp;
}

void G_Combat_SetBaseArmour(uint32_t uid, float armour_pc)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_BASE_ARMOUR,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_FLOAT,
            .val.as_float = armour_pc
        }
    });
}

float G_Combat_GetBaseArmour(uint32_t uid)
{
    struct combat_cmd *cmd;
    cmd = snoop_most_recent_command(COMBAT_CMD_SET_BASE_ARMOUR,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->args[1].val.as_float;
    }
    cmd = snoop_most_recent_command(COMBAT_CMD_ADD,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return 0.0f;
    }

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    return cs->stats.base_armour_pc;
}

void G_Combat_SetBaseDamage(uint32_t uid, int dmg)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_BASE_DAMAGE,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_INT,
            .val.as_int = dmg
        }
    });
}

int G_Combat_GetBaseDamage(uint32_t uid)
{
    struct combat_cmd *cmd;
    cmd = snoop_most_recent_command(COMBAT_CMD_SET_BASE_DAMAGE,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->args[1].val.as_int;
    }
    cmd = snoop_most_recent_command(COMBAT_CMD_ADD,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return 0;
    }

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    return cs->stats.base_dmg;
}

void G_Combat_SetCurrentHP(uint32_t uid, int hp)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_CURRENT_HP,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_INT,
            .val.as_int = hp
        }
    });
}

void G_Combat_SetMaxHP(uint32_t uid, int hp)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_MAX_HP,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_INT,
            .val.as_int = hp
        }
    });
}

int G_Combat_GetMaxHP(uint32_t uid)
{
    struct combat_cmd *cmd;
    cmd = snoop_most_recent_command(COMBAT_CMD_SET_MAX_HP,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->args[1].val.as_int;
    }
    cmd = snoop_most_recent_command(COMBAT_CMD_ADD,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return 0;
    }

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    return cs->stats.max_hp;
}

void G_Combat_SetRange(uint32_t uid, float range)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_RANGE,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_FLOAT,
            .val.as_float = range
        }
    });
}

void G_Combat_SetProjDesc(uint32_t uid, const struct proj_desc *pd)
{
    struct proj_desc *copy = malloc(sizeof(struct proj_desc));
    if(!copy)
        return;
    *copy = (struct proj_desc){
        pf_strdup(pd->basedir),
        pf_strdup(pd->pfobj),
        pd->scale,
        pd->speed,
    };

    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_PROJ_DESC,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_POINTER,
            .val.as_pointer = copy
        }
    });
}

float G_Combat_GetRange(uint32_t uid)
{
    struct combat_cmd *cmd;
    cmd = snoop_most_recent_command(COMBAT_CMD_SET_RANGE,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->args[1].val.as_float;
    }
    cmd = snoop_most_recent_command(COMBAT_CMD_ADD,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return 0.0f;
    }

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    return cs->stats.attack_range;
}

bool G_Combat_SaveState(struct SDL_RWops *stream)
{
    struct attr num_ents = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_entity_state_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_ents, "num_ents"));

    uint32_t curr_ticks = SDL_GetTicks();
    uint32_t key;
    struct combatstate curr;

    kh_foreach(s_entity_state_table, key, curr, {

        assert(G_EntityExists(key));
        struct attr uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = key
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid, "uid"));
    
        /* The HP is already loaded and set along with the entity */

        struct attr stance = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.stance
        };
        CHK_TRUE_RET(Attr_Write(stream, &stance, "stance"));

        struct attr state = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.state
        };
        CHK_TRUE_RET(Attr_Write(stream, &state, "state"));

        struct attr sticky = (struct attr){
            .type = TYPE_BOOL,
            .val.as_int = curr.sticky
        };
        CHK_TRUE_RET(Attr_Write(stream, &sticky, "sticky"));

        struct attr target_uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.target_uid 
        };
        CHK_TRUE_RET(Attr_Write(stream, &target_uid, "target_uid"));

        struct attr move_cmd_interrupted = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.move_cmd_interrupted
        };
        CHK_TRUE_RET(Attr_Write(stream, &move_cmd_interrupted, "move_cmd_interrupted"));

        struct attr move_cmd_attacking = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.move_cmd_attacking
        };
        CHK_TRUE_RET(Attr_Write(stream, &move_cmd_attacking, "move_cmd_attacking"));

        struct attr move_cmd_xz = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.move_cmd_xz
        };
        CHK_TRUE_RET(Attr_Write(stream, &move_cmd_xz, "move_cmd_xz"));

        struct attr attack_elapsed_ticks = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr_ticks - curr.attack_start_tick
        };
        CHK_TRUE_RET(Attr_Write(stream, &attack_elapsed_ticks, "attack_elapsed_ticks"));

        struct attr pd_basedir = (struct attr){
            .type = TYPE_STRING,
        };
        pf_strlcpy(pd_basedir.val.as_string, curr.pd.basedir, sizeof(pd_basedir.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &pd_basedir, "pd_basedir"));

        struct attr pd_pfobj = (struct attr){
            .type = TYPE_STRING,
        };
        pf_strlcpy(pd_pfobj.val.as_string, curr.pd.pfobj, sizeof(pd_pfobj.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &pd_pfobj, "pd_pfobj"));

        struct attr pd_scale  = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = curr.pd.scale,
        };
        CHK_TRUE_RET(Attr_Write(stream, &pd_scale, "pd_scale"));

        struct attr pd_speed  = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = curr.pd.speed,
        };
        CHK_TRUE_RET(Attr_Write(stream, &pd_speed, "pd_speed"));
        Sched_TryYield();
    });

    struct attr num_dying = (struct attr){
        .type = TYPE_INT,
        .val.as_int = vec_size(&s_dying_ents)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_dying, "num_dying"));
    Sched_TryYield();

    for(int i = 0; i < vec_size(&s_dying_ents); i++) {
    
        uint32_t curr_ent = vec_AT(&s_dying_ents, i);
        struct attr uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr_ent
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid, "dying_ent_uid"));
        Sched_TryYield();
    }

    return true;
}

bool G_Combat_LoadState(struct SDL_RWops *stream)
{
    /* Flush the commands submitted during loading */
    combat_update_gamestate();
    combat_process_cmds();

    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    Sched_TryYield();

    const size_t num_ents = attr.val.as_int;
    uint32_t curr_ticks = SDL_GetTicks();

    for(int i = 0; i < num_ents; i++) {
    
        uint32_t uid;
        struct combatstate *cs;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uid = attr.val.as_int;

        /* The entity should have already been loaded from the scripting state */
        khiter_t k = kh_get(state, s_entity_state_table, uid);
        CHK_TRUE_RET(k != kh_end(s_entity_state_table));
        cs = &kh_value(s_entity_state_table, k);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        cs->stance = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        cs->state = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        cs->sticky = attr.val.as_bool;

        if(cs->state == STATE_ATTACK_ANIM_PLAYING) {
            CHK_TRUE_RET(G_EntityExists(uid));
            E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_attack_anim_finish, 
                (void*)((uintptr_t)uid), G_RUNNING);
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        cs->target_uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        cs->move_cmd_interrupted = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        cs->move_cmd_attacking = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        cs->move_cmd_xz = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        cs->attack_start_tick = curr_ticks - attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        cs->pd.basedir = pf_strdup(attr.val.as_string);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        cs->pd.pfobj = pf_strdup(attr.val.as_string);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC3);
        cs->pd.scale = attr.val.as_vec3;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        cs->pd.speed = attr.val.as_float;
        Sched_TryYield();
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_dying = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_dying; i++) {
    
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t uid = attr.val.as_int;
        CHK_TRUE_RET(G_EntityExists(uid));
        vec_entity_push(&s_dying_ents, uid);

        uint32_t flags = G_FlagsGet(uid);
        if(flags & ENTITY_FLAG_ANIMATED) {
            E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_death_anim_finish, 
                (void*)((uintptr_t)uid), G_RUNNING);
        }else{
            Entity_DisappearAnimated(uid, s_map, on_disappear_finish, 
                (void*)((uintptr_t)uid));
        }
        Sched_TryYield();
    }

    return true;
}

