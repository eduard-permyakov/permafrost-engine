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

#define MEM_FILE_SYS MEM_SYS_MOVE
#define MEM_FILE_SUB MEM_SUB_MOVE_FLOCKING

#include "flocking.h"
#include "../lib/public/simd.h"
#include "../map/public/map.h"
#include "../nav/public/nav.h"

#include <assert.h>
#include <math.h>

#define SIGNUM(x)    (((x) > 0) - ((x) < 0))
#define MAX(a, b)    ((a) > (b) ? (a) : (b))
#define MIN(a, b)    ((a) < (b) ? (a) : (b))

static vec2_t cohesion_force(uint32_t uid, vec2_t ent_xz_pos, int hz, const struct flock_snap *snap);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

/* Set at init to the widest supported cohesion kernel. */
static vec2_t (*s_cohesion_force)(uint32_t uid, vec2_t ent_xz_pos, int hz,
                                  const struct flock_snap *snap) = cohesion_force;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/* Cohesion is a behaviour that causes agents to steer towards the center of mass of nearby agents.
 */
static vec2_t cohesion_force(uint32_t uid, vec2_t ent_xz_pos, int hz, const struct flock_snap *snap)
{
    vec2_t COM = (vec2_t){0.0f};
    size_t neighbour_count = 0;

    for(size_t i = 0; i < snap->nents; i++) {

        if(snap->uids[i] == uid)
            continue;

        vec2_t diff;
        vec2_t curr_xz_pos = (vec2_t){snap->pos_x[i], snap->pos_z[i]};
        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);

        float t = (PFM_Vec2_Len(&diff) - COHESION_NEIGHBOUR_RADIUS*0.75)
                / COHESION_NEIGHBOUR_RADIUS;
        float scale = exp(-6.0f * t);

        PFM_Vec2_Scale(&curr_xz_pos, scale, &curr_xz_pos);
        PFM_Vec2_Add(&COM, &curr_xz_pos, &COM);
        neighbour_count++;
    }

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    vec2_t ret;
    PFM_Vec2_Scale(&COM, 1.0f / neighbour_count, &COM);
    PFM_Vec2_Sub(&COM, &ent_xz_pos, &ret);
    Move_Flock_Truncate(&ret, MOVE_FLOCK_MAX_FORCE(hz));
    return ret;
}

/* Cephes-derived polynomial exp over 8 lanes (relative error ~1e-7; the
 * result only weighs flock members in a steering heuristic). */
SIMD_TARGET_AVX2
static inline __m256 exp256_ps(__m256 x)
{
    const __m256 exp_hi = _mm256_set1_ps( 88.3762626647949f);
    const __m256 exp_lo = _mm256_set1_ps(-88.3762626647949f);
    const __m256 log2e  = _mm256_set1_ps(1.44269504088896341f);
    const __m256 c1     = _mm256_set1_ps(0.693359375f);
    const __m256 c2     = _mm256_set1_ps(-2.12194440e-4f);
    const __m256 one    = _mm256_set1_ps(1.0f);

    x = _mm256_min_ps(x, exp_hi);
    x = _mm256_max_ps(x, exp_lo);

    __m256 fx = _mm256_floor_ps(_mm256_add_ps(_mm256_mul_ps(x, log2e),
        _mm256_set1_ps(0.5f)));
    x = _mm256_sub_ps(x, _mm256_mul_ps(fx, c1));
    x = _mm256_sub_ps(x, _mm256_mul_ps(fx, c2));

    __m256 z = _mm256_mul_ps(x, x);
    __m256 y = _mm256_set1_ps(1.9875691500e-4f);
    y = _mm256_add_ps(_mm256_mul_ps(y, x), _mm256_set1_ps(1.3981999507e-3f));
    y = _mm256_add_ps(_mm256_mul_ps(y, x), _mm256_set1_ps(8.3334519073e-3f));
    y = _mm256_add_ps(_mm256_mul_ps(y, x), _mm256_set1_ps(4.1665795894e-2f));
    y = _mm256_add_ps(_mm256_mul_ps(y, x), _mm256_set1_ps(1.6666665459e-1f));
    y = _mm256_add_ps(_mm256_mul_ps(y, x), _mm256_set1_ps(5.0000001201e-1f));
    y = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(y, z), x), one);

    __m256i emm0 = _mm256_cvttps_epi32(fx);
    emm0 = _mm256_slli_epi32(_mm256_add_epi32(emm0, _mm256_set1_epi32(0x7f)), 23);
    return _mm256_mul_ps(y, _mm256_castsi256_ps(emm0));
}

SIMD_TARGET_AVX2
static vec2_t cohesion_force_avx2(uint32_t uid, vec2_t ent_xz_pos, int hz, const struct flock_snap *snap)
{
    const __m256 ex = _mm256_set1_ps(ent_xz_pos.x);
    const __m256 ez = _mm256_set1_ps(ent_xz_pos.z);
    const __m256 k_off = _mm256_set1_ps(COHESION_NEIGHBOUR_RADIUS * 0.75f);
    const __m256 k_scale = _mm256_set1_ps(-6.0f / COHESION_NEIGHBOUR_RADIUS);
    const __m256i vuid = _mm256_set1_epi32((int)uid);

    __m256 sum_x = _mm256_setzero_ps();
    __m256 sum_z = _mm256_setzero_ps();

    size_t nblock = snap->nents & ~7ull;
    for(size_t i = 0; i < nblock; i += 8) {

        __m256 px = _mm256_loadu_ps(&snap->pos_x[i]);
        __m256 pz = _mm256_loadu_ps(&snap->pos_z[i]);
        __m256i ids = _mm256_loadu_si256((const __m256i*)&snap->uids[i]);

        __m256 dx = _mm256_sub_ps(px, ex);
        __m256 dz = _mm256_sub_ps(pz, ez);
        __m256 dist = _mm256_sqrt_ps(_mm256_add_ps(_mm256_mul_ps(dx, dx),
            _mm256_mul_ps(dz, dz)));

        __m256 scale = exp256_ps(_mm256_mul_ps(_mm256_sub_ps(dist, k_off), k_scale));

        __m256 self = _mm256_castsi256_ps(_mm256_cmpeq_epi32(ids, vuid));
        scale = _mm256_andnot_ps(self, scale);

        sum_x = _mm256_add_ps(sum_x, _mm256_mul_ps(px, scale));
        sum_z = _mm256_add_ps(sum_z, _mm256_mul_ps(pz, scale));
    }

    float lanes_x[8], lanes_z[8];
    _mm256_storeu_ps(lanes_x, sum_x);
    _mm256_storeu_ps(lanes_z, sum_z);

    vec2_t COM = (vec2_t){0.0f};
    for(int i = 0; i < 8; i++) {
        COM.x += lanes_x[i];
        COM.z += lanes_z[i];
    }

    size_t neighbour_count = 0;
    for(size_t i = 0; i < nblock; i++) {
        neighbour_count += (snap->uids[i] != uid);
    }
    for(size_t i = nblock; i < snap->nents; i++) {

        if(snap->uids[i] == uid)
            continue;

        vec2_t diff;
        vec2_t curr_xz_pos = (vec2_t){snap->pos_x[i], snap->pos_z[i]};
        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);

        float t = (PFM_Vec2_Len(&diff) - COHESION_NEIGHBOUR_RADIUS*0.75)
                / COHESION_NEIGHBOUR_RADIUS;
        float scale = exp(-6.0f * t);

        COM.x += curr_xz_pos.x * scale;
        COM.z += curr_xz_pos.z * scale;
        neighbour_count++;
    }

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    vec2_t ret;
    PFM_Vec2_Scale(&COM, 1.0f / neighbour_count, &COM);
    PFM_Vec2_Sub(&COM, &ent_xz_pos, &ret);
    Move_Flock_Truncate(&ret, MOVE_FLOCK_MAX_FORCE(hz));
    return ret;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void Move_Flock_Init(void)
{
    if(simd_avx2_supported()) {
        s_cohesion_force = cohesion_force_avx2;
    }
}

void Move_Flock_Truncate(vec2_t *inout, float max_len)
{
    if(PFM_Vec2_Len(inout) > max_len) {

        PFM_Vec2_Normal(inout, inout);
        PFM_Vec2_Scale(inout, max_len, inout);
    }
}

quat_t Move_Flock_DirQuat(vec2_t velocity)
{
    assert(PFM_Vec2_Len(&velocity) > EPSILON);

    float angle_rad = atan2(velocity.raw[1], velocity.raw[0]) - M_PI/2.0f;
    return (quat_t) {
        0.0f, 
        1.0f * sin(angle_rad / 2.0f),
        0.0f,
        cos(angle_rad / 2.0f)
    };
}

vec2_t Move_Flock_FacingDir(quat_t rot)
{
    float theta = 2.0 * atan2(rot.y, rot.w);
    return (vec2_t){-sin(theta), cos(theta)};
}

/* Rotate 'cur' towards 'target' by at most 'max_deg' degrees about the Y axis,
 * picking a stable direction even when the orientations are antipodal.
 */
quat_t Move_Flock_TurnToward(quat_t cur, quat_t target, float max_deg)
{
    float angle_deg = RAD_TO_DEG(PFM_Quat_PitchDiff(&cur, &target));

    /* Near 180 degrees the shortest-arc direction is undefined: the cross-product
     * term collapses to zero and its sign is pure noise. Force a fixed sweep so the
     * unit never flips.
     */
    if(180.0f - fabs(angle_deg) < 1.0f)
        angle_deg = 180.0f;

    float turn_deg = MIN(max_deg, fabs(angle_deg)) * -SIGNUM(angle_deg);
    mat4x4_t rotmat;
    PFM_Mat4x4_MakeRotY(DEG_TO_RAD(turn_deg), &rotmat);

    quat_t rot, final;
    PFM_Quat_FromRotMat(&rotmat, &rot);
    PFM_Quat_MultQuat(&rot, &cur, &final);
    PFM_Quat_Normal(&final, &final);
    return final;
}

/* The part of a step a unit can take facing the way it does. Within the
 * band of the facing the step is kept whole; beyond it, only its share along
 * the nearer edge of the band. A slow step is a shove, and keeps within
 * MOVE_STEP_BAND; a unit walking in earnest turns after its travel and keeps
 * within MOVE_WALK_BAND. Neither moves the unit sideways or backward.
 */
vec2_t Move_Flock_StepWithinHeading(vec2_t step, vec2_t facing, float max_step)
{
    if(PFM_Vec2_Len(&step) < EPSILON)
        return step;

    float cross = facing.x * step.z - facing.z * step.x;
    float dot = PFM_Vec2_Dot(&facing, &step);
    float angle = atan2f(cross, dot);
    bool shove = PFM_Vec2_Len(&step) < FACING_FOLLOW_ENGAGE * max_step;
    float band = DEG_TO_RAD(shove ? MOVE_STEP_BAND : MOVE_WALK_BAND);
    if(fabsf(angle) <= band)
        return step;

    float edge_angle = (angle > 0.0f) ? band : -band;
    vec2_t edge = (vec2_t){
        facing.x * cosf(edge_angle) - facing.z * sinf(edge_angle),
        facing.x * sinf(edge_angle) + facing.z * cosf(edge_angle)
    };
    float along = PFM_Vec2_Dot(&step, &edge);
    if(along <= 0.0f)
        return (vec2_t){0.0f, 0.0f};
    PFM_Vec2_Scale(&edge, along, &edge);
    return edge;
}

/* A unit dressing its rank faces the way the rank does and steps into its
 * cell from there: ahead at its pace, any other way at a shuffle.
 */
vec2_t Move_Flock_DressStep(vec2_t step, vec2_t facing, float max_step)
{
    float len = PFM_Vec2_Len(&step);
    float shuffle = FACING_FOLLOW_ENGAGE * max_step;
    if(len <= shuffle)
        return step;

    float cross = facing.x * step.z - facing.z * step.x;
    float dot = PFM_Vec2_Dot(&facing, &step);
    if(fabsf(atan2f(cross, dot)) <= DEG_TO_RAD(MOVE_STEP_BAND))
        return step;

    PFM_Vec2_Scale(&step, shuffle / len, &step);
    return step;
}

float Move_Flock_CellBearing(vec2_t to_cell, quat_t rank_orientation)
{
    vec2_t rank_dir = Move_Flock_FacingDir(rank_orientation);
    float cross = rank_dir.x * to_cell.z - rank_dir.z * to_cell.x;
    float dot = PFM_Vec2_Dot(&rank_dir, &to_cell);
    return RAD_TO_DEG(fabsf(atan2f(cross, dot)));
}

/* Seek behaviour makes the entity target and approach a particular destination point.
 */
vec2_t Move_Flock_Seek(vec2_t pos_xz, vec2_t velocity, float max_speed, int hz, vec2_t target_xz)
{
    vec2_t ret, desired_velocity;

    PFM_Vec2_Sub(&target_xz, &pos_xz, &desired_velocity);
    if(PFM_Vec2_Len(&desired_velocity) >= EPSILON) {
        PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
    }
    PFM_Vec2_Scale(&desired_velocity, max_speed / hz, &desired_velocity);

    PFM_Vec2_Sub(&desired_velocity, &velocity, &ret);
    return ret;
}

/* Arrival behaviour is like 'seek' but the entity decelerates and comes to a halt when it is 
 * within a threshold radius of the destination point.
 * 
 * When not within line of sight of the destination, this will steer the entity along the 
 * flow field.
 */
vec2_t Move_Flock_ArrivePoint(vec2_t pos_xz, vec2_t velocity, int hz, vec2_t target_xz,
                              vec2_t vdes, bool has_dest_los, float speed)
{
    vec2_t ret, desired_velocity;
    float distance;

    if(has_dest_los) {

        PFM_Vec2_Sub(&target_xz, &pos_xz, &desired_velocity);
        distance = PFM_Vec2_Len(&desired_velocity);
        /* Standing on the goal point; normalising the zero offset would NaN. */
        if(distance >= EPSILON) {
            PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
        }
        PFM_Vec2_Scale(&desired_velocity, speed / hz, 
            &desired_velocity);

        if(distance < ARRIVE_SLOWING_RADIUS) {
            PFM_Vec2_Scale(&desired_velocity, distance / ARRIVE_SLOWING_RADIUS, &desired_velocity);
        }
    }else{
        PFM_Vec2_Scale(&vdes, speed / hz, &desired_velocity);
    }

    PFM_Vec2_Sub(&desired_velocity, &velocity, &ret);
    Move_Flock_Truncate(&ret, MOVE_FLOCK_MAX_FORCE(hz));
    return ret;
}

vec2_t Move_Flock_ArriveCell(vec2_t pos_xz, float max_speed, int hz, vec2_t cell_xz, vec2_t vdes)
{
    float distance;

    vec2_t desired_velocity;
    PFM_Vec2_Sub(&cell_xz, &pos_xz, &desired_velocity);
    distance = PFM_Vec2_Len(&desired_velocity);

    if(distance < ARRIVE_SLOWING_RADIUS) {
        PFM_Vec2_Scale(&desired_velocity, distance / ARRIVE_SLOWING_RADIUS, &desired_velocity);
    }else{
        PFM_Vec2_Scale(&vdes, max_speed / hz, &desired_velocity);
    }
    return desired_velocity;
}

vec2_t Move_Flock_ArriveEnemies(vec2_t velocity, float max_speed, int hz, vec2_t vdes)
{
    vec2_t ret, desired_velocity;

    PFM_Vec2_Scale(&vdes, max_speed / hz, &desired_velocity);
    PFM_Vec2_Sub(&desired_velocity, &velocity, &ret);
    Move_Flock_Truncate(&ret, MOVE_FLOCK_MAX_FORCE(hz));
    return ret;
}

vec2_t Move_Flock_Cohesion(uint32_t uid, vec2_t pos_xz, int hz, const struct flock_snap *snap)
{
    return s_cohesion_force(uid, pos_xz, hz, snap);
}

/* Separation is a behaviour that causes agents to steer away from nearby agents.
 */
float Move_Flock_SeparationReach(float radius_sum)
{
    return MAX(SEPARATION_NEIGHB_RADIUS, SEPARATION_NEIGHB_SCALE * radius_sum);
}

/* One neighbour's share of the separation push, summed into 'inout_sum';
 * 'diff' is the way from the unit to the neighbour.
 */
void Move_Flock_Separate(vec2_t diff, float radius_sum, float buffer_dist,
                         vec2_t *inout_sum, float *inout_min_gap)
{
    float radius = radius_sum + buffer_dist;

    if(PFM_Vec2_Len(&diff) < EPSILON)
        return;
    if(PFM_Vec2_Len(&diff) > Move_Flock_SeparationReach(radius_sum))
        return;

    float gap = PFM_Vec2_Len(&diff) - radius_sum;
    if(gap < *inout_min_gap)
        *inout_min_gap = gap;

    /* Exponential decay with y=1 when diff = radius*0.85 
     * Use smooth decay curves in order to curb the 'toggling' or oscillating 
     * behaviour that may arise when there are discontinuities in the forces. 
     */
    float eq = 0.85f;
    float steep = 20.0f;
    float t = (PFM_Vec2_Len(&diff) - radius*eq) / PFM_Vec2_Len(&diff);
    float scale = exp(MIN(-steep * t, 40.0f));
    PFM_Vec2_Scale(&diff, scale, &diff);

    PFM_Vec2_Add(inout_sum, &diff, inout_sum);
}

/* A separation push against a live desired direction backs the unit out of
 * the way it wants to go; redirect that component sideways (the remembered
 * passing side) so nose-to-nose units step around each other instead of
 * bouncing along the line of centres. The redirected share blends smoothly
 * to zero as interpenetration deepens (a hard cut oscillates at the
 * boundary), so a wedged pair keeps the radial push-out.
 */
vec2_t Move_Flock_RedirectBackward(vec2_t force, vec2_t vdes, int side, float min_gap)
{
    if(PFM_Vec2_Len(&vdes) < EPSILON || min_gap <= SEP_REDIRECT_MIN_GAP)
        return force;

    float frac = MIN(1.0f, (min_gap - SEP_REDIRECT_MIN_GAP) / -SEP_REDIRECT_MIN_GAP);

    vec2_t vhat;
    PFM_Vec2_Normal(&vdes, &vhat);
    float back = PFM_Vec2_Dot(&force, &vhat);
    if(back >= 0.0f)
        return force;

    vec2_t lat = (vec2_t){-vhat.z * side, vhat.x * side};
    vec2_t backward, lateral;
    PFM_Vec2_Scale(&vhat, back * frac, &backward);
    PFM_Vec2_Sub(&force, &backward, &force);
    PFM_Vec2_Scale(&lat, -back * frac, &lateral);
    PFM_Vec2_Add(&force, &lateral, &force);
    return force;
}

bool Move_Flock_LandingOk(const struct map *map, enum nav_layer layer, bool on_blocked, vec2_t pos)
{
    return M_NavPositionPathable(map, layer, pos)
        && (on_blocked || !M_NavPositionBlocked(map, layer, pos));
}

/* The part of a step that keeps to ground the unit may land on. The solver's
 * obstacles are discs, on the tile centres for walls and on the bodies for
 * neighbours, while the ground is refused by the tile, so its answer can aim
 * a little into a wall the field runs beside or onto the stamp beside a body
 * it clears; a step dropped whole there leaves the unit frozen for as long as
 * what it avoids stays put.
 */
vec2_t Move_Flock_Slide(const struct map *map, vec2_t pos, enum nav_layer layer, bool on_blocked,
                        vec2_t vel)
{
    vec2_t end_x, end_z;
    vec2_t along_x = (vec2_t){vel.x, 0.0f};
    vec2_t along_z = (vec2_t){0.0f, vel.z};
    PFM_Vec2_Add(&pos, &along_x, &end_x);
    PFM_Vec2_Add(&pos, &along_z, &end_z);
    bool x_ok = (vel.x != 0.0f) && Move_Flock_LandingOk(map, layer, on_blocked, end_x);
    bool z_ok = (vel.z != 0.0f) && Move_Flock_LandingOk(map, layer, on_blocked, end_z);
    if(x_ok && z_ok)
        return (fabsf(vel.x) >= fabsf(vel.z)) ? along_x : along_z;
    if(x_ok)
        return along_x;
    if(z_ok)
        return along_z;
    return (vec2_t){0.0f, 0.0f};
}

/* Nullify the components of the force which would guide
 * the entity towards an impassable tile. */
void Move_Flock_NullifyImpass(const struct map *map, enum nav_layer layer, vec2_t pos,
                              bool on_blocked, vec2_t *inout_force)
{
    vec2_t nt_dims = N_TileDims();
    vec2_t left =  (vec2_t){pos.x + nt_dims.x, pos.z};
    vec2_t right = (vec2_t){pos.x - nt_dims.x, pos.z};
    vec2_t top =   (vec2_t){pos.x, pos.z + nt_dims.z};
    vec2_t bot =   (vec2_t){pos.x, pos.z - nt_dims.z};

    if(inout_force->x > 0 
    && (!M_NavPositionPathable(map, layer, left)  
      || (!on_blocked && M_NavPositionBlocked(map, layer, left))))
        inout_force->x = 0.0f;

    if(inout_force->x < 0 
    && (!M_NavPositionPathable(map, layer, right) 
     || (!on_blocked && M_NavPositionBlocked(map, layer, right))))
        inout_force->x = 0.0f;

    if(inout_force->z > 0 
    && (!M_NavPositionPathable(map, layer, top) 
      || (!on_blocked && M_NavPositionBlocked(map, layer, top))))
        inout_force->z = 0.0f;

    if(inout_force->z < 0 
    && (!M_NavPositionPathable(map, layer, bot) 
      || (!on_blocked && M_NavPositionBlocked(map, layer, bot))))
        inout_force->z = 0.0f;
}

/* A step forward and aside on the committed side, at a fraction of the pace */
vec2_t Move_Flock_EscapeStep(vec2_t vdes, int side, float max_step)
{
    vec2_t vhat, lat, ret;
    PFM_Vec2_Normal(&vdes, &vhat);
    lat = (vec2_t){-vhat.z * side, vhat.x * side};
    PFM_Vec2_Add(&vhat, &lat, &ret);
    PFM_Vec2_Normal(&ret, &ret);
    PFM_Vec2_Scale(&ret, MOVE_ESCAPE_FLOOR * max_step, &ret);
    return ret;
}

float Move_Flock_WallDistance(const vec2_t *tiles, size_t ntiles, vec2_t pos)
{
    float ret = INFINITY;
    for(size_t i = 0; i < ntiles; i++) {
        vec2_t diff;
        PFM_Vec2_Sub((vec2_t*)&tiles[i], &pos, &diff);
        ret = MIN(ret, PFM_Vec2_Len(&diff));
    }
    return ret;
}

/* Keep what runs along the wall, drop the sideways detour. */
vec2_t Move_Flock_HugWall(vec2_t vpref, vec2_t vdes, int side)
{
    vec2_t vhat;
    PFM_Vec2_Normal(&vdes, &vhat);

    vec2_t lat = (vec2_t){-vhat.z * side, vhat.x * side};
    float along = PFM_Vec2_Dot(&vpref, &lat);
    if(along <= 0.0f)
        return vpref;

    PFM_Vec2_Scale(&lat, along, &lat);
    PFM_Vec2_Sub(&vpref, &lat, &vpref);
    return vpref;
}
