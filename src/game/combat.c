/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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
#include "../event.h"
#include "../entity.h"
#include "public/game.h"
#include "../lib/public/khash.h"

#include <assert.h>
#include <float.h>


#define ENEMY_TARGET_ACQUISITION_RANGE (50.0f)
#define ENEMY_MELEE_ATTACK_RANGE       (5.0f)
#define EPSILON                        (1.0f/1024)
#define MAX(a, b)                      ((a) > (b) ? (a) : (b))

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
 *                 +->[STATE_CAN_ATTACK]<---------+
 *                      |(target alive)           |
 *                      V                         |(anim cycle finishes)
 *                    [STATE_ATTACK_ANIM_PLAYING]-+
 */

struct combatstate{
    int                current_hp;
    enum combat_stance stance;
    enum{
        STATE_NOT_IN_COMBAT,
        STATE_MOVING_TO_TARGET,
        STATE_CAN_ATTACK,
        STATE_ATTACK_ANIM_PLAYING,
    }state;
    struct entity     *target;
    vec2_t             prev_target_pos;
    /* If the target gained a target while moving, save and restore
     * its' intial move command once it finishes combat. */
    bool               move_cmd_interrupted;
    vec2_t             move_cmd_xz;
};

KHASH_MAP_INIT_INT(state, struct combatstate)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

khash_t(state) *s_entity_state_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/* The returned pointer is guaranteed to be valid to write to for
 * so long as we don't add anything to the table. At that point, there
 * is a case that a 'realloc' might take place. */
static struct combatstate *combatstate_get(const struct entity *ent)
{
    khiter_t k = kh_get(state, s_entity_state_table, ent->uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;

    assert(ent->flags & ENTITY_FLAG_COMBATABLE);
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
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
}

static bool enemies(const struct entity *a, const struct entity *b)
{
    if(a->faction_id == b->faction_id)
        return false;
    enum diplomacy_state ds;
    bool result = G_GetDiplomacyState(a->faction_id, b->faction_id, &ds);
    assert(result);
    return (ds == DIPLOMACY_STATE_WAR);
}

static float ents_distance(const struct entity *a, const struct entity *b)
{
    vec2_t dist;
    vec2_t xz_pos_a = (vec2_t){a->pos.x, a->pos.z};
    vec2_t xz_pos_b = (vec2_t){b->pos.x, b->pos.z};
    PFM_Vec2_Sub(&xz_pos_a, &xz_pos_b, &dist);
    return PFM_Vec2_Len(&dist) - a->selection_radius - b->selection_radius;
}

static struct entity *closest_enemy_in_range(const struct entity *ent, const khash_t(entity) *all)
{
    uint32_t key;
    struct entity *curr;

    float min_dist = FLT_MAX;
    struct entity *ret = NULL;

    kh_foreach(all, key, curr, {

        if(key == ent->uid)
            continue;
        if(!(curr->flags & ENTITY_FLAG_COMBATABLE))
            continue;
        if(!enemies(ent, curr))
            continue;
   
        float dist = ents_distance(ent, curr);
        if(dist <= ENEMY_TARGET_ACQUISITION_RANGE && dist < min_dist) {
            min_dist = dist; 
            ret = curr;
        }
    });

    return ret;
}

static quat_t quat_from_vec(vec2_t dir)
{
    assert(PFM_Vec2_Len(&dir) > EPSILON);

    float angle_rad = atan2(dir.raw[1], dir.raw[0]) - M_PI/2.0f;
    return (quat_t) {
        0.0f, 
        1.0f * sin(angle_rad / 2.0f),
        0.0f,
        cos(angle_rad / 2.0f)
    };
}

static void entity_turn_to_target(struct entity *ent, const struct entity *target)
{
    vec2_t ent_pos_xz = (vec2_t){ent->pos.x, ent->pos.z};
    vec2_t tar_pos_xz = (vec2_t){target->pos.x, target->pos.z};

    vec2_t ent_to_target;
    PFM_Vec2_Sub(&tar_pos_xz, &ent_pos_xz, &ent_to_target);
    PFM_Vec2_Normal(&ent_to_target, &ent_to_target);
    ent->rotation = quat_from_vec(ent_to_target);
}

static void on_attack_anim_finish(void *user, void *event)
{
    const struct entity *self = user;
    assert(self);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, self->uid, on_attack_anim_finish);

    struct combatstate *cs = combatstate_get(self);
    assert(cs);
    assert(cs->state == STATE_ATTACK_ANIM_PLAYING);
    assert(cs->target);

    cs->state = STATE_CAN_ATTACK;

    if(ents_distance(self, cs->target) <= ENEMY_MELEE_ATTACK_RANGE) {

        struct combatstate *target_cs = combatstate_get(cs->target);
        if(!target_cs)
            return; /* Our target already got 'killed' */

        float dmg = self->ca.base_dmg * (1.0f - cs->target->ca.base_armour_pc);
        target_cs->current_hp = MAX(0.0f, target_cs->current_hp - dmg);

        if(target_cs->current_hp == 0.0f) {

            G_Combat_RemoveEntity(cs->target);
            G_Move_RemoveEntity(cs->target);
            E_Entity_Notify(EVENT_ENTITY_DEATH, cs->target->uid, NULL, ES_ENGINE);
            cs->target->flags &= ~ENTITY_FLAG_COMBATABLE;

            if(cs->target->flags & ENTITY_FLAG_SELECTABLE) {
            
                G_Sel_Remove(cs->target);
                cs->target->flags &= ~ENTITY_FLAG_SELECTABLE;
            }
        }
    }
}

static void on_30hz_tick(void *user, void *event)
{
    uint32_t key;
    struct entity *curr;

    kh_foreach(G_GetDynamicEntsSet(), key, curr, {

        if(!(curr->flags & ENTITY_FLAG_COMBATABLE))
            continue;

        struct combatstate *cs = combatstate_get(curr);
        assert(cs);

        switch(cs->state) {
        case STATE_NOT_IN_COMBAT: 
        {
            if(cs->stance == COMBAT_STANCE_NO_ENGAGEMENT)
                break;

            /* Find and assign targets for entities. Make the entity move towards its' target. */
            struct entity *enemy;
            if((enemy = closest_enemy_in_range(curr, G_GetDynamicEntsSet() )) != NULL) {

                if(ents_distance(curr, enemy) <= ENEMY_MELEE_ATTACK_RANGE) {

                    assert(cs->stance == COMBAT_STANCE_AGGRESSIVE 
                        || cs->stance == COMBAT_STANCE_HOLD_POSITION);

                    cs->target = enemy;
                    cs->state = STATE_CAN_ATTACK;
                    G_Move_RemoveEntity(curr);
                    entity_turn_to_target(curr, enemy);
                    E_Entity_Notify(EVENT_ATTACK_START, curr->uid, NULL, ES_ENGINE);
                
                }else if(cs->stance == COMBAT_STANCE_AGGRESSIVE) {

                    cs->target = enemy;
                    cs->state = STATE_MOVING_TO_TARGET;
                    cs->prev_target_pos = (vec2_t){enemy->pos.x, enemy->pos.z};

                    vec2_t move_dest_xz;
                    if(!cs->move_cmd_interrupted && G_Move_GetDest(curr, &move_dest_xz)) {
                        cs->move_cmd_interrupted = true; 
                        cs->move_cmd_xz = move_dest_xz;
                    }
                
                    vec2_t enemy_pos_xz = (vec2_t){enemy->pos.x, enemy->pos.z};
                    G_Move_SetDest(curr, enemy_pos_xz);
                }
            }
            break;
        }
        case STATE_MOVING_TO_TARGET:
        {
            assert(cs->target);

            /* Handle the case where our target dies before we reach it */
            struct entity *enemy = closest_enemy_in_range(curr, G_GetDynamicEntsSet());
            if(!enemy) {

                cs->state = STATE_NOT_IN_COMBAT; 
                cs->target = NULL;

                if(cs->move_cmd_interrupted) {
                    G_Move_SetDest(curr, cs->move_cmd_xz);
                    cs->move_cmd_interrupted = false;
                }
                break;
            /* And the case where a different target becomes even closer */
            }else if(enemy != cs->target) {
            
                vec2_t enemy_pos_xz = (vec2_t){enemy->pos.x, enemy->pos.z};
                G_Move_SetDest(curr, enemy_pos_xz);
                cs->target = enemy;
            }

            /* Check if we're within attacking range of our target */
            if(ents_distance(curr, cs->target) <= ENEMY_MELEE_ATTACK_RANGE) {

                cs->state = STATE_CAN_ATTACK;
                G_Move_RemoveEntity(curr);
                entity_turn_to_target(curr, enemy);
                E_Entity_Notify(EVENT_ATTACK_START, curr->uid, NULL, ES_ENGINE);

            /* If not, update the seek position for a moving target */
            }else if(cs->prev_target_pos.raw[0] != cs->target->pos.x 
                  || cs->prev_target_pos.raw[1] != cs->target->pos.z) {
                  
                vec2_t enemy_pos_xz = (vec2_t){cs->target->pos.x, cs->target->pos.z};
                G_Move_SetDest(curr, enemy_pos_xz);
                cs->prev_target_pos = enemy_pos_xz;
            }
        
            break;
        }
        case STATE_CAN_ATTACK:
        {
            /* Perform combat simulation between entities with targets within range */
            assert(cs->target);

            /* Our target could have 'died' and had its' combatstate removed - check this first. */
            struct combatstate *target_cs;
            if((target_cs = combatstate_get(cs->target)) == NULL
            || ents_distance(curr, cs->target) > ENEMY_MELEE_ATTACK_RANGE) {

                cs->state = STATE_NOT_IN_COMBAT; 
                E_Entity_Notify(EVENT_ATTACK_END, curr->uid, NULL, ES_ENGINE);

                if(cs->move_cmd_interrupted) {
                    G_Move_SetDest(curr, cs->move_cmd_xz);
                    cs->move_cmd_interrupted = false;
                }

            }else{
                cs->state = STATE_ATTACK_ANIM_PLAYING;
                E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, curr->uid, on_attack_anim_finish, curr);
            }

            break;
        }
        case STATE_ATTACK_ANIM_PLAYING:
            /* No-op */
            break;
        default: assert(0);
        };
    
    });
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Combat_Init(void)
{
    if(NULL == (s_entity_state_table = kh_init(state)))
        return false;

    E_Global_Register(EVENT_30HZ_TICK, on_30hz_tick, NULL, G_RUNNING);
    return true;
}

void G_Combat_Shutdown(void)
{
    E_Global_Unregister(EVENT_30HZ_TICK, on_30hz_tick);
    kh_destroy(state, s_entity_state_table);
}

void G_Combat_AddEntity(const struct entity *ent, enum combat_stance initial)
{
    assert(combatstate_get(ent) == NULL);
    assert(ent->flags & ENTITY_FLAG_COMBATABLE);

    struct combatstate new_cs = (struct combatstate) {
        .current_hp = ent->ca.max_hp,
        .stance = initial,
        .state = STATE_NOT_IN_COMBAT,
        .target = NULL,
        .move_cmd_interrupted = false
    };
    combatstate_set(ent, &new_cs);
}

void G_Combat_RemoveEntity(const struct entity *ent)
{
    if(!(ent->flags & ENTITY_FLAG_COMBATABLE))
        return;

    struct combatstate *cs = combatstate_get(ent);
    assert(cs);
    if(cs->state == STATE_ATTACK_ANIM_PLAYING) {
        E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, ent->uid, on_attack_anim_finish);
    }
    if(cs->state == STATE_ATTACK_ANIM_PLAYING
    || cs->state == STATE_CAN_ATTACK) {
        E_Entity_Notify(EVENT_ATTACK_END, ent->uid, NULL, ES_ENGINE);
    }
    combatstate_remove(ent);
}

bool G_Combat_SetStance(const struct entity *ent, enum combat_stance stance)
{
    struct combatstate *cs = combatstate_get(ent);
    if(!cs)
        return false;

    if(stance == cs->stance)
        return true;

    if(stance == COMBAT_STANCE_NO_ENGAGEMENT) {
        G_Combat_StopAttack(ent);
    }

    if(stance == COMBAT_STANCE_HOLD_POSITION && cs->state == STATE_MOVING_TO_TARGET) {

        G_Move_RemoveEntity(ent);
        cs->state = STATE_NOT_IN_COMBAT;
        cs->target = NULL;
        cs->move_cmd_interrupted = false;
    }

    cs->stance = stance;
    return true;
}

void G_Combat_ClearSavedMoveCmd(const struct entity *ent)
{
    struct combatstate *cs = combatstate_get(ent);
    if(cs) {
        cs->move_cmd_interrupted = false;
    }
}

void G_Combat_StopAttack(const struct entity *ent)
{
    struct combatstate *cs = combatstate_get(ent);
    if(!cs)
        return;

    if(cs->state == STATE_ATTACK_ANIM_PLAYING) {
        E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, ent->uid, on_attack_anim_finish);
    }

    if(cs->state == STATE_ATTACK_ANIM_PLAYING
    || cs->state == STATE_CAN_ATTACK) {
        E_Entity_Notify(EVENT_ATTACK_END, ent->uid, NULL, ES_ENGINE);
    }

    cs->state = STATE_NOT_IN_COMBAT;
    cs->target = NULL;

    if(cs->move_cmd_interrupted) {
        G_Move_SetDest(ent, cs->move_cmd_xz);
        cs->move_cmd_interrupted = false;
    }
}

int G_Combat_GetCurrentHP(const struct entity *ent)
{
    assert(ent->flags & ENTITY_FLAG_COMBATABLE);

    struct combatstate *cs = combatstate_get(ent);
    assert(cs);
    return cs->current_hp;
}

