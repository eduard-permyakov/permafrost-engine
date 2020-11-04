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
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"

#include <assert.h>
#include <float.h>


#define ENEMY_TARGET_ACQUISITION_RANGE (50.0f)
#define ENEMY_MELEE_ATTACK_RANGE       (5.0f)
#define EPSILON                        (1.0f/1024)
#define MAX(a, b)                      ((a) > (b) ? (a) : (b))
#define MIN(a, b)                      ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)                    (sizeof(a)/sizeof(a[0]))

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
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
 *                 +->[STATE_CAN_ATTACK]<---------+
 *                      |(target alive)           |
 *                      V                         |(anim cycle finishes)
 *                    [STATE_ATTACK_ANIM_PLAYING]-+
 * 
 * From any of the states, an entity can move to the [STATE_DEATH_ANIM_PLAYING] 
 * state upon receiving a fatal hit. At the next EVENT_ANIM_CYCLE_FINISHED
 * event, the entity is reaped.
 */

struct combatstats{
    int   base_dmg;         /* The base damage per hit */
    float base_armour_pc;   /* Percentage of damage blocked. Valid range: [0.0 - 1.0] */
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
    }state;
    bool               sticky;
    uint32_t           target_uid;
    /* If the target gained a target while moving, save and restore
     * its' intial move command once it finishes combat. */
    bool               move_cmd_interrupted;
    vec2_t             move_cmd_xz;
};

KHASH_MAP_INIT_INT(state, struct combatstate)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const char *s_name_for_state[] = {
    [STATE_NOT_IN_COMBAT]           = "NOT_IN_COMBAT",
    [STATE_MOVING_TO_TARGET]        = "MOVING_TO_TARGET",
    [STATE_MOVING_TO_TARGET_LOCKED] = "MOVING_TO_TARGET_LOCKED",
    [STATE_CAN_ATTACK]              = "STATE_CAN_ATTACK",
    [STATE_ATTACK_ANIM_PLAYING]     = "ATTACK_ANIM_PLAYING",
    [STATE_DEATH_ANIM_PLAYING]      = "DEATH_ANIM_PLAYING"
};

static khash_t(state) *s_entity_state_table;
/* For saving/restoring state */
static vec_pentity_t   s_dying_ents;

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
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
}

static bool pentities_equal(struct entity *const *a, struct entity *const *b)
{
    return ((*a) == (*b));
}

static void dying_remove(const struct entity *ent)
{
    int idx = vec_pentity_indexof(&s_dying_ents, (struct entity*)ent, pentities_equal);
    if(idx == -1)
        return;
    vec_pentity_del(&s_dying_ents, idx);
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
    vec2_t xz_pos_a = G_Pos_GetXZ(a->uid);
    vec2_t xz_pos_b = G_Pos_GetXZ(b->uid);
    PFM_Vec2_Sub(&xz_pos_a, &xz_pos_b, &dist);
    return PFM_Vec2_Len(&dist) - a->selection_radius - b->selection_radius;
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
    vec2_t ent_pos_xz = G_Pos_GetXZ(ent->uid);
    vec2_t tar_pos_xz = G_Pos_GetXZ(target->uid);

    vec2_t ent_to_target;
    PFM_Vec2_Sub(&tar_pos_xz, &ent_pos_xz, &ent_to_target);
    PFM_Vec2_Normal(&ent_to_target, &ent_to_target);
    ent->rotation = quat_from_vec(ent_to_target);
}

static void on_death_anim_finish(void *user, void *event)
{
    struct entity *self = user;
    assert(self);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, self->uid, on_death_anim_finish);
    G_Zombiefy(self);
}

static void on_attack_anim_finish(void *user, void *event)
{
    const struct entity *self = user;
    assert(self);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, self->uid, on_attack_anim_finish);

    struct combatstate *cs = combatstate_get(self->uid);
    assert(cs);
    assert(cs->state == STATE_ATTACK_ANIM_PLAYING);

    cs->state = STATE_CAN_ATTACK;

    struct entity *target = G_EntityForUID(cs->target_uid);
    if(!target || (target->flags & ENTITY_FLAG_ZOMBIE))
        return; /* Our target already got 'killed' */

    struct combatstate *target_cs = combatstate_get(cs->target_uid);
    assert(target_cs);
    if(target_cs->state == STATE_DEATH_ANIM_PLAYING)
        return; 

    if(ents_distance(self, target) <= ENEMY_MELEE_ATTACK_RANGE) {

        float dmg = G_Combat_GetBaseDamage(self) * (1.0f - G_Combat_GetBaseArmour(target));
        target_cs->current_hp = MAX(0, target_cs->current_hp - dmg);

        if(target_cs->current_hp == 0 && target->max_hp > 0) {

            G_Move_Stop(target);

            if(target->flags & ENTITY_FLAG_SELECTABLE) {
                G_Sel_Remove(target);
                target->flags &= ~ENTITY_FLAG_SELECTABLE;
            }

            E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, cs->target_uid, on_attack_anim_finish);
            E_Global_Notify(EVENT_ENTITY_DIED, target, ES_ENGINE);
            E_Entity_Notify(EVENT_ENTITY_DEATH, cs->target_uid, NULL, ES_ENGINE);

            if(target->flags & ENTITY_FLAG_ANIMATED) {
                E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, cs->target_uid, on_death_anim_finish, target, G_RUNNING);
            }else{
                G_Zombiefy(target);
            }

            vec_pentity_push(&s_dying_ents, target);
            target_cs->state = STATE_DEATH_ANIM_PLAYING;
        }
    }
}

static bool entity_dead(const struct entity *ent)
{
    if(!ent  /* dead and gone */
    || (ent->flags & ENTITY_FLAG_ZOMBIE) /* zombie */
    || combatstate_get(ent->uid)->state == STATE_DEATH_ANIM_PLAYING) /* dying */
        return true;

    return false;
}

static void on_30hz_tick(void *user, void *event)
{
    PERF_ENTER();

    uint32_t key;
    struct entity *curr;
    (void)key;

    kh_foreach(G_GetDynamicEntsSet(), key, curr, {

        if(!(curr->flags & ENTITY_FLAG_COMBATABLE))
            continue;

        struct combatstate *cs = combatstate_get(curr->uid);
        assert(cs);

        switch(cs->state) {
        case STATE_NOT_IN_COMBAT: 
        {
            if(cs->stance == COMBAT_STANCE_NO_ENGAGEMENT)
                break;

            /* Make the entity seek enemy units. */
            struct entity *enemy;
            if((enemy = G_Combat_ClosestEligibleEnemy(curr)) != NULL) {

                if(ents_distance(curr, enemy) <= ENEMY_MELEE_ATTACK_RANGE) {

                    assert(cs->stance == COMBAT_STANCE_AGGRESSIVE 
                        || cs->stance == COMBAT_STANCE_HOLD_POSITION);

                    cs->target_uid = enemy->uid;
                    cs->state = STATE_CAN_ATTACK;

                    entity_turn_to_target(curr, enemy);
                    E_Entity_Notify(EVENT_ATTACK_START, curr->uid, NULL, ES_ENGINE);
                
                }else if(cs->stance == COMBAT_STANCE_AGGRESSIVE) {

                    cs->target_uid = enemy->uid;
                    cs->state = STATE_MOVING_TO_TARGET;

                    vec2_t move_dest_xz;
                    if(!cs->move_cmd_interrupted && G_Move_GetDest(curr, &move_dest_xz)) {
                        cs->move_cmd_interrupted = true; 
                        cs->move_cmd_xz = move_dest_xz;
                    }
                    G_Move_SetSeekEnemies(curr);
                }
            }
            break;
        }
        case STATE_MOVING_TO_TARGET:
        {
            /* Handle the case where our target dies before we reach it */
            struct entity *enemy = G_Combat_ClosestEligibleEnemy(curr);
            if(!enemy) {

                cs->state = STATE_NOT_IN_COMBAT; 

                if(cs->move_cmd_interrupted) {
                    G_Move_SetDest(curr, cs->move_cmd_xz);
                    cs->move_cmd_interrupted = false;
                }else {
                    G_Move_Stop(curr);
                }
                break;

            /* And the case where a different target becomes even closer */
            }else if(enemy->uid != cs->target_uid) {
                cs->target_uid = enemy->uid;
            }

            /* Check if we're within attacking range of our target */
            if(ents_distance(curr, enemy) <= ENEMY_MELEE_ATTACK_RANGE) {

                cs->state = STATE_CAN_ATTACK;
                G_Move_Stop(curr);
                entity_turn_to_target(curr, enemy);
                E_Entity_Notify(EVENT_ATTACK_START, curr->uid, NULL, ES_ENGINE);
            }
            break;
        }
        case STATE_MOVING_TO_TARGET_LOCKED:
        {
            struct entity *target = G_EntityForUID(cs->target_uid);
            if(!target || !(target->flags & ENTITY_FLAG_COMBATABLE)) {

                cs->state = STATE_NOT_IN_COMBAT;
                cs->sticky = false;
                G_Move_Stop(curr);
                break;
            }

            /* If our target goes out of vision, give up the pursuit */
            struct obb obb;
            Entity_CurrentOBB(target, &obb, false);

            uint16_t pmask = G_GetPlayerControlledFactions();
            if(!G_Fog_ObjVisible(pmask, &obb)) {
            
                cs->state = STATE_NOT_IN_COMBAT;
                cs->sticky = false;
                G_Move_Stop(curr);
                break;
            }

            /* Check if we're within attacking range of our target */
            if(ents_distance(curr, target) <= ENEMY_MELEE_ATTACK_RANGE) {

                cs->state = STATE_CAN_ATTACK;
                G_Move_Stop(curr);
                entity_turn_to_target(curr, target);
                E_Entity_Notify(EVENT_ATTACK_START, curr->uid, NULL, ES_ENGINE);
                break;
            }

            /* We approached the target, but it slipped away from us. Re-engage. */
            if(G_Move_Still(curr)) {
                G_Move_SetSurroundEntity(curr, target);
            }
            break;
        }
        case STATE_CAN_ATTACK:
        {
            /* Our target could have 'died' or gotten out of combat range - check this first. */
            const struct entity *target = G_EntityForUID(cs->target_uid);

            if(entity_dead(target)
            || ents_distance(curr, target) > ENEMY_MELEE_ATTACK_RANGE) {

                if(cs->sticky) {
                    if(!entity_dead(target)) {

                        E_Entity_Notify(EVENT_ATTACK_END, curr->uid, NULL, ES_ENGINE);
                        cs->state = STATE_MOVING_TO_TARGET_LOCKED;
                        G_Move_SetSurroundEntity(curr, target);
                        break;
                    }else{
                        cs->sticky = false;
                    }
                }

                /* First check if there's another suitable target */
                struct entity *enemy = G_Combat_ClosestEligibleEnemy(curr);
                if(enemy && ents_distance(curr, enemy) <= ENEMY_MELEE_ATTACK_RANGE) {

                    cs->target_uid = enemy->uid;
                    entity_turn_to_target(curr, enemy);
                    break;
                }

                cs->state = STATE_NOT_IN_COMBAT; 
                E_Entity_Notify(EVENT_ATTACK_END, curr->uid, NULL, ES_ENGINE);

                if(cs->move_cmd_interrupted) {
                    G_Move_SetDest(curr, cs->move_cmd_xz); 
                    cs->move_cmd_interrupted = false;
                }

            }else{
                /* Perform combat simulation between entities with targets within range */
                cs->state = STATE_ATTACK_ANIM_PLAYING;
                E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, curr->uid, on_attack_anim_finish, curr, G_RUNNING);
            }

            break;
        }
        case STATE_ATTACK_ANIM_PLAYING:
        case STATE_DEATH_ANIM_PLAYING:
            /* No-op */
            break;
        default: assert(0);
        };
    
    });
    PERF_RETURN_VOID();
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

static void on_render_3d(void *user, void *event)
{
    const struct camera *cam = G_GetActiveCamera();

    struct sval setting;
    ss_e status;
    (void)status;

    status = Settings_Get("pf.debug.show_combat_targets", &setting);
    assert(status == SS_OKAY);

    if(!setting.as_bool)
        return;

    uint32_t key;
    struct combatstate curr;

    kh_foreach(s_entity_state_table, key, curr, {

        vec2_t ent_pos = G_Pos_GetXZ(key);
        mat4x4_t ident;
        PFM_Mat4x4_Identity(&ident);

        const float radius = ENEMY_TARGET_ACQUISITION_RANGE;
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

        vec2_t ss_pos = Entity_TopScreenPos(G_EntityForUID(key));
        struct rect bounds = (struct rect){ss_pos.x - 75, ss_pos.y + 5, 150, 16};
        struct rgba color = (struct rgba){255, 0, 0, 255};
        UI_DrawText(s_name_for_state[curr.state], bounds, color);
    });
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Combat_Init(void)
{
    if(NULL == (s_entity_state_table = kh_init(state)))
        return false;

    vec_pentity_init(&s_dying_ents);
    E_Global_Register(EVENT_30HZ_TICK, on_30hz_tick, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);
    return true;
}

void G_Combat_Shutdown(void)
{
    E_Global_Unregister(EVENT_30HZ_TICK, on_30hz_tick);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    vec_pentity_destroy(&s_dying_ents);
    kh_destroy(state, s_entity_state_table);
}

void G_Combat_AddEntity(const struct entity *ent, enum combat_stance initial)
{
    assert(combatstate_get(ent->uid) == NULL);
    assert(ent->flags & ENTITY_FLAG_COMBATABLE);

    struct combatstate new_cs = (struct combatstate) {
        .stats = {0},
        .current_hp = ent->max_hp,
        .stance = initial,
        .state = STATE_NOT_IN_COMBAT,
        .sticky = false,
        .move_cmd_interrupted = false
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
    dying_remove(ent);
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

    if(stance == COMBAT_STANCE_HOLD_POSITION && cs->state == STATE_MOVING_TO_TARGET) {

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

    enum selection_type sel_type;
    const vec_pentity_t *sel = G_Sel_Get(&sel_type);

    if(vec_size(sel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return CTX_ACTION_NONE;

    const struct entity *first = vec_AT(sel, 0);
    if(!(first->flags & ENTITY_FLAG_COMBATABLE))
        return CTX_ACTION_NONE;

    if(G_Combat_GetBaseDamage(first) == 0)
        return CTX_ACTION_NONE;

    if(first->faction_id == hovered->faction_id)
        return CTX_ACTION_NONE;

    if((hovered->flags & ENTITY_FLAG_MARKER) || (hovered->flags & ENTITY_FLAG_ZOMBIE))
        return CTX_ACTION_NONE;

    if(!(hovered->flags & ENTITY_FLAG_COMBATABLE))
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

    cs->sticky = true;
    cs->target_uid = target->uid;
    cs->state = STATE_MOVING_TO_TARGET_LOCKED;
    cs->move_cmd_interrupted = false;

    G_Move_SetSurroundEntity(ent, target);
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
        G_Move_SetDest(ent, cs->move_cmd_xz);
        cs->move_cmd_interrupted = false;
    }
}

struct entity *G_Combat_ClosestEligibleEnemy(const struct entity *ent)
{
    vec2_t pos = G_Pos_GetXZ(ent->uid);
    struct entity *ret = G_Pos_NearestWithPred(pos, valid_enemy, (void*)ent, ENEMY_TARGET_ACQUISITION_RANGE);

    if(!ret)
        return NULL;

    vec2_t enemy_pos = G_Pos_GetXZ(ret->uid);
    vec2_t delta;
    PFM_Vec2_Sub(&pos, &enemy_pos, &delta);
    assert(PFM_Vec2_Len(&delta) <= ENEMY_TARGET_ACQUISITION_RANGE);
    return ret;
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

void G_Combat_SetHP(const struct entity *ent, int hp)
{
    struct combatstate *cs = combatstate_get(ent->uid);
    assert(cs);
    cs->current_hp = MIN(hp, ent->max_hp);
}

bool G_Combat_SaveState(struct SDL_RWops *stream)
{
    struct attr num_ents = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_entity_state_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_ents, "num_ents"));

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

        struct attr move_cmd_xz = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.move_cmd_xz
        };
        CHK_TRUE_RET(Attr_Write(stream, &move_cmd_xz, "move_cmd_xz"));
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
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        cs->move_cmd_xz = attr.val.as_vec2;
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
        E_Entity_Register(EVENT_ANIM_CYCLE_FINISHED, uid, on_death_anim_finish, ent, G_RUNNING);
    }

    return true;
}

