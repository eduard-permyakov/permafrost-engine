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

#ifndef ARRIVAL_H
#define ARRIVAL_H

#include "../pf_math.h"
#include "../navigation/public/nav.h"
#include <stdbool.h>
#include <stdint.h>

struct map;
struct camera;

#define ARRIVAL_MAX_SLOTS (4096)   /* Cap on the reachable open-slot set per flock */

enum arrival_phase{
    ARRIVAL_PHASE_INACTIVE,   /* no region built; not arriving */
    ARRIVAL_PHASE_FILLING,    /* region built; guiding and settling members */
    ARRIVAL_PHASE_DONE,       /* region built; not filling (all settled, or deactivated) */
};

enum arrival_substate{
    ARRIVAL_SUBSTATE_APPROACH,
    ARRIVAL_SUBSTATE_APPROACH_ARMED,
    ARRIVAL_SUBSTATE_SEEK,
    ARRIVAL_SUBSTATE_SEEK_ARMED,
};

/* Per-flock arrival state: a fixed-area, single-connected footprint of 
 * reachable tiles about 'centre' (a blocker reshapes it, never shrinks it), 
 * thinned to unit-spaced 'slots'.
 */
struct arrival_state{
    enum arrival_phase phase;
    enum nav_layer   layer;
    vec2_t           centre;
    vec2_t           axis;
    /* COM of the slots + a dest field toward it: the fallback seek target. 
     */
    vec2_t           com;
    dest_id_t        com_dest_id;
    uint16_t         radius;
    float            unit_radius;
    int              num_slots;
    int              active_row;
    int              realloc_counter;
    int              stall_counter;
    int              prev_occ;
    float            fill_frac;
    int              num_rows;
    float            row_height;
    vec2_t           slots[ARRIVAL_MAX_SLOTS];
    /* Per-slot fill rank: ring 0 = farthest (geodesic) from where the flock enters the
     * footprint, filled first. Recomputed from the live entry until the fill commits. 
     */
    int              slot_ring[ARRIVAL_MAX_SLOTS];
    /* Sorted tile keys of the static-open footprint (snapshot at build). Lets the fine tier
     * tell a wall (cross it -> route around) from a settled friendly unit (sits on it). 
     */
    int              num_region;
    uint64_t         region_keys[ARRIVAL_MAX_SLOTS];
};

/* A flock's arrival overlay is computed per nav layer (radius bucket), mirroring the
 * per-layer movement fields: each present layer gets its own footprint, slots, and
 * obstacle pads. Entries are allocated lazily for the layers actually present.
 */
struct arrival_group{
    struct arrival_state *layers[NAV_LAYER_MAX];
};

struct arrival_unit_state{
    enum arrival_substate substate;
    /* The assigned slot */ 
    bool   sink_valid;
    vec2_t sink;
    /* Wedge anchor: 'stuck' counts ticks spent within a small range 
     * of it, so a unit oscillating in place accrues the wedge while 
     * one making headway re-seeds. 
     */
    int    stuck;
    vec2_t progress_anchor;
    bool   progress_anchored;
    /* Debounced bee-line vs around-the-wall stage: the raw LOS test 
     * flickers on a wall seam, so the latch flips only after it 
     * disagrees for a number of ticks. 
     */
    bool   los_latched;
    int    los_hyst;
    /* Start grace: a unit must travel some distance from its' 
     * initial position before proximity-settling can fire, so 
     * one ordered amongst arrived units heads for its goal first. 
     */
    vec2_t order_pos;
};

struct arrival_member{
    vec2_t                     pos;
    bool                       settled;
    enum nav_layer             layer;
    float                      radius;
    struct arrival_unit_state *us;
};

void G_Arrival_InitFlock(struct arrival_state *as);
void G_Arrival_Deactivate(struct arrival_state *as);
void G_Arrival_InitUnit(struct arrival_unit_state *us, vec2_t order_pos);
bool G_Arrival_IsActive(const struct arrival_state *as);

void G_Arrival_UpdateFlock(struct arrival_state *as, const struct map *map, vec2_t target_xz,
                           enum nav_layer layer, float unit_radius, int total_members,
                           const struct arrival_member *members, int nmembers);
void G_Arrival_RequestField(const struct arrival_state *as, const struct map *map);

bool G_Arrival_DesiredVelocity(const struct arrival_state *as, struct arrival_unit_state *us,
                               const struct map *region_map, const struct map *nav_map,
                               vec2_t pos, vec2_t velocity, bool has_dest_los, vec2_t *out_vel);
bool G_Arrival_ShouldSettle(const struct arrival_state *as, struct arrival_unit_state *us,
                            const struct map *region_map, const struct map *nav_map,
                            vec2_t new_pos, vec2_t velocity, float radius, int nsettled);
vec2_t G_Arrival_SeekTarget(const struct arrival_state *as, const struct arrival_unit_state *us,
                            vec2_t target_xz);
bool G_Arrival_NeighbourSettling(const struct arrival_unit_state *us, vec2_t neighb_pos,
                                 float radius);

void G_Arrival_RenderDebug(const struct arrival_state *as, const struct camera *cam,
                           const struct map *map, dest_id_t dest_id, vec3_t color,
                           const struct arrival_member *members, int nmembers);

void G_ArrivalGroup_Init(struct arrival_group *grp);
void G_ArrivalGroup_Destroy(struct arrival_group *grp);
void G_ArrivalGroup_Reset(struct arrival_group *grp);
void G_ArrivalGroup_Deactivate(struct arrival_group *grp);
void G_ArrivalGroup_Update(struct arrival_group *grp, const struct map *map, vec2_t target_xz,
                           const struct arrival_member *members, int nmembers);
void G_ArrivalGroup_RequestFields(const struct arrival_group *grp, const struct map *map);
bool G_ArrivalGroup_IsActive(const struct arrival_group *grp);
struct arrival_state *G_ArrivalGroup_ForLayer(const struct arrival_group *grp, enum nav_layer layer);
void G_ArrivalGroup_RenderDebug(const struct arrival_group *grp, const struct camera *cam,
                                const struct map *map, dest_id_t dest_id, vec3_t color,
                                const struct arrival_member *members, int nmembers);

struct SDL_RWops;
bool G_Arrival_SaveState(struct SDL_RWops *stream, const struct arrival_group *grp);
bool G_Arrival_LoadState(struct SDL_RWops *stream, struct arrival_group *grp);
bool G_Arrival_SaveUnitState(struct SDL_RWops *stream, const struct arrival_unit_state *us);
bool G_Arrival_LoadUnitState(struct SDL_RWops *stream, struct arrival_unit_state *us);

#endif

