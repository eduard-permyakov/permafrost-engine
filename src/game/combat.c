/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2026 Eduard Permyakov 
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
#define MEM_FILE_SUB MEM_SUB_GAME_COMBAT

#include "combat.h"
#include "game_private.h"
#include "movement.h"
#include "formation.h"
#include "building.h"
#include "fog_of_war.h"
#include "position.h"
#include "garrison.h"
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
#include "../sprite.h"
#include "../phys/public/collision.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../phys/public/phys.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_string.h"
#include "../mem.h"
#include "../lib/public/queue.h"
#include "../lib/public/string_intern.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <SDL.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_COMBAT)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_COMBAT)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_COMBAT)


#define TARGET_ACQUISITION_RANGE     (100.0f)
/* Non-combatant hang-back threat radii; the trigger/safe pairs form two
 * hysteresis bands. */
#define NONCOMBATANT_FLEE_TRIGGER_RANGE (30.0f)  /* enemy closer -> run away */
#define NONCOMBATANT_FLEE_SAFE_RANGE    (45.0f)  /* no enemy within -> stop running, stand */
#define NONCOMBATANT_HOLD_TRIGGER_RANGE (60.0f)  /* enemy closer -> freeze the advance */
#define NONCOMBATANT_HOLD_SAFE_RANGE    (80.0f)  /* no enemy within -> resume the advance */
/* Re-targeting eagerness scales with distance: a unit right behind the fight
 * waits in line on its live target, a far backline unit re-picks freely, with
 * a linear cadence ramp between. A dead target is always replaced.
 */
#define RETARGET_QUEUE_RANGE         (40.0f)
#define RETARGET_EAGER_RANGE         (80.0f)
#define RETARGET_HOLD_MAX            (5)
/* A melee chaser closer than its personal pin range waits in line behind the
 * front instead of following the shared enemy-seek field around it; farther
 * units flank. The per-unit roll makes the pinned depth ragged: at infantry
 * spacing the second and third ranks always pin, the sixth never.
 */
#define SEEK_PIN_RANGE_MIN           (25.0f)
#define SEEK_PIN_RANGE_MAX           (50.0f)
#define PROJECTILE_DEFAULT_SPEED     (100.0f)
#define EPSILON                      (1.0f/1024)
#define DEFAULT_ATTACK_PERIOD        (4.0f/3.0f)
#define COMBAT_FIRE_FACING_TOLERANCE (15.0f)
#define MAX(a, b)                    ((a) > (b) ? (a) : (b))
#define MIN(a, b)                    ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)                  (sizeof(a)/sizeof(a[0]))
#define X_BINS_PER_CHUNK             (8)
#define Z_BINS_PER_CHUNK             (8)
#define MAX_COMBAT_TASKS             (64)
#define DEFAULT_CORPSE_DURATION_SECS (30)

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
    int   base_armour;      /* Flat armour points. Valid range: [ARMOUR_MIN_POINTS - ARMOUR_MAX_POINTS] */
    float attack_range;
    int   dmg_type;
    int   armour_type;
};

/* The running total of every live modifier of each kind, kept as a sum so the
 * damage path never walks the modifier list.
 */
struct combatmods{
    float bonus[COMBAT_MOD_MAX];
};

struct combatstate{
    struct combatstats stats;
    struct combatmods  mods;
    int                current_hp;
    /* Takes no damage and cannot be killed by an attack. */
    bool               invulnerable;
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
        /* Non-combatant hang-back: holding the advance / giving ground. */
        STATE_STANDING_GROUND,
        STATE_NON_COMBATANT_FLEEING,
    }state;
    /* Set between a sent EVENT_ATTACK_START and its matching EVENT_ATTACK_END so
     * that the two are always emitted as a pair; clients may assert the pairing.
     */
    bool               attack_notified;
    bool               sticky;
    uint32_t           target_uid;
    /* Ticks before a live target may be swapped for a closer one. Transient. */
    uint16_t           retarget_hold;
    /* The seek pin last handed to movement; NULL_UID when unpinned. Transient. */
    uint32_t           seek_pin_uid;
    /* If the target gained a target while moving, save and restore
     * its' intial move command once it finishes combat. */
    bool               move_cmd_interrupted;
    bool               move_cmd_attacking;
    vec2_t             move_cmd_xz;
    uint32_t           attack_start_tick;
    /* only used by ranged entities */
    struct proj_desc      pd;
    struct proj_fire_desc fd;
    /* Optional model to be displayed after an entity's death */
    const char        *corpse_dir;
    const char        *corpse_pfobj;
    vec3_t             corpse_scale;
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
    COMBAT_ACTION_TRYHIT,
    COMBAT_ACTION_HOLD_GROUND,
    COMBAT_ACTION_FLEE,
    COMBAT_ACTION_RESUME_MOVE
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
    bg_ent_t              *postree;
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
    COMBAT_CMD_SET_PROJ_FIRE_DESC,
    COMBAT_CMD_SET_CORPSE_MODEL,
    COMBAT_CMD_PROJ_HIT,
    COMBAT_CMD_SET_DAMAGE_TYPE,
    COMBAT_CMD_SET_ARMOUR_TYPE,
    COMBAT_CMD_SET_DAMAGE_TABLE,
    COMBAT_CMD_SET_INVULNERABLE,
    COMBAT_CMD_ADD_MODIFIER,
    COMBAT_CMD_REMOVE_MODIFIER,
    COMBAT_CMD_CLEAR_MODIFIERS,
    COMBAT_CMD_SET_GROUP_BONUS,
    COMBAT_CMD_CLEAR_GROUP_BONUS,
    COMBAT_CMD_REFRESH_BONUSES
};

/* Commands carry small typed payloads; the uid is hoisted out of the union so
 * the queue-snooping helpers can read it uniformly (0 for commands that don't
 * target an entity).
 */
struct combat_cmd{
    enum combat_cmd_type type;
    uint32_t             uid;
    union{
        struct{
            enum combat_stance initial;
        }add;
        struct{
            int    faction_id;
            vec2_t pos;
        }add_ref;
        struct{
            int    faction_id;
            vec2_t pos;
        }remove_ref;
        struct{
            int    oldfac;
            int    newfac;
            vec2_t pos;
        }update_ref;
        struct{
            vec3_t proj_pos;
        }tryhit;
        struct{
            uint32_t target;
        }attack_unit;
        struct{
            enum combat_stance stance;
        }set_stance;
        struct{
            int hp;
        }current_hp;
        struct{
            int armour;
        }base_armour;
        struct{
            bool on;
        }invulnerable;
        struct{
            enum combat_mod_kind kind;
            float                amount;
            bool                 percent;
            uint32_t             secs;
            char                 tag[COMBAT_MOD_TAG_LEN];
        }add_mod;
        struct{
            char tag[COMBAT_MOD_TAG_LEN];
        }remove_mod;
        struct{
            struct group_bonus_desc desc;
        }group_bonus;
        struct{
            char tag[COMBAT_MOD_TAG_LEN];
        }clear_group_bonus;
        struct{
            int dmg;
        }base_damage;
        struct{
            int hp;
        }max_hp;
        struct{
            float range;
        }set_range;
        struct{
            uint32_t delta;
        }time_delta;
        struct{
            struct proj_desc *pd;
        }proj_desc;
        struct{
            struct proj_fire_desc *fd;
        }proj_fire_desc;
        struct{
            char   dir[256];
            char   pfobj[256];
            vec3_t scale;
        }corpse_model;
        struct{
            struct proj_hit *hit;
        }proj_hit;
        struct{
            int type;
        }damage_type;
        struct{
            int type;
        }armour_type;
        struct{
            float mult[DAMAGE_TYPE_MAX * ARMOUR_TYPE_MAX];
        }damage_table;
    }u;
};

struct corpse{
    uint32_t    uid;
    uint32_t    secs_left;
    const char *dir;
    const char *pfobj;
};

/* One live stat modifier. 'secs_left' of 0 means it never expires on its own.
 * A percent modifier holds a fraction of the base stat, resolved on every resum.
 */
struct combat_mod{
    uint32_t             uid;
    uint32_t             secs_left;
    enum combat_mod_kind kind;
    float                amount;
    bool                 percent;
    char                 tag[COMBAT_MOD_TAG_LEN];
};

KHASH_MAP_INIT_INT(state, struct combatstate)
KHASH_SET_INIT_INT(animpend)

QUEUE_TYPE(cmd, struct combat_cmd)
QUEUE_IMPL(static, cmd, struct combat_cmd)

VEC_TYPE(corpse, struct corpse);
VEC_IMPL(static, corpse, struct corpse);

VEC_TYPE(mod, struct combat_mod);
VEC_IMPL(static, mod, struct combat_mod);

/* Indexed by entity: every add, removal and resum touches one entity's records,
 * so a flat list would make each of them cost the whole battle's worth.
 */
KHASH_MAP_INIT_INT(modlist, vec_mod_t)

/* Kept out of the combatstate, which is copied into the worker snapshot every
 * tick; only the resolved sums are read on the damage path.
 */
struct group_bonus_rec{
    uint32_t                uid;
    struct group_bonus_desc desc;
};

VEC_TYPE(gbonus, struct group_bonus_rec);
VEC_IMPL(static, gbonus, struct group_bonus_rec);

static void combat_push_cmd(struct combat_cmd cmd);
static void on_attack_anim_tick(void *user, void *event);
static void attack_anim_pending_remove(uint32_t uid);
static void on_death_anim_finish(void *user, void *event);
static void do_stop_attack(uint32_t uid);
static bool entity_dead(uint32_t uid);
static bool garrisoned(uint32_t uid);
static void combat_notify_attack_start(uint32_t uid, struct combatstate *cs);
static void combat_notify_attack_end(uint32_t uid, struct combatstate *cs);
static struct combat_cmd *snoop_most_recent_command(enum combat_cmd_type type, void *arg,
                                                    bool (*pred)(void*, struct combat_cmd*));
static bool uids_match(void *arg, struct combat_cmd *cmd);
static bool any_command(void *arg, struct combat_cmd *cmd);
static float combat_dmg_mult(int dmg_type, int armour_type);
static void combat_tick(void *user, void *event);
static void group_bonuses_remove_ent(uint32_t uid);
static float target_distance(uint32_t uid, uint32_t target);
static uint16_t retarget_hold_for_dist(float dist);
static uint32_t seek_pin_for(const struct combatstate *cs, uint32_t uid, uint32_t enemy);

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
    [STATE_STANDING_GROUND]         = "STANDING_GROUND",
    [STATE_NON_COMBATANT_FLEEING]   = "NON_COMBATANT_FLEEING",
};

static khash_t(state)    *s_entity_state_table;
/* Units whose attack animation is playing, polled once per frame for the
 * frame at which the hit lands. Batched into a single global handler. 
 */
static khash_t(animpend) *s_attack_anim_pending;
/* For saving/restoring state */
static vec_entity_t       s_dying_ents;
static const struct map  *s_map;
/* How many units of a faction currently currently occupy that bin.
 * For quickly finding that there are no enemy units nearby 
 */
static uint16_t          *s_fac_refcnts[MAX_FACTIONS];

static struct combat_work s_combat_work;
/* uid -> identity aabb, persistent across ticks; referenced directly by the
 * gamestate snapshot. Entries for removed entities linger until the periodic
 * clear.
 */
static khash_t(aabb)          *s_aabb_cache;
static size_t                  s_fog_snap_ntiles;
static queue_cmd_t        s_combat_commands;
static unsigned long      s_last_tick;
static enum combat_hz     s_combat_hz = COMBAT_HZ_1;
static bool               s_combat_hz_dirty = false;

static khash_t(stridx)   *s_stridx;
static mp_strbuff_t       s_stringpool;
/* [damage_type][armour_type] multipliers, owned by the scripting layer. All
 * 1.0 until a table is uploaded, so the system is a no-op by default.
 */
static float              s_dmg_mult[DAMAGE_TYPE_MAX][ARMOUR_TYPE_MAX];
static vec_corpse_t       s_corpses;
static khash_t(modlist)  *s_mods;
static vec_gbonus_t       s_gbonuses;

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

static float combat_dmg_mult(int dmg_type, int armour_type)
{
    if(dmg_type < 0 || dmg_type >= DAMAGE_TYPE_MAX)
        return 1.0f;
    if(armour_type < 0 || armour_type >= ARMOUR_TYPE_MAX)
        return 1.0f;
    return s_dmg_mult[dmg_type][armour_type];
}

/* The single point where armour turns into a damage scale. Every damage site
 * funnels its effective armour through here, so the formula and its clamp
 * exist in exactly one place.
 */
static float combat_armour_mult(int armour)
{
    armour = MIN(MAX(armour, ARMOUR_MIN_POINTS), ARMOUR_MAX_POINTS);
    return ((float)ARMOUR_K) / (ARMOUR_K + armour);
}

static int combat_effective_armour(const struct combatstate *cs)
{
    return cs->stats.base_armour + (int)roundf(cs->mods.bonus[COMBAT_MOD_ARMOUR]);
}

static int combat_effective_damage(const struct combatstate *cs)
{
    return MAX(0, cs->stats.base_dmg + (int)roundf(cs->mods.bonus[COMBAT_MOD_DAMAGE]));
}

/* A range of 0 is the melee sentinel that decides which attack path an entity
 * takes, so a bonus must never lift a melee unit off it.
 */
static float combat_effective_range(const struct combatstate *cs)
{
    if(cs->stats.attack_range == 0.0f)
        return 0.0f;
    return MAX(0.0f, cs->stats.attack_range + cs->mods.bonus[COMBAT_MOD_RANGE]);
}

static bool combat_effective_invulnerable(const struct combatstate *cs)
{
    return cs->invulnerable || (cs->mods.bonus[COMBAT_MOD_INVULNERABLE] > 0.0f);
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

static float combat_base_stat(uint32_t uid, const struct combatstate *cs,
                              enum combat_mod_kind kind)
{
    switch(kind) {
    case COMBAT_MOD_ARMOUR:
        return cs->stats.base_armour;
    case COMBAT_MOD_DAMAGE:
        return cs->stats.base_dmg;
    case COMBAT_MOD_RANGE:
        return cs->stats.attack_range;
    case COMBAT_MOD_SPEED: {
        float speed = 0.0f;
        G_Move_GetMaxSpeed(uid, &speed);
        return speed;
    }
    default:
        return 0.0f;
    }
}

static vec_mod_t *combat_mods_for(uint32_t uid, bool create)
{
    khiter_t k = kh_get(modlist, s_mods, uid);
    if(k != kh_end(s_mods))
        return &kh_value(s_mods, k);
    if(!create)
        return NULL;

    int status;
    k = kh_put(modlist, s_mods, uid, &status);
    if(status == -1)
        return NULL;

    vec_mod_init(&kh_value(s_mods, k));
    return &kh_value(s_mods, k);
}

/* The bonus sums are derived state: they are always rebuilt from the surviving
 * records rather than patched, so a record that leaks can never leave a stat
 * permanently shifted. The group bonus folds in here too, so leaving a group
 * drops it with nothing to unwind.
 */
static void combat_mods_resum(uint32_t uid)
{
    struct combatstate *cs = combatstate_get(uid);
    if(!cs)
        return;

    struct combatmods mods = {0};
    vec_mod_t *list = combat_mods_for(uid, false);
    for(int i = 0; list && i < vec_size(list); i++) {
        const struct combat_mod *curr = &vec_AT(list, i);
        if(curr->kind < 0 || curr->kind >= COMBAT_MOD_MAX)
            continue;
        mods.bonus[curr->kind] += curr->percent
                                ? curr->amount * combat_base_stat(uid, cs, curr->kind)
                                : curr->amount;
    }

    int gid = G_Group_ForEnt(uid);
    if(gid) {
        for(int kind = 0; kind < COMBAT_MOD_MAX; kind++) {
            float flat = 0.0f, percent = 0.0f;
            G_Group_GetBonus(gid, kind, &flat, &percent);
            mods.bonus[kind] += flat + percent * combat_base_stat(uid, cs, kind);
        }
    }

    float old_speed = cs->mods.bonus[COMBAT_MOD_SPEED];
    cs->mods = mods;

    if(mods.bonus[COMBAT_MOD_SPEED] != old_speed) {
        G_Move_SetSpeedBonus(uid, mods.bonus[COMBAT_MOD_SPEED]);
    }
}

static void combat_mods_remove_ent(uint32_t uid)
{
    khiter_t k = kh_get(modlist, s_mods, uid);
    if(k == kh_end(s_mods))
        return;

    vec_mod_destroy(&kh_value(s_mods, k));
    kh_del(modlist, s_mods, k);
}

static void combat_mods_tick(void)
{
    uint32_t uid;
    vec_mod_t *list;

    kh_foreach_val_ptr(s_mods, uid, list, {

        bool expired = false;
        for(int i = 0; i < vec_size(list); i++) {

            struct combat_mod *curr = &vec_AT(list, i);
            if(curr->secs_left == 0)
                continue;

            if(--curr->secs_left == 0) {
                vec_mod_del(list, i);
                i--;
                expired = true;
            }
        }
        if(expired) {
            combat_mods_resum(uid);
        }
    });
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

static struct proj_fire_desc combat_default_fire(void)
{
    return (struct proj_fire_desc){
        .frame_offset = 0,
        .fire_mode = FIRE_MODE_LOW,
        .bone_name = {0},
        .offset = (vec3_t){0},
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
    const struct combatstate *cs = combatstate_get(uid);
    vec2_t pos = G_Pos_GetXZFrom(gs->positions, uid);
    float range = MAX(TARGET_ACQUISITION_RANGE, combat_effective_range(cs));
    int binlen = MAX(
        (float)(X_COORDS_PER_TILE * TILES_PER_CHUNK_WIDTH)  / X_BINS_PER_CHUNK,
        (float)(Z_COORDS_PER_TILE * TILES_PER_CHUNK_HEIGHT) / Z_BINS_PER_CHUNK
    );
    int binrange = ceil(range / binlen);

    struct map_resolution mapres;
    M_GetResolution(s_map, &mapres);

    struct map_resolution binres = (struct map_resolution){
        mapres.chunk_w, mapres.chunk_h,
        X_BINS_PER_CHUNK, Z_BINS_PER_CHUNK,
		mapres.field_w, mapres.field_h
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

    const struct combatstate *cs = combatstate_get(uid);
    assert(cs->stance != COMBAT_STANCE_HOLD_POSITION);

    if(cs->stats.attack_range == 0.0f) {
        G_Move_SetSurroundEntity(uid, target);
    }else{
        if(M_NavLocationsReachable(s_map, Entity_NavLayer(uid),
            G_Pos_GetXZ(uid), G_Pos_GetXZ(target))) {

            G_Move_SetSurroundEntity(uid, target);
        }else{
            G_Move_SetEnterRange(uid, target, combat_effective_range(cs));
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

/* Workers read the gamestate snapshot; the main thread reads live state. */
static bool on_main_thread(void)
{
    return (SDL_ThreadID() == g_main_thread_id);
}

static vec2_t entity_xz(uint32_t uid)
{
    if(on_main_thread())
        return G_Pos_GetXZ(uid);
    return G_Pos_GetXZFrom(s_combat_work.gamestate.positions, uid);
}

static uint32_t entity_flags(uint32_t uid)
{
    if(on_main_thread())
        return G_FlagsGet(uid);
    return G_FlagsGetFrom(s_combat_work.gamestate.flags, uid);
}

static float entity_sel_radius(uint32_t uid)
{
    if(on_main_thread())
        return G_GetSelectionRadius(uid);
    return G_GetSelectionRadiusFrom(s_combat_work.gamestate.sel_radiuses, uid);
}

static quat_t entity_rot(uint32_t uid)
{
    if(on_main_thread())
        return Entity_GetRot(uid);
    return Entity_GetRotFrom(s_combat_work.gamestate.transforms, uid);
}

static void entity_obb(uint32_t uid, struct obb *out)
{
    if(on_main_thread()) {
        Entity_CurrentOBB(uid, out, false);
        return;
    }
    current_obb_from_gamestate(uid, out);
}

static bool entities_adjacent(uint32_t ent, uint32_t target)
{
    uint32_t flags = entity_flags(target);

    if(flags & ENTITY_FLAG_MOVABLE) {

        vec2_t ent_pos = entity_xz(ent);
        vec2_t target_pos = entity_xz(target);

        float ent_radius = entity_sel_radius(ent);
        float target_radius = entity_sel_radius(target);

        return M_NavObjAdjacentToDynamicWith(s_map, ent_pos, ent_radius, 
            target_pos, target_radius);
    }else{

        struct obb obb;
        entity_obb(target, &obb);

        vec2_t ent_pos = entity_xz(ent);
        float ent_radius = entity_sel_radius(ent);

        return M_NavObjAdjacentToStaticWith(s_map, ent_pos, ent_radius, &obb);
    }
}

static bool entity_can_attack_melee(uint32_t uid, uint32_t target)
{
    return entities_adjacent(uid, target);
}

static bool entity_can_attack(uint32_t uid, uint32_t target)
{
    const struct combatstate *cs = combatstate_get(uid);
    if(cs->stats.attack_range == 0.0f) {
        return entity_can_attack_melee(uid, target);
    }

    vec2_t xz_src = entity_xz(uid);
    vec2_t xz_dst = entity_xz(target);

    vec2_t delta;
    PFM_Vec2_Sub(&xz_src, &xz_dst, &delta);
    return (PFM_Vec2_Len(&delta) <= combat_effective_range(cs));
}

static bool valid_enemy(uint32_t curr, void *arg)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    uint32_t ent = (uintptr_t)arg;
    uint32_t ent_flags = G_FlagsGetFrom(gs->flags, ent);
    uint32_t curr_flags = G_FlagsGetFrom(gs->flags, curr);

    struct combatstate *ent_cs = combatstate_get(ent);
    assert(ent_cs);

    if(curr == ent)
        return false;
    if(!(curr_flags & ENTITY_FLAG_COMBATABLE))
        return false;
    if((curr_flags & ENTITY_FLAG_BUILDING) 
    && !G_Building_IsFoundedFrom(gs->buildstate, curr))
        return false;
    if(!enemies(ent, curr))
        return false;
    if(!(ent_flags & ENTITY_FLAG_AIR)
    && (curr_flags & ENTITY_FLAG_AIR)
    && (ent_cs->stats.attack_range == 0.0f))
        return false;

    struct combatstate *cs = combatstate_get(curr);
    if(!cs)
        return false;
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
    vec2_t ent_pos_xz = entity_xz(uid);
    vec2_t tar_pos_xz = entity_xz(target);

    vec2_t ent_to_target;
    PFM_Vec2_Sub(&tar_pos_xz, &ent_pos_xz, &ent_to_target);

    if(PFM_Vec2_Len(&ent_to_target) < EPSILON) {
        return entity_rot(uid);
    }

    PFM_Vec2_Normal(&ent_to_target, &ent_to_target);
    return quat_from_vec(ent_to_target);
}

static vec3_t entity_facing_dir(uint32_t uid)
{
    /* Recover the horizontal heading from the entity's Y-axis rotation - the inverse of
     * quat_from_vec, which is how units are turned to face their targets.
     */
    quat_t rot = Entity_GetRot(uid);
    float angle = 2.0f * atan2(rot.y, rot.w);
    return (vec3_t){ -sin(angle), 0.0f, cos(angle) };
}

static bool fires_from_formation(uint32_t uid)
{
    return (G_FlagsGet(uid) & ENTITY_FLAG_MOVABLE)
        && (G_Formation_GetForEnt(uid) != NULL_FID);
}

static bool faces_target(uint32_t uid, uint32_t target)
{
    quat_t cur = Entity_GetRot(uid);
    quat_t want = entity_turn_dir(uid, target);
    return RAD_TO_DEG(fabs(PFM_Quat_PitchDiff(&cur, &want))) <= COMBAT_FIRE_FACING_TOLERANCE;
}

static void combat_notify_attack_start(uint32_t uid, struct combatstate *cs)
{
    assert(!cs->attack_notified);
    cs->attack_notified = true;
    E_Entity_Notify(EVENT_ATTACK_START, uid, NULL, ES_ENGINE);
}

static void combat_notify_attack_end(uint32_t uid, struct combatstate *cs)
{
    assert(cs->attack_notified);
    cs->attack_notified = false;
    E_Entity_Notify(EVENT_ATTACK_END, uid, NULL, ES_ENGINE);
}

static void entity_turn_to_target(uint32_t uid, uint32_t target)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    struct combatstate *cs = combatstate_get(uid);
    assert(cs);

    uint32_t flags = G_FlagsGet(uid);

    /* Formation members hold their ground and fire without leaving the formation. */
    if(fires_from_formation(uid)) {
        G_Move_SetCombatFacing(uid, entity_turn_dir(uid, target));
        cs->state = STATE_TURNING_TO_TARGET;
        return;
    }

    if(!cs->move_cmd_interrupted
    && G_Move_GetDest(uid, &cs->move_cmd_xz, &cs->move_cmd_attacking)) {
        cs->move_cmd_interrupted = true;
    }
    G_Move_Stop(uid);

    if(!(flags & ENTITY_FLAG_MOVABLE)) {
        cs->state = STATE_CAN_ATTACK;
        combat_notify_attack_start(uid, cs);
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

    struct combatstate *cs = combatstate_get(uid);
    if(cs) {
        cs->attack_notified = false;
        cs->state = STATE_NOT_IN_COMBAT;
    }

    G_Move_Stop(uid);

    uint32_t flags = G_FlagsGet(uid);
    if(flags & ENTITY_FLAG_SELECTABLE) {
        G_Sel_Remove(uid);
        flags &= ~ENTITY_FLAG_SELECTABLE;
        G_FlagsSet(uid, flags);
    }

    attack_anim_pending_remove(uid);
    E_Global_Notify(EVENT_ENTITY_DIED, (void*)((uintptr_t)uid), ES_ENGINE);
    E_Entity_Notify(EVENT_ENTITY_DEATH, uid, NULL, ES_ENGINE);
    vec_entity_push(&s_dying_ents, uid);

    /* All units garrisoned inside a 'dead' entity die also. 
     */
    if(flags & ENTITY_FLAG_GARRISONABLE) {
        vec_entity_t garrisoned;
        vec_entity_init(&garrisoned);
        G_Garrison_GetUnits(uid, &garrisoned);
        for(int i = 0; i < vec_size(&garrisoned); i++) {
            uint32_t unit = vec_AT(&garrisoned, i);
            E_Global_Notify(EVENT_ENTITY_DIED, (void*)((uintptr_t)unit), ES_ENGINE);
            E_Entity_Notify(EVENT_ENTITY_DEATH_IMMEDIATE, unit, NULL, ES_ENGINE);
        }
        vec_entity_destroy(&garrisoned);
        G_Garrison_ClearGarrison(uid);
    }

    if(flags & ENTITY_FLAG_ANIMATED 
    && !(flags & ENTITY_FLAG_BUILDING)
    && !(flags & ENTITY_FLAG_WATER)
    && !(flags & ENTITY_FLAG_AIR)) {

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

    if(entity_dead(target) || garrisoned(target))
        return;

    struct combatstate *cs = combatstate_get(uid);
    struct combatstate *target_cs = combatstate_get(cs->target_uid);
    if(!target_cs)
        return;

    if(combat_effective_invulnerable(target_cs))
        return;

    /* The float damage is truncated by the assignment back into the integer HP,
     * which makes any nonzero damage take at least a whole point.
     */
    float dmg = combat_effective_damage(cs)
              * combat_dmg_mult(cs->stats.dmg_type, target_cs->stats.armour_type)
              * combat_armour_mult(combat_effective_armour(target_cs));
    target_cs->current_hp = MAX(0, target_cs->current_hp - dmg);

    if(target_cs->current_hp == 0 && target_cs->stats.max_hp > 0) {
        entity_die(target);
    }
}

static void entity_ranged_attack(uint32_t uid, uint32_t target, vec3_t proj_pos)
{
    ASSERT_IN_MAIN_THREAD();

    struct combat_gamestate *gs = &s_combat_work.gamestate;
    struct combatstate *cs = combatstate_get(uid);
    assert(cs);

    /* Once the fire frame plays we always loose a projectile. Aim at the target while it
     * still exists (even while it is dying); if it is already gone, lob a shot in the
     * direction we are facing - which is where the target was.
     */
    vec3_t target_pos;
    if(G_EntityExists(target)) {
        target_pos = Entity_CenterPos(target);
    }else{
        vec3_t fwd = entity_facing_dir(uid);
        PFM_Vec3_Scale(&fwd, combat_effective_range(cs), &fwd);
        PFM_Vec3_Add(&proj_pos, &fwd, &target_pos);
    }

    float ent_dmg = combat_effective_damage(cs);
    vec3_t vel;
    if(!P_Projectile_VelocityForTarget(proj_pos, target_pos, cs->pd.speed,
        cs->fd.fire_mode, &vel)) {
        return; /* Degenerate: the target is right on top of the muzzle. */
    }

    P_Projectile_Add(proj_pos, vel, uid, G_GetFactionID(uid),
        ent_dmg, cs->stats.dmg_type, PROJ_ONLY_HIT_COMBATABLE | PROJ_ONLY_HIT_ENEMIES, cs->pd);
}

static bool garrisoned(uint32_t uid)
{
    return (entity_flags(uid) & ENTITY_FLAG_GARRISONED);
}

static struct result corpse_disappear_task(void *arg)
{
    uint32_t uid = (uintptr_t)arg;
    vec3_t start_pos = G_Pos_Get(uid);
    uint32_t flags = G_FlagsGet(uid);

    struct obb obb;
    Entity_CurrentOBB(uid, &obb, true);
    int height = obb.half_lengths[1] * 2.0f;

    uint32_t newflags = G_FlagsGet(uid);
    newflags |= ENTITY_FLAG_TRANSLUCENT;
    G_FlagsSet(uid, newflags);

    const float duration = 1000.0f;
    uint32_t elapsed = 0;
    uint32_t start = SDL_GetTicks();

    while(elapsed < duration) {
    
        Task_AwaitEvent(EVENT_UPDATE_START, &(int){0});
        uint32_t curr = SDL_GetTicks();

        /* The entity can theoretically be forecefully removed during the 
         * disappearing animation. Make sure we don't crap out if this happens
         */
        if(!G_EntityExists(uid))
            return NULL_RESULT;

        uint32_t prev_long = elapsed / 250;
        uint32_t curr_long =  (curr - start) / 250;
        elapsed = curr - start;

        float pc = (elapsed - (prev_long * 250)) / 250;
        vec3_t curr_pos = (vec3_t){
            start_pos.x,
            start_pos.y - (elapsed / duration) * height,
            start_pos.z,
        };
        G_Pos_Set(uid, curr_pos);
    }

    G_DeferredRemove(uid);
    return NULL_RESULT;
}

static void add_corpse(const char *dir, const char *pfobj, uint32_t duration,
                        vec3_t pos, vec3_t scale, quat_t rot)
{
    const uint32_t uid = Entity_NewUID();
    uint32_t flags;
    bool loaded = AL_EntityFromPFObj(dir, pfobj, "__corpse__", uid, &flags);
    if(!loaded)
        return;

    G_AddEntity(uid, flags, pos);
    Entity_SetRot(uid, rot);
    Entity_SetScale(uid, scale);
    G_Pos_Set(uid, pos);

    vec_corpse_push(&s_corpses, (struct corpse){uid, duration, dir, pfobj});
}

static void on_death_anim_finish(void *user, void *event)
{
    uint32_t self = (uintptr_t)user;
    struct combatstate *cs = combatstate_get(self);
    assert(cs);

    khash_t(id) *flags = s_combat_work.gamestate.flags;
    khiter_t k = kh_get(id, flags, self);
    assert(k != kh_end(flags));
    kh_value(flags, k) = kh_value(flags, k) | ENTITY_FLAG_ZOMBIE;

    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, self, on_death_anim_finish);
    G_Zombiefy(self, true);

    struct combat_gamestate *gs = &s_combat_work.gamestate;
    vec3_t pos = G_Pos_GetFrom(gs->positions, self);
    quat_t rot = Entity_GetRotFrom(gs->transforms, self);

    if(cs->corpse_dir && cs->corpse_pfobj) {
        add_corpse(cs->corpse_dir, cs->corpse_pfobj, DEFAULT_CORPSE_DURATION_SECS,
            pos, cs->corpse_scale, rot);
    }
}

static void do_add_entity(uint32_t uid, enum combat_stance initial)
{
    ASSERT_IN_MAIN_THREAD();

    assert(combatstate_get(uid) == NULL);
    struct combatstate new_cs = (struct combatstate) {
        /* Both type IDs default to 0, the type an entity gets when it declares none. */
        .stats = {0},
        .current_hp = 0,
        .stance = initial,
        .state = STATE_NOT_IN_COMBAT,
        .sticky = false,
        .seek_pin_uid = NULL_UID,
        .move_cmd_interrupted = false,
        .pd = combat_default_proj(),
        .fd = combat_default_fire(),
        .corpse_dir = NULL,
        .corpse_pfobj = NULL,
    };
    combatstate_set(uid, &new_cs);
}

static void do_remove_entity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    if(!cs)
        return;

    attack_anim_pending_remove(uid);
    E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_death_anim_finish);

    if(cs->state == STATE_ATTACK_ANIM_PLAYING
    || cs->state == STATE_CAN_ATTACK) {
        combat_notify_attack_end(uid, cs);
    }

    PF_FREE(cs->pd.basedir);
    PF_FREE(cs->pd.pfobj);
    if(cs->pd.flags & PROJ_HAS_IMPACT_SPRITE) {
        PF_FREE(cs->pd.impact_sprite.filename);
    }
    if(cs->pd.flags & PROJ_HAS_TRAIL_SPRITE) {
        PF_FREE(cs->pd.trail_sprite.filename);
    }

    combat_dying_remove(uid);
    combat_mods_remove_ent(uid);
    group_bonuses_remove_ent(uid);
    combatstate_remove(uid);
}

static void do_tryhit(uint32_t uid, vec3_t proj_pos)
{
    ASSERT_IN_MAIN_THREAD();

    if(entity_dead(uid))
        return; /* Our unit already got 'killed' */

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    if(cs->state == STATE_DEATH_ANIM_PLAYING
    || cs->state == STATE_NOT_IN_COMBAT)
        return;

    /* A stale hit can arrive after the unit left its attack (re-targeted, moved off
     * to garrison, etc.); don't resurrect the attack state without a paired start.
     */
    if(!cs->attack_notified)
        return;

    cs->state = STATE_CAN_ATTACK;

    /* Ranged units always loose their projectile once the fire frame is reached - even if
     * the target has since died or slipped out of range; entity_ranged_attack lobs a
     * best-effort shot in those cases.
     */
    if(cs->stats.attack_range > 0.0f) {
        entity_ranged_attack(uid, cs->target_uid, proj_pos);
        return;
    }

    if(entity_dead(cs->target_uid) || garrisoned(cs->target_uid)) {
        return; /* Our (melee) target already got 'killed' */
    }

    quat_t target_dir = entity_turn_dir(uid, cs->target_uid);
    quat_t ent_rot = Entity_GetRot(uid);
    float angle_diff = PFM_Quat_PitchDiff(&ent_rot, &target_dir);

    if(RAD_TO_DEG(fabs(angle_diff)) > 5.0f) {

        combat_notify_attack_end(uid, cs);
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
    if(entity_dead(hit->ent_uid) || garrisoned(hit->ent_uid))
        return;

    struct combatstate *cs = combatstate_get(hit->ent_uid);
    if(combat_effective_invulnerable(cs))
        return;

    /* Truncated into the integer HP, as in the melee path. */
    float dmg = hit->cookie
              * combat_dmg_mult(hit->dmg_type, cs->stats.armour_type)
              * combat_armour_mult(combat_effective_armour(cs));
    cs->current_hp = MAX(0, cs->current_hp - dmg);

    if(cs->current_hp == 0 && cs->stats.max_hp > 0) {
        entity_die(hit->ent_uid);
    }
}

static void do_set_stance(uint32_t uid, enum combat_stance stance)
{
    ASSERT_IN_MAIN_THREAD();

    /* The stance command is deferred, so the entity may have been removed
     * (e.g. despawned by a script) or have died before this is processed.
     * In the latter case, resetting its state would resurrect it.
     */
    struct combatstate *cs = combatstate_get(uid);
    if(!cs || cs->state == STATE_DEATH_ANIM_PLAYING)
        return;

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

    /* This deferred command may be processed after the entity died (and is now
     * playing its death animation); acting on it would reset the combat state and
     * resurrect the entity, leading to a second death.
     */
    if(cs->state == STATE_DEATH_ANIM_PLAYING)
        return;

    do_stop_attack(uid);
    cs->stance = COMBAT_STANCE_AGGRESSIVE;

    uint32_t flags = G_FlagsGet(uid);
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
        combat_notify_attack_start(uid, cs);
    }
}

static void do_stop_attack(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    if(!cs || cs->state == STATE_DEATH_ANIM_PLAYING)
        return;

    attack_anim_pending_remove(uid);

    if(cs->state == STATE_ATTACK_ANIM_PLAYING
    || cs->state == STATE_CAN_ATTACK) {
        combat_notify_attack_end(uid, cs);
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
        X_BINS_PER_CHUNK, Z_BINS_PER_CHUNK,
		mapres.field_w, mapres.field_h
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
        X_BINS_PER_CHUNK, Z_BINS_PER_CHUNK,
		mapres.field_w, mapres.field_h
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

    do_remove_ref(oldfac, pos);
    do_add_ref(newfac, pos);
}

static void do_set_base_armour(uint32_t uid, int armour)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    cs->stats.base_armour = armour;
}

static void do_set_invulnerable(uint32_t uid, bool on)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    cs->invulnerable = on;
}

static void do_add_modifier(uint32_t uid, enum combat_mod_kind kind, float amount,
                            bool percent, uint32_t secs, const char *tag)
{
    ASSERT_IN_MAIN_THREAD();

    if(!combatstate_get(uid))
        return;

    /* A tagged modifier is a slot, not an accumulator: re-applying an aura
     * refreshes it instead of stacking it up.
     */
    vec_mod_t *list = combat_mods_for(uid, true);
    if(!list)
        return;

    if(tag[0] != '\0') {
        for(int i = 0; i < vec_size(list); i++) {
            if(strcmp(vec_AT(list, i).tag, tag))
                continue;
            vec_mod_del(list, i);
            break;
        }
    }

    struct combat_mod mod = (struct combat_mod){
        .uid = uid,
        .secs_left = secs,
        .kind = kind,
        .amount = amount,
        .percent = percent
    };
    pf_strlcpy(mod.tag, tag, sizeof(mod.tag));
    vec_mod_push(list, mod);

    combat_mods_resum(uid);
}

static void do_remove_modifier(uint32_t uid, const char *tag)
{
    ASSERT_IN_MAIN_THREAD();

    vec_mod_t *list = combat_mods_for(uid, false);
    for(int i = 0; list && i < vec_size(list); i++) {
        if(strcmp(vec_AT(list, i).tag, tag))
            continue;
        vec_mod_del(list, i);
        i--;
    }
    combat_mods_resum(uid);
}

static void do_clear_modifiers(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    combat_mods_remove_ent(uid);
    combat_mods_resum(uid);
}

static void do_set_group_bonus(uint32_t uid, const struct group_bonus_desc *desc)
{
    ASSERT_IN_MAIN_THREAD();

    if(!combatstate_get(uid))
        return;

    for(int i = 0; i < vec_size(&s_gbonuses); i++) {
        const struct group_bonus_rec *curr = &vec_AT(&s_gbonuses, i);
        if(curr->uid != uid || strcmp(curr->desc.tag, desc->tag))
            continue;
        vec_gbonus_del(&s_gbonuses, i);
        break;
    }

    vec_gbonus_push(&s_gbonuses, (struct group_bonus_rec){
        .uid = uid,
        .desc = *desc
    });
    G_Group_RefreshBonus(G_Group_ForEnt(uid));
}

static void do_clear_group_bonus(uint32_t uid, const char *tag)
{
    ASSERT_IN_MAIN_THREAD();

    for(int i = 0; i < vec_size(&s_gbonuses); i++) {
        const struct group_bonus_rec *curr = &vec_AT(&s_gbonuses, i);
        if(curr->uid != uid || strcmp(curr->desc.tag, tag))
            continue;
        vec_gbonus_del(&s_gbonuses, i);
        i--;
    }
    G_Group_RefreshBonus(G_Group_ForEnt(uid));
}

static void group_bonuses_remove_ent(uint32_t uid)
{
    for(int i = 0; i < vec_size(&s_gbonuses); i++) {
        if(vec_AT(&s_gbonuses, i).uid != uid)
            continue;
        vec_gbonus_del(&s_gbonuses, i);
        i--;
    }
}

static void do_refresh_bonuses(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    combat_mods_resum(uid);
}

static void do_set_damage_type(uint32_t uid, int type)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    cs->stats.dmg_type = type;
}

static void do_set_armour_type(uint32_t uid, int type)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    cs->stats.armour_type = type;
}

static void do_set_damage_table(const float *mult)
{
    ASSERT_IN_MAIN_THREAD();

    memcpy(s_dmg_mult, mult, sizeof(s_dmg_mult));
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
    if(cs->pd.flags & PROJ_HAS_IMPACT_SPRITE) {
        PF_FREE(cs->pd.impact_sprite.filename);
    }
    if(cs->pd.flags & PROJ_HAS_TRAIL_SPRITE) {
        PF_FREE(cs->pd.trail_sprite.filename);
    }
    cs->pd = *pd;
}

static void do_set_proj_fire_desc(uint32_t uid, const struct proj_fire_desc *fd)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    cs->fd = *fd;
}

static void do_set_corpse_model(uint32_t uid, const char *dir, const char *pfobj, vec3_t scale)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    cs->corpse_dir = dir;
    cs->corpse_pfobj = pfobj;
    cs->corpse_scale = scale;
}

static vec3_t projectile_spawn_pos(uint32_t uid)
{
    struct combatstate *cs = combatstate_get(uid);
    assert(cs);

    vec4_t proj_pos = (vec4_t){0};
    if(cs->stats.attack_range > 0.0) {

        mat4x4_t pose_mat;
        mat4x4_t model;
        Entity_ModelMatrix(uid, &model);
        struct proj_fire_desc *fd = &cs->fd;

        if(strlen(cs->fd.bone_name) > 0 
        && A_GetBoneCurrPoseMat(uid, cs->fd.bone_name, &pose_mat)) {

            vec4_t bone_pos_homo;
            vec4_t offset_homo = (vec4_t){fd->offset.x, fd->offset.y, fd->offset.z, 1.0};

            PFM_Mat4x4_Mult4x1(&pose_mat, &offset_homo, &bone_pos_homo);
            PFM_Mat4x4_Mult4x1(&model, &bone_pos_homo, &proj_pos);
        }else{
            /* Entity_CenterPos is already in world space, so the offset is
             * applied directly without re-transforming by the model matrix. */
            vec3_t center = Entity_CenterPos(uid);
            PFM_Vec3_Add(&center, &fd->offset, &center);
            proj_pos = (vec4_t){center.x, center.y, center.z, 1.0f};
        }
    }
    return (vec3_t){proj_pos.x, proj_pos.y, proj_pos.z};
}

static void attack_anim_pending_add(uint32_t uid)
{
    int status;
    kh_put(animpend, s_attack_anim_pending, uid, &status);
    assert(status != -1);
}

static void attack_anim_pending_remove(uint32_t uid)
{
    khiter_t k = kh_get(animpend, s_attack_anim_pending, uid);
    if(k != kh_end(s_attack_anim_pending)) {
        kh_del(animpend, s_attack_anim_pending, k);
    }
}

static void attack_anim_tick(uint32_t self)
{
    int curr_frame = A_GetCurrFrameIndex(self);
    struct combatstate *cs = combatstate_get(self);
    assert(cs);

    if(curr_frame != cs->fd.frame_offset)
        return;

    attack_anim_pending_remove(self);
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_TRYHIT,
        .uid = self,
        .u.tryhit = {
            .proj_pos = projectile_spawn_pos(self)
        }
    });
}

static void on_attack_anim_tick(void *user, void *event)
{
    /* Snapshot: attack_anim_tick removes entries as hits land. */
    size_t npend = kh_size(s_attack_anim_pending);
    if(npend == 0)
        return;

    STALLOC(uint32_t, pend, npend);
    size_t n = 0;
    uint32_t uid;
    kh_foreach_key(s_attack_anim_pending, uid, {
        pend[n++] = uid;
    });

    for(size_t i = 0; i < n; i++) {
        attack_anim_tick(pend[i]);
    }
    STFREE(pend);
}

static bool entity_dead(uint32_t uid)
{
    struct combatstate *cs = combatstate_get(uid);
    if(!cs || (cs->state == STATE_DEATH_ANIM_PLAYING)
    || (entity_flags(uid) & ENTITY_FLAG_ZOMBIE))
        return true;

    return false;
}

static void entity_target_enemy(uint32_t uid, uint32_t enemy)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    struct combatstate *cs = combatstate_get(uid);
    assert(cs);

    /* Either party may have been removed since the work snapshot chose this
     * pairing, and the positions read below are live.
     */
    if(!G_EntityExists(uid) || !G_EntityExists(enemy))
        return;

    if(entity_can_attack(uid, enemy)) {

        assert(cs->stance == COMBAT_STANCE_AGGRESSIVE 
            || cs->stance == COMBAT_STANCE_HOLD_POSITION);

        cs->target_uid = enemy;
        cs->retarget_hold = retarget_hold_for_dist(target_distance(uid, enemy));
        entity_turn_to_target(uid, enemy);
        return;
    }

    uint32_t flags = G_FlagsGet(uid);
    if(cs->stance == COMBAT_STANCE_AGGRESSIVE && (flags & ENTITY_FLAG_MOVABLE)) {

        cs->target_uid = enemy;
        cs->retarget_hold = retarget_hold_for_dist(target_distance(uid, enemy));
        cs->seek_pin_uid = seek_pin_for(cs, uid, enemy);
        cs->state = STATE_MOVING_TO_TARGET;

        if(!cs->move_cmd_interrupted 
        && G_Move_GetDest(uid, &cs->move_cmd_xz, &cs->move_cmd_attacking)) {
            cs->move_cmd_interrupted = true; 
        }
        G_Move_SetSeekEnemies(uid);
    }else{
        /* Can neither attack nor chase the target (e.g. a stationary unit whose enemy
         * is out of range); leave combat so the state matches the cleared notification. */
        cs->state = STATE_NOT_IN_COMBAT;
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

    /* Formation members keep their move state so they rejoin the advance once the hold lifts. */
    if(G_Formation_GetForEnt(uid) != NULL_FID)
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

static bool under_attack_order(uint32_t uid)
{
    vec2_t dest;
    bool attacking;
    if(!G_Move_GetDest(uid, &dest, &attacking))
        return false;
    return attacking;
}

static void entity_hold_ground(uint32_t uid, uint32_t threat)
{
    struct combatstate *cs = combatstate_get(uid);
    assert(cs);

    G_Move_StopFlee(uid);

    /* Only commit to holding while advancing under an attack-move order. */
    if(!under_attack_order(uid) || G_Move_Still(uid)) {
        cs->state = STATE_NOT_IN_COMBAT;
        return;
    }

    cs->state = STATE_STANDING_GROUND;
    cs->target_uid = threat;
    if(!entity_dead(threat)) {
        G_Move_SetCombatFacing(uid, entity_turn_dir(uid, threat));
    }
}

uint32_t closest_eligible_entity(uint32_t uid)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    struct combatstate *cs = combatstate_get(uid);
    vec2_t pos = G_Pos_GetXZFrom(gs->positions, uid);
    float range = MAX(TARGET_ACQUISITION_RANGE, combat_effective_range(cs));

    return G_Pos_NearestWithPredFrom(gs->postree, gs->positions, gs->flags,
        pos, valid_enemy, (void*)((uintptr_t)uid), range);
}

static uint32_t closest_eligible_entity_range(uint32_t uid, float range)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    vec2_t pos = G_Pos_GetXZFrom(gs->positions, uid);

    return G_Pos_NearestWithPredFrom(gs->postree, gs->positions, gs->flags,
        pos, valid_enemy, (void*)((uintptr_t)uid), range);
}

static float target_distance(uint32_t uid, uint32_t target)
{
    struct combat_gamestate *gs = &s_combat_work.gamestate;
    vec2_t pos = G_Pos_GetXZFrom(gs->positions, uid);
    vec2_t tpos = G_Pos_GetXZFrom(gs->positions, target);
    vec2_t delta;
    PFM_Vec2_Sub(&tpos, &pos, &delta);
    return PFM_Vec2_Len(&delta);
}

static uint16_t retarget_hold_for_dist(float dist)
{
    if(dist >= RETARGET_EAGER_RANGE)
        return 0;
    if(dist <= RETARGET_QUEUE_RANGE)
        return RETARGET_HOLD_MAX;
    float t = (dist - RETARGET_QUEUE_RANGE)
            / (RETARGET_EAGER_RANGE - RETARGET_QUEUE_RANGE);
    return (uint16_t)((1.0f - t) * RETARGET_HOLD_MAX + 0.5f);
}

static float pin_range(uint32_t uid)
{
    float roll = ((uid * 2654435761u) >> 8) / (float)(1u << 24);
    return SEEK_PIN_RANGE_MIN + roll * (SEEK_PIN_RANGE_MAX - SEEK_PIN_RANGE_MIN);
}

static uint32_t seek_pin_for(const struct combatstate *cs, uint32_t uid, uint32_t enemy)
{
    if(cs->stats.attack_range > 0.0f)
        return NULL_UID;
    return (target_distance(uid, enemy) <= pin_range(uid)) ? enemy : NULL_UID;
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

    /* Garrisoned units are off the battlefield: freeze their combat state so they
     * can't re-engage and desync the attack-notified pairing while inside a carrier.
     * do_garrison cleanly disengages combat when they enter. */
    if(flags & ENTITY_FLAG_GARRISONED)
        return;

    switch(curr->state) {
    case STATE_NOT_IN_COMBAT: 
    {
        if(curr->stance == COMBAT_STANCE_NO_ENGAGEMENT)
            break;

        if(flags & ENTITY_FLAG_BUILDING 
        && !G_Building_IsCompletedFrom(gs->buildstate, uid))
            break;

        if(curr->stats.base_dmg == 0) {

            /* Non-combatants don't acquire targets; movable ones instead
             * hold back from nearby threats during an attack-move. */
            if(!(flags & ENTITY_FLAG_MOVABLE))
                break;
            if(!maybe_enemy_near(uid))
                break;
            uint32_t threat = closest_eligible_entity_range(uid,
                NONCOMBATANT_HOLD_TRIGGER_RANGE);
            if(threat == NULL_UID)
                break;

            out->action = COMBAT_ACTION_HOLD_GROUND;
            out->action_args[0] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            out->action_args[1] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = threat
            };
            break;
        }

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

        if(curr->retarget_hold > 0)
            curr->retarget_hold--;

        /* Wait in line close behind the fight; re-pick freely further back. */
        uint32_t enemy = curr->target_uid;
        bool invalid = entity_dead(enemy) || garrisoned(enemy);
        float dist = invalid ? 0.0f : target_distance(uid, enemy);

        if(invalid || (dist > RETARGET_QUEUE_RANGE && curr->retarget_hold == 0)) {

            enemy = closest_eligible_entity(uid);
            if(enemy == NULL_UID) {

                out->action = COMBAT_ACTION_STOP_COMBAT;
                out->action_args[0] = (struct attr){
                    .type = TYPE_INT,
                    .val.as_int = uid
                };
                break;
            }
            if(enemy != curr->target_uid) {
                curr->target_uid = enemy;
                curr->retarget_hold = retarget_hold_for_dist(target_distance(uid, enemy));
            }
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
        }else{
            curr->seek_pin_uid = seek_pin_for(curr, uid, enemy);
        }
        break;
    }
    case STATE_MOVING_TO_TARGET_LOCKED:
    {
        assert(flags & ENTITY_FLAG_MOVABLE);

        if(entity_dead(curr->target_uid) || garrisoned(curr->target_uid)) {

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
        if(entity_dead(curr->target_uid) 
        || garrisoned(curr->target_uid)
        || !entity_can_attack(uid, curr->target_uid)) {

            if(curr->sticky) {

                if(!entity_dead(curr->target_uid)
                || garrisoned(curr->target_uid)) {

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

        if(entity_dead(curr->target_uid) 
        || garrisoned(curr->target_uid)
        || !entity_can_attack(uid, curr->target_uid)) {

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
        out->action_args[1] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr->target_uid
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
    case STATE_STANDING_GROUND: {

        if(curr->stance == COMBAT_STANCE_NO_ENGAGEMENT || curr->stats.base_dmg > 0) {
            curr->state = STATE_NOT_IN_COMBAT;
            out->action = COMBAT_ACTION_RESUME_MOVE;
            out->action_args[0] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            break;
        }

        uint32_t threat = closest_eligible_entity_range(uid,
            NONCOMBATANT_HOLD_SAFE_RANGE);
        if(threat == NULL_UID) {
            curr->state = STATE_NOT_IN_COMBAT;
            out->action = COMBAT_ACTION_RESUME_MOVE;
            out->action_args[0] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            break;
        }
        curr->target_uid = threat;

        vec2_t pos = G_Pos_GetXZFrom(gs->positions, uid);
        vec2_t threat_pos = G_Pos_GetXZFrom(gs->positions, threat);
        vec2_t delta;
        PFM_Vec2_Sub(&threat_pos, &pos, &delta);

        if(PFM_Vec2_Len(&delta) <= NONCOMBATANT_FLEE_TRIGGER_RANGE) {
            curr->state = STATE_NON_COMBATANT_FLEEING;
            out->action = COMBAT_ACTION_FLEE;
            out->action_args[0] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            out->action_args[1] = (struct attr){
                .type = TYPE_VEC2,
                .val.as_vec2 = threat_pos
            };
        }
        break;
    }
    case STATE_NON_COMBATANT_FLEEING: {

        if(curr->stance == COMBAT_STANCE_NO_ENGAGEMENT || curr->stats.base_dmg > 0) {
            curr->state = STATE_NOT_IN_COMBAT;
            out->action = COMBAT_ACTION_RESUME_MOVE;
            out->action_args[0] = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            break;
        }

        uint32_t threat = closest_eligible_entity_range(uid,
            NONCOMBATANT_FLEE_SAFE_RANGE);
        if(threat == NULL_UID) {
            /* Safety reached: stand and face the last known threat. */
            curr->state = STATE_STANDING_GROUND;
            out->action = COMBAT_ACTION_HOLD_GROUND;
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
        curr->target_uid = threat;

        out->action = COMBAT_ACTION_FLEE;
        out->action_args[0] = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        };
        out->action_args[1] = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = G_Pos_GetXZFrom(gs->positions, threat)
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

    if(entity_dead(uid))
        return;

    /* The worker computed its next state from a snapshot, so anything the main
     * thread owns outright has to survive the copy-back.
     */
    int live_hp = cs->current_hp;
    struct combatmods live_mods = cs->mods;
    bool live_invulnerable = cs->invulnerable;
    uint32_t old_pin = cs->seek_pin_uid;
    *cs = out->next_state;
    cs->current_hp = live_hp;
    cs->mods = live_mods;
    cs->invulnerable = live_invulnerable;

    if(out->notify_attack_end) {
        combat_notify_attack_end(uid, cs);
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
        attack_anim_pending_add(uid);
        break;
    }
    case COMBAT_ACTION_ATTACK_IF_STILL: {
        uint32_t old_uid = uid;
        uint32_t uid = out->action_args[0].val.as_int;
        assert(old_uid == uid);

        uint32_t target = out->action_args[1].val.as_int;
        bool ready = fires_from_formation(uid) ? faces_target(uid, target) : G_Move_Still(uid);
        if(ready) {
            cs->state = STATE_CAN_ATTACK;
            combat_notify_attack_start(uid, cs);
        }
        break;
    }
    case COMBAT_ACTION_TRYHIT: {
        uint32_t uid = out->action_args[0].val.as_int;
        vec3_t proj_pos = projectile_spawn_pos(uid);
        do_tryhit(uid, proj_pos);
        break;
    }
    case COMBAT_ACTION_HOLD_GROUND: {
        uint32_t uid = out->action_args[0].val.as_int;
        uint32_t threat = out->action_args[1].val.as_int;
        entity_hold_ground(uid, threat);
        break;
    }
    case COMBAT_ACTION_FLEE: {
        uint32_t uid = out->action_args[0].val.as_int;
        vec2_t threat_xz = out->action_args[1].val.as_vec2;
        G_Move_SetFlee(uid, threat_xz);
        break;
    }
    case COMBAT_ACTION_RESUME_MOVE: {
        uint32_t uid = out->action_args[0].val.as_int;
        G_Move_StopFlee(uid);
        break;
    }
    default:
        assert(0);
    }

    /* Keep engaged formation members held in place so they fire without leaving the formation. */
    bool engaged = (cs->state == STATE_TURNING_TO_TARGET || cs->state == STATE_CAN_ATTACK
                 || cs->state == STATE_ATTACK_ANIM_PLAYING || cs->state == STATE_ATTACKING);
    uint32_t eflags = G_FlagsGet(uid);

    bool hold = (engaged && fires_from_formation(uid))
             || (cs->state == STATE_STANDING_GROUND);
    if(hold && !(eflags & ENTITY_FLAG_COMBAT_HELD)) {
        G_Move_SetCombatHeld(uid, true);
    }else if(!hold && (eflags & ENTITY_FLAG_COMBAT_HELD)) {
        G_Move_SetCombatHeld(uid, false);
    }

    /* A player order can race the snapshot; reconcile in combat's favour. */
    if(cs->stats.base_dmg == 0 && cs->state != STATE_NON_COMBATANT_FLEEING
    && G_Move_IsFleeing(uid)) {
        G_Move_StopFlee(uid);
    }

    /* The pin only means something while chasing. */
    if(cs->state != STATE_MOVING_TO_TARGET)
        cs->seek_pin_uid = NULL_UID;
    if(cs->seek_pin_uid != old_pin)
        G_Move_SetSeekPin(uid, cs->seek_pin_uid);
}

static void combat_push_cmd(struct combat_cmd cmd)
{
    queue_cmd_push(&s_combat_commands, &cmd);
}

static bool uids_match(void *arg, struct combat_cmd *cmd)
{
    uint32_t desired_uid = (uintptr_t)arg;
    return (desired_uid == cmd->uid);
}

static bool any_command(void *arg, struct combat_cmd *cmd)
{
    return true;
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

static enum movement_hz event_to_hz(enum eventtype event)
{
    static const enum movement_hz mapping[] = {
        [EVENT_10HZ_TICK] = COMBAT_HZ_10,
        [EVENT_5HZ_TICK] = COMBAT_HZ_5,
        [EVENT_1HZ_TICK] = COMBAT_HZ_1,
        [EVENT_HALFHZ_TICK] = COMBAT_HZ_HALF,
    };
    return mapping[event];
}

static void register_callback_for_hz(enum combat_hz hz)
{
    assert(hz >= 0 && hz <= COMBAT_HZ_HALF);
    const enum eventtype mapping[] = {
        [COMBAT_HZ_10] = EVENT_10HZ_TICK,
        [COMBAT_HZ_5 ] = EVENT_5HZ_TICK,
        [COMBAT_HZ_1 ] = EVENT_1HZ_TICK,
        [COMBAT_HZ_HALF] = EVENT_HALFHZ_TICK,
    };
    E_Global_Register(mapping[hz], combat_tick, (void*)(uintptr_t)mapping[hz], G_RUNNING);
}

static void unregister_callback_for_hz(enum combat_hz hz)
{
    assert(hz >= 0 && hz <= COMBAT_HZ_HALF);
    const enum eventtype mapping[] = {
        [COMBAT_HZ_10] = EVENT_10HZ_TICK,
        [COMBAT_HZ_5 ] = EVENT_5HZ_TICK,
        [COMBAT_HZ_1 ] = EVENT_1HZ_TICK,
        [COMBAT_HZ_HALF] = EVENT_HALFHZ_TICK,
    };
    E_Global_Unregister(mapping[hz], combat_tick);
}

static void combat_handle_hz_update(enum eventtype curr)
{
    if(!s_combat_hz_dirty)
        return;

    s_combat_hz_dirty = false;

    static const enum eventtype mapping[] = {
        [COMBAT_HZ_10] = EVENT_10HZ_TICK,
        [COMBAT_HZ_5 ] = EVENT_5HZ_TICK,
        [COMBAT_HZ_1 ] = EVENT_1HZ_TICK,
        [COMBAT_HZ_HALF] = EVENT_HALFHZ_TICK,
    };
    enum eventtype next = mapping[s_combat_hz];

    if(curr == next)
        return;

    enum combat_hz curr_hz = event_to_hz(curr);
    enum combat_hz next_hz = s_combat_hz;

    unregister_callback_for_hz(curr_hz);
    register_callback_for_hz(next_hz);
}

static void combat_process_cmds(void)
{
    struct combat_cmd cmd;
    while(queue_cmd_pop(&s_combat_commands, &cmd)) {
        switch(cmd.type) {
        case COMBAT_CMD_ADD: {
            do_add_entity(cmd.uid, cmd.u.add.initial);
            break;
        }
        case COMBAT_CMD_REMOVE: {
            do_remove_entity(cmd.uid);
            break;
        }
        case COMBAT_CMD_TRYHIT: {
            do_tryhit(cmd.uid, cmd.u.tryhit.proj_pos);
            break;
        }
        case COMBAT_CMD_SET_STANCE: {
            do_set_stance(cmd.uid, cmd.u.set_stance.stance);
            break;
        }
        case COMBAT_CMD_CLEAR_SAVED_MOVE_CMD: {
            do_clear_saved_move_cmd(cmd.uid);
            break;
        }
        case COMBAT_CMD_ATTACK_UNIT: {
            do_attack_unit(cmd.uid, cmd.u.attack_unit.target);
            break;
        }
        case COMBAT_CMD_STOP_ATTACK: {
            do_stop_attack(cmd.uid);
            break;
        }
        case COMBAT_CMD_ADD_TIME_DELTA: {
            do_add_time_delta(cmd.u.time_delta.delta);
            break;
        }
        case COMBAT_CMD_ADD_REF: {
            do_add_ref(cmd.u.add_ref.faction_id, cmd.u.add_ref.pos);
            break;
        }
        case COMBAT_CMD_REMOVE_REF: {
            do_remove_ref(cmd.u.remove_ref.faction_id, cmd.u.remove_ref.pos);
            break;
        }
        case COMBAT_CMD_UPDATE_REF: {
            do_update_ref(cmd.u.update_ref.oldfac, cmd.u.update_ref.newfac,
                cmd.u.update_ref.pos);
            break;
        }
        case COMBAT_CMD_SET_BASE_ARMOUR: {
            do_set_base_armour(cmd.uid, cmd.u.base_armour.armour);
            break;
        }
        case COMBAT_CMD_SET_BASE_DAMAGE: {
            do_set_base_damage(cmd.uid, cmd.u.base_damage.dmg);
            break;
        }
        case COMBAT_CMD_SET_CURRENT_HP: {
            do_set_current_hp(cmd.uid, cmd.u.current_hp.hp);
            break;
        }
        case COMBAT_CMD_SET_MAX_HP: {
            do_set_max_hp(cmd.uid, cmd.u.max_hp.hp);
            break;
        }
        case COMBAT_CMD_SET_RANGE: {
            do_set_range(cmd.uid, cmd.u.set_range.range);
            break;
        }
        case COMBAT_CMD_SET_PROJ_DESC: {
            struct proj_desc *pd = cmd.u.proj_desc.pd;
            do_set_proj_desc(cmd.uid, pd);
            PF_FREE(pd);
            break;
        }
        case COMBAT_CMD_SET_PROJ_FIRE_DESC: {
            struct proj_fire_desc *fd = cmd.u.proj_fire_desc.fd;
            do_set_proj_fire_desc(cmd.uid, fd);
            PF_FREE(fd);
            break;
        }
        case COMBAT_CMD_SET_CORPSE_MODEL: {

            const char *dirkey = si_intern(cmd.u.corpse_model.dir, &s_stringpool, s_stridx);
            const char *objkey = si_intern(cmd.u.corpse_model.pfobj, &s_stringpool, s_stridx);
            if(dirkey && objkey) {
                do_set_corpse_model(cmd.uid, dirkey, objkey, cmd.u.corpse_model.scale);
            }
            break;
        }
        case COMBAT_CMD_PROJ_HIT: {
            struct proj_hit *hit = cmd.u.proj_hit.hit;
            do_proj_tryhit(hit);
            PF_FREE(hit);
            break;
        }
        case COMBAT_CMD_SET_DAMAGE_TYPE: {
            do_set_damage_type(cmd.uid, cmd.u.damage_type.type);
            break;
        }
        case COMBAT_CMD_SET_ARMOUR_TYPE: {
            do_set_armour_type(cmd.uid, cmd.u.armour_type.type);
            break;
        }
        case COMBAT_CMD_SET_DAMAGE_TABLE: {
            do_set_damage_table(cmd.u.damage_table.mult);
            break;
        }
        case COMBAT_CMD_SET_INVULNERABLE: {
            do_set_invulnerable(cmd.uid, cmd.u.invulnerable.on);
            break;
        }
        case COMBAT_CMD_ADD_MODIFIER: {
            do_add_modifier(cmd.uid, cmd.u.add_mod.kind, cmd.u.add_mod.amount,
                cmd.u.add_mod.percent, cmd.u.add_mod.secs, cmd.u.add_mod.tag);
            break;
        }
        case COMBAT_CMD_REMOVE_MODIFIER: {
            do_remove_modifier(cmd.uid, cmd.u.remove_mod.tag);
            break;
        }
        case COMBAT_CMD_CLEAR_MODIFIERS: {
            do_clear_modifiers(cmd.uid);
            break;
        }
        case COMBAT_CMD_SET_GROUP_BONUS: {
            do_set_group_bonus(cmd.uid, &cmd.u.group_bonus.desc);
            break;
        }
        case COMBAT_CMD_CLEAR_GROUP_BONUS: {
            do_clear_group_bonus(cmd.uid, cmd.u.clear_group_bonus.tag);
            break;
        }
        case COMBAT_CMD_REFRESH_BONUSES: {
            do_refresh_bonuses(cmd.uid);
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

        if(ncomputed % 16 == 0)
            Task_Yield();
    }
    return NULL_RESULT;
}

static void combat_complete_work(void)
{
    Sched_AwaitAll(s_combat_work.tids, s_combat_work.futures, s_combat_work.ntasks);
}

static khash_t(aabb) *combat_update_aabb_cache(void)
{
    PERF_ENTER();
    const khash_t(entity) *ents = G_GetAllEntsSet();

    if(kh_size(s_aabb_cache) > 2 * kh_size(ents) + 1024)
        kh_clear(aabb, s_aabb_cache);

    uint32_t uid;
    kh_foreach_key(ents, uid, {
        khiter_t k = kh_get(aabb, s_aabb_cache, uid);
        if(k == kh_end(s_aabb_cache)) {
            int ret;
            k = kh_put(aabb, s_aabb_cache, uid, &ret);
            assert(ret != -1);
            kh_value(s_aabb_cache, k) = AL_EntityGet(uid)->identity_aabb;
        }
    });
    PERF_RETURN(s_aabb_cache);
}

static void combat_copy_gamestate(void)
{
    PERF_ENTER();
    s_combat_work.gamestate.factions = G_GetFactions(NULL, NULL, NULL);
    s_combat_work.gamestate.player_factions = G_GetPlayerControlledFactions();
    s_combat_work.gamestate.fog_enabled = G_Fog_Enabled();
    s_combat_work.gamestate.flags = G_FlagsCopyTableInto(s_combat_work.gamestate.flags);
    s_combat_work.gamestate.positions =
        G_Pos_CopyTableInto(s_combat_work.gamestate.positions);
    s_combat_work.gamestate.postree =
        G_Pos_CopyBitmapGridInto(s_combat_work.gamestate.postree);
    s_combat_work.gamestate.transforms =
        Entity_CopyTransformsInto(s_combat_work.gamestate.transforms);
    s_combat_work.gamestate.sel_radiuses =
        G_SelectionRadiusCopyTableInto(s_combat_work.gamestate.sel_radiuses);
    s_combat_work.gamestate.faction_ids =
        G_FactionIDCopyTableInto(s_combat_work.gamestate.faction_ids);
    s_combat_work.gamestate.diptable = G_CopyDiplomacyTable();
    s_combat_work.gamestate.buildstate = G_Building_CopyState();
    s_combat_work.gamestate.aabbs = combat_update_aabb_cache();
    s_combat_work.gamestate.fog_state =
        G_Fog_CopyStateInto(s_combat_work.gamestate.fog_state, &s_fog_snap_ntiles);
    PERF_RETURN_VOID();
}

static void combat_destroy_gamestate(void)
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
        G_Pos_DestroyBitmapGrid(s_combat_work.gamestate.postree);
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
        kh_destroy(state, s_combat_work.gamestate.buildstate);
        s_combat_work.gamestate.buildstate = NULL;
    }
    /* Aliases the persistent cache, which is destroyed separately */
    s_combat_work.gamestate.aabbs = NULL;
    if(s_combat_work.gamestate.fog_state) {
        PF_FREE(s_combat_work.gamestate.fog_state);
        s_combat_work.gamestate.fog_state = NULL;
    }
    s_fog_snap_ntiles = 0;
    PERF_RETURN_VOID();
}

/* The copied tables are retained and refilled by the next tick's copy; only
 * the small per-tick resources are dropped here.
 */
static void combat_release_gamestate(void)
{
    PERF_ENTER();
    if(s_combat_work.gamestate.diptable) {
        PF_FREE(s_combat_work.gamestate.diptable);
        s_combat_work.gamestate.diptable = NULL;
    }
    if(s_combat_work.gamestate.buildstate) {
        kh_destroy(state, s_combat_work.gamestate.buildstate);
        s_combat_work.gamestate.buildstate = NULL;
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
            "combat_task", &s_combat_work.futures[s_combat_work.ntasks], TASK_BIG_STACK);

        if(s_combat_work.tids[s_combat_work.ntasks] == NULL_TID) {
            combat_work(arg->begin_idx, arg->end_idx);
        }else{
            s_combat_work.ntasks++;
        }
    }
}

static void combat_tick(void *user, void *event)
{
    if(s_last_tick == g_frame_idx)
        return;

    PERF_PUSH("combat::combat_tick");
    enum eventtype curr_event = (uintptr_t)user;

    combat_finish_work();
    combat_handle_hz_update(curr_event);
    combat_process_cmds();
    combat_release_gamestate();

    /* Snapshot after the drain, so it agrees with the combat state table. */
    combat_prepare_work();
    combat_copy_gamestate();

    uint32_t uid;
    kh_foreach_key(s_entity_state_table, uid, {
        combat_push_work((struct combat_work_in){uid});
    });
    combat_submit_work();
    s_last_tick = g_frame_idx;

    PERF_POP();
}

static void on_1hz_tick(void *user, void *event)
{
    combat_mods_tick();

    size_t ncorpses = vec_size(&s_corpses);
    for(int i = 0; i < ncorpses; i++) {
        struct corpse *curr = &vec_AT(&s_corpses, i);
        curr->secs_left--;
        if(curr->secs_left == 0) {

            uint32_t uid = curr->uid;
            uint32_t tid = Sched_Create(1, corpse_disappear_task, (void*)(uintptr_t)uid, 
                "corpse_disappear_task", NULL, TASK_MAIN_THREAD_PINNED | TASK_BIG_STACK);
            Sched_RunSync(tid);

            struct corpse *last = &vec_AT(&s_corpses, ncorpses-1);
            memmove(curr, last, sizeof(struct corpse));
            ncorpses--;
            s_corpses.size--;
        }
    }
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

            if(entity_dead(curr.target_uid) || garrisoned(curr.target_uid))
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

        const float radius = combat_effective_range(&curr);
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
    struct proj_hit *copy = PF_MALLOC(sizeof(struct proj_hit));
    if(!copy)
        return;
    *copy = *hit;

    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_PROJ_HIT,
        .u.proj_hit = {
            .hit = copy
        }
    });
}

static float hz_count(enum combat_hz hz)
{
    switch(hz) {
    case COMBAT_HZ_10:    return 10.0f;
    case COMBAT_HZ_5:     return 5.0f;
    case COMBAT_HZ_1:     return 1.0f;
    case COMBAT_HZ_HALF:  return 0.5f;
    default: assert(0);
    }
    return 0;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Combat_Init(const struct map *map)
{
    for(int d = 0; d < DAMAGE_TYPE_MAX; d++) {
        for(int a = 0; a < ARMOUR_TYPE_MAX; a++) {
            s_dmg_mult[d][a] = 1.0f;
        }
    }

    if(NULL == (s_entity_state_table = kh_init(state)))
        return false;

    if(NULL == (s_attack_anim_pending = kh_init(animpend))) {
        kh_destroy(state, s_entity_state_table);
        return false;
    }

    if(NULL == (s_aabb_cache = kh_init(aabb))) {
        kh_destroy(animpend, s_attack_anim_pending);
        kh_destroy(state, s_entity_state_table);
        return false;
    }

    memset(&s_combat_work, 0, sizeof(s_combat_work));
    if(!stalloc_init(&s_combat_work.mem))
        goto fail_stack;

    if(!queue_cmd_init(&s_combat_commands, 256))
        goto fail_queue;

    if(!si_init(&s_stringpool, &s_stridx, 512))
        goto fail_strintern;

    struct map_resolution res;
    M_GetResolution(map, &res);

    for(int i = 0; i < MAX_FACTIONS; i++) {
        s_fac_refcnts[i] = PF_CALLOC((res.chunk_w * X_BINS_PER_CHUNK) 
                                * (res.chunk_h * Z_BINS_PER_CHUNK) 
                                * sizeof(uint16_t), 1);
        if(!s_fac_refcnts[i])
            goto fail_refcnts;
    }

    vec_entity_init(&s_dying_ents);
    E_Global_Register(EVENT_1HZ_TICK, on_1hz_tick, NULL, G_RUNNING);
    register_callback_for_hz(s_combat_hz);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, G_ALL);
    E_Global_Register(EVENT_PROJECTILE_HIT, on_proj_hit, NULL, G_RUNNING);
    E_Global_Register(EVENT_UPDATE_START, on_attack_anim_tick, NULL, G_RUNNING);
    s_map = map;
    combat_copy_gamestate();
    vec_corpse_init(&s_corpses);
    s_mods = kh_init(modlist);
    vec_gbonus_init(&s_gbonuses);
    return true;

fail_refcnts:
    for(int i = 0; i < MAX_FACTIONS; i++)
        PF_FREE(s_fac_refcnts[i]);
    si_shutdown(&s_stringpool, s_stridx);
fail_strintern:
    queue_cmd_destroy(&s_combat_commands);
fail_queue:
    stalloc_destroy(&s_combat_work.mem);
fail_stack:
    kh_destroy(animpend, s_attack_anim_pending);
    kh_destroy(state, s_entity_state_table);
    return false;
}

void G_Combat_Shutdown(void)
{
    combat_complete_work();
    s_map = NULL;

    E_Global_Unregister(EVENT_1HZ_TICK, on_1hz_tick);
    unregister_callback_for_hz(s_combat_hz);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    E_Global_Unregister(EVENT_PROJECTILE_HIT, on_proj_hit);
    E_Global_Unregister(EVENT_UPDATE_START, on_attack_anim_tick);

    uint32_t uid;
    kh_foreach_key(s_entity_state_table, uid, {
        attack_anim_pending_remove(uid);
        E_Entity_Unregister(EVENT_ANIM_CYCLE_FINISHED, uid, on_death_anim_finish);
    });

    combat_destroy_gamestate();
    vec_entity_destroy(&s_dying_ents);
    for(int i = 0; i < MAX_FACTIONS; i++) {
        PF_FREE(s_fac_refcnts[i]);
    }
    queue_cmd_destroy(&s_combat_commands);
    stalloc_destroy(&s_combat_work.mem);
    kh_destroy(aabb, s_aabb_cache);
    kh_destroy(animpend, s_attack_anim_pending);
    kh_destroy(state, s_entity_state_table);

    vec_corpse_destroy(&s_corpses);
    if(s_mods) {
        vec_mod_t curr;
        kh_foreach_value(s_mods, curr, {
            vec_mod_destroy(&curr);
        });
        kh_destroy(modlist, s_mods);
        s_mods = NULL;
    }
    vec_gbonus_destroy(&s_gbonuses);
    si_shutdown(&s_stringpool, s_stridx);
}

bool G_Combat_HasWork(void)
{
    return (queue_size(s_combat_commands) > 0);
}

bool G_Combat_GetHPDisplay(uint32_t uid, int *out_curr, int *out_max)
{
    ASSERT_IN_MAIN_THREAD();

    struct combatstate *cs = combatstate_get(uid);
    if(!cs)
        return false;
    *out_curr = cs->current_hp;
    *out_max  = cs->stats.max_hp;
    return true;
}

void G_Combat_FlushWork(void)
{
    if(!s_map)
        return;
    combat_finish_work();
    combat_process_cmds();
}

void G_Combat_AddEntity(uint32_t uid, enum combat_stance initial)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_ADD,
        .uid = uid,
        .u.add = {
            .initial = initial
        }
    });
}

void G_Combat_RemoveEntity(uint32_t uid)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_REMOVE,
        .uid = uid
    });
}

void G_Combat_SetStance(uint32_t uid, enum combat_stance stance)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_STANCE,
        .uid = uid,
        .u.set_stance = {
            .stance = stance
        }
    });
}

enum combat_stance G_Combat_GetStance(uint32_t uid)
{
    struct combat_cmd *cmd = snoop_most_recent_command(COMBAT_CMD_SET_STANCE,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->u.set_stance.stance;
    }
    cmd = snoop_most_recent_command(COMBAT_CMD_ADD,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->u.add.initial;
    }

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    return cs->stance;
}

void G_Combat_ClearSavedMoveCmd(uint32_t uid)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_CLEAR_SAVED_MOVE_CMD,
        .uid = uid
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

bool G_Combat_Idle(uint32_t uid)
{
    struct combatstate *cs = combatstate_get(uid);
    if(!cs)
        return true;
    /* Hang-back states count as idle so automatic abilities keep firing. */
    return (cs->state == STATE_NOT_IN_COMBAT)
        || (cs->state == STATE_STANDING_GROUND)
        || (cs->state == STATE_NON_COMBATANT_FLEEING);
}

void G_Combat_AttackUnit(uint32_t uid, uint32_t target)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_ATTACK_UNIT,
        .uid = uid,
        .u.attack_unit.target = target
    });
}

void G_Combat_StopAttack(uint32_t uid)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_STOP_ATTACK,
        .uid = uid
    });
}

void G_Combat_AddTimeDelta(uint32_t delta)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_ADD_TIME_DELTA,
        .u.time_delta.delta = delta
    });
}

void G_Combat_AddRef(int faction_id, vec2_t pos)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_ADD_REF,
        .u.add_ref = {
            .faction_id = faction_id,
            .pos = pos
        }
    });
}

void G_Combat_RemoveRef(int faction_id, vec2_t pos)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_REMOVE_REF,
        .u.remove_ref = {
            .faction_id = faction_id,
            .pos = pos
        }
    });
}

/* A remove+add pair whose endpoints fall in the same position bin cancels
 * exactly at drain time; skip enqueueing both.
 */
void G_Combat_MoveRef(int faction_id, vec2_t from, vec2_t to)
{
    if(s_map) {
        struct map_resolution mapres;
        M_GetResolution(s_map, &mapres);

        struct map_resolution binres = (struct map_resolution){
            mapres.chunk_w, mapres.chunk_h,
            X_BINS_PER_CHUNK, Z_BINS_PER_CHUNK,
            mapres.field_w, mapres.field_h
        };

        struct tile_desc from_td, to_td;
        if(M_Tile_DescForPoint2D(binres, M_GetPos(s_map), from, &from_td)
        && M_Tile_DescForPoint2D(binres, M_GetPos(s_map), to, &to_td)
        && from_td.chunk_r == to_td.chunk_r && from_td.chunk_c == to_td.chunk_c
        && from_td.tile_r == to_td.tile_r && from_td.tile_c == to_td.tile_c)
            return;
    }
    G_Combat_RemoveRef(faction_id, from);
    G_Combat_AddRef(faction_id, to);
}

void G_Combat_UpdateRef(int oldfac, int newfac, vec2_t pos)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_UPDATE_REF,
        .u.update_ref = {
            .oldfac = oldfac,
            .newfac = newfac,
            .pos = pos
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
        return cmd->u.current_hp.hp;
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

void G_Combat_SetBaseArmour(uint32_t uid, int armour)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_BASE_ARMOUR,
        .uid = uid,
        .u.base_armour.armour = armour
    });
}

int G_Combat_GetBaseArmour(uint32_t uid)
{
    struct combat_cmd *cmd;
    cmd = snoop_most_recent_command(COMBAT_CMD_SET_BASE_ARMOUR,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->u.base_armour.armour;
    }
    cmd = snoop_most_recent_command(COMBAT_CMD_ADD,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return 0;
    }

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    return cs->stats.base_armour;
}

int G_Combat_ArmourPointsForFrac(float frac)
{
    if(frac <= 0.0f)
        return ARMOUR_MIN_POINTS;
    if(frac >= 1.0f)
        return ARMOUR_MAX_POINTS;

    int points = (int)roundf(ARMOUR_K * frac / (1.0f - frac));
    return MIN(MAX(points, ARMOUR_MIN_POINTS), ARMOUR_MAX_POINTS);
}

float G_Combat_FracForArmourPoints(int armour)
{
    return 1.0f - combat_armour_mult(armour);
}

void G_Combat_SetInvulnerable(uint32_t uid, bool on)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_INVULNERABLE,
        .uid = uid,
        .u.invulnerable.on = on
    });
}

bool G_Combat_GetInvulnerable(uint32_t uid)
{
    struct combat_cmd *cmd;
    cmd = snoop_most_recent_command(COMBAT_CMD_SET_INVULNERABLE,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->u.invulnerable.on;
    }
    cmd = snoop_most_recent_command(COMBAT_CMD_ADD,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return false;
    }

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    return cs->invulnerable;
}

void G_Combat_AddModifier(uint32_t uid, enum combat_mod_kind kind, float amount,
                          bool percent, uint32_t secs, const char *tag)
{
    struct combat_cmd cmd = (struct combat_cmd){
        .type = COMBAT_CMD_ADD_MODIFIER,
        .uid = uid,
        .u.add_mod.kind = kind,
        .u.add_mod.amount = amount,
        .u.add_mod.percent = percent,
        .u.add_mod.secs = secs
    };
    pf_strlcpy(cmd.u.add_mod.tag, tag ? tag : "", sizeof(cmd.u.add_mod.tag));
    combat_push_cmd(cmd);
}

void G_Combat_SetGroupBonus(uint32_t uid, const struct group_bonus_desc *desc)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_GROUP_BONUS,
        .uid = uid,
        .u.group_bonus.desc = *desc
    });
}

void G_Combat_ClearGroupBonus(uint32_t uid, const char *tag)
{
    struct combat_cmd cmd = (struct combat_cmd){
        .type = COMBAT_CMD_CLEAR_GROUP_BONUS,
        .uid = uid
    };
    pf_strlcpy(cmd.u.clear_group_bonus.tag, tag ? tag : "",
        sizeof(cmd.u.clear_group_bonus.tag));
    combat_push_cmd(cmd);
}

int G_Combat_GetGroupBonuses(uint32_t uid, struct group_bonus_desc *out, size_t maxout)
{
    ASSERT_IN_MAIN_THREAD();

    size_t ret = 0;
    for(int i = 0; i < vec_size(&s_gbonuses) && ret < maxout; i++) {
        const struct group_bonus_rec *curr = &vec_AT(&s_gbonuses, i);
        if(curr->uid != uid)
            continue;
        out[ret++] = curr->desc;
    }
    return ret;
}

void G_Combat_RefreshBonuses(uint32_t uid)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_REFRESH_BONUSES,
        .uid = uid
    });
}

void G_Combat_RemoveModifier(uint32_t uid, const char *tag)
{
    struct combat_cmd cmd = (struct combat_cmd){
        .type = COMBAT_CMD_REMOVE_MODIFIER,
        .uid = uid
    };
    pf_strlcpy(cmd.u.remove_mod.tag, tag ? tag : "", sizeof(cmd.u.remove_mod.tag));
    combat_push_cmd(cmd);
}

void G_Combat_ClearModifiers(uint32_t uid)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_CLEAR_MODIFIERS,
        .uid = uid
    });
}

/* True if a command queued after 'start' drops a modifier of this entity
 * carrying 'tag': a clear, a matching removal, or a matching tagged add, which
 * replaces rather than stacks. An untagged modifier only ever loses to a clear.
 */
static bool mod_superseded(uint32_t uid, const char *tag, int start, size_t nleft)
{
    for(int i = start; nleft > 0; nleft--, i = (i + 1) % s_combat_commands.capacity) {

        const struct combat_cmd *curr = &s_combat_commands.mem[i];
        if(curr->uid != uid)
            continue;

        if(curr->type == COMBAT_CMD_CLEAR_MODIFIERS)
            return true;
        if(tag[0] == '\0')
            continue;
        if(curr->type == COMBAT_CMD_REMOVE_MODIFIER && !strcmp(curr->u.remove_mod.tag, tag))
            return true;
        if(curr->type == COMBAT_CMD_ADD_MODIFIER && !strcmp(curr->u.add_mod.tag, tag))
            return true;
    }
    return false;
}

/* The base a percent modifier resolves against, as the queue will leave it. The
 * simulation's combat_base_stat reads the committed state instead, which is not
 * yet there for an entity created this frame.
 */
static float combat_base_stat_queued(uint32_t uid, enum combat_mod_kind kind)
{
    switch(kind) {
    case COMBAT_MOD_ARMOUR:
        return G_Combat_GetBaseArmour(uid);
    case COMBAT_MOD_DAMAGE:
        return G_Combat_GetBaseDamage(uid);
    case COMBAT_MOD_RANGE:
        return G_Combat_GetRange(uid);
    case COMBAT_MOD_SPEED: {
        float speed = 0.0f;
        G_Move_GetMaxSpeed(uid, &speed);
        return speed;
    }
    default:
        return 0.0f;
    }
}

/* Modifiers accumulate rather than overwrite, so a single most-recent snoop
 * cannot answer this. The queued commands are replayed over the committed
 * records instead, which is what makes a read right after an add report what
 * that add will actually commit. The group's contribution is not queued, so it
 * is read straight from the aggregate.
 */
float G_Combat_GetBonus(uint32_t uid, enum combat_mod_kind kind)
{
    ASSERT_IN_MAIN_THREAD();

    if(kind < 0 || kind >= COMBAT_MOD_MAX)
        return 0.0f;

    const float base = combat_base_stat_queued(uid, kind);
    const size_t npending = queue_size(s_combat_commands);
    const int head = s_combat_commands.ihead;
    float bonus = 0.0f;

    const vec_mod_t *list = combat_mods_for(uid, false);
    for(int i = 0; list && i < vec_size(list); i++) {
        const struct combat_mod *curr = &vec_AT(list, i);
        if(curr->kind != kind)
            continue;
        if(mod_superseded(uid, curr->tag, head, npending))
            continue;
        bonus += curr->percent ? curr->amount * base : curr->amount;
    }

    size_t left = npending;
    for(int i = head; left > 0; left--, i = (i + 1) % s_combat_commands.capacity) {

        const struct combat_cmd *curr = &s_combat_commands.mem[i];
        if(curr->uid != uid || curr->type != COMBAT_CMD_ADD_MODIFIER)
            continue;
        if(curr->u.add_mod.kind != kind)
            continue;

        int next = (i + 1) % s_combat_commands.capacity;
        if(mod_superseded(uid, curr->u.add_mod.tag, next, left - 1))
            continue;
        bonus += curr->u.add_mod.percent
               ? curr->u.add_mod.amount * base
               : curr->u.add_mod.amount;
    }

    int gid = G_Group_ForEnt(uid);
    if(gid) {
        float flat = 0.0f, percent = 0.0f;
        G_Group_GetBonus(gid, kind, &flat, &percent);
        bonus += flat + percent * base;
    }
    return bonus;
}

int G_Combat_GetEffectiveArmour(uint32_t uid)
{
    return G_Combat_GetBaseArmour(uid)
         + (int)roundf(G_Combat_GetBonus(uid, COMBAT_MOD_ARMOUR));
}

int G_Combat_GetEffectiveDamage(uint32_t uid)
{
    return MAX(0, G_Combat_GetBaseDamage(uid)
                + (int)roundf(G_Combat_GetBonus(uid, COMBAT_MOD_DAMAGE)));
}

float G_Combat_GetEffectiveRange(uint32_t uid)
{
    float base = G_Combat_GetRange(uid);
    if(base == 0.0f)
        return 0.0f;
    return MAX(0.0f, base + G_Combat_GetBonus(uid, COMBAT_MOD_RANGE));
}

bool G_Combat_GetEffectiveInvulnerable(uint32_t uid)
{
    return G_Combat_GetInvulnerable(uid)
        || (G_Combat_GetBonus(uid, COMBAT_MOD_INVULNERABLE) > 0.0f);
}

void G_Combat_SetBaseDamage(uint32_t uid, int dmg)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_BASE_DAMAGE,
        .uid = uid,
        .u.base_damage.dmg = dmg
    });
}

int G_Combat_GetBaseDamage(uint32_t uid)
{
    struct combat_cmd *cmd;
    cmd = snoop_most_recent_command(COMBAT_CMD_SET_BASE_DAMAGE,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->u.base_damage.dmg;
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

void G_Combat_SetDamageType(uint32_t uid, int type)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_DAMAGE_TYPE,
        .uid = uid,
        .u.damage_type.type = type
    });
}

int G_Combat_GetDamageType(uint32_t uid)
{
    struct combat_cmd *cmd;
    cmd = snoop_most_recent_command(COMBAT_CMD_SET_DAMAGE_TYPE,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->u.damage_type.type;
    }
    cmd = snoop_most_recent_command(COMBAT_CMD_ADD,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return 0;
    }

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    return cs->stats.dmg_type;
}

void G_Combat_SetArmourType(uint32_t uid, int type)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_ARMOUR_TYPE,
        .uid = uid,
        .u.armour_type.type = type
    });
}

int G_Combat_GetArmourType(uint32_t uid)
{
    struct combat_cmd *cmd;
    cmd = snoop_most_recent_command(COMBAT_CMD_SET_ARMOUR_TYPE,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->u.armour_type.type;
    }
    cmd = snoop_most_recent_command(COMBAT_CMD_ADD,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return 0;
    }

    struct combatstate *cs = combatstate_get(uid);
    assert(cs);
    return cs->stats.armour_type;
}

void G_Combat_SetDamageTable(const float *mult, int nrows, int ncols)
{
    assert(nrows >= 0 && nrows <= DAMAGE_TYPE_MAX);
    assert(ncols >= 0 && ncols <= ARMOUR_TYPE_MAX);

    struct combat_cmd cmd = (struct combat_cmd){
        .type = COMBAT_CMD_SET_DAMAGE_TABLE,
        .uid = 0
    };
    float (*dst)[ARMOUR_TYPE_MAX] = (float (*)[ARMOUR_TYPE_MAX])cmd.u.damage_table.mult;
    for(int d = 0; d < DAMAGE_TYPE_MAX; d++) {
        for(int a = 0; a < ARMOUR_TYPE_MAX; a++) {
            dst[d][a] = (d < nrows && a < ncols) ? mult[d * ncols + a] : 1.0f;
        }
    }
    combat_push_cmd(cmd);
}

void G_Combat_GetDamageTable(float *out_mult)
{
    struct combat_cmd *cmd;
    cmd = snoop_most_recent_command(COMBAT_CMD_SET_DAMAGE_TABLE, NULL, any_command);
    if(cmd) {
        memcpy(out_mult, cmd->u.damage_table.mult, sizeof(cmd->u.damage_table.mult));
        return;
    }
    memcpy(out_mult, s_dmg_mult, sizeof(s_dmg_mult));
}

void G_Combat_SetCurrentHP(uint32_t uid, int hp)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_CURRENT_HP,
        .uid = uid,
        .u.current_hp.hp = hp
    });
}

void G_Combat_SetMaxHP(uint32_t uid, int hp)
{
    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_MAX_HP,
        .uid = uid,
        .u.max_hp.hp = hp
    });
}

int G_Combat_GetMaxHP(uint32_t uid)
{
    struct combat_cmd *cmd;
    cmd = snoop_most_recent_command(COMBAT_CMD_SET_MAX_HP,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->u.max_hp.hp;
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
        .uid = uid,
        .u.set_range.range = range
    });
}

void G_Combat_SetProjDesc(uint32_t uid, const struct proj_desc *pd)
{
    struct proj_desc *copy = PF_MALLOC(sizeof(struct proj_desc));
    if(!copy)
        return;
    *copy = *pd;
    copy->basedir = pf_strdup(pd->basedir);
    copy->pfobj = pf_strdup(pd->pfobj);
    if(pd->flags & PROJ_HAS_IMPACT_SPRITE) {
        copy->impact_sprite.filename = pf_strdup(pd->impact_sprite.filename);
    }
    if(pd->flags & PROJ_HAS_TRAIL_SPRITE) {
        copy->trail_sprite.filename = pf_strdup(pd->trail_sprite.filename);
    }

    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_PROJ_DESC,
        .uid = uid,
        .u.proj_desc.pd = copy
    });
}

void G_Combat_SetProjFireDesc(uint32_t uid, const struct proj_fire_desc *fd)
{
    struct proj_fire_desc *copy = PF_MALLOC(sizeof(struct proj_fire_desc));
    if(!copy)
        return;
    *copy = *fd;

    combat_push_cmd((struct combat_cmd){
        .type = COMBAT_CMD_SET_PROJ_FIRE_DESC,
        .uid = uid,
        .u.proj_fire_desc.fd = copy
    });
}

float G_Combat_GetRange(uint32_t uid)
{
    struct combat_cmd *cmd;
    cmd = snoop_most_recent_command(COMBAT_CMD_SET_RANGE,
        (void*)(uintptr_t)uid, uids_match);
    if(cmd) {
        return cmd->u.set_range.range;
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

void G_Combat_SetCorpseModel(uint32_t uid, const char *dir, const char *pfobj, vec3_t scale)
{
    struct combat_cmd cmd = (struct combat_cmd){
        .type = COMBAT_CMD_SET_CORPSE_MODEL,
        .uid = uid,
        .u.corpse_model.scale = scale
    };
    pf_strlcpy(cmd.u.corpse_model.dir, dir, sizeof(cmd.u.corpse_model.dir));
    pf_strlcpy(cmd.u.corpse_model.pfobj, pfobj, sizeof(cmd.u.corpse_model.pfobj));
    combat_push_cmd(cmd);
}

struct kh_id_s *G_Combat_GetDyingSetCopy(void)
{
    khash_t(id) *ret = kh_init(id);
    if(!ret)
        return NULL;

    uint32_t key;
    struct combatstate curr;

    kh_foreach(s_entity_state_table, key, curr, {
        if(curr.state == STATE_DEATH_ANIM_PLAYING) {

            int result;
            khiter_t k = kh_put(id, ret, key, &result);
            if(result == -1) {
                kh_destroy(id, ret);
                return NULL;
            }
        }
    });
    return ret;
}

void G_Combat_SetTickHz(enum combat_hz hz)
{
    s_combat_hz_dirty = (s_combat_hz != hz);
    s_combat_hz = hz;
}

float G_Combat_GetTickHz(void)
{
    return hz_count(s_combat_hz);
}

bool G_Combat_SaveState(struct SDL_RWops *stream)
{
    struct attr ndmg = (struct attr){
        .type = TYPE_INT,
        .val.as_int = DAMAGE_TYPE_MAX
    };
    CHK_TRUE_RET(Attr_Write(stream, &ndmg, "num_damage_types"));

    struct attr narmour = (struct attr){
        .type = TYPE_INT,
        .val.as_int = ARMOUR_TYPE_MAX
    };
    CHK_TRUE_RET(Attr_Write(stream, &narmour, "num_armour_types"));

    for(int d = 0; d < DAMAGE_TYPE_MAX; d++) {
        for(int a = 0; a < ARMOUR_TYPE_MAX; a++) {
            struct attr mult = (struct attr){
                .type = TYPE_FLOAT,
                .val.as_float = s_dmg_mult[d][a]
            };
            CHK_TRUE_RET(Attr_Write(stream, &mult, "dmg_mult"));
        }
    }

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

        struct attr invulnerable = (struct attr){
            .type = TYPE_BOOL,
            .val.as_int = curr.invulnerable
        };
        CHK_TRUE_RET(Attr_Write(stream, &invulnerable, "invulnerable"));

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

        struct attr pd_flags = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.pd.flags,
        };
        CHK_TRUE_RET(Attr_Write(stream, &pd_flags, "pd_flags"));
        Sched_TryYield();

        CHK_TRUE_RET(Sprite_SaveDesc(&curr.pd.impact_sprite, stream));

        struct attr pd_impact_size = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.pd.impact_size,
        };
        CHK_TRUE_RET(Attr_Write(stream, &pd_impact_size, "pd_impact_size"));

        CHK_TRUE_RET(Sprite_SaveDesc(&curr.pd.trail_sprite, stream));

        struct attr pd_trail_size = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.pd.trail_size,
        };
        CHK_TRUE_RET(Attr_Write(stream, &pd_trail_size, "pd_trail_size"));

        struct attr pd_trail_freq = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = curr.pd.trail_freq,
        };
        CHK_TRUE_RET(Attr_Write(stream, &pd_trail_freq, "pd_trail_freq"));
        Sched_TryYield();

        struct attr fd_frame_offset  = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.fd.frame_offset,
        };
        CHK_TRUE_RET(Attr_Write(stream, &fd_frame_offset, "fd_frame_offset"));

        struct attr fd_fire_mode  = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.fd.fire_mode,
        };
        CHK_TRUE_RET(Attr_Write(stream, &fd_fire_mode, "fd_fire_mode"));

        struct attr fd_bone_name  = (struct attr){
            .type = TYPE_STRING,
        };
        pf_strlcpy(fd_bone_name.val.as_string, curr.fd.bone_name,
            sizeof(fd_bone_name.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &fd_bone_name, "fd_bone_name"));

        struct attr fd_offset  = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = curr.fd.offset,
        };
        CHK_TRUE_RET(Attr_Write(stream, &fd_offset, "fd_offset"));
        Sched_TryYield();

        struct attr corpse_basedir = (struct attr){
            .type = TYPE_STRING,
            .val.as_string = "NULL"
        };
        if(curr.corpse_dir) {
            pf_strlcpy(corpse_basedir.val.as_string, curr.corpse_dir, 
                sizeof(corpse_basedir.val.as_string));
        }
        CHK_TRUE_RET(Attr_Write(stream, &corpse_basedir, "corpse_basedir"));

        struct attr corpse_pfobj = (struct attr){
            .type = TYPE_STRING,
            .val.as_string = "NULL"
        };
        if(curr.corpse_pfobj) {
            pf_strlcpy(corpse_pfobj.val.as_string, curr.corpse_pfobj, 
                sizeof(corpse_pfobj.val.as_string));
        }
        CHK_TRUE_RET(Attr_Write(stream, &corpse_pfobj, "corpse_pfobj"));

        struct attr corpse_scale  = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = curr.corpse_scale,
        };
        CHK_TRUE_RET(Attr_Write(stream, &corpse_scale, "corpse_scale"));
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

    struct attr num_corpses = (struct attr){
        .type = TYPE_INT,
        .val.as_int = vec_size(&s_corpses)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_corpses, "num_corpses"));
    Sched_TryYield();

    for(int i = 0; i < vec_size(&s_corpses); i++) {

        uint32_t corpse_ent = vec_AT(&s_corpses, i).uid;
        struct attr corpse_ent_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = corpse_ent
        };
        CHK_TRUE_RET(Attr_Write(stream, &corpse_ent_attr, "corpse_ent"));

        uint32_t secs_left = vec_AT(&s_corpses, i).secs_left;
        struct attr secs_left_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = secs_left
        };
        CHK_TRUE_RET(Attr_Write(stream, &secs_left_attr, "secs_left"));

        struct attr corpse_loc_attr = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = G_Pos_Get(corpse_ent)
        };
        CHK_TRUE_RET(Attr_Write(stream, &corpse_loc_attr, "corpse_loc"));

        struct attr corpse_rot_attr = (struct attr){
            .type = TYPE_QUAT,
            .val.as_quat = Entity_GetRot(corpse_ent)
        };
        CHK_TRUE_RET(Attr_Write(stream, &corpse_rot_attr, "corpse_rot"));

        struct attr corpse_scale_attr = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = Entity_GetScale(corpse_ent)
        };
        CHK_TRUE_RET(Attr_Write(stream, &corpse_scale_attr, "corpse_scale"));

        struct attr corpse_dir = (struct attr){
            .type = TYPE_STRING,
        };
        const char *dir = vec_AT(&s_corpses, i).dir;
        pf_strlcpy(corpse_dir.val.as_string, dir, sizeof(corpse_dir.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &corpse_dir, "corpse_dir"));

        struct attr corpse_pfobj = (struct attr){
            .type = TYPE_STRING,
        };
        const char *pfobj = vec_AT(&s_corpses, i).pfobj;
        pf_strlcpy(corpse_pfobj.val.as_string, pfobj, sizeof(corpse_pfobj.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &corpse_pfobj, "corpse_pfobj"));

        Sched_TryYield();
    }

    size_t total_mods = 0;
    vec_mod_t mods_curr;
    kh_foreach_value(s_mods, mods_curr, {
        total_mods += vec_size(&mods_curr);
    });

    struct attr num_mods = (struct attr){
        .type = TYPE_INT,
        .val.as_int = total_mods
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_mods, "num_mods"));

    vec_mod_t *mods_list;
    kh_foreach_ptr(s_mods, mods_list, {
    for(int i = 0; i < vec_size(mods_list); i++) {

        const struct combat_mod *mod = &vec_AT(mods_list, i);
        struct attr mod_ent = (struct attr){
            .type = TYPE_INT,
            .val.as_int = mod->uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &mod_ent, "mod_ent"));

        struct attr mod_secs = (struct attr){
            .type = TYPE_INT,
            .val.as_int = mod->secs_left
        };
        CHK_TRUE_RET(Attr_Write(stream, &mod_secs, "mod_secs_left"));

        struct attr mod_kind = (struct attr){
            .type = TYPE_INT,
            .val.as_int = mod->kind
        };
        CHK_TRUE_RET(Attr_Write(stream, &mod_kind, "mod_kind"));

        struct attr mod_amount = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = mod->amount
        };
        CHK_TRUE_RET(Attr_Write(stream, &mod_amount, "mod_amount"));

        struct attr mod_percent = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = mod->percent
        };
        CHK_TRUE_RET(Attr_Write(stream, &mod_percent, "mod_percent"));

        struct attr mod_tag = (struct attr){
            .type = TYPE_STRING,
        };
        pf_strlcpy(mod_tag.val.as_string, mod->tag, sizeof(mod_tag.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &mod_tag, "mod_tag"));

        Sched_TryYield();
    }});

    struct attr num_gbonuses = (struct attr){
        .type = TYPE_INT,
        .val.as_int = vec_size(&s_gbonuses)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_gbonuses, "num_gbonuses"));

    for(int i = 0; i < vec_size(&s_gbonuses); i++) {

        const struct group_bonus_rec *rec = &vec_AT(&s_gbonuses, i);
        struct attr gb_ent = (struct attr){
            .type = TYPE_INT,
            .val.as_int = rec->uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &gb_ent, "gbonus_ent"));

        struct attr gb_kind = (struct attr){
            .type = TYPE_INT,
            .val.as_int = rec->desc.kind
        };
        CHK_TRUE_RET(Attr_Write(stream, &gb_kind, "gbonus_kind"));

        struct attr gb_amount = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = rec->desc.amount
        };
        CHK_TRUE_RET(Attr_Write(stream, &gb_amount, "gbonus_amount"));

        struct attr gb_percent = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = rec->desc.percent
        };
        CHK_TRUE_RET(Attr_Write(stream, &gb_percent, "gbonus_percent"));

        struct attr gb_tag = (struct attr){
            .type = TYPE_STRING,
        };
        pf_strlcpy(gb_tag.val.as_string, rec->desc.tag, sizeof(gb_tag.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &gb_tag, "gbonus_tag"));

        struct attr gb_icon = (struct attr){
            .type = TYPE_STRING,
        };
        pf_strlcpy(gb_icon.val.as_string, rec->desc.icon, sizeof(gb_icon.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &gb_icon, "gbonus_icon"));

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
    const int ndmg = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int narmour = attr.val.as_int;

    for(int d = 0; d < DAMAGE_TYPE_MAX; d++) {
        for(int a = 0; a < ARMOUR_TYPE_MAX; a++) {
            s_dmg_mult[d][a] = 1.0f;
        }
    }

    for(int d = 0; d < ndmg; d++) {
        for(int a = 0; a < narmour; a++) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_FLOAT);
            if(d < DAMAGE_TYPE_MAX && a < ARMOUR_TYPE_MAX) {
                s_dmg_mult[d][a] = attr.val.as_float;
            }
        }
    }

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
        cs->invulnerable = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        cs->sticky = attr.val.as_bool;

        if(cs->state == STATE_ATTACK_ANIM_PLAYING) {
            CHK_TRUE_RET(G_EntityExists(uid));
            attack_anim_pending_add(uid);
        }

        /* Re-establish the attack-notified invariant for an entity reloaded mid-attack
         * and re-emit the start so the client scripts resume the attack animation. The
         * event only reaches the G_RUNNING-masked handlers when running, but the flag
         * must track the state regardless so the pairing asserts hold. */
        if(cs->state == STATE_CAN_ATTACK || cs->state == STATE_ATTACK_ANIM_PLAYING) {
            combat_notify_attack_start(uid, cs);
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

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        cs->pd.flags = attr.val.as_int;

        CHK_TRUE_RET(Sprite_LoadDesc(&cs->pd.impact_sprite, stream));

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        cs->pd.impact_size = attr.val.as_vec2;

        CHK_TRUE_RET(Sprite_LoadDesc(&cs->pd.trail_sprite, stream));

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        cs->pd.trail_size = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        cs->pd.trail_freq = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        cs->fd.frame_offset = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        cs->fd.fire_mode = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        cs->fd.bone_name[0] = 0;
        if(strcmp(attr.val.as_string, "NULL") != 0) {
            pf_strlcpy(cs->fd.bone_name, attr.val.as_string, sizeof(cs->fd.bone_name));
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC3);
        cs->fd.offset = attr.val.as_vec3;
        Sched_TryYield();

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        cs->corpse_dir = NULL;
        if(strcmp(attr.val.as_string, "NULL") != 0) {
            const char *dirkey = si_intern(attr.val.as_string, &s_stringpool, s_stridx);
            if(dirkey) {
                cs->corpse_dir = dirkey;
            }
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        cs->corpse_pfobj = NULL;
        if(strcmp(attr.val.as_string, "NULL") != 0) {
            const char *objkey = si_intern(attr.val.as_string, &s_stringpool, s_stridx);
            if(objkey) {
                cs->corpse_pfobj = objkey;
            }
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC3);
        cs->corpse_scale = attr.val.as_vec3;
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

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_corpses = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_corpses; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t secs_left = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC3);
        vec3_t corpse_pos = attr.val.as_vec3;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_QUAT);
        quat_t corpse_rot = attr.val.as_quat;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC3);
        vec3_t corpse_scale = attr.val.as_vec3;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        const char *dirkey = si_intern(attr.val.as_string, &s_stringpool, s_stridx);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        const char *objkey = si_intern(attr.val.as_string, &s_stringpool, s_stridx);

        if(dirkey && objkey) {
            add_corpse(dirkey, objkey, secs_left, corpse_pos, corpse_scale, corpse_rot);
        }
        Sched_TryYield();
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_mods = attr.val.as_int;

    vec_mod_t mods_clear;
    kh_foreach_value(s_mods, mods_clear, {
        vec_mod_destroy(&mods_clear);
    });
    kh_clear(modlist, s_mods);

    for(int i = 0; i < num_mods; i++) {

        struct combat_mod mod = {0};

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        mod.uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        mod.secs_left = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        mod.kind = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        mod.amount = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        mod.percent = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        pf_strlcpy(mod.tag, attr.val.as_string, sizeof(mod.tag));

        vec_mod_t *list = combat_mods_for(mod.uid, true);
        CHK_TRUE_RET(list);
        vec_mod_push(list, mod);
        Sched_TryYield();
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_gbonuses = attr.val.as_int;

    vec_gbonus_reset(&s_gbonuses);
    for(int i = 0; i < num_gbonuses; i++) {

        struct group_bonus_rec rec = {0};

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        rec.uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        rec.desc.kind = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        rec.desc.amount = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        rec.desc.percent = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        pf_strlcpy(rec.desc.tag, attr.val.as_string, sizeof(rec.desc.tag));

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        pf_strlcpy(rec.desc.icon, attr.val.as_string, sizeof(rec.desc.icon));

        vec_gbonus_push(&s_gbonuses, rec);
        Sched_TryYield();
    }

    /* The sums are derived, so they are rebuilt from the restored records
     * rather than saved alongside them.
     */
    uint32_t resum_uid;
    kh_foreach_key(s_mods, resum_uid, {
        combat_mods_resum(resum_uid);
    });

    return true;
}

