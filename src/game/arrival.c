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

#include "arrival.h"
#include "public/game.h"
#include "../camera.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../lib/public/mem.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#define EPSILON                   (1.0f/1024)

#define ARRIVAL_FIELD_PLAN_RADIUS (150.0f) /* Plan the zone once a member is this near the goal */
#define ARRIVAL_MIN_UNITS         (4)      /* Below this, just seek the goal; no ball fill */
#define ARRIVAL_STUCK_SPEED       (0.1f)   /* Per-tick speed below which a unit counts as wedged */
#define ARRIVAL_STUCK_LIMIT       (12)     /* Wedged this many ticks -> settle in place */
#define ARRIVAL_STUCK_DISP        (1.5f)   /* Travel under this over the window = no progress */
#define ARRIVAL_LOS_HYST          (12)     /* Opposite-LOS ticks before the approach stage flips */
#define ARRIVAL_ENGAGE_DIST       (4.0f)   /* Travel this far from the order point before settling */
#define ARRIVAL_SETTLE_RANGE      (1.5f)   /* Give up only within this x the region radius of the centre */
#define ARRIVAL_REALLOC_PERIOD    (4)      /* Re-balance + advance the frontier every N ticks */
#define ARRIVAL_ROW_FILL_THRESH   (1.0f)   /* Row this full -> advance the frontier */
#define ARRIVAL_ROW_STALL_TICKS   (80)     /* Frontier flat this long -> force past the row */
#define ARRIVAL_REORIENT_FREEZE   (0.25f)  /* Re-orient the fill until the ball is this full */
#define ARRIVAL_COMPACT_FACTOR    (1.5f)   /* Footprint cap as a multiple of disc radius */
#define ARRIVAL_SETTLE_CONTACTS   (3)      /* Settled neighbours needed to settle */
#define ARRIVAL_FILL_RELAX_2      (0.75f)  /* Ball this full -> settle on 2 neighbours */
#define ARRIVAL_FILL_RELAX_1      (0.90f)  /* Ball this full -> settle on 1 neighbour */
#define ARRIVAL_SINK_TOLERANCE    (1.5f)   /* Settle within this x radius of the slot */
#define ARRIVAL_SLOT_SPACING      (1.85f)  /* Inter-slot spacing, x unit radius */
#define ARRIVAL_ZONE_PAD          (3)      /* Tiles of open border around the packed ball */

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static vec2_t s_slot_sort_ref;
static vec2_t s_slot_sort_axis;
static vec2_t s_slot_sort_perp;
static float  s_slot_sort_band;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool unit_committed(const struct arrival_unit_state *us)
{
    return us->substate == ARRIVAL_SUBSTATE_SEEK
        || us->substate == ARRIVAL_SUBSTATE_SEEK_ARMED;
}

static bool unit_armed(const struct arrival_unit_state *us)
{
    return us->substate == ARRIVAL_SUBSTATE_APPROACH_ARMED
        || us->substate == ARRIVAL_SUBSTATE_SEEK_ARMED;
}

static void unit_commit(struct arrival_unit_state *us)
{
    if(us->substate == ARRIVAL_SUBSTATE_APPROACH)
        us->substate = ARRIVAL_SUBSTATE_SEEK;
    else if(us->substate == ARRIVAL_SUBSTATE_APPROACH_ARMED)
        us->substate = ARRIVAL_SUBSTATE_SEEK_ARMED;
}

static void unit_arm(struct arrival_unit_state *us)
{
    if(us->substate == ARRIVAL_SUBSTATE_APPROACH)
        us->substate = ARRIVAL_SUBSTATE_APPROACH_ARMED;
    else if(us->substate == ARRIVAL_SUBSTATE_SEEK)
        us->substate = ARRIVAL_SUBSTATE_SEEK_ARMED;
}

static int slot_cmp_near_ref(const void *a, const void *b)
{
    vec2_t da, db;
    PFM_Vec2_Sub((vec2_t*)a, &s_slot_sort_ref, &da);
    PFM_Vec2_Sub((vec2_t*)b, &s_slot_sort_ref, &db);
    float la = PFM_Vec2_Dot(&da, &da), lb = PFM_Vec2_Dot(&db, &db);
    return (la > lb) - (la < lb);
}

/* Prioritize further bands over closer ones. This makes units tend to 
 * the opposite end of the goal region and "filling up the container".
 */
static int slot_cmp_far_banded(const void *a, const void *b)
{
    vec2_t ra, rb;
    PFM_Vec2_Sub((vec2_t*)a, &s_slot_sort_ref, &ra);
    PFM_Vec2_Sub((vec2_t*)b, &s_slot_sort_ref, &rb);
    int ba = (int)floorf(PFM_Vec2_Dot(&ra, &s_slot_sort_axis) / s_slot_sort_band);
    int bb = (int)floorf(PFM_Vec2_Dot(&rb, &s_slot_sort_axis) / s_slot_sort_band);
    if(ba != bb)
        return (ba < bb) - (ba > bb);   /* deeper band first */
    float la = PFM_Vec2_Dot(&ra, &s_slot_sort_perp);
    float lb = PFM_Vec2_Dot(&rb, &s_slot_sort_perp);
    return (la > lb) - (la < lb);        /* lateral within the band */
}

/* The tile budget (area) to hold the entire flock. If blockers take up some
 * of the area, it is pushed outwards but the size stays constant.
 */
static int arrival_region_area(const struct arrival_state *as)
{
    int area = (int)(M_PI * as->radius * as->radius + 0.5f);
    return (area < ARRIVAL_MAX_SLOTS) ? area : ARRIVAL_MAX_SLOTS;
}

static bool arrival_in_region(const struct arrival_state *as, const struct map *map, vec2_t pos)
{
    return M_NavSegmentWithinRegion(map, pos, pos, as->region_keys, as->num_region);
}

static bool arrival_near_region(const struct arrival_state *as, const struct map *map, vec2_t pos)
{
    if(arrival_in_region(as, map, pos))
        return true;
    struct map_resolution res;
    M_NavGetResolution(map, &res);
    float td = (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE) / (float)res.tile_w;
    /* Add a 1-tile pad around the goal */
    for(int dz = -1; dz <= 1; dz++) {
    for(int dx = -1; dx <= 1; dx++) {
        if(dx == 0 && dz == 0)
            continue;
        if(arrival_in_region(as, map, (vec2_t){pos.x + dx * td, pos.z + dz * td}))
            return true;
    }}
    return false;
}

static void arrival_build_slots(struct arrival_state *as, const struct map *map)
{
    int n = M_NavClosestConnectedPathableTiles(map, as->layer, as->centre, as->slots,
        arrival_region_area(as), 0.0f);

    /* Snapshot the footprint while still empty of our units, so the fine tier tells a wall
     * from a friendly unit. */
    as->num_region = M_NavTileKeysForPositions(map, as->slots, n, as->region_keys);

    as->num_slots = n;
}

/* Reduce the dense tile flood into the final set of settle spots, roughly one
 * per unit with no overlap.
 */
static void arrival_thin_centered_slots(struct arrival_state *as, const struct map *map, int target,
                                        float spacing)
{
    if(as->num_slots == 0)
        return;
    /* Bias the anchor toward the far edge: leaders reach the rim, pad sits behind. */
    struct map_resolution res;
    M_NavGetResolution(map, &res);
    float tile_dim = (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE) / (float)res.tile_w;
    float fwd = (ARRIVAL_ZONE_PAD - 1) * tile_dim;
    s_slot_sort_ref = (vec2_t){
        as->centre.x + as->axis.x * fwd,
        as->centre.z + as->axis.z * fwd
    };
    qsort(as->slots, as->num_slots, sizeof(vec2_t), slot_cmp_near_ref);

    float sp2 = spacing * spacing;
    int kept = 0;
    for(int i = 0; i < as->num_slots && kept < target; i++) {
        vec2_t cand = as->slots[i];
        bool ok = true;
        for(int j = 0; j < kept; j++) {
            vec2_t d;
            PFM_Vec2_Sub(&cand, &as->slots[j], &d);
            if(PFM_Vec2_Dot(&d, &d) < sp2) {
                ok = false;
                break;
            }
        }
        if(ok)
            as->slots[kept++] = cand;
    }
    as->num_slots = kept;
}

static void arrival_order_slots_far_near(struct arrival_state *as)
{
    qsort(as->slots, as->num_slots, sizeof(vec2_t), slot_cmp_far_banded);
}

static vec2_t arrival_approach_axis(const struct arrival_member *members, int n, vec2_t target_xz)
{
    vec2_t centroid = (vec2_t){0.0f, 0.0f};
    for(int i = 0; i < n; i++)
        PFM_Vec2_Add(&centroid, (vec2_t*)&members[i].pos, &centroid);
    if(n > 0)
        PFM_Vec2_Scale(&centroid, 1.0f / n, &centroid);

    vec2_t axis;
    PFM_Vec2_Sub(&target_xz, &centroid, &axis);
    if(PFM_Vec2_Len(&axis) > EPSILON)
        PFM_Vec2_Normal(&axis, &axis);
    else
        axis = (vec2_t){0.0f, 1.0f};
    return axis;
}

/* Rank slots into fill rings by geodesic distance from the flock's entry, 
 * (ring 0 = deepest), so the entry side fills last and never seals off the 
 * slots behind it.
 */
static void arrival_compute_geodesic_rings(struct arrival_state *as, const struct map *map,
                                           const struct arrival_member *members, int n)
{
    int M = as->num_slots;
    as->num_rows = 0;
    if(M == 0)
        return;

    vec2_t entry = (vec2_t){0.0f, 0.0f};
    int cnt = 0;
    for(int i = 0; i < n; i++) {
        if(members[i].settled)
            continue;
        PFM_Vec2_Add(&entry, (vec2_t*)&members[i].pos, &entry);
        cnt++;
    }
    if(cnt > 0)
        PFM_Vec2_Scale(&entry, 1.0f / cnt, &entry);
    else
        entry = as->centre;

    STALLOC(int, gdist, M);
    int maxd = M_NavRegionGeodesicDist(map, as->region_keys, as->num_region, entry,
        as->slots, M, gdist);
    if(maxd < 0)
        maxd = 0;

    struct map_resolution res;
    M_NavGetResolution(map, &res);
    float tile_dim = (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE) / (float)res.tile_w;
    int ring_w = (int)(as->row_height / tile_dim + 0.5f);
    if(ring_w < 1)
        ring_w = 1;

    as->num_rows = maxd / ring_w + 1;
    for(int i = 0; i < M; i++) {
        int g = (gdist[i] >= 0) ? gdist[i] : 0;   /* unreachable -> deepest ring */
        int r = (maxd - g) / ring_w;
        if(r < 0)
            r = 0;
        if(r >= as->num_rows)
            r = as->num_rows - 1;
        as->slot_ring[i] = r;
    }
    STFREE(gdist);
}

static bool arrival_nearest_open_slot(const struct arrival_state *as, const struct map *map,
                                      vec2_t from, vec2_t *out)
{
    float best = INFINITY;
    bool found = false;
    for(int i = 0; i < as->num_slots; i++) {
        if(as->slot_ring[i] > as->active_row)
            continue;
        if(M_NavPositionBlocked(map, as->layer, as->slots[i]))
            continue;
        vec2_t d;
        PFM_Vec2_Sub((vec2_t*)&as->slots[i], &from, &d);
        float dd = PFM_Vec2_Dot(&d, &d);
        if(dd < best) {
            best = dd;
            *out = as->slots[i];
            found = true;
        }
    }
    return found;
}

static bool arrival_near_open_slot(const struct arrival_state *as, const struct map *map, vec2_t pos,
                                   float tol)
{
    float tol2 = tol * tol;
    for(int i = 0; i < as->num_slots; i++) {
        if(as->slot_ring[i] > as->active_row)
            continue;
        vec2_t d;
        PFM_Vec2_Sub((vec2_t*)&as->slots[i], &pos, &d);
        if(PFM_Vec2_Dot(&d, &d) > tol2)
            continue;
        if(M_NavPositionBlocked(map, as->layer, as->slots[i]))
            continue;
        return true;
    }
    return false;
}

static bool arrival_nearest_slot(const struct arrival_state *as, vec2_t from, vec2_t *out)
{
    float best = INFINITY;
    bool found = false;
    for(int i = 0; i < as->num_slots; i++) {
        if(as->slot_ring[i] > as->active_row)
            continue;
        vec2_t d;
        PFM_Vec2_Sub((vec2_t*)&as->slots[i], &from, &d);
        float dd = PFM_Vec2_Dot(&d, &d);
        if(dd < best) {
            best = dd;
            *out = as->slots[i];
            found = true;
        }
    }
    return found;
}

static uint16_t arrival_zone_radius(const struct map *map, int nunits, float unit_radius)
{
    /* Disc sized to the slot packing plus ARRIVAL_ZONE_PAD tiles of open border. */
    struct map_resolution res;
    M_NavGetResolution(map, &res);
    float tile_dim = (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE) / (float)res.tile_w;
    float per_unit = (ARRIVAL_SLOT_SPACING * unit_radius) / tile_dim;
    per_unit *= per_unit;
    if(per_unit < 1.0f)
        per_unit = 1.0f;
    float ntiles = nunits * per_unit;
    return (uint16_t)(ceilf(sqrtf(ntiles / M_PI)) + ARRIVAL_ZONE_PAD);
}

static bool arrival_near_goal(const struct arrival_member *members, int n, vec2_t target_xz)
{
    for(int i = 0; i < n; i++) {
        vec2_t delta;
        PFM_Vec2_Sub(&target_xz, (vec2_t*)&members[i].pos, &delta);
        if(PFM_Vec2_Len(&delta) < ARRIVAL_FIELD_PLAN_RADIUS)
            return true;
    }
    return false;
}

/* Periodic many-to-one fill: each in-region unsettled unit is aimed at its nearest open slot
 * at/behind the frontier, which advances as rows fill.
 */
static void arrival_realloc_slots(struct arrival_state *as, const struct map *map,
                                  const struct arrival_member *members, int n)
{
    int M = as->num_slots;
    if(M == 0 || as->num_rows == 0)
        return;

    /* Re-orient toward where units actually enter until the fill commits: a path wrapping a wall
     * makes them arrive from an unanticipated side, and a stale order seals the rest off.
     */
    if(as->fill_frac < ARRIVAL_REORIENT_FREEZE) {
        arrival_compute_geodesic_rings(as, map, members, n);
        as->active_row = 0;
    }

    int R = as->num_rows;
    STALLOC(bool, occupied, M);
    STALLOC(int, occ, R);
    STALLOC(int, cap, R);
    memset(occupied, 0, sizeof(bool) * M);
    memset(occ, 0, sizeof(int) * R);
    memset(cap, 0, sizeof(int) * R);
    for(int i = 0; i < M; i++)
        cap[as->slot_ring[i]]++;

    /* A slot a blocker sits on can't be filled: mark it allocated so it's never handed out and
     * still counts toward its row, else the frontier stalls on the blocked row.
     */
    for(int i = 0; i < M; i++) {
        if(M_NavPositionBlocked(map, as->layer, as->slots[i]))
            occupied[i] = true;
    }

    for(int m = 0; m < n; m++) {
        if(!members[m].settled)
            continue;
        vec2_t pos = members[m].pos;
        /* Attribute a settled unit to the slot it sits on, but only if that slot is open: else a
         * packed-in unit claims a far empty slot, inflating the fill and hiding open slots.
         */
        int best = -1;
        float bestd = INFINITY;
        for(int i = 0; i < M; i++) {
            vec2_t d;
            PFM_Vec2_Sub((vec2_t*)&as->slots[i], &pos, &d);
            float dd = PFM_Vec2_Dot(&d, &d);
            if(dd < bestd) {
                bestd = dd;
                best = i;
            }
        }
        if(best >= 0 && !occupied[best])
            occupied[best] = true;
    }
    for(int i = 0; i < M; i++)
        if(occupied[i])
            occ[as->slot_ring[i]]++;

    int total_occ = 0;
    for(int r = 0; r < R; r++)
        total_occ += occ[r];
    as->fill_frac = (M > 0) ? (float)total_occ / M : 0.0f;

    int crowd = 0;
    for(int m = 0; m < n; m++) {
        if(members[m].settled)
            continue;
        if(arrival_in_region(as, map, members[m].pos))
            crowd++;
    }

    bool rising = occ[as->active_row] > as->prev_occ;
    bool advanced = false;
    while(as->active_row < R - 1
       && occ[as->active_row] >= ARRIVAL_ROW_FILL_THRESH * cap[as->active_row]) {
        as->active_row++;
        advanced = true;
    }
    if(advanced || rising || crowd == 0)
        as->stall_counter = 0;
    else if(++as->stall_counter >= ARRIVAL_ROW_STALL_TICKS / ARRIVAL_REALLOC_PERIOD
         && as->active_row < R - 1) {
        as->active_row++;
        as->stall_counter = 0;
    }
    as->prev_occ = occ[as->active_row];

    for(int m = 0; m < n; m++) {
        if(members[m].settled)
            continue;
        vec2_t pos = members[m].pos;
        if(!arrival_in_region(as, map, pos)) {
            members[m].us->sink_valid = false;
            continue;
        }
        /* Latch in-region behaviour even with no slot to claim now, so a unit jostled off 
         * the goal region heads back to the cluster instead of routing out around a wall.
         */
        unit_commit(members[m].us);
        int best = -1;
        float bestd = INFINITY;
        for(int i = 0; i < M; i++) {
            if(occupied[i])
                continue;
            if(as->slot_ring[i] > as->active_row)
                continue;
            vec2_t d;
            PFM_Vec2_Sub((vec2_t*)&as->slots[i], &pos, &d);
            float dd = PFM_Vec2_Dot(&d, &d);
            if(dd < bestd) {
                bestd = dd;
                best = i;
            }
        }
        if(best >= 0) {
            members[m].us->sink = as->slots[best];
            members[m].us->sink_valid = true;
        }else{
            members[m].us->sink_valid = false;
        }
    }

    STFREE(cap);
    STFREE(occ);
    STFREE(occupied);
}

static void arrival_render_field(const struct arrival_state *as, const struct map *map, vec3_t color)
{
    if(as->phase != ARRIVAL_PHASE_FILLING || as->num_slots == 0)
        return;

    struct map_resolution nav_res;
    M_NavGetResolution(map, &nav_res);
    const float hx = ((TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE) / (float)nav_res.tile_w) / 2.0f;
    const float hz = ((TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE) / (float)nav_res.tile_h) / 2.0f;

    static vec2_t corners[ARRIVAL_MAX_SLOTS * 4];
    static vec3_t colors[ARRIVAL_MAX_SLOTS];

    int n = as->num_slots;
    for(int i = 0; i < n; i++) {
        vec2_t s = as->slots[i];
        corners[i * 4 + 0] = (vec2_t){s.x + hx, s.z - hz};
        corners[i * 4 + 1] = (vec2_t){s.x + hx, s.z + hz};
        corners[i * 4 + 2] = (vec2_t){s.x - hx, s.z + hz};
        corners[i * 4 + 3] = (vec2_t){s.x - hx, s.z - hz};
        colors[i] = color;
    }

    mat4x4_t model;
    PFM_Mat4x4_MakeTrans(0.0f, 0.0f, 0.0f, &model);
    size_t count = n;
    bool on_water = false;
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawMapOverlayQuads,
        .nargs = 6,
        .args = {
            R_PushArg(corners, sizeof(vec2_t) * n * 4),
            R_PushArg(colors, sizeof(vec3_t) * n),
            R_PushArg(&count, sizeof(count)),
            R_PushArg(&model, sizeof(model)),
            R_PushArg(&on_water, sizeof(bool)),
            (void*)G_GetPrevTickMap(),
        },
    });
}

static void arrival_render_assignment(const struct arrival_state *as,
                                      const struct arrival_member *members, int nmembers)
{
    const vec3_t magenta = (vec3_t){1.0f, 0.0f, 1.0f};
    const float circle_radius = 1.5f;
    const float width = 0.5f;

    for(int i = 0; i < as->num_slots; i++) {
        vec2_t pos = as->slots[i];
        R_PushCmd((struct rcmd){
            .func = R_GL_DrawSelectionCircle,
            .nargs = 5,
            .args = {
                R_PushArg(&pos, sizeof(pos)),
                R_PushArg(&circle_radius, sizeof(circle_radius)),
                R_PushArg(&width, sizeof(width)),
                R_PushArg(&magenta, sizeof(magenta)),
                (void*)G_GetPrevTickMap(),
            }
        });
    }

    for(int m = 0; m < nmembers; m++) {
        if(!members[m].us->sink_valid)
            continue;
        vec2_t from = members[m].pos;
        vec2_t endpoints[] = {from, members[m].us->sink};
        R_PushCmd((struct rcmd){
            .func = R_GL_DrawLine,
            .nargs = 4,
            .args = {
                R_PushArg(endpoints, sizeof(endpoints)),
                R_PushArg(&width, sizeof(width)),
                R_PushArg(&magenta, sizeof(magenta)),
                (void*)G_GetPrevTickMap()
            }
        });
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void G_Arrival_InitFlock(struct arrival_state *as)
{
    as->phase = ARRIVAL_PHASE_INACTIVE;
}

void G_Arrival_Deactivate(struct arrival_state *as)
{
    /* Stop filling but keep the built region. */
    if(as->phase == ARRIVAL_PHASE_FILLING)
        as->phase = ARRIVAL_PHASE_DONE;
}

bool G_Arrival_IsActive(const struct arrival_state *as)
{
    return as->phase == ARRIVAL_PHASE_FILLING;
}

void G_Arrival_InitUnit(struct arrival_unit_state *us, vec2_t order_pos)
{
    us->substate = ARRIVAL_SUBSTATE_APPROACH;
    us->los_latched = false;
    us->los_hyst = 0;
    us->sink_valid = false;
    us->stuck = 0;
    us->progress_anchored = false;
    us->order_pos = order_pos;
}

void G_Arrival_UpdateFlock(struct arrival_state *as, const struct map *map, vec2_t target_xz,
                           enum nav_layer layer, float unit_radius, int total_members,
                           const struct arrival_member *members, int nmembers)
{
    bool built = (as->phase != ARRIVAL_PHASE_INACTIVE);
    if(built) {
        bool all_settled = true;
        for(int i = 0; i < nmembers; i++) {
            if(!members[i].settled) {
                all_settled = false;
                break;
            }
        }
        if(all_settled) {
            as->phase = ARRIVAL_PHASE_DONE;
            return;
        }
    }else{
        if(total_members < ARRIVAL_MIN_UNITS)
            return;
        if(!arrival_near_goal(members, nmembers, target_xz))
            return;
    }

    as->layer = layer;
    as->radius = arrival_zone_radius(map, total_members, unit_radius);
    as->phase = ARRIVAL_PHASE_FILLING;

    if(!built) {
        /* Pin the centre to the nearest reachable tile once; recomputing it each tick would let
         * the footprint flip-flop across walls as units settle.
         */
        vec2_t snapped;
        as->centre = M_NavClosestPathable(map, layer, target_xz, &snapped) ? snapped : target_xz;
        as->axis = arrival_approach_axis(members, nmembers, target_xz);
        arrival_build_slots(as, map);
        arrival_thin_centered_slots(as, map, total_members, ARRIVAL_SLOT_SPACING * unit_radius);

        /* Drop slots whose straight line to the centre leaves the goal region: the flood pops by
         * Euclidean distance and can wrap a thin wall, placing slots reachable only by detour.
         * Keeping the clear-line ones matches the fine tier, which beelines on that test.
         */
        int kept = 0;
        for(int si = 0; si < as->num_slots; si++) {
            if(!M_NavSegmentWithinRegion(map, as->centre, as->slots[si],
                                         as->region_keys, as->num_region))
                continue;
            as->slots[kept++] = as->slots[si];
        }
        as->num_slots = kept;
        /* COM of the kept slots + a dest field toward it - the fallback seek target. */
        vec2_t com = (vec2_t){0.0f, 0.0f};
        for(int si = 0; si < as->num_slots; si++) {
            com.x += as->slots[si].x;
            com.z += as->slots[si].z;
        }
        if(as->num_slots > 0) {
            com.x /= as->num_slots;
            com.z /= as->num_slots;
        }
        vec2_t com_snapped;
        as->com = M_NavClosestPathable(map, layer, com, &com_snapped) ? com_snapped : com;
        as->com_dest_id = M_NavDestIDForPos(map, as->com, layer);

        /* Sort frame for the far->near + lateral ordering. */
        s_slot_sort_ref  = as->centre;
        s_slot_sort_axis = as->axis;
        s_slot_sort_perp = (vec2_t){-as->axis.z, as->axis.x};
        s_slot_sort_band = ARRIVAL_SLOT_SPACING * unit_radius;

        arrival_order_slots_far_near(as);
        as->row_height = ARRIVAL_SLOT_SPACING * unit_radius;
        arrival_compute_geodesic_rings(as, map, members, nmembers);

        as->active_row = 0;
        as->stall_counter = 0;
        as->prev_occ = 0;
        as->fill_frac = 0.0f;
        as->realloc_counter = 0;
        arrival_realloc_slots(as, map, members, nmembers);

    }else if(++as->realloc_counter >= ARRIVAL_REALLOC_PERIOD) {
        as->realloc_counter = 0;
        arrival_realloc_slots(as, map, members, nmembers);
    }
}

void G_Arrival_RequestField(const struct arrival_state *as, const struct map *map)
{
    if(as->phase != ARRIVAL_PHASE_FILLING)
        return;
    M_NavRequestAsyncGroupArrivalField(map, as->layer, as->centre, as->radius);
}

bool G_Arrival_DesiredVelocity(const struct arrival_state *as, struct arrival_unit_state *us,
                               const struct map *region_map, const struct map *nav_map,
                               vec2_t pos, vec2_t velocity, bool has_dest_los, vec2_t *out_vel)
{
    if(as->phase != ARRIVAL_PHASE_FILLING)
        return false;

    /* Reaching the region (or its one-tile pad) commits the unit to the SEEK substate; the latch,
     * not the live test, gates it thereafter, so a unit shoved off the blue heads back in rather
     * than dropping to the approach field.
     */
    if(arrival_near_region(as, region_map, pos))
        unit_commit(us);

    switch(us->substate) {
    case ARRIVAL_SUBSTATE_SEEK:
    case ARRIVAL_SUBSTATE_SEEK_ARMED: {
        /* Re-target if the assigned slot got blocked out from under us. */
        if(us->sink_valid && M_NavPositionBlocked(nav_map, as->layer, us->sink))
            us->sink_valid = arrival_nearest_open_slot(as, nav_map, pos, &us->sink);

        /* Beeline the slot only while the line to it stays on the footprint; else fall through to
         * the nearest-slot push, which drives a walled-off unit onto the cluster.
         */
        if(us->sink_valid
        && M_NavSegmentWithinRegion(nav_map, pos, us->sink, as->region_keys, as->num_region)) {
            vec2_t v;
            PFM_Vec2_Sub(&us->sink, &pos, &v);
            /* Count ticks we want the slot but barely move and make no progress: crowd-wedged. */
            if(PFM_Vec2_Len(&velocity) < ARRIVAL_STUCK_SPEED
            && PFM_Vec2_Dot(&velocity, &v) <= 0.0f)
                us->stuck++;
            else
                us->stuck = 0;
            if(PFM_Vec2_Len(&v) > EPSILON)
                PFM_Vec2_Normal(&v, &v);
            *out_vel = v;
            return true;
        }
        /* No reachable slot: head for the nearest slot (occupied or not) onto the cluster, where
         * it stops on contact, rather than a bare tile or the empty centre.
         */
        vec2_t to_slot;
        if(arrival_nearest_slot(as, pos, &to_slot)) {
            PFM_Vec2_Sub(&to_slot, &pos, &to_slot);
            if(PFM_Vec2_Len(&to_slot) > EPSILON)
                PFM_Vec2_Normal(&to_slot, &to_slot);
            *out_vel = to_slot;
            return true;
        }
        *out_vel = (vec2_t){0.0f, 0.0f};
        return true;
    }
    case ARRIVAL_SUBSTATE_APPROACH:
    case ARRIVAL_SUBSTATE_APPROACH_ARMED: {
        /* Approach on the TARGET_ZONE field, which routes around a wall boxing the goal in (the
         * point field would steer the long way to a blocked click's nearest tile).
         */
        vec2_t zvel;
        bool at_slot;
        if(M_NavDesiredGroupArrivalVelocity(nav_map, as->layer, pos, as->centre, as->radius,
            &zvel, &at_slot)
        && !at_slot && PFM_Vec2_Len(&zvel) > EPSILON) {
            *out_vel = zvel;
            return true;
        }
        /* Zone field gave nothing: with static LOS, beeline the goal and push in; else route
         * around on the COM field. Debounce the LOS flip so a wall seam can't oscillate it.
         */
        if(has_dest_los == us->los_latched)
            us->los_hyst = 0;
        else if(++us->los_hyst >= ARRIVAL_LOS_HYST) {
            us->los_latched = has_dest_los;
            us->los_hyst = 0;
        }
        if(us->los_latched) {
            vec2_t toc;
            PFM_Vec2_Sub((vec2_t*)&as->centre, &pos, &toc);
            if(PFM_Vec2_Len(&toc) > EPSILON)
                PFM_Vec2_Normal(&toc, &toc);
            *out_vel = toc;
            return true;
        }
        *out_vel = M_NavDesiredPointSeekVelocity(nav_map, as->com_dest_id, pos, as->com);
        return true;
    }
    }
    return false;
}

bool G_Arrival_ShouldSettle(const struct arrival_state *as, struct arrival_unit_state *us,
                            const struct map *region_map, const struct map *nav_map,
                            vec2_t new_pos, vec2_t velocity, float radius, int nsettled)
{
    /* Settle on reaching an open slot (at_sink), being surrounded by enough 
     * settled neighbours (by_prop), or as a last resort - being boxed in too 
     * long (by_stuck).
     */
    bool in_region = arrival_in_region(as, region_map, new_pos);
    bool near_region = arrival_near_region(as, region_map, new_pos);
    bool at_sink = in_region
                && arrival_near_open_slot(as, nav_map, new_pos, radius * ARRIVAL_SINK_TOLERANCE);
    int settle_contacts = ARRIVAL_SETTLE_CONTACTS;
    if(as->fill_frac >= ARRIVAL_FILL_RELAX_1)
        settle_contacts = 2;
    else if(as->fill_frac >= ARRIVAL_FILL_RELAX_2)
        settle_contacts = 3;
    bool reachable_slot = us->sink_valid
        && M_NavSegmentWithinRegion(nav_map, new_pos, us->sink, as->region_keys, as->num_region);

    /* A slotless unit snuggles onto the cluster only towards the end (fill_done), 
     * else it settles short and strings the flock shape out.
     */
    bool fill_done = as->active_row >= as->num_rows - 1;
    /* A unit closing on a reachable slot is left to reach it, 
     * not settled short by the cascade. 
     */
    bool advancing = false;
    if(us->sink_valid) {
        vec2_t to_sink;
        PFM_Vec2_Sub((vec2_t*)&us->sink, &new_pos, &to_sink);
        advancing = PFM_Vec2_Dot(&velocity, &to_sink) > 0.0f;
    }
    /* Start grace: a unit ordered among already-arrived units looks 
     * 'arrived' initially. Gate the proximity settles on first travelling 
     * clear of the order point (armed).
     */
    if(!unit_armed(us)) {
        vec2_t from_order;
        PFM_Vec2_Sub(&new_pos, (vec2_t*)&us->order_pos, &from_order);
        if(PFM_Vec2_Len(&from_order) > ARRIVAL_ENGAGE_DIST)
            unit_arm(us);
    }
    bool by_prop = unit_armed(us) && !at_sink && in_region
                && nsettled >= settle_contacts
                && (reachable_slot ? !advancing : fill_done);

    /* Give up only when plausibly final: near the footprint, or - within settle range of the
     * goal centre - pressed against an arrived unit. Brushing an arrived unit far from the goal
     * is not enough; that unit keeps heading in.
     */
    struct map_resolution res;
    M_NavGetResolution(region_map, &res);
    float tile_dim = (TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE) / (float)res.tile_w;
    float settle_range = as->radius * tile_dim * ARRIVAL_SETTLE_RANGE;
    vec2_t to_centre;
    PFM_Vec2_Sub(&new_pos, (vec2_t*)&as->centre, &to_centre);
    bool within_settle_range = PFM_Vec2_Dot(&to_centre, &to_centre) <= settle_range * settle_range;
    bool stuck_eligible = near_region || (nsettled >= 1 && within_settle_range);
    if(!at_sink && !us->sink_valid && stuck_eligible && unit_armed(us)) {
        /* Net-progress check (not instantaneous speed): a unit ping-ponging 
         * a field discontinuity twitches above a speed threshold once a cycle. 
         */
        if(!us->progress_anchored) {
            us->progress_anchor = new_pos;
            us->progress_anchored = true;
            us->stuck = 0;
        }
        vec2_t prog_disp;
        PFM_Vec2_Sub(&new_pos, &us->progress_anchor, &prog_disp);
        if(PFM_Vec2_Len(&prog_disp) > ARRIVAL_STUCK_DISP) {
            us->progress_anchor = new_pos;
            us->stuck = 0;
        }else{
            us->stuck++;
        }
    }
    bool by_stuck = !at_sink && !by_prop && stuck_eligible
                 && us->stuck >= ARRIVAL_STUCK_LIMIT;

    /* Relax the settle condition when enough units have settled. This allows "stray"
     * units to make a best-effort stop in the presence of difficult dynamic conditions.
     */
    bool by_contact = unit_armed(us) && !at_sink && near_region && !reachable_slot
                   && nsettled >= 1 && as->fill_frac >= ARRIVAL_FILL_RELAX_1;
    return (at_sink || by_prop || by_stuck || by_contact);
}

vec2_t G_Arrival_SeekTarget(const struct arrival_state *as, const struct arrival_unit_state *us,
                            vec2_t target_xz)
{
    if(as->phase == ARRIVAL_PHASE_FILLING && unit_committed(us) && us->sink_valid)
        return us->sink;
    return target_xz;
}

bool G_Arrival_NeighbourSettling(const struct arrival_unit_state *us, vec2_t neighb_pos,
                                 float radius)
{
    bool at_slot = unit_committed(us) && us->sink_valid;
    if(at_slot) {
        vec2_t sd;
        PFM_Vec2_Sub((vec2_t*)&us->sink, &neighb_pos, &sd);
        at_slot = PFM_Vec2_Len(&sd) < radius * ARRIVAL_SINK_TOLERANCE;
    }
    return at_slot;
}

void G_Arrival_RenderDebug(const struct arrival_state *as, const struct camera *cam,
                           const struct map *map, dest_id_t dest_id, vec3_t color,
                           const struct arrival_member *members, int nmembers)
{
    arrival_render_field(as, map, color);
    arrival_render_assignment(as, members, nmembers);
    M_NavRenderVisibleGroupArrivalField(map, cam, as->layer, as->centre, as->radius, dest_id);
}

