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

#ifndef MOVEMENT_H
#define MOVEMENT_H

#include "../../game/public/game.h"
#include "../../pf_math.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct map;
struct SDL_RWops;
struct nav_tick_sample;
struct kh_aabb_s;
struct kh_footprint_s;

/*###########################################################################*/
/* MOVEMENT GENERAL                                                          */
/*###########################################################################*/

enum movement_hz{
    MOVE_HZ_20,
    MOVE_HZ_10,
    MOVE_HZ_5,
    MOVE_HZ_1
};

/* The subset of the gamestate that is necessary 
 * to derive the new entity velocities and positions. 
 * We make a copy of this state so that movement 
 * computations can safely be done asynchronously,
 * or even be spread over multiple frames. 
 */
struct move_world{
    khash_t(id)           *flags;
    khash_t(pos)          *positions;
    bg_ent_t              *postree;
    khash_t(range)        *sel_radiuses;
    khash_t(id)           *faction_ids;
    /* Movement's own snapshot of the navigation state */
    struct map            *map;
    /* Additional state needed for nav_unit_query_ctx */
    struct kh_aabb_s      *aabbs;
    struct kh_footprint_s *footprints;
    void                  *transforms;
    bool                  fog_enabled;
    uint32_t              *fog_state;
    size_t                 fog_ntiles;
    struct kh_id_s        *dying_set;
    enum diplomacy_state (*diptable)[MAX_FACTIONS];
    uint16_t               player_controllable;
};

bool Move_Init(const struct map *map, const struct move_world *world);
void Move_Shutdown(void);
bool Move_HasWork(void);
/* Drop the tick in flight and take up the orders given; a new 'world' is
 * wanted when the last one was let go of by Move_TickPrepare.
 */
void Move_FlushWork(const struct move_world *world);

int  Move_HzCount(enum movement_hz hz);
/* The configured rate, which the next tick submitted runs at */
void Move_SetTickHz(enum movement_hz hz);
unsigned long Move_TicksDone(void);
void Move_SetUseGPU(bool use);

void Move_ClearPath_Init(const struct map *map);
void Move_ClearPath_Shutdown(void);

bool Move_SaveState(struct SDL_RWops *stream);
bool Move_LoadState(struct SDL_RWops *stream, const struct move_world *world);

/* Diagnostic: the movement tick counter of the [mv-trace] lines */
unsigned Move_TraceTick(void);
size_t Move_DebugDesc(uint32_t uid, char *out, size_t len);

/*###########################################################################*/
/* MOVEMENT TICK                                                             */
/*###########################################################################*/

/* Where a unit that moved is to be shown by the time of the next tick */
struct move_step{
    uint32_t uid;
    bool     has_pos;
    bool     has_rot;
    vec3_t   pos;
    quat_t   rot;
    /* Set by the game: the unit is still drawn short of the step */
    bool     owed;
};

struct move_facing{
    uint32_t uid;
    quat_t   rot;
};

/* A unit whose drawing starts afresh; with 'drop' what was left of its step
 * is not drawn.
 */
struct move_reset{
    uint32_t uid;
    bool     drop;
};

/* What a pass leaves for the game to change, good until the next pass */
struct move_output{
    struct move_step   *steps;
    size_t              nsteps;
    /* Units whose move ended */
    uint32_t           *stopped;
    size_t              nstopped;
    /* Held units standing, and the rotation Move_TickPivot turns each to */
    uint32_t           *held_still;
    size_t              nheld_still;
    struct move_facing *facings;
    size_t              nfacings;
    struct move_reset  *resets;
    size_t              nresets;
};

/* The passes of a tick on the main thread, in the order they are run. The
 * first three follow a finished tick; the last two start the next.
 */
bool Move_TickFinished(void);
/* Join the tick in flight, which reads the other subsystems' state */
void Move_FinishTick(void);
bool Move_TickConsume(void);
void Move_TickResume(void);
void Move_TickPivot(void);
void Move_TickCommands(void);
void Move_TickPrepare(void);
void Move_TickSubmit(const struct move_world *world, enum movement_hz hz);
void Move_Output(struct move_output *out);

/* How far the drawing of the steps got */
void Move_StepsBegun(const struct move_step *steps, size_t nsteps);
bool Move_StepDone(uint32_t uid);
bool Move_Tracks(uint32_t uid);

void Move_FrameStart(void);
struct nav_tick_sample *Move_TickStats(void);

/*###########################################################################*/
/* MOVEMENT UNITS                                                            */
/*###########################################################################*/

void Move_AddEntity(uint32_t uid, vec3_t pos, float sel_radius, int faction_id);
void Move_RemoveEntity(uint32_t uid);

void Move_UpdatePos(uint32_t uid, vec2_t pos);
void Move_UpdateFactionID(uint32_t uid, int oldfac, int newfac);
void Move_UpdateSelectionRadius(uint32_t uid, float sel_radius);

void Move_Unblock(uint32_t uid);
void Move_BlockAt(uint32_t uid, vec3_t pos);

bool Move_GetMaxSpeed(uint32_t uid, float *out);
bool Move_SetMaxSpeed(uint32_t uid, float speed);

/* The speed the entity actually moves at is the base speed plus the bonus the
 * combat modifiers hand down, floored at zero.
 */
bool Move_SetSpeedBonus(uint32_t uid, float bonus);
bool Move_GetEffectiveSpeed(uint32_t uid, float *out);

/*###########################################################################*/
/* MOVEMENT ORDERS                                                           */
/*###########################################################################*/

void Move_SetDest(uint32_t uid, vec2_t dest_xz, bool attack);
void Move_Stop(uint32_t uid);
/* Stop to fight: the order is paused, not finished (Move_ResumeOrder) */
void Move_StopForCombat(uint32_t uid);
/* Send the unit back to the order it kept while it fought; false when it
 * has no order left to go back to.
 */
bool Move_ResumeOrder(uint32_t uid);
void Move_SetSeekEnemies(uint32_t uid);
void Move_SetSeekPin(uint32_t uid, uint32_t target);
void Move_SetSurroundEntity(uint32_t uid, uint32_t target);
void Move_SetChangeDirection(uint32_t uid, quat_t target);
void Move_SetEnterRange(uint32_t uid, uint32_t target, float range);
void Move_SetFlee(uint32_t uid, vec2_t threat_xz);
void Move_StopFlee(uint32_t uid);
void Move_SetCombatHeld(uint32_t uid, bool held);
void Move_SetCombatFacing(uint32_t uid, quat_t dir);

void Move_ArrangeInFormation(vec_entity_t *ents, vec2_t target, 
                             vec2_t orientation, enum formation_type type);
void Move_AttackInFormation(vec_entity_t *ents, vec2_t target, 
                            vec2_t orientation, enum formation_type type);
/* The units of 'ents' standing on ground they can be ordered from */
void Move_FilterPathable(const vec_entity_t *ents, vec_entity_t *out);

/*###########################################################################*/
/* MOVEMENT QUERIES                                                          */
/*###########################################################################*/

bool Move_Still(uint32_t uid);
bool Move_GetDest(uint32_t uid, vec2_t *out_xz, bool *out_attack);
bool Move_GetSurrounding(uint32_t uid, uint32_t *out_uid);
bool Move_IsFleeing(uint32_t uid);

/*###########################################################################*/
/* MOVEMENT FORMATIONS                                                       */
/*###########################################################################*/

#define NULL_FID (~((uint32_t)0))
/* Formation orders for more units than this get truncated to the first
 * MAX_FORMATION_UNITS entities, with the excess deselected.
 */
#define MAX_FORMATION_UNITS (512)

typedef uint32_t formation_id_t;

bool           Move_Formation_Init(const struct map *map);
void           Move_Formation_Shutdown(void);

formation_id_t Move_Formation_GetForEnt(uint32_t uid);
void           Move_Formation_RemoveEntity(uint32_t uid);

void           Move_Formation_Arrange(enum formation_type type, vec_entity_t *ents);
void           Move_Formation_SetPreferred(uint32_t uid, enum formation_type type);
enum formation_type Move_Formation_GetPreferred(uint32_t uid);
enum formation_type Move_Formation_PreferredForSet(const vec_entity_t *ents);

vec2_t         Move_Formation_AutoOrientation(vec2_t target, const vec_entity_t *ents);
void           Move_Formation_RenderPlacement(const vec_entity_t *ents, vec2_t pos, vec2_t orientation);

bool           Move_Formation_SaveState(struct SDL_RWops *stream);
bool           Move_Formation_LoadState(struct SDL_RWops *stream);

#endif

