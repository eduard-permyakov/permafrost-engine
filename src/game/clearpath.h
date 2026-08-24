/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2026 Eduard Permyakov
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

#ifndef CLEARPATH_H
#define CLEARPATH_H

#include "../pf_math.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/* A neighbour is considered once the centre distance is within this
 * multiple of the radius sum (at least the floor), so every pair meets the
 * same cone geometry at first sight as a soldier pair does at 10.
 */
#define CLEARPATH_NEIGHBOUR_RADIUS (10.0f)
#define CLEARPATH_NEIGHBOUR_SCALE  (10.0f / 6.5f)
/* Blocked tiles constrain the unit's CENTRE (the layer's dilation already
 * accounts for the body): the disc covers the 4x4 tile alone.
 */
#define CLEARPATH_TILE_RADIUS      (2.8284271f)
#define CLEARPATH_MAX_TILE_OBS     (12)
/* This is added to the entity's radius so that it will take wider turns
 * and leave this as a buffer between it and the obstacle.
 */
#define CLEARPATH_BUFFER_RADIUS    (0.0f)

struct map;

struct cp_ent{
    vec2_t xz_pos;
    vec2_t xz_vel; /* specified per pathfinding tick */
    float  radius;
};

/* The ground a candidate velocity must land on: the unit's nav layer, and
 * whether it already stands on a blocked tile (then it may step onto one).
 * The landing point is the candidate clamped to the unit's per-tick step,
 * matching the truncation the movement code applies to the solver's result.
 * A NULL map disables the test.
 */
struct cp_terrain{
    const struct map *map;
    int               layer;
    bool              on_blocked;
    float             max_step;
};

/* How a solve was resolved, for the per-tick mechanism counters. */
struct cp_solve_diag{
    uint8_t retries;
    bool    gave_up;
    bool    fallback;
    /* Sign of the deflection from the preferred velocity, 0 when none */
    int8_t  side;
};

void G_ClearPath_Init(const struct map *map);
void G_ClearPath_Shutdown(void);

/* The single entity whose ClearPath state should be saved for the debug
 * overlay this tick, or NULL_UID. */
uint32_t G_ClearPath_DebugUid(void);

/* The neighbour arrays are scratch: the retry loop compacts them in place.
 * 'out_diag' may be NULL. */
vec2_t G_ClearPath_NewVelocity(struct cp_ent ent,
                               uint32_t ent_uid,
                               vec2_t ent_des_v,
                               struct cp_ent *dyn_neighbs,
                               size_t ndyn,
                               struct cp_ent *stat_neighbs,
                               size_t nstat,
                               const vec2_t *tile_obs,
                               size_t ntiles,
                               struct cp_terrain terrain,
                               int side,
                               bool save_debug,
                               struct cp_solve_diag *out_diag);

#endif

