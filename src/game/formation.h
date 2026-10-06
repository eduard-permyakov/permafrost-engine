/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2023 Eduard Permyakov 
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

#ifndef FORMATION_H
#define FORMATION_H

#include "public/game.h"
#include "../pf_math.h"
#include "../navigation/public/nav.h"

#define NULL_FID (~((uint32_t)0))
/* Formation orders for more units than this get truncated to the first
 * MAX_FORMATION_UNITS entities, with the excess deselected.
 */
#define MAX_FORMATION_UNITS (512)

/* The layout of a box formation: a set of concentric square shells, one per
 * unit group, with the highest-priority group forming the outer wall and the
 * lowest-priority one the solid core.
 *
 * A shell is described by the side of its bounding square and the side of the
 * unused square at its centre, which is the bounding square of the next shell
 * inwards. Both share a parity, so shells are concentric with an integer inset
 * of (outer_side - side) / 2 cells.
 *
 * Cell grids are row-major and follow the formation convention that row
 * (side - 1) is the front row and row 0 the back row.
 */

/* The box is abandoned in favour of a rank layout past this side, bounding a
 * selection made up of pathologically many distinct unit types.
 */
#define MAX_BOX_SIDE (47)

struct box_shell{
    int side;
    int hole;
};

typedef uint32_t formation_id_t;

struct map;

bool           G_Formation_Init(const struct map *map);
void           G_Formation_Shutdown(void);

vec2_t         G_Formation_AutoOrientation(vec2_t target, const vec_entity_t *ents);
void           G_Formation_Create(vec2_t target, vec2_t orientation,
                                  const vec_entity_t *ents, enum formation_type type);
formation_id_t G_Formation_GetForEnt(uint32_t uid);
void           G_Formation_RemoveUnit(uint32_t uid);
void           G_Formation_RemoveEntity(uint32_t uid);

bool           G_Formation_InRangeOfCell(uint32_t uid);
bool           G_Formation_CanUseArrivalField(uint32_t uid);
vec2_t         G_Formation_DesiredArrivalVelocity(uint32_t uid);
vec2_t         G_Formation_ApproximateDesiredArrivalVelocity(uint32_t uid);
bool           G_Formation_AssignmentReady(uint32_t uid);
bool           G_Formation_ArrivedAtCell(uint32_t uid);
void           G_Formation_ConcedeCell(uint32_t uid);
bool           G_Formation_AssignedToCell(uint32_t uid);
vec2_t         G_Formation_CellPosition(uint32_t uid);
quat_t         G_Formation_TargetOrientation(uint32_t uid);
void           G_Formation_UpdateFieldIfNeeded(uint32_t uid);
float          G_Formation_Speed(uint32_t uid);
enum formation_type G_Formation_Type(formation_id_t fid);
void           G_Formation_RenderPlacement(const vec_entity_t *ents, vec2_t pos, vec2_t orientation);

vec2_t         G_Formation_CohesionForce(uint32_t uid);
vec2_t         G_Formation_AlignmentForce(uint32_t uid);
vec2_t         G_Formation_DragForce(uint32_t uid);

/* All the per-entity formation state read by the movement submit loop,
 * resolved with a single set of lookups. Returns false (and sets fid to
 * NULL_FID) when the entity is not part of a formation.
 */
struct formation_submit_state{
    formation_id_t fid;
    bool           assignment_ready;
    bool           assigned_to_cell;
    bool           in_range_of_cell;
    bool           arrived_at_cell;
    /* Whether the cells in front of this unit's are taken, so it may take its
     * own, and whether its own side has walled it out of its cell.
     */
    bool           may_park;
    bool           straggler;
    vec2_t         cohesion_force;
    vec2_t         alignment_force;
    vec2_t         drag_force;
    quat_t         target_orientation;
    float          speed;
};

bool           G_Formation_SubmitState(uint32_t uid, struct formation_submit_state *out);
bool           G_Formation_SubmitStateGather(uint32_t uid, struct formation_submit_state *out);
void           G_Formation_SetGatherActive(bool active);

/* Size one shell per group, from the innermost group outwards. 'counts' is
 * ordered from the outermost group to the innermost and must hold no zeroes.
 * Returns false if the box would grow past MAX_BOX_SIDE.
 */
bool           G_FormationBox_Shells(const size_t *counts, size_t ngroups, struct box_shell *out);

/* Mark the 'nunits' cells of 'shell' which the group occupies in the
 * (side * side) row-major grid 'out'. Cells are taken ring by ring from the
 * outside in, and front-first within a ring, so a group too small to fill its
 * shell screens the front and leaves its gap at the back centre.
 */
void           G_FormationBox_Mask(struct box_shell shell, size_t nunits, uint8_t *out);

bool           G_Formation_SaveState(struct SDL_RWops *stream);
bool           G_Formation_LoadState(struct SDL_RWops *stream);

#endif

