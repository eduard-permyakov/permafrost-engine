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

/* References:
 *     [1] ClearPath: Highly Parallel Collision Avoidance for
 *         Multi-Agent Simulation
 *         (http://gamma.cs.unc.edu/CA/ClearPath.pdf)
 *     [2] The Hybrid Reciprocal Velocity Obstacle
 *         (http://gamma.cs.unc.edu/HRVO/HRVO-T-RO.pdf)
 */

#define MEM_FILE_SYS MEM_SYS_GAME
#define MEM_FILE_SUB MEM_SUB_GAME_CLEARPATH

#include "clearpath.h"
#include "../lib/public/vec.h"
#include "../lib/public/simd.h"
#include "public/game.h"
#include "movement.h"
#include "game_private.h"
#include "../main.h"
#include "../event.h"
#include "../entity.h"
#include "../settings.h"
#include "../ui.h"
#include "../perf.h"
#include "../phys/public/collision.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../map/public/map.h"
#include "../lib/public/pf_string.h"
#include "../mem.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_CLEARPATH)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_CLEARPATH)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_CLEARPATH)


#define EPSILON         (1.0/1024)
#define MAX_SAVED_VOS   (512)
/* Rays participating in the pairwise-intersection candidate stage. With
 * distance-ordered neighbours these are the nearest obstacles; every ray still
 * bounds the accepted set via the containment test and contributes projection
 * candidates, so the cap only thins candidate generation.
 */
#define MAX_PAIRWISE_RAYS (24)
/* Neighbour-dropping retries of the full solve before giving up */
#define MAX_SOLVE_RETRIES (3)
/* A candidate on the other side of the preferred velocity than the
 * remembered one must be this much closer to win. */
#define CP_SIDE_PENALTY   (4.0f)

VEC_TYPE(vec2, vec2_t)
VEC_IMPL(static inline, vec2, vec2_t)

struct VO{
    vec2_t xz_apex;
    vec2_t xz_left_side;
    vec2_t xz_right_side;
};

struct RVO{
    vec2_t xz_apex;
    vec2_t xz_left_side;
    vec2_t xz_right_side;
};

struct HRVO{
    vec2_t xz_apex;
    vec2_t xz_left_side;
    vec2_t xz_right_side;
};

struct saved_ctx{
    struct cp_ent cpent;
    vec2_t        ent_des_v;
    struct HRVO   hrvos[MAX_SAVED_VOS];
    struct VO     vos[MAX_SAVED_VOS];
    size_t        n_hrvos;
    size_t        n_vos;
    vec2_t        v_new;
    vec_vec2_t    xpoints;
    bool          des_v_in_pcr;
    bool          valid;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct saved_ctx s_debug_saved;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/* Local inline vec2 helpers: the solve runs up to n_rays^2 * n_rays inner
 * iterations per unit, where the out-of-line PFM_Vec2 calls and their
 * double-precision sqrt dominate. */
static inline float cp_dot(vec2_t a, vec2_t b)   { return a.x * b.x + a.y * b.y; }
static inline vec2_t cp_add(vec2_t a, vec2_t b)  { return (vec2_t){a.x + b.x, a.y + b.y}; }
static inline vec2_t cp_sub(vec2_t a, vec2_t b)  { return (vec2_t){a.x - b.x, a.y - b.y}; }
static inline vec2_t cp_scale(vec2_t a, float s) { return (vec2_t){a.x * s, a.y * s}; }
static inline float cp_len2(vec2_t a)            { return a.x * a.x + a.y * a.y; }

static inline vec2_t cp_norm(vec2_t a)
{
    float len = sqrtf(cp_len2(a));
    return (vec2_t){a.x / len, a.y / len};
}

static bool same_position(vec2_t a, vec2_t b)
{
    return cp_len2(cp_sub(b, a)) < (float)(EPSILON * EPSILON);
}

/* Determinant-form ray/ray intersection. Unlike the slope-form
 * C_RayRayIntersection2D, the |det| parallel test rejects near-parallel steep
 * pairs whose far-away spurious intersections the slope-difference test
 * admits, and axis-aligned rays follow the intended geometry instead of the
 * NaN fall-throughs. Verified against the reference in bench/clearpath. */
static inline bool cp_ray_ray_isec(struct line_2d a, struct line_2d b, vec2_t *out)
{
    float det = a.dir.x * b.dir.z - a.dir.z * b.dir.x;
    if(fabsf(det) < EPSILON)
        return false;

    vec2_t d = cp_sub(b.point, a.point);
    float t = (d.x * b.dir.z - d.z * b.dir.x) / det;
    float s = (d.x * a.dir.z - d.z * a.dir.x) / det;
    if(t < 0.0f || s < 0.0f)
        return false;

    out->x = a.point.x + t * a.dir.x;
    out->z = a.point.z + t * a.dir.z;
    return true;
}

static void compute_vo_edges(struct cp_ent ent, struct cp_ent neighb,
                             vec2_t *out_xz_right, vec2_t *out_xz_left)
{
    vec2_t ent_to_nb = cp_norm(cp_sub(neighb.xz_pos, ent.xz_pos));
    vec2_t right = cp_scale((vec2_t){-ent_to_nb.z, ent_to_nb.x},
        neighb.radius + ent.radius + CLEARPATH_BUFFER_RADIUS);

    vec2_t right_tangent = cp_add(neighb.xz_pos, right);
    vec2_t left_tangent = cp_sub(neighb.xz_pos, right);

    *out_xz_right = cp_norm(cp_sub(right_tangent, ent.xz_pos));
    *out_xz_left = cp_norm(cp_sub(left_tangent, ent.xz_pos));
}

static struct VO compute_vo(struct cp_ent ent, struct cp_ent neighb)
{
    struct VO ret;
    compute_vo_edges(ent, neighb, &ret.xz_right_side, &ret.xz_left_side);
    ret.xz_apex = cp_add(ent.xz_pos, neighb.xz_vel);
    return ret;
}

static struct RVO compute_rvo(struct cp_ent ent, struct cp_ent neighb)
{
    struct RVO ret;
    compute_vo_edges(ent, neighb, &ret.xz_right_side, &ret.xz_left_side);
    ret.xz_apex = cp_add(ent.xz_pos, cp_scale(cp_add(ent.xz_vel, neighb.xz_vel), 0.5f));
    return ret;
}

static struct HRVO compute_hrvo(struct cp_ent ent, struct cp_ent neighb, int side)
{
    struct HRVO ret;
    struct RVO rvo = compute_rvo(ent, neighb);
    struct line_2d l1, l2;
    vec2_t intersec_point;

    vec2_t centerline = cp_add(rvo.xz_left_side, rvo.xz_right_side);
    vec2_t vo_apex = cp_add(ent.xz_pos, neighb.xz_vel);

    /* A still unit has no velocity to pick the passing side from; keep the
     * side it last deflected to rather than degrading to a symmetric RVO. */
    float det = (centerline.x * ent.xz_vel.y) - (centerline.y * ent.xz_vel.x);
    if(fabsf(det) <= EPSILON)
        det = side;
    if(det > EPSILON) { /* the entity velocity is left of the RVO centerline */

        l1 = (struct line_2d){rvo.xz_apex, rvo.xz_left_side};
        l2 = (struct line_2d){vo_apex, rvo.xz_right_side};

        bool collide = C_InfiniteLineIntersection(l1, l2, &intersec_point);
        assert(collide);
        ret.xz_apex = intersec_point;

    }else if(det < -EPSILON) { /* the entity velocity is right of the RVO centerline */

        l1 = (struct line_2d){rvo.xz_apex, rvo.xz_right_side};
        l2 = (struct line_2d){vo_apex, rvo.xz_left_side};

        bool collide = C_InfiniteLineIntersection(l1, l2, &intersec_point);
        assert(collide);
        ret.xz_apex = intersec_point;
    
    }else{ /* The entity velocity is right on the centerline */

        ret.xz_apex = rvo.xz_apex;
    }
    
    ret.xz_right_side = rvo.xz_right_side;
    ret.xz_left_side = rvo.xz_left_side;
    return ret;
}

static size_t compute_all_vos(struct cp_ent ent, const struct cp_ent *stat_neighbs,
                              size_t nstat, struct VO *out)
{
    size_t ret = 0;

    for(size_t i = 0; i < nstat; i++) {

        if(same_position(ent.xz_pos, stat_neighbs[i].xz_pos))
            continue;
        out[ret++] = compute_vo(ent, stat_neighbs[i]);
    }

    return ret;
}

static size_t compute_all_hrvos(struct cp_ent ent, const struct cp_ent *dyn_neighbs,
                                size_t ndyn, int side, struct HRVO *out)
{
    size_t ret = 0;

    for(size_t i = 0; i < ndyn; i++) {

        if(same_position(ent.xz_pos, dyn_neighbs[i].xz_pos))
            continue;
        out[ret++] = compute_hrvo(ent, dyn_neighbs[i], side);
    }

    return ret;
}

static void rays_repr(const struct HRVO *hrvos, size_t n_hrvos,
                      const struct VO *vos, size_t n_vos,
                      struct line_2d *out)
{
    size_t rays_idx  = 0;

    for(int i = 0; i < n_hrvos; i++) {
         
        out[rays_idx + 0].point = hrvos[i].xz_apex;
        out[rays_idx + 0].dir = hrvos[i].xz_left_side;

        out[rays_idx + 1].point = hrvos[i].xz_apex;
        out[rays_idx + 1].dir = hrvos[i].xz_right_side;

        rays_idx += 2;
    }

    for(int i = 0; i < n_vos; i++) {
    
        out[rays_idx + 0].point = vos[i].xz_apex;
        out[rays_idx + 0].dir = vos[i].xz_left_side;

        out[rays_idx + 1].point = vos[i].xz_apex;
        out[rays_idx + 1].dir = vos[i].xz_right_side;

        rays_idx += 2;
    }
}

/* One lane per VO (32 dynamic + 32 static neighbours max), rounded up to a
 * whole number of 8-lane groups. */
#define MAX_SOA_VOS (72)

/* SoA mirror of the VO ray pairs, one lane per VO, tail-padded with sentinel
 * VOs whose left test always fails ("outside"). */
struct rays_soa{
    float apex_x[MAX_SOA_VOS], apex_z[MAX_SOA_VOS];
    float ldir_x[MAX_SOA_VOS], ldir_z[MAX_SOA_VOS];
    float rdir_x[MAX_SOA_VOS], rdir_z[MAX_SOA_VOS];
    size_t nvos;
};

static void rays_soa_repr(const struct line_2d *rays, size_t n_rays, struct rays_soa *out)
{
    size_t nvos = n_rays / 2;
    assert(nvos + 8 <= MAX_SOA_VOS);
    out->nvos = nvos;
    for(size_t i = 0; i < nvos; i++) {
        out->apex_x[i] = rays[2*i + 0].point.x;
        out->apex_z[i] = rays[2*i + 0].point.z;
        out->ldir_x[i] = rays[2*i + 0].dir.x;
        out->ldir_z[i] = rays[2*i + 0].dir.z;
        out->rdir_x[i] = rays[2*i + 1].dir.x;
        out->rdir_z[i] = rays[2*i + 1].dir.z;
    }
    size_t npad = (nvos + 7) & ~7ull;
    for(size_t i = nvos; i < npad; i++) {
        out->apex_x[i] = 0.0f;
        out->apex_z[i] = 0.0f;
        out->ldir_x[i] = 0.0f;
        out->ldir_z[i] = 0.0f;
        out->rdir_x[i] = 0.0f;
        out->rdir_z[i] = 0.0f;
    }
}

/* Points exactly 'on' the boundary will be considered as 'not inside' of the PCR for our purposes.
 * The VO's two rays share an apex, so one subtraction serves both sign tests; comparing the raw
 * determinant against EPSILON * |d| preserves the normalized-compare boundary without the two
 * normalizations. */
static bool inside_pcr_scalar(const struct rays_soa *soa, vec2_t test)
{
    for(size_t i = 0; i < soa->nvos; i++) {

        vec2_t d = cp_sub(test, (vec2_t){soa->apex_x[i], soa->apex_z[i]});
        float len2 = cp_len2(d);
        if(len2 < (float)(EPSILON * EPSILON))
            continue;
        float eps_len = (float)EPSILON * sqrtf(len2);

        float left_det = (d.z * soa->ldir_x[i]) - (d.x * soa->ldir_z[i]);
        if(left_det < eps_len)
            continue;

        float right_det = (d.z * soa->rdir_x[i]) - (d.x * soa->rdir_z[i]);
        if(right_det > -eps_len)
            continue;

        return true;
    }

    return false;
}

/* 8-wide inside_pcr over the SoA rays. The sign tests compare det against
 * EPSILON * |d| like the scalar form, expressed sqrt-free as
 * (det > 0) && (det^2 >= EPS^2 * len2). */
SIMD_TARGET_AVX2
static bool inside_pcr_avx2(const struct rays_soa *soa, vec2_t test)
{
    const __m256 eps2 = _mm256_set1_ps((float)(EPSILON * EPSILON));
    const __m256 tx = _mm256_set1_ps(test.x);
    const __m256 tz = _mm256_set1_ps(test.z);
    const __m256 zero = _mm256_setzero_ps();

    size_t npad = (soa->nvos + 7) & ~7ull;
    for(size_t i = 0; i < npad; i += 8) {

        __m256 dx = _mm256_sub_ps(tx, _mm256_loadu_ps(&soa->apex_x[i]));
        __m256 dz = _mm256_sub_ps(tz, _mm256_loadu_ps(&soa->apex_z[i]));
        __m256 len2 = _mm256_add_ps(_mm256_mul_ps(dx, dx), _mm256_mul_ps(dz, dz));
        __m256 eps2_len2 = _mm256_mul_ps(eps2, len2);

        __m256 ldet = _mm256_sub_ps(
            _mm256_mul_ps(dz, _mm256_loadu_ps(&soa->ldir_x[i])),
            _mm256_mul_ps(dx, _mm256_loadu_ps(&soa->ldir_z[i])));
        __m256 rdet = _mm256_sub_ps(
            _mm256_mul_ps(dz, _mm256_loadu_ps(&soa->rdir_x[i])),
            _mm256_mul_ps(dx, _mm256_loadu_ps(&soa->rdir_z[i])));

        __m256 valid = _mm256_cmp_ps(len2, eps2, _CMP_GE_OQ);
        __m256 lpos = _mm256_and_ps(
            _mm256_cmp_ps(ldet, zero, _CMP_GT_OQ),
            _mm256_cmp_ps(_mm256_mul_ps(ldet, ldet), eps2_len2, _CMP_GE_OQ));
        __m256 rneg = _mm256_and_ps(
            _mm256_cmp_ps(rdet, zero, _CMP_LT_OQ),
            _mm256_cmp_ps(_mm256_mul_ps(rdet, rdet), eps2_len2, _CMP_GE_OQ));

        __m256 inside = _mm256_and_ps(valid, _mm256_and_ps(lpos, rneg));
        if(_mm256_movemask_ps(inside))
            return true;
    }
    return false;
}

/* Set at init to the widest supported PCR-containment kernel. */
static bool (*s_inside_pcr)(const struct rays_soa *soa, vec2_t test) = inside_pcr_scalar;

/* Running-min candidate accumulator: replaces the heap candidate vector and
 * the final full-scan selection. Candidates are worldspace points; the stored
 * velocity is the point converted to the entity's local space. */
struct vnew_min{
    float  min_dist2;
    vec2_t vnew;
    bool   any;
};

static inline bool cp_landing_ok(const struct cp_terrain *terrain, vec2_t ent_pos,
                                 vec2_t cand_ws)
{
    if(!terrain->map)
        return true;

    vec2_t v = cp_sub(cand_ws, ent_pos);
    float len = sqrtf(cp_len2(v));
    if(len > terrain->max_step && len > EPSILON)
        v = cp_scale(v, terrain->max_step / len);
    vec2_t land = cp_add(ent_pos, v);

    if(!M_NavPositionPathable(terrain->map, terrain->layer, land))
        return false;
    return terrain->on_blocked || !M_NavPositionBlocked(terrain->map, terrain->layer, land);
}

static inline int cp_side_of(vec2_t des_v, vec2_t v)
{
    float cross = des_v.x * v.z - des_v.z * v.x;
    return (cross > EPSILON) ? 1 : (cross < -EPSILON) ? -1 : 0;
}

/* The landing test runs only for a candidate that would become the new
 * minimum, so the tile lookups stay at a handful per solve. */
static inline void vnew_consider(struct vnew_min *m, vec2_t cand_ws,
                                 vec2_t des_v_ws, vec2_t ent_xz_pos,
                                 const struct cp_terrain *terrain, int side)
{
    float dist2 = cp_len2(cp_sub(des_v_ws, cand_ws));
    if(side != 0) {
        vec2_t des_v = cp_sub(des_v_ws, ent_xz_pos);
        vec2_t cand = cp_sub(cand_ws, ent_xz_pos);
        if(cp_side_of(des_v, cand) == -side)
            dist2 *= CP_SIDE_PENALTY;
    }
    if(dist2 < m->min_dist2 && cp_landing_ok(terrain, ent_xz_pos, cand_ws)) {
        m->min_dist2 = dist2;
        m->vnew = cp_sub(cand_ws, ent_xz_pos);
        m->any = true;
    }
}

static void remove_furthest(vec2_t xz_pos, struct cp_ent *dyn, size_t *ndyn,
                            struct cp_ent *stat, size_t *nstat)
{
    float max_dist2 = -INFINITY;
    struct cp_ent *del_arr = NULL;
    size_t *del_count = NULL;
    int del_idx = -1;

    for(int i = 0; i < 2; i++) {

        struct cp_ent *arr = (i == 0) ? dyn : stat;
        size_t count = (i == 0) ? *ndyn : *nstat;
        for(size_t j = 0; j < count; j++) {

            float len2 = cp_len2(cp_sub(xz_pos, arr[j].xz_pos));
            if(len2 > max_dist2) {
                max_dist2 = len2;
                del_arr = arr;
                del_count = (i == 0) ? ndyn : nstat;
                del_idx = (int)j;
            }
        }
    }

    if(max_dist2 > -INFINITY) {
        assert(del_idx != -1);
        del_arr[del_idx] = del_arr[--(*del_count)];
    }
}

static void on_render_3d(void *user, void *event)
{
    if(!s_debug_saved.valid)
        return;

    size_t idx = 0;

    const struct map *map = user;
    const struct cp_ent *cpent = &s_debug_saved.cpent;
    const size_t n_vos = s_debug_saved.n_hrvos + s_debug_saved.n_vos;

    vec3_t yellow = (vec3_t){1.0f, 1.0f, 0.0f};
    vec3_t blue = (vec3_t){0.0f, 0.0f, 1.0f};
    vec3_t green = (vec3_t){0.0f, 1.0f, 0.0f};

    STALLOC(vec2_t, apexes, n_vos);
    STALLOC(vec2_t, left_rays, n_vos);
    STALLOC(vec2_t, right_rays, n_vos);

    for(int i = 0; i < s_debug_saved.n_hrvos; i++, idx++) {
        apexes[idx] = s_debug_saved.hrvos[i].xz_apex;
        left_rays[idx] = s_debug_saved.hrvos[i].xz_left_side; 
        right_rays[idx] = s_debug_saved.hrvos[i].xz_right_side; 
    }

    for(int i = 0; i < s_debug_saved.n_vos; i++, idx++) {
        apexes[idx] = s_debug_saved.vos[i].xz_apex;
        left_rays[idx] = s_debug_saved.vos[i].xz_left_side; 
        right_rays[idx] = s_debug_saved.vos[i].xz_right_side; 
    }

    assert(idx == n_vos);
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawCombinedHRVO,
        .nargs = 5,
        .args = {
            R_PushArg(apexes, n_vos * sizeof(vec2_t)),
            R_PushArg(left_rays, n_vos * sizeof(vec2_t)),
            R_PushArg(right_rays, n_vos * sizeof(vec2_t)),
            R_PushArg(&n_vos, sizeof(n_vos)),
            (void*)G_GetPrevTickMap(),
        },
    });

    float reach = 2.0f * cpent->radius * CLEARPATH_NEIGHBOUR_SCALE;
    float radius = (reach > CLEARPATH_NEIGHBOUR_RADIUS) ? reach : CLEARPATH_NEIGHBOUR_RADIUS;
    float width = 0.5f;

    R_PushCmd((struct rcmd){
        .func = R_GL_DrawSelectionCircle,
        .nargs = 5,
        .args = {
            R_PushArg(&cpent->xz_pos, sizeof(cpent->xz_pos)),
            R_PushArg(&radius, sizeof(radius)),
            R_PushArg(&width, sizeof(width)),
            R_PushArg(&yellow, sizeof(yellow)),
            (void*)G_GetPrevTickMap(),
        },
    });

    mat4x4_t ident;
    PFM_Mat4x4_Identity(&ident);

    vec3_t origin_pos = (vec3_t){
        cpent->xz_pos.x, 
        M_HeightAtPoint(map, cpent->xz_pos) + 5.0f, 
        cpent->xz_pos.z
    };

    vec2_t des_v = s_debug_saved.ent_des_v;
    vec3_t des_vel_dir = (vec3_t){des_v.x, 0.0f, des_v.z};
    PFM_Vec3_Normal(&des_vel_dir, &des_vel_dir);

    float t = PFM_Vec2_Len(&des_v) * G_Move_GetTickHz();
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawRay,
        .nargs = 5,
        .args = {
            R_PushArg(&origin_pos, sizeof(origin_pos)),
            R_PushArg(&des_vel_dir, sizeof(des_vel_dir)),
            R_PushArg(&ident, sizeof(ident)),
            R_PushArg(&blue, sizeof(blue)),
            R_PushArg(&t, sizeof(t)),
        },
    });

    vec2_t v_new = s_debug_saved.v_new;
    vec3_t vel_dir = (vec3_t){v_new.x, 0.0f, v_new.z};
    PFM_Vec3_Normal(&vel_dir, &vel_dir);

    t = PFM_Vec2_Len(&v_new) * G_Move_GetTickHz();
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawRay,
        .nargs = 5,
        .args = {
            R_PushArg(&origin_pos, sizeof(origin_pos)),
            R_PushArg(&vel_dir, sizeof(vel_dir)),
            R_PushArg(&ident, sizeof(ident)),
            R_PushArg(&green, sizeof(green)),
            R_PushArg(&t, sizeof(t)),
        },
    });

    radius = 1.0f;
    width = 1.0f;

    for(int i = 0; i < vec_size(&s_debug_saved.xpoints); i++) {

        R_PushCmd((struct rcmd){
            .func = R_GL_DrawSelectionCircle,
            .nargs = 5,
            .args = {
                R_PushArg(&vec_AT(&s_debug_saved.xpoints, i), sizeof(vec_AT(&s_debug_saved.xpoints, 0))),
                R_PushArg(&radius, sizeof(radius)),
                R_PushArg(&width, sizeof(width)),
                R_PushArg(&green, sizeof(green)),
                (void*)G_GetPrevTickMap(),
            },
        });
    }

    char strbuff[256];
    pf_strlcpy(strbuff, "Desired Velocity in PCR:", sizeof(strbuff));
    pf_strlcat(strbuff, s_debug_saved.des_v_in_pcr ? "true" : "false", sizeof(strbuff));
    struct rgba text_color = s_debug_saved.des_v_in_pcr ? (struct rgba){255, 0, 0, 255}
                                                        : (struct rgba){0, 255, 0, 255};
    UI_DrawText(strbuff, (struct rect){5,50,200,50}, text_color);

    STFREE(apexes);
    STFREE(left_rays);
    STFREE(right_rays);
}

static bool clearpath_new_velocity(struct cp_ent cpent,
                                   vec2_t ent_des_v,
                                   const struct cp_ent *dyn_neighbs,
                                   size_t ndyn,
                                   const struct cp_ent *stat_neighbs,
                                   size_t nstat,
                                   const struct cp_terrain *terrain,
                                   int side,
                                   bool save_debug,
                                   vec2_t *out)
{
    bool status = false;
    STALLOC(struct HRVO, dyn_hrvos, ndyn);
    STALLOC(struct VO, stat_vos, nstat);

    size_t n_hrvos = compute_all_hrvos(cpent, dyn_neighbs, ndyn, side, dyn_hrvos);
    size_t n_vos = compute_all_vos(cpent, stat_neighbs, nstat, stat_vos);

    /* We may have skipped the neighbours that are at the exact same
     * or nearly same position as the entity.
     */
    assert(n_hrvos <= ndyn);
    assert(n_vos <= nstat);

    /* Following the ClearPath approach, which is applicable to many variations
     * of velocity obstacles, we represent the combined hybrid reciprocal velocity
     * obstacle as a union of line segments.
     */
    const size_t n_rays = (n_hrvos + n_vos) * 2;
    STALLOC(struct line_2d, rays, n_rays);
    rays_repr(dyn_hrvos, n_hrvos, stat_vos, n_vos, rays);

    struct rays_soa soa;
    rays_soa_repr(rays, n_rays, &soa);

    if(save_debug) {

        size_t nsaved_hrvos = n_hrvos <= MAX_SAVED_VOS ? n_hrvos : MAX_SAVED_VOS;
        memcpy(s_debug_saved.hrvos, dyn_hrvos, nsaved_hrvos * sizeof(struct HRVO));
        s_debug_saved.n_hrvos = nsaved_hrvos;

        size_t nsaved_vos = n_vos <= MAX_SAVED_VOS ? n_vos : MAX_SAVED_VOS;
        memcpy(s_debug_saved.vos, stat_vos, nsaved_vos * sizeof(struct VO));
        s_debug_saved.n_vos = nsaved_vos;

        vec_vec2_reset(&s_debug_saved.xpoints);

        s_debug_saved.cpent = cpent;
        s_debug_saved.ent_des_v = ent_des_v;
        s_debug_saved.v_new = ent_des_v;
        s_debug_saved.valid = true;
    }

    vec2_t des_v_ws = cp_add(cpent.xz_pos, ent_des_v);

    if(!s_inside_pcr(&soa, des_v_ws) && cp_landing_ok(terrain, cpent.xz_pos, des_v_ws)) {

        if(save_debug)
            s_debug_saved.des_v_in_pcr = false;
        *out = ent_des_v;
        status = true;
        goto out;
    }

    /* The line segments are intersected pairwise and the intersection points
     * inside the combined hybrid reciprocal velocity obstacle are discarded.
     * The remaining intersection points are permissible new velocities on the
     * boundary of the combined hybrid reciprocal velocity obstacle. Of those,
     * only the one closest to the preferred velocity is kept.
     */
    struct vnew_min m = {INFINITY, {0.0f, 0.0f}, false};

    const size_t n_pair = (n_rays < MAX_PAIRWISE_RAYS) ? n_rays : MAX_PAIRWISE_RAYS;
    for(size_t i = 0; i < n_pair; i++) {
    for(size_t j = i + 1; j < n_pair; j++) {

        vec2_t isec_point;
        if(!cp_ray_ray_isec(rays[i], rays[j], &isec_point))
            continue;
        if(s_inside_pcr(&soa, isec_point))
            continue;

        vnew_consider(&m, isec_point, des_v_ws, cpent.xz_pos, terrain, side);
        if(save_debug)
            vec_vec2_push(&s_debug_saved.xpoints, isec_point);
    }}

    /* In addition we project the preferred velocity (des_v) on to the line
     * segments (xz_left_side and xz_right_side of each hrvo) and also retain
     * those points that are outside the combined hybrid reciprocal velocity
     * obstacle.
     */
    for(size_t i = 0; i < n_rays; i++) {

        float len = cp_dot(rays[i].dir, ent_des_v);
        vec2_t proj = cp_add(rays[i].point, cp_scale(rays[i].dir, len));

        if(!s_inside_pcr(&soa, proj)) {
            vnew_consider(&m, proj, des_v_ws, cpent.xz_pos, terrain, side);
            if(save_debug)
                vec_vec2_push(&s_debug_saved.xpoints, proj);
        }
    }

    if(!m.any)
        goto out;

    if(save_debug) {
        s_debug_saved.v_new = m.vnew;
        s_debug_saved.des_v_in_pcr = true;
    }

    *out = m.vnew;
    status = true;

out:
    STFREE(dyn_hrvos);
    STFREE(stat_vos);
    STFREE(rays);
    return status;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

uint32_t G_ClearPath_DebugUid(void)
{
    ASSERT_IN_MAIN_THREAD();

    struct sval setting;
    ss_e status = Settings_Get("pf.debug.show_first_sel_combined_hrvo", &setting);
    assert(status == SS_OKAY);

    if(!setting.as_bool)
        return NULL_UID;

    enum selection_type seltype;
    const vec_entity_t *sel = G_Sel_Get(&seltype);

    if(vec_size(sel) == 0)
        return NULL_UID;

    return vec_AT(sel, 0);
}

void G_ClearPath_Init(const struct map *map)
{
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, (struct map*)map,
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    vec_vec2_init(&s_debug_saved.xpoints);
    if(simd_avx2_supported()) {
        s_inside_pcr = inside_pcr_avx2;
    }
}

void G_ClearPath_Shutdown(void)
{
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    vec_vec2_destroy(&s_debug_saved.xpoints);
}

vec2_t G_ClearPath_NewVelocity(struct cp_ent cpent,
                               uint32_t ent_uid,
                               vec2_t ent_des_v,
                               struct cp_ent *dyn_neighbs,
                               size_t ndyn,
                               struct cp_ent *stat_neighbs,
                               size_t nstat,
                               struct cp_terrain terrain,
                               int side,
                               bool save_debug,
                               struct cp_solve_diag *out_diag)
{
    PERF_ENTER();

    if(out_diag) {
        *out_diag = (struct cp_solve_diag){0};
    }

    int retries = 0;
    do{
        vec2_t ret;
        bool found = clearpath_new_velocity(cpent, ent_des_v,
            dyn_neighbs, ndyn, stat_neighbs, nstat, &terrain, side, save_debug, &ret);
        if(found) {
            if(out_diag) {
                out_diag->retries = retries;
                if(!same_position(ret, ent_des_v))
                    out_diag->side = cp_side_of(ent_des_v, ret);
            }
            PERF_RETURN(ret);
        }

        if(++retries > MAX_SOLVE_RETRIES)
            break;
        remove_furthest(cpent.xz_pos, dyn_neighbs, &ndyn, stat_neighbs, &nstat);

        /* remove_furthest drops from either class; retry while any remain. */
    }while(ndyn > 0 || nstat > 0);

    if(out_diag) {
        out_diag->retries = retries;
        out_diag->gave_up = true;
    }
    PERF_RETURN((vec2_t){0.0f, 0.0f});
}

