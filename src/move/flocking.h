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

#ifndef FLOCKING_H
#define FLOCKING_H

#include "../pf_math.h"
#include "../nav/public/nav.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct map;

#define EPSILON               (1.0f/1024)
#define MAX_FORCE             (0.75f)
/* The most a behaviour may ask of a unit in a tick at the rate '_hz' */
#define MOVE_FLOCK_MAX_FORCE(_hz)       (MAX_FORCE / (_hz) * 20.0)

#define COHESION_NEIGHBOUR_RADIUS       (50.0f)
#define ARRIVE_SLOWING_RADIUS           (10.0f)

/* A gap to a still crowd within this fraction of the unit's radius is contact */
#define SEPARATION_NEIGHB_RADIUS        (30.0f)
/* Beyond this multiple of the radius sum the separation term is e^-10 of its
 * contact value; the floor keeps the soldier query as it was. */
#define SEPARATION_NEIGHB_SCALE         (1.3f)
/* Deeper interpenetration than this keeps the pure radial push-out */
#define SEP_REDIRECT_MIN_GAP            (-2.0f)
/* A unit stepping aside out of contact moves at this share of its pace */
#define MOVE_ESCAPE_FLOOR               (0.25f)

#define MOVE_STEP_BAND                  (30.0f) /* degrees; a shove keeps this close to the facing */
#define MOVE_WALK_BAND                  (45.0f) /* degrees; a walking step keeps this close to the facing */
/* The facing follows the travel direction once the unit moves at this
 * fraction of its commanded speed, and stops following below the lower one,
 * so a unit crawling in a crowd holds its heading instead of sweeping after
 * every displacement.
 */
#define FACING_FOLLOW_ENGAGE            (0.35f)
#define FACING_FOLLOW_RELEASE           (0.2f)

/* Dense per-flock snapshot of member uids and positions, in flock iteration
 * order, so the O(flock) cohesion scan reads packed arrays instead of probing
 * two hash tables per member. */
struct flock_snap{
    const uint32_t *uids;
    const float    *pos_x;
    const float    *pos_z;
    size_t          nents;
};

void   Move_Flock_Init(void);

void   Move_Flock_Truncate(vec2_t *inout, float max_len);
quat_t Move_Flock_DirQuat(vec2_t velocity);
vec2_t Move_Flock_FacingDir(quat_t rot);
quat_t Move_Flock_TurnToward(quat_t cur, quat_t target, float max_deg);
vec2_t Move_Flock_StepWithinHeading(vec2_t step, vec2_t facing, float max_step);
vec2_t Move_Flock_DressStep(vec2_t step, vec2_t facing, float max_step);
float  Move_Flock_CellBearing(vec2_t to_cell, quat_t rank_orientation);

vec2_t Move_Flock_Seek(vec2_t pos_xz, vec2_t velocity, float max_speed, int hz, vec2_t target_xz);
vec2_t Move_Flock_ArrivePoint(vec2_t pos_xz, vec2_t velocity, int hz, vec2_t target_xz,
                              vec2_t vdes, bool has_dest_los, float speed);
vec2_t Move_Flock_ArriveCell(vec2_t pos_xz, float max_speed, int hz, vec2_t cell_xz, vec2_t vdes);
vec2_t Move_Flock_ArriveEnemies(vec2_t velocity, float max_speed, int hz, vec2_t vdes);
vec2_t Move_Flock_Cohesion(uint32_t uid, vec2_t pos_xz, int hz, const struct flock_snap *snap);

float  Move_Flock_SeparationReach(float radius_sum);
void   Move_Flock_Separate(vec2_t diff, float radius_sum, float buffer_dist,
                           vec2_t *inout_sum, float *inout_min_gap);
vec2_t Move_Flock_RedirectBackward(vec2_t force, vec2_t vdes, int side, float min_gap);

bool   Move_Flock_LandingOk(const struct map *map, enum nav_layer layer, bool on_blocked, vec2_t pos);
vec2_t Move_Flock_Slide(const struct map *map, vec2_t pos, enum nav_layer layer, bool on_blocked,
                        vec2_t vel);
void   Move_Flock_NullifyImpass(const struct map *map, enum nav_layer layer, vec2_t pos,
                                bool on_blocked, vec2_t *inout_force);
vec2_t Move_Flock_EscapeStep(vec2_t vdes, int side, float max_step);

float  Move_Flock_WallDistance(const vec2_t *tiles, size_t ntiles, vec2_t pos);
vec2_t Move_Flock_HugWall(vec2_t vpref, vec2_t vdes, int side);

#endif
