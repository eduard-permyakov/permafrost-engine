/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2022-2026 Eduard Permyakov 
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


#version 430 core

/* The ClearPath solve of src/game/clearpath.c (G_ClearPath_NewVelocity), one
 * workgroup per unit. The CPU gathers every input exactly as for its own
 * solve and lays them out as below; a change to the solve must be made in
 * both places.
 *
 * The lanes build the unit's obstacles together, then split its candidates
 * between them. Each lane keeps its best candidate in the CPU's evaluation
 * order, so the least penalised distance over the lanes, the earliest
 * candidate on a tie, is the CPU's pick.
 *
 * The solve's candidates lie on obstacle boundaries, where its inside test
 * turns on the last bit of a world-space coordinate, so the arithmetic must
 * round as the CPU's does: every value is precise (no fused multiply-adds)
 * and division and square root are the correctly rounded div_rn and sqrt_rn.
 */

#define LANES               (64)
layout(local_size_x = LANES) in;

#define EPSILON             (1.0 / 1024.0)
#define MAX_PAIRWISE_RAYS   (24)
#define MAX_SOLVE_RETRIES   (3)
#define CP_SIDE_PENALTY     (4.0)
#define TILE_RADIUS         (2.8284271)
#define BUFFER_RADIUS       (0.0)
/* CLEARPATH_STALL_SPEED squared, as the CPU's float product */
#define STALL_SPEED2_BITS   (0x3B23D70Bu)
#define MAX_DYN             (32)
#define MAX_STAT            (32)
#define MAX_TILES           (12)
#define MAX_VOS             (MAX_DYN + MAX_STAT + MAX_TILES)
/* A chunk's side in world units: TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE
 * of src/map/public/tile.h */
#define CHUNK_COORDS        (256.0)
#define NO_CANDIDATE        (0xFFFFFFFFu)

#define UNIT_RELAX          (1u << 0)
#define UNIT_ON_BLOCKED     (1u << 1)

#define DIAG_GAVE_UP        (1u << 8)
#define DIAG_PATCH_MISS     (1u << 9)

/* The obstacle class in the top bits of a VO's source index */
#define SRC_STAT            (1u << 30)
#define SRC_TILE            (2u << 30)
#define SRC_CLASS           (3u << 30)

/* Must match struct gpu_cp_unit in movement.c */
struct unit{
    float px, pz;
    float vx, vz;
    float dx, dz;
    float radius;
    float max_step;
    float horizon;
    uint  flags;
    int   side;
    uint  first;
    uint  ndyn, nstat, ntiles;
    int   prow, pcol;
    uint  pdim;
    uint  pfirst;
};

/* Must match struct gpu_cp_nb in movement.c; a tile obstacle leaves the
 * velocity and radius unused. */
struct nb{
    float px, pz;
    float vx, vz;
    float radius;
};

/* Must match struct gpu_cp_result in movement.c */
struct result{
    float vx, vz;
    int   side;
    uint  diag;
};

layout(std430, binding = 0) readonly buffer units_buff{ unit units[]; };
layout(std430, binding = 1) readonly buffer nbs_buff{ nb nbs[]; };
layout(std430, binding = 2) readonly buffer patch_buff{ uint patch_words[]; };
layout(std430, binding = 3) writeonly buffer results_buff{ result results[]; };

/* The navigation grid: its resolution (chunks, then tiles per chunk) and
 * position */
uniform ivec4 nav_resolution;
uniform vec2  nav_pos;
uniform int   num_sim_ents;

/*****************************************************************************/
/* PER-INVOCATION STATE                                                      */
/*****************************************************************************/

unit          s_unit;
precise vec2  s_pos;
precise vec2  s_vel;
bool          s_miss;
uint          s_lane;

/*****************************************************************************/
/* PER-UNIT STATE                                                            */
/*****************************************************************************/

/* The obstacles still in the solve, as indices into nbs[]: the retries drop
 * the furthest one each */
shared uint   sh_dyn[MAX_DYN];
shared uint   sh_stat[MAX_STAT];
shared uint   sh_tile[MAX_TILES];
shared uint   sh_ndyn, sh_nstat, sh_ntiles;

/* The combined obstacle: dynamic neighbours' HRVOs, then static neighbours'
 * VOs, then wall tiles' VOs, each from the obstacle in sh_src */
shared uint   sh_src[MAX_VOS];
shared vec2   sh_apex[MAX_VOS];
shared vec2   sh_left[MAX_VOS];
shared vec2   sh_right[MAX_VOS];
shared uint   sh_nvos;

/* Each lane's best candidate, then the unit's */
shared float  sh_best_d[LANES];
shared uint   sh_best_c[LANES];
shared vec2   sh_best_v[LANES];
shared bool   sh_found;
shared vec2   sh_vnew;
shared bool   sh_miss;

/*****************************************************************************/
/* HELPERS                                                                   */
/*****************************************************************************/

/* IEEE round-to-nearest a / b: the approximate reciprocal refined, then the
 * quotient corrected twice by its exact residual */
float div_rn(float a, float b)
{
    precise float y = 1.0 / b;
    precise float e = fma(-b, y, 1.0);
    y = fma(y, e, y);
    precise float q = a * y;
    precise float r = fma(-b, q, a);
    q = fma(r, y, q);
    r = fma(-b, q, a);
    q = fma(r, y, q);
    return q;
}

/* IEEE round-to-nearest sqrt(x), the same way */
float sqrt_rn(float x)
{
    if(x <= 0.0)
        return sqrt(x);
    precise float y = inversesqrt(x);
    precise float s = x * y;
    precise float h = 0.5 * y;
    precise float r = fma(-s, s, x);
    s = fma(r, h, s);
    r = fma(-s, s, x);
    s = fma(r, h, s);
    return s;
}

float len2(vec2 a)
{
    precise float ret = a.x * a.x + a.y * a.y;
    return ret;
}

vec2 cp_norm(vec2 a)
{
    precise float len = sqrt_rn(len2(a));
    precise vec2 ret = vec2(div_rn(a.x, len), div_rn(a.y, len));
    return ret;
}

bool same_position(vec2 a, vec2 b)
{
    precise vec2 d = b - a;
    return len2(d) < float(EPSILON * EPSILON);
}

vec2 nb_pos(uint i) { return vec2(nbs[i].px, nbs[i].pz); }
vec2 nb_vel(uint i) { return vec2(nbs[i].vx, nbs[i].vz); }

/* C_InfiniteLineIntersection of src/phys/collision.c, with its NaN slopes
 * spelled out as flags */
bool infinite_line_isec(vec2 p1, vec2 d1, vec2 p2, vec2 d2, out vec2 isec)
{
    bool v1 = abs(d1.x) < EPSILON;
    bool v2 = abs(d2.x) < EPSILON;
    precise float s1 = v1 ? 0.0 : div_rn(d1.y, d1.x);
    precise float s2 = v2 ? 0.0 : div_rn(d2.y, d2.x);

    if(v1 && v2)
        return false;
    precise float ds = s1 - s2;
    if(!v1 && !v2 && abs(ds) < EPSILON)
        return false;

    precise float x, y;
    if(v1) {
        x = p1.x;
        y = (p1.x - p2.x) * s2 + p2.y;
    }else if(v2) {
        x = p2.x;
        y = (p2.x - p1.x) * s1 + p2.y;
    }else{
        x = div_rn(s1 * p1.x - s2 * p2.x + p2.y - p1.y, ds);
        y = s2 * (x - p2.x) + p2.y;
    }
    isec = vec2(x, y);
    return true;
}

/* Determinant-form ray/ray intersection (cp_ray_ray_isec) */
bool ray_ray_isec(vec2 ap, vec2 ad, vec2 bp, vec2 bd, out vec2 isec)
{
    precise float det = ad.x * bd.y - ad.y * bd.x;
    if(abs(det) < EPSILON)
        return false;

    precise vec2 d = bp - ap;
    precise float t = div_rn(d.x * bd.y - d.y * bd.x, det);
    precise float s = div_rn(d.x * ad.y - d.y * ad.x, det);
    if(t < 0.0 || s < 0.0)
        return false;

    precise vec2 ret = vec2(ap.x + t * ad.x, ap.y + t * ad.y);
    isec = ret;
    return true;
}

void vo_edges(vec2 nb_p, float nb_r, out vec2 right_side, out vec2 left_side)
{
    precise vec2 ent_to_nb = cp_norm(nb_p - s_pos);
    precise float rsum = nb_r + s_unit.radius + BUFFER_RADIUS;
    precise vec2 right = vec2(-ent_to_nb.y, ent_to_nb.x) * rsum;

    precise vec2 right_tangent = nb_p + right;
    precise vec2 left_tangent = nb_p - right;

    right_side = cp_norm(right_tangent - s_pos);
    left_side = cp_norm(left_tangent - s_pos);
}

void set_vo(uint slot, vec2 apex, vec2 left_side, vec2 right_side)
{
    sh_apex[slot] = apex;
    sh_left[slot] = left_side;
    sh_right[slot] = right_side;
}

void hrvo(uint slot, uint i)
{
    precise vec2 p = nb_pos(i), v = nb_vel(i);
    precise vec2 rs, ls;
    vo_edges(p, nbs[i].radius, rs, ls);
    precise vec2 rvo_apex = s_pos + (s_vel + v) * 0.5;

    precise vec2 centerline = ls + rs;
    precise vec2 vo_apex = s_pos + v;

    /* A still unit keeps the side it last deflected to */
    precise float det = (centerline.x * s_vel.y) - (centerline.y * s_vel.x);
    if(abs(det) <= EPSILON)
        det = float(s_unit.side);

    precise vec2 apex = rvo_apex;
    precise vec2 isec;
    if(det > EPSILON) {
        if(infinite_line_isec(rvo_apex, ls, vo_apex, rs, isec))
            apex = isec;
    }else if(det < -EPSILON) {
        if(infinite_line_isec(rvo_apex, rs, vo_apex, ls, isec))
            apex = isec;
    }
    set_vo(slot, apex, ls, rs);
}

void stat_vo(uint slot, uint i)
{
    precise vec2 rs, ls;
    vo_edges(nb_pos(i), nbs[i].radius, rs, ls);
    precise vec2 apex = s_pos + nb_vel(i);
    set_vo(slot, apex, ls, rs);
}

void tile_vo(uint slot, uint i)
{
    precise vec2 tile = nb_pos(i);
    precise vec2 to_tile = tile - s_pos;
    precise float dist = sqrt_rn(len2(to_tile));
    precise vec2 ent_to_nb = to_tile * div_rn(1.0, dist);
    precise vec2 right = vec2(-ent_to_nb.y, ent_to_nb.x) * TILE_RADIUS;

    precise vec2 right_tangent = tile + right;
    precise vec2 left_tangent = tile - right;

    precise float gap = dist - TILE_RADIUS;
    precise float closing = (gap > 0.0 && s_unit.horizon > 0.0) ? div_rn(gap, s_unit.horizon) : 0.0;

    precise vec2 apex = s_pos + ent_to_nb * closing;
    set_vo(slot, apex, cp_norm(left_tangent - s_pos), cp_norm(right_tangent - s_pos));
}

/* The first lane lists the obstacles in the CPU's order, then the lanes
 * build one VO each */
void build_vos()
{
    if(s_lane == 0u) {
        uint n = 0u;
        for(uint k = 0; k < sh_ndyn; k++) {
            if(!same_position(s_pos, nb_pos(sh_dyn[k])))
                sh_src[n++] = sh_dyn[k];
        }
        for(uint k = 0; k < sh_nstat; k++) {
            if(!same_position(s_pos, nb_pos(sh_stat[k])))
                sh_src[n++] = sh_stat[k] | SRC_STAT;
        }
        for(uint k = 0; k < sh_ntiles; k++) {
            if(!same_position(s_pos, nb_pos(sh_tile[k])))
                sh_src[n++] = sh_tile[k] | SRC_TILE;
        }
        sh_nvos = n;
    }
    barrier();

    for(uint v = s_lane; v < sh_nvos; v += LANES) {
        uint src = sh_src[v];
        uint i = src & ~SRC_CLASS;
        switch(src & SRC_CLASS) {
        case 0u:       hrvo(v, i);    break;
        case SRC_STAT: stat_vo(v, i); break;
        default:       tile_vo(v, i); break;
        }
    }
    barrier();
}

/* Points exactly on the boundary are 'not inside' (inside_pcr_avx2) */
bool inside_pcr(vec2 test)
{
    const float eps2 = float(EPSILON * EPSILON);
    for(uint i = 0; i < sh_nvos; i++) {

        precise vec2 d = test - sh_apex[i];
        precise float l2 = d.x * d.x + d.y * d.y;
        precise float eps2_len2 = eps2 * l2;
        precise float ldet = d.y * sh_left[i].x - d.x * sh_left[i].y;
        precise float rdet = d.y * sh_right[i].x - d.x * sh_right[i].y;
        precise float ldet2 = ldet * ldet;
        precise float rdet2 = rdet * rdet;

        if(l2 >= eps2
        && ldet > 0.0 && ldet2 >= eps2_len2
        && rdet < 0.0 && rdet2 >= eps2_len2)
            return true;
    }
    return false;
}

void ray(uint k, out vec2 point, out vec2 dir)
{
    uint vo = k >> 1;
    point = sh_apex[vo];
    dir = ((k & 1u) == 0u) ? sh_left[vo] : sh_right[vo];
}

/* M_NavPositionPathable and M_NavPositionBlocked on the unit's patch, which
 * covers every point a step can land on */
void tile_state(vec2 p, out bool pathable, out bool blocked)
{
    pathable = false;
    blocked = false;

    precise float field_w = CHUNK_COORDS;
    precise float field_h = CHUNK_COORDS;
    precise float tile_x = div_rn(field_w, float(nav_resolution.z));
    precise float tile_z = div_rn(field_h, float(nav_resolution.w));
    precise float width = float(nav_resolution.x) * field_w;
    precise float height = float(nav_resolution.y) * field_h;

    /* C_BoxPointIntersection; X increases to the left */
    precise float min_x = nav_pos.x - width;
    precise float max_z = nav_pos.y + height;
    if(!(p.x <= nav_pos.x && p.x >= min_x
      && p.y >= nav_pos.y && p.y <= max_z))
        return;

    precise float cdz = nav_pos.y - p.y;
    precise float cdx = nav_pos.x - p.x;
    int chunk_r = clamp(int(div_rn(abs(cdz), field_h)), 0, nav_resolution.y - 1);
    int chunk_c = clamp(int(div_rn(abs(cdx), field_w)), 0, nav_resolution.x - 1);
    precise float chunk_base_x = nav_pos.x - float(chunk_c) * field_w;
    precise float chunk_base_z = nav_pos.y + float(chunk_r) * field_h;
    precise float tdz = chunk_base_z - p.y;
    precise float tdx = chunk_base_x - p.x;
    int tile_r = clamp(int(div_rn(abs(tdz), tile_z)), 0, nav_resolution.w - 1);
    int tile_c = clamp(int(div_rn(abs(tdx), tile_x)), 0, nav_resolution.z - 1);

    int dr = chunk_r * nav_resolution.w + tile_r - s_unit.prow;
    int dc = chunk_c * nav_resolution.z + tile_c - s_unit.pcol;
    if(dr < 0 || dc < 0 || dr >= int(s_unit.pdim) || dc >= int(s_unit.pdim)) {
        s_miss = true;
        return;
    }
    uint bit = uint(dr * int(s_unit.pdim) + dc) * 2u;
    uint word = patch_words[s_unit.pfirst + (bit >> 5)];
    pathable = ((word >> (bit & 31u)) & 1u) != 0u;
    blocked = ((word >> ((bit & 31u) + 1u)) & 1u) != 0u;
}

bool landing_ok(vec2 cand_ws)
{
    precise vec2 v = cand_ws - s_pos;
    precise float len = sqrt_rn(len2(v));
    if(len > s_unit.max_step && len > EPSILON)
        v = v * div_rn(s_unit.max_step, len);
    precise vec2 land = s_pos + v;

    bool pathable, blocked;
    tile_state(land, pathable, blocked);
    if(!pathable)
        return false;
    return ((s_unit.flags & UNIT_ON_BLOCKED) != 0u) || !blocked;
}

int side_of(vec2 des_v, vec2 v)
{
    precise float cross = des_v.x * v.y - des_v.y * v.x;
    return (cross > EPSILON) ? 1 : (cross < -EPSILON) ? -1 : 0;
}

/* Candidate 'c' in the CPU's order: the pairwise intersections of the first
 * 'n_pair' rays, then the projections onto every ray */
bool candidate(uint c, uint n_pair, uint npairs, vec2 des_v, out vec2 cand)
{
    if(c < npairs) {
        uint i = 0u, rem = c;
        while(rem >= n_pair - 1u - i) {
            rem -= n_pair - 1u - i;
            i++;
        }
        uint j = i + 1u + rem;
        precise vec2 ip, id, jp, jd, isec;
        ray(i, ip, id);
        ray(j, jp, jd);
        if(!ray_ray_isec(ip, id, jp, jd, isec))
            return false;
        cand = isec;
        return true;
    }
    precise vec2 p, d;
    ray(c - npairs, p, d);
    precise float len = d.x * des_v.x + d.y * des_v.y;
    precise vec2 proj = vec2(p.x + d.x * len, p.y + d.y * len);
    cand = proj;
    return true;
}

/* One pass of the solve; the unit's lanes all return the same answer */
bool solve_once(vec2 des_v, out vec2 vnew)
{
    build_vos();
    precise vec2 des_v_ws = s_pos + des_v;

    if(!inside_pcr(des_v_ws) && landing_ok(des_v_ws)) {
        vnew = des_v;
        return true;
    }

    precise float min_speed2 = ((s_unit.flags & UNIT_RELAX) != 0u)
                             ? uintBitsToFloat(STALL_SPEED2_BITS) : 0.0;
    uint n_rays = sh_nvos * 2u;
    uint n_pair = min(n_rays, uint(MAX_PAIRWISE_RAYS));
    uint npairs = (n_pair * (n_pair - 1u)) / 2u;

    /* vnew_consider over this lane's candidates, in order */
    precise float best_d = uintBitsToFloat(0x7F800000u);
    uint best_c = NO_CANDIDATE;
    precise vec2 best_v = vec2(0.0);
    for(uint c = s_lane; c < npairs + n_rays; c += LANES) {

        precise vec2 cand;
        if(!candidate(c, n_pair, npairs, des_v, cand))
            continue;
        if(inside_pcr(cand))
            continue;

        precise vec2 to_des = des_v_ws - cand;
        precise float dist2 = len2(to_des);
        precise vec2 v = cand - s_pos;
        if(len2(v) < min_speed2)
            continue;
        if(s_unit.side != 0) {
            precise vec2 dv = des_v_ws - s_pos;
            if(side_of(dv, v) == -s_unit.side)
                dist2 *= CP_SIDE_PENALTY;
        }
        if(dist2 < best_d && landing_ok(cand)) {
            best_d = dist2;
            best_c = c;
            best_v = v;
        }
    }
    sh_best_d[s_lane] = best_d;
    sh_best_c[s_lane] = best_c;
    sh_best_v[s_lane] = best_v;
    barrier();

    if(s_lane == 0u) {
        float d = uintBitsToFloat(0x7F800000u);
        uint bc = NO_CANDIDATE;
        vec2 bv = vec2(0.0);
        for(uint l = 0; l < LANES; l++) {
            if(sh_best_d[l] < d || (sh_best_d[l] == d && sh_best_c[l] < bc)) {
                d = sh_best_d[l];
                bc = sh_best_c[l];
                bv = sh_best_v[l];
            }
        }
        sh_found = (bc != NO_CANDIDATE);
        sh_vnew = bv;
    }
    barrier();

    vnew = sh_vnew;
    return sh_found;
}

void remove_furthest()
{
    precise float max_dist2 = uintBitsToFloat(0xFF800000u);
    int cls = -1;
    uint idx = 0u;

    for(uint k = 0; k < sh_ndyn; k++) {
        precise vec2 d = s_pos - nb_pos(sh_dyn[k]);
        precise float d2 = len2(d);
        if(d2 > max_dist2) { max_dist2 = d2; cls = 0; idx = k; }
    }
    for(uint k = 0; k < sh_nstat; k++) {
        precise vec2 d = s_pos - nb_pos(sh_stat[k]);
        precise float d2 = len2(d);
        if(d2 > max_dist2) { max_dist2 = d2; cls = 1; idx = k; }
    }
    for(uint k = 0; k < sh_ntiles; k++) {
        precise vec2 d = s_pos - nb_pos(sh_tile[k]);
        precise float d2 = len2(d);
        if(d2 > max_dist2) { max_dist2 = d2; cls = 2; idx = k; }
    }

    if(cls == 0)      sh_dyn[idx] = sh_dyn[--sh_ndyn];
    else if(cls == 1) sh_stat[idx] = sh_stat[--sh_nstat];
    else if(cls == 2) sh_tile[idx] = sh_tile[--sh_ntiles];
}

void main()
{
    uint idx = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
    if(idx >= uint(num_sim_ents))
        return;

    s_lane = gl_LocalInvocationID.x;
    s_unit = units[idx];
    s_pos = vec2(s_unit.px, s_unit.pz);
    s_vel = vec2(s_unit.vx, s_unit.vz);
    s_miss = false;

    if(s_lane == 0u) {
        sh_ndyn = s_unit.ndyn;
        sh_nstat = s_unit.nstat;
        sh_ntiles = s_unit.ntiles;
        for(uint k = 0; k < sh_ndyn; k++)
            sh_dyn[k] = s_unit.first + k;
        for(uint k = 0; k < sh_nstat; k++)
            sh_stat[k] = s_unit.first + s_unit.ndyn + k;
        for(uint k = 0; k < sh_ntiles; k++)
            sh_tile[k] = s_unit.first + s_unit.ndyn + s_unit.nstat + k;
        sh_miss = false;
    }
    barrier();

    precise vec2 des_v = vec2(s_unit.dx, s_unit.dz);
    uint retries = 0u;
    bool gave_up = true;
    precise vec2 vnew = vec2(0.0);

    while(true) {
        precise vec2 ret;
        if(solve_once(des_v, ret)) {
            vnew = ret;
            gave_up = false;
            break;
        }
        if(++retries > uint(MAX_SOLVE_RETRIES))
            break;
        if(s_lane == 0u)
            remove_furthest();
        barrier();
        if(sh_ndyn == 0u && sh_nstat == 0u && sh_ntiles == 0u)
            break;
    }

    if(s_miss)
        sh_miss = true;
    barrier();

    if(s_lane == 0u) {
        int side = 0;
        if(!gave_up && !same_position(vnew, des_v))
            side = side_of(des_v, vnew);
        results[idx].vx = gave_up ? 0.0 : vnew.x;
        results[idx].vz = gave_up ? 0.0 : vnew.y;
        results[idx].side = side;
        results[idx].diag = retries | (gave_up ? DIAG_GAVE_UP : 0u) | (sh_miss ? DIAG_PATCH_MISS : 0u);
    }
}
