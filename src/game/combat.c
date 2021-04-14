/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2020 Eduard Permyakov 
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
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../phys/public/phys.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/mem.h"

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

#define CHK_TRUE_RET(_pred)         \
    do{                             \
        if(!(_pred))                \
            return false;           \
    }while(0)

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

KHASH_MAP_INIT_INT(state, struct combatstate)

static void on_attack_anim_finish(void *user, void *event);
static void on_death_anim_finish(void *user, void *event);

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

static khash_t(state)   *s_entity_state_table;
/* For saving/restoring state */
static vec_pentity_t     s_dying_ents;
static const struct map *s_map;
/* How many units of a faction currently currently occupy that bin.
 * For quickly finding that there are no enemy units nearby */
static uint16_t         *s_fac_refcnts[MAX_FACTIONS];

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

static void combatstate_set(const struct entity *ent, const struct combatstate *cs)
{
    assert(ent->flags & ENTITY_FLAG_COMBATABLE);

    int ret;
    khiter_t k = kh_put(state, s_entity_state_table, ent->uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_state_table, k) = *cs;
}

static void combatstate_remove(const struct entity *ent)
{
    assert(ent->flags & ENTITY_FLAG_COMBATABLE);

    khiter_t k = kh_get(state, s_entity_state_table, ent->uid);
    assert(k != kh_end(s_entity_state_table));
    kh_del(state, s_entity_state_table, k);
}

static bool pentities_equal(struct entity *const *a, struct entity *const *b)
{
    return ((*a) == (*b));
}

static void combat_dying_remove(const struct entity *ent)
{
    int idx = vec_pentity_indexof(&s_dying_ents, (struct entity*)ent, pentities_equal);
    if(idx == -1)
        return;
    vec_pentity_del(&s_dying_ents, idx);
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

static bool enemies(const struct entity *a, const struct entity *b)
{
    if(G_GetFactionID(a->uid) == G_GetFactionID(b->uid))
        return false;

    enum diplomacy_state ds;
    bool result = G_GetDiplomacyState(G_GetFactionID(a->uid), G_GetFactionID(b->uid), &ds);

    assert(result);
    return (ds == DIPLOMACY_STATE_WAR);
}

static bool enemies_in_bin(int faction_id, struct map_resolution binres, struct tile_desc td)
{
    uint16_t facs = G_GetFactions(NULL, NULL, NULL);

    size_t x = td.chunk_c * X_BINS_PER_CHUNK + td.tile_c;
    size_t z = td.chunk_r * Z_BINS_PER_CHUNK + td.tile_r;
    size_t idx = x * (binres.chunk_w * binres.tile_w) + z;

    for(int i = 0; facs; facs >>= 1, i++) {

        if(!(facs & 0x1))
            continue;
        if(i == faction_id)
            continue;

        enum diplomacy_state ds;
        G_GetDiplomacyState(faction_id, i, &ds);
        if(ds != DIPLOMACY_STATE_WAR)
            continue;

        if(s_fac_refcnts[i][idx] > 0)
            return true;
    }
    return false;
}

static bool maybe_enemy_near(const struct entity *ent)
{
    PERF_ENTER();
    vec2_t pos = G_Pos_GetXZ(ent->uid);
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
        if(enemies_in_bin(G_GetFactionID(ent->uid), binres, bin))
            PERF_RETURN(true);
    }}
    PERF_RETURN(false);
}

static void entity_move_in_range(const struct entity *ent, const struct entity *target)
{
    const struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs->stance != COMBAT_STANCE_HOLD_POSITION);
    if(cs->stats.attack_range == 0.0f) {
        G_Move_SetSurroundEntity(ent, target);
    }else{
        if(M_NavLocationsReachable(s_map, Entity_NavLayer(ent), 
            G_Pos_GetXZ(ent->uid), G_Pos_GetXZ(target->uid))) {

            G_Move_SetSurroundEntity(ent, target);
        }else{
            G_Move_SetEnterRange(ent, target, cs->stats.attack_range);
        }
    }
}

static bool entity_can_attack_melee(const struct entity *ent, const struct entity *target)
{
    if(!Entity_MaybeAdjacentFast(ent, target, 10.0))
        return false;
    return M_NavObjAdjacent(s_map, ent, target);
}

static bool entity_can_attack(const struct entity *ent, const struct entity *target)
{
    const struct combatstate *cs = combatstate_get(ent->uid);
    if(cs->stats.attack_range == 0.0f) {
        return entity_can_attack_melee(ent, target);
    }

    vec2_t xz_src = G_Pos_GetXZ(ent->uid);
    vec2_t xz_dst = G_Pos_GetXZ(target->uid);

    vec2_t delta;
    PFM_Vec2_Sub(&xz_src, &xz_dst, &delta);
    return (PFM_Vec2_Len(&delta) <= cs->stats.attack_range);
}

static bool valid_enemy(const struct entity *curr, void *arg)
{
    const struct entity *ent = arg;

    if(curr == ent)
        return false;
    if(!(curr->flags & ENTITY_FLAG_COMBATABLE))
        return false;
    if((curr->flags & ENTITY_FLAG_BUILDING) && !G_Building_IsFounded(curr))
        return false;
    if(!enemies(ent, curr))
        return false;

    struct combatstate *cs = combatstate_get(curr->uid);
    assert(cs);
    if(cs->state == STATE_DEATH_ANIM_PLAYING)
        return false;

    struct obb obb;
    Entity_CurrentOBB(curr, &obb, false);

    uint16_t pmask = G_GetPlayerControlledFactions();
    if(!G_Fog_ObjVisible(pmask, &obb))
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

static quat_t entity_turn_dir(struct entity *ent, const struct entity *target)
{
    vec2_t ent_pos_xz = G_Pos_GetXZ(ent->uid);
    vec2_t tar_pos_xz = G_Pos_GetXZ(target->uid);

    vec2_t ent_to_target;
    PFM_Vec2_Sub(&tar_pos_xz, &ent_pos_xz, &ent_to_target);

    if(PFM_Vec2_Len(&ent_to_target) < EPSILON) {
        return Entity_GetRot(ent->uid);
    }

    PFM_Vec2_Normal(&ent_to_target, &ent_to_target);
    quat_t curr = Entity_GetRot(ent->uid);
    return quat_from_vec(ent_to_target);
}

static void entity_turn_to_target(struct entity *ent, const struct entity *target)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);

    if(!cs->move_cmd_interrupted 
    && G_Move_GetDest(ent, &cs->move_cmd_xz, &cs->move_cmd_attacking)) {
        cs->move_cmd_interrupted = true; 
    }
    G_Move_Stop(ent);

    if(!(ent->flags & ENTITY_FLAG_MOVABLE)) {
        cs->state = STATE_CAN_ATTACK;
    }else{
        quat_t rot = entity_turn_dir(ent, target);
        G_Move_SetChangeDirection(ent, rot);
        cs->state = STATE_TURNING_TO_TARGET;
    }
}

static void on_disappear_finish(void *arg)
{
    struct entity *self = arg;
    assert(self);
    self->flags |= ENTITY_FLAG_INVISIBLE;
    E_Entity_Notify(EVENT_ENTITY_DISAPPEARED, self->uid, NULL, ES_ENGINE);
}

static void entity_die(struct entity *ent)
{
    G_Move_Stop(ent);

    if(ent->flags & ENTITY_FLAG_SELECTABLE) {
        G_Sel_Remove(ent);
        ent->flags &= ~ENTITY_FLAG_SELECTABLE;
    }

    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, ent->uid, on_attack_anim_finish);
    E_Global_Notify(EVENT_ENTITY_DIED, ent, ES_ENGINE);
    E_Entity_Notify(EVENT_ENTITY_DEATH, ent->uid, NULL, ES_ENGINE);
    vec_pentity_push(&s_dying_ents, ent);

    if(ent->flags & ENTITY_FLAG_ANIMATED && !(ent->flags & ENTITY_FLAG_BUILDING)) {

        struct combatstate *cs = combatstate_get(ent->uid);
        cs->state = STATE_DEATH_ANIM_PLAYING;
        E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, ent->uid, on_death_anim_finish, ent, G_RUNNING);

    }else{

        G_Zombiefy(ent, false);
        Entity_DisappearAnimated(ent, s_map, on_disappear_finish, ent);
    }
}

static void entity_melee_attack(const struct entity *self, struct entity *target)
{
    struct combatstate *cs = combatstate_get(self->uid);
    struct combatstate *target_cs = combatstate_get(cs->target_uid);

    float dmg = G_Combat_GetBaseDamage(self) * (1.0f - G_Combat_GetBaseArmour(target));
    target_cs->current_hp = MAX(0, target_cs->current_hp - dmg);

    if(target_cs->current_hp == 0 && target_cs->stats.max_hp > 0) {
        entity_die(target);
    }
}

static void entity_ranged_attack(const struct entity *self, struct entity *target)
{
    vec3_t ent_pos = Entity_CenterPos(self);
    vec3_t target_pos = Entity_CenterPos(target);
    float ent_dmg = G_Combat_GetBaseDamage(self);

    struct combatstate *cs = combatstate_get(self->uid);
    assert(cs);

    vec3_t vel;
    if(!P_Projectile_VelocityForTarget(ent_pos, target_pos, cs->pd.speed, &vel)) {
        /* We resort to just shooting nothing when we can't hit our target. This case 
         * should never be hit so long as the initial velocity is high enough */
        return;
    }

    P_Projectile_Add(ent_pos, vel, self->uid, G_GetFactionID(self->uid), 
        ent_dmg, PROJ_ONLY_HIT_COMBATABLE | PROJ_ONLY_HIT_ENEMIES, cs->pd);
}

static void on_death_anim_finish(void *user, void *event)
{
    struct entity *self = user;
    assert(self);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, self->uid, on_death_anim_finish);
    G_Zombiefy(self, true);
}

static void entity_combat_action(struct entity *ent)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);
    cs->state = STATE_CAN_ATTACK;

    struct entity *target = G_EntityForUID(cs->target_uid);
    if(!target || (target->flags & ENTITY_FLAG_ZOMBIE)) {
        return; /* Our target already got 'killed' */
    }

    struct combatstate *target_cs = combatstate_get(cs->target_uid);
    assert(target_cs);
    if(target_cs->state == STATE_DEATH_ANIM_PLAYING) {
        return;  /* Our target is dying */
    }

    quat_t target_dir = entity_turn_dir(ent, target);
    quat_t ent_rot = Entity_GetRot(ent->uid);
    float angle_diff = PFM_Quat_PitchDiff(&ent_rot, &target_dir);

    /* Ranged units fire their shot regardless */
    if(cs->stats.attack_range > 0.0f) {
        entity_ranged_attack(ent, target);
        return;
    }

    if(RAD_TO_DEG(fabs(angle_diff)) > 5.0f) {

        E_Entity_Notify(EVENT_ATTACK_END, ent->uid, NULL, ES_ENGINE);
        entity_turn_to_target(ent, target);
        return;
    }

    if(!entity_can_attack(ent, target)) {
        return; /* Target slipped out of range */
    }

    entity_melee_attack(ent, target);
}

static void on_attack_anim_finish(void *user, void *event)
{
    struct entity *self = user;
    assert(self);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, self->uid, on_attack_anim_finish);
    entity_combat_action(self);
}

static bool entity_dead(const struct entity *ent)
{
    if(!ent  /* dead and gone */
    || !(ent->flags & ENTITY_FLAG_COMBATABLE) /* zombie or stray target */
    || combatstate_get(ent->uid)->state == STATE_DEATH_ANIM_PLAYING) /* dying */
        return true;

    return false;
}

static void entity_target_enemy(struct entity *ent, const struct entity *enemy)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);

    if(entity_can_attack(ent, enemy)) {

        assert(cs->stance == COMBAT_STANCE_AGGRESSIVE 
            || cs->stance == COMBAT_STANCE_HOLD_POSITION);

        cs->target_uid = enemy->uid;
        entity_turn_to_target(ent, enemy);
        return;
    }

    if(cs->stance == COMBAT_STANCE_AGGRESSIVE && (ent->flags & ENTITY_FLAG_MOVABLE)) {

        cs->target_uid = enemy->uid;
        cs->state = STATE_MOVING_TO_TARGET;

        if(!cs->move_cmd_interrupted 
        && G_Move_GetDest(ent, &cs->move_cmd_xz, &cs->move_cmd_attacking)) {
            cs->move_cmd_interrupted = true; 
        }
        G_Move_SetSeekEnemies(ent);
    }
}

static void entity_stop_combat(const struct entity *ent)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);
    cs->state = STATE_NOT_IN_COMBAT; 

    if(!(ent->flags & ENTITY_FLAG_MOVABLE))
        return;

    if(cs->move_cmd_interrupted) {
        G_Move_SetDest(ent, cs->move_cmd_xz, cs->move_cmd_attacking);
        cs->move_cmd_interrupted = false;
    }else {
        G_Move_Stop(ent);
    }
}

static void on_20hz_tick(void *user, void *event)
{
    PERF_PUSH("combat::on_20hz_tick");

    uint32_t key;
    kh_foreach(s_entity_state_table, key, (struct combatstate){0}, {

        struct combatstate *curr = combatstate_get(key);
        struct entity *ent = G_EntityForUID(key);

        assert(ent->flags & ENTITY_FLAG_COMBATABLE);

        switch(curr->state) {
        case STATE_NOT_IN_COMBAT: 
        {
            if(curr->stance == COMBAT_STANCE_NO_ENGAGEMENT)
                break;

            if(ent->flags & ENTITY_FLAG_BUILDING && !G_Building_IsCompleted(ent))
                break;

            if(G_Combat_GetBaseDamage(ent) == 0)
                break;

            if(!maybe_enemy_near(ent))
                break;

            struct entity *enemy = G_Combat_ClosestEligibleEnemy(ent);
            if(!enemy)
                break;

            entity_target_enemy(ent, enemy);
            break;
        }
        case STATE_MOVING_TO_TARGET:
        {
            assert(ent->flags & ENTITY_FLAG_MOVABLE);

            /* Handle the case where our target dies before we reach it */
            struct entity *enemy = G_Combat_ClosestEligibleEnemy(ent);
            if(!enemy) {
                entity_stop_combat(ent);
                break;
            }

            /* And the case where a different target becomes even closer */
            if(enemy->uid != curr->target_uid) {
                curr->target_uid = enemy->uid;
            }

            /* Check if we're within attacking range of our target */
            if(entity_can_attack(ent, enemy)) {
                entity_turn_to_target(ent, enemy);
            }
            break;
        }
        case STATE_MOVING_TO_TARGET_LOCKED:
        {
            assert(ent->flags & ENTITY_FLAG_MOVABLE);

            struct entity *target = G_EntityForUID(curr->target_uid);
            if(!target || !(target->flags & ENTITY_FLAG_COMBATABLE)) {

                curr->state = STATE_NOT_IN_COMBAT;
                curr->sticky = false;
                G_Move_Stop(ent);
                break;
            }

            /* If our target goes out of vision, give up the pursuit */
            struct obb obb;
            Entity_CurrentOBB(target, &obb, false);

            uint16_t pmask = G_GetPlayerControlledFactions();
            if(!G_Fog_ObjVisible(pmask, &obb)) {
            
                curr->state = STATE_NOT_IN_COMBAT;
                curr->sticky = false;
                G_Move_Stop(ent);
                break;
            }

            /* Check if we're within attacking range of our target */
            if(entity_can_attack(ent, target)) {
                entity_turn_to_target(ent, target);
                break;
            }

            /* We approached the target, but it slipped away from us. Re-engage. */
            if(G_Move_Still(ent)) {
                entity_move_in_range(ent, target);
            }
            break;
        }
        case STATE_CAN_ATTACK:
        {
            const struct entity *target = G_EntityForUID(curr->target_uid);

            /* Our target could have 'died' or gotten out of combat range - check this first. */
            if(entity_dead(target) || !entity_can_attack(ent, target)) {

                if(curr->sticky) {

                    assert(curr->stance != COMBAT_STANCE_HOLD_POSITION);
                    if(!entity_dead(target)) {

                        E_Entity_Notify(EVENT_ATTACK_END, ent->uid, NULL, ES_ENGINE);
                        entity_move_in_range(ent, target);
                        curr->state = STATE_MOVING_TO_TARGET_LOCKED;
                        break;
                    }else{
                        curr->sticky = false;
                    }
                }

                /* Check if there's another suitable target */
                struct entity *enemy = G_Combat_ClosestEligibleEnemy(ent);
                if(!enemy) {
                    E_Entity_Notify(EVENT_ATTACK_END, ent->uid, NULL, ES_ENGINE);
                    entity_stop_combat(ent);
                    break;
                }

                if(curr->stance == COMBAT_STANCE_HOLD_POSITION && !entity_can_attack(ent, enemy)) {
                    E_Entity_Notify(EVENT_ATTACK_END, ent->uid, NULL, ES_ENGINE);
                    entity_stop_combat(ent);
                    break;
                }

                E_Entity_Notify(EVENT_ATTACK_END, ent->uid, NULL, ES_ENGINE);
                entity_target_enemy(ent, enemy);
                break;
            }

            /* Perform combat simulation between entities with targets within range */
            if(ent->flags & ENTITY_FLAG_ANIMATED) {
                curr->state = STATE_ATTACK_ANIM_PLAYING;
                E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, ent->uid, on_attack_anim_finish, ent, G_RUNNING);
            }else{
                curr->state = STATE_ATTACKING;
                curr->attack_start_tick = SDL_GetTicks();
            }

            break;
        }
        case STATE_TURNING_TO_TARGET: {

            const struct entity *target = G_EntityForUID(curr->target_uid);
            if(entity_dead(target) || !entity_can_attack(ent, target)) {
                entity_stop_combat(ent);
                break;
            }

            if(G_Move_Still(ent)) {

                curr->state = STATE_CAN_ATTACK;
                E_Entity_Notify(EVENT_ATTACK_START, ent->uid, NULL, ES_ENGINE);
            }
            break;
        }
        case STATE_ATTACKING: {

            uint32_t ticks = SDL_GetTicks();
            uint32_t period = DEFAULT_ATTACK_PERIOD * 1000.0f;
            if(!SDL_TICKS_PASSED(ticks, curr->attack_start_tick + period))
                break;

            entity_combat_action(ent);
            break;
        }
        case STATE_ATTACK_ANIM_PLAYING:
        case STATE_DEATH_ANIM_PLAYING:
            /* No-op */
            break;
        default: assert(0);
        };
    
    });
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
    const vec_pentity_t *sel = G_Sel_Get(&sel_type);
    size_t nattacking = 0;

    if(vec_size(sel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return;

    struct entity *first = vec_AT(sel, 0);
    struct entity *target = G_Sel_GetHovered();

    if(!target || !(target->flags & ENTITY_FLAG_COMBATABLE) || !enemies(first, target))
        return;

    for(int i = 0; i < vec_size(sel); i++) {

        struct entity *curr = vec_AT(sel, i);
        if(!(curr->flags & ENTITY_FLAG_COMBATABLE))
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

    uint32_t key;
    struct combatstate curr;

    kh_foreach(s_entity_state_table, key, curr, {

        vec2_t ent_pos = G_Pos_GetXZ(key);
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
        
            vec2_t delta;
            vec2_t target_pos = G_Pos_GetXZ(curr.target_uid);
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

        vec2_t ss_pos = Entity_TopScreenPos(G_EntityForUID(key), winw, winh);
        struct rect bounds = (struct rect){ss_pos.x - 75, ss_pos.y + 5, 150, 16};
        struct rgba color = (struct rgba){255, 0, 0, 255};
        UI_DrawText(s_name_for_state[curr.state], bounds, color);
    });
}

static void combat_render_ranges(void)
{
    uint32_t key;
    struct combatstate curr;

    kh_foreach(s_entity_state_table, key, curr, {

        if(curr.stats.attack_range == 0.0f)
            continue;

        vec2_t ent_pos = G_Pos_GetXZ(key);
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
    struct entity *target = G_EntityForUID(hit->ent_uid);

    if(entity_dead(target))
        return;

    float dmg = hit->cookie * (1.0f - G_Combat_GetBaseArmour(target));
    struct combatstate *cs = combatstate_get(hit->ent_uid);
    cs->current_hp = MAX(0, cs->current_hp - dmg);

    if(cs->current_hp == 0 && cs->stats.max_hp > 0) {
        entity_die(target);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Combat_Init(const struct map *map)
{
    if(NULL == (s_entity_state_table = kh_init(state)))
        return false;

    struct map_resolution res;
    M_GetResolution(map, &res);

    for(int i = 0; i < MAX_FACTIONS; i++) {
        s_fac_refcnts[i] = calloc((res.chunk_w * X_BINS_PER_CHUNK) 
                                * (res.chunk_h * Z_BINS_PER_CHUNK) 
                                * sizeof(uint16_t), 1);
        if(!s_fac_refcnts[i])
            goto fail_refcnts;
    }

    vec_pentity_init(&s_dying_ents);
    E_Global_Register(EVENT_30HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, G_ALL);
    E_Global_Register(EVENT_PROJECTILE_HIT, on_proj_hit, NULL, G_RUNNING);
    s_map = map;
    return true;

fail_refcnts:
    for(int i = 0; i < MAX_FACTIONS; i++)
        PF_FREE(s_fac_refcnts[i]);
    kh_destroy(state, s_entity_state_table);
    return false;
}

void G_Combat_Shutdown(void)
{
    s_map = NULL;
    E_Global_Unregister(EVENT_30HZ_TICK, on_20hz_tick);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    E_Global_Unregister(EVENT_PROJECTILE_HIT, on_proj_hit);
    vec_pentity_destroy(&s_dying_ents);
    for(int i = 0; i < MAX_FACTIONS; i++)
        PF_FREE(s_fac_refcnts[i]);
    kh_destroy(state, s_entity_state_table);
}

void G_Combat_AddEntity(const struct entity *ent, enum combat_stance initial)
{
    assert(combatstate_get(ent->uid) == NULL);
    assert(ent->flags & ENTITY_FLAG_COMBATABLE);

    struct combatstate new_cs = (struct combatstate) {
        .stats = {0},
        .current_hp = 0,
        .stance = initial,
        .state = STATE_NOT_IN_COMBAT,
        .sticky = false,
        .move_cmd_interrupted = false,
        .pd = combat_default_proj(),
    };
    combatstate_set(ent, &new_cs);
}

void G_Combat_RemoveEntity(const struct entity *ent)
{
    if(!(ent->flags & ENTITY_FLAG_COMBATABLE))
        return;

    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);

    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, ent->uid, on_attack_anim_finish);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, ent->uid, on_death_anim_finish);

    if(cs->state == STATE_ATTACK_ANIM_PLAYING
    || cs->state == STATE_CAN_ATTACK) {
        E_Entity_Notify(EVENT_ATTACK_END, ent->uid, NULL, ES_ENGINE);
    }

    PF_FREE(cs->pd.basedir);
    PF_FREE(cs->pd.pfobj);

    combat_dying_remove(ent);
    combatstate_remove(ent);
}

bool G_Combat_SetStance(const struct entity *ent, enum combat_stance stance)
{
    assert(ent->flags & ENTITY_FLAG_COMBATABLE);
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);

    if(stance == cs->stance)
        return true;

    if(stance == COMBAT_STANCE_NO_ENGAGEMENT) {
        G_Combat_StopAttack(ent);
    }

    if(stance == COMBAT_STANCE_HOLD_POSITION 
    && (cs->state == STATE_MOVING_TO_TARGET || cs->state == STATE_MOVING_TO_TARGET_LOCKED)) {

        G_Move_RemoveEntity(ent);
        cs->state = STATE_NOT_IN_COMBAT;
        cs->move_cmd_interrupted = false;
    }

    cs->stance = stance;
    return true;
}

void G_Combat_ClearSavedMoveCmd(const struct entity *ent)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    if(cs) {
        cs->move_cmd_interrupted = false;
    }
}

int G_Combat_CurrContextualAction(void)
{
    struct entity *hovered = G_Sel_GetHovered();
    if(!hovered)
        return CTX_ACTION_NONE;

    if(M_MouseOverMinimap(s_map))
        return CTX_ACTION_NONE;

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    if(S_UI_MouseOverWindow(mouse_x, mouse_y))
        return CTX_ACTION_NONE;

    enum selection_type sel_type;
    const vec_pentity_t *sel = G_Sel_Get(&sel_type);

    if(vec_size(sel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return CTX_ACTION_NONE;

    const struct entity *first = vec_AT(sel, 0);
    if(!(first->flags & ENTITY_FLAG_COMBATABLE))
        return CTX_ACTION_NONE;

    if(G_Combat_GetBaseDamage(first) == 0)
        return CTX_ACTION_NONE;

    if(G_GetFactionID(first->uid) == G_GetFactionID(hovered->uid))
        return CTX_ACTION_NONE;

    if((hovered->flags & ENTITY_FLAG_MARKER) || (hovered->flags & ENTITY_FLAG_ZOMBIE))
        return CTX_ACTION_NONE;

    bool can_target = (hovered->flags & ENTITY_FLAG_MOVABLE) && !(hovered->flags & ENTITY_FLAG_RESOURCE);
    if(!(hovered->flags & ENTITY_FLAG_COMBATABLE) && !can_target)
        return CTX_ACTION_NONE;

    if(!(hovered->flags & ENTITY_FLAG_COMBATABLE) && can_target)
        return CTX_ACTION_NO_ATTACK;

    if(enemies(hovered, first)) {
        return CTX_ACTION_ATTACK;
    }else{
        return CTX_ACTION_NO_ATTACK;
    }
}

void G_Combat_AttackUnit(const struct entity *ent, const struct entity *target)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);

    G_Combat_StopAttack(ent);
    cs->stance = COMBAT_STANCE_AGGRESSIVE;

    if(ent->flags & ENTITY_FLAG_MOVABLE) {
    
        cs->sticky = true;
        cs->target_uid = target->uid;
        cs->state = STATE_MOVING_TO_TARGET_LOCKED;
        cs->move_cmd_interrupted = false;

        entity_move_in_range(ent, target);

    }else if(entity_can_attack(ent, target)) {
    
        cs->sticky = true;
        cs->target_uid = target->uid;
        cs->state = STATE_CAN_ATTACK;
    }
}

void G_Combat_StopAttack(const struct entity *ent)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    if(!cs)
        return;

    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, ent->uid, on_attack_anim_finish);

    if(cs->state == STATE_ATTACK_ANIM_PLAYING
    || cs->state == STATE_CAN_ATTACK) {
        E_Entity_Notify(EVENT_ATTACK_END, ent->uid, NULL, ES_ENGINE);
    }

    cs->state = STATE_NOT_IN_COMBAT;

    if(cs->move_cmd_interrupted) {
        G_Move_SetDest(ent, cs->move_cmd_xz, cs->move_cmd_attacking);
        cs->move_cmd_interrupted = false;
    }
}

struct entity *G_Combat_ClosestEligibleEnemy(const struct entity *ent)
{
    vec2_t pos = G_Pos_GetXZ(ent->uid);
    struct entity *ents[128];
    size_t nents =  G_Pos_EntsInCircleWithPred(pos, TARGET_ACQUISITION_RANGE, ents, 
        ARR_SIZE(ents), valid_enemy, (void*)ent);

    if(!nents)
        return NULL;

    float min_dist = INFINITY;
    struct entity *ret = NULL;

    for(int i = 0; i < nents; i++) {
    
        if(M_NavObjAdjacent(s_map, ent, ents[i]))
            return ents[i];

        vec2_t enemy_pos = G_Pos_GetXZ(ents[i]->uid);
        vec2_t delta;
        PFM_Vec2_Sub(&pos, &enemy_pos, &delta);
        assert(PFM_Vec2_Len(&delta) <= TARGET_ACQUISITION_RANGE);
        float dist = PFM_Vec2_Len(&delta);

        if(dist < min_dist) {
            min_dist = dist;
            ret = ents[i];
        }
    }
    return ret;
}

void G_Combat_AddTimeDelta(uint32_t delta)
{
    uint32_t key;
    kh_foreach(s_entity_state_table, key, (struct combatstate){0}, {

        struct combatstate *curr = combatstate_get(key);
        if(curr->state != STATE_ATTACKING)
            continue;
        curr->attack_start_tick += delta;
    });
}

void G_Combat_AddRef(int faction_id, vec2_t pos)
{
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

    s_fac_refcnts[faction_id][idx]++;
}

void G_Combat_RemoveRef(int faction_id, vec2_t pos)
{
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
    s_fac_refcnts[faction_id][idx]--;
}

void G_Combat_UpdateRef(int oldfac, int newfac, vec2_t pos)
{
    G_Combat_RemoveRef(oldfac, pos);
    G_Combat_AddRef(newfac, pos);
}

bool G_Combat_IsDying(uint32_t uid)
{
    struct combatstate *cs = combatstate_get(uid);
    if(!cs)
        return false;
    return (cs->state == STATE_DEATH_ANIM_PLAYING);
}

int G_Combat_GetCurrentHP(const struct entity *ent)
{
    assert(ent->flags & ENTITY_FLAG_COMBATABLE);

    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);
    return cs->current_hp;
}

void G_Combat_SetBaseArmour(const struct entity *ent, float armour_pc)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);
    cs->stats.base_armour_pc = armour_pc;
}

float G_Combat_GetBaseArmour(const struct entity *ent)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);
    return cs->stats.base_armour_pc;
}

void G_Combat_SetBaseDamage(const struct entity *ent, int dmg)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);
    cs->stats.base_dmg = dmg;
}

int G_Combat_GetBaseDamage(const struct entity *ent)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);
    return cs->stats.base_dmg;
}

void G_Combat_SetCurrentHP(const struct entity *ent, int hp)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);
    cs->current_hp = MIN(hp, cs->stats.max_hp);
}

void G_Combat_SetMaxHP(const struct entity *ent, int hp)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);
    cs->stats.max_hp = hp;
}

int G_Combat_GetMaxHP(const struct entity *ent)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);
    return cs->stats.max_hp;
}

void G_Combat_SetRange(const struct entity *ent, float range)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);
    cs->stats.attack_range = range;
}

void G_Combat_SetProjDesc(const struct entity *ent, const struct proj_desc *pd)
{
    struct combatstate *cs = combatstate_get(ent->uid);
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

float G_Combat_GetRange(const struct entity *ent)
{
    struct combatstate *cs = combatstate_get(ent->uid);
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
    });

    struct attr num_dying = (struct attr){
        .type = TYPE_INT,
        .val.as_int = vec_size(&s_dying_ents)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_dying, "num_dying"));

    for(int i = 0; i < vec_size(&s_dying_ents); i++) {
    
        const struct entity *curr_ent = vec_AT(&s_dying_ents, i);
        struct attr uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr_ent->uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid, "dying_ent_uid"));
    }

    return true;
}

bool G_Combat_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);

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
            struct entity *ent = G_EntityForUID(uid);
            CHK_TRUE_RET(ent);
            E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_attack_anim_finish, ent, G_RUNNING);
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
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_dying = attr.val.as_int;

    for(int i = 0; i < num_dying; i++) {
    
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t uid = attr.val.as_int;

        struct entity *ent = G_EntityForUID(uid);
        CHK_TRUE_RET(ent);
        vec_pentity_push(&s_dying_ents, ent);
        if(ent->flags & ENTITY_FLAG_ANIMATED) {
            E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_death_anim_finish, ent, G_RUNNING);
        }else{
            Entity_DisappearAnimated(ent, s_map, on_disappear_finish, ent);
        }
    }

    return true;
}

