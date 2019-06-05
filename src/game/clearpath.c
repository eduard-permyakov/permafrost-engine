/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019 Eduard Permyakov 
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

#include "clearpath.h"
#include "public/game.h"
#include "movement.h"
#include "../event.h"
#include "../entity.h"
#include "../collision.h"
#include "../settings.h"
#include "../ui.h"
#include "../render/public/render.h"
#include "../map/public/map.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>


#define EPSILON         (1.0/1024)
#define MAX_SAVED_VOS   (512)

typedef kvec_t(vec2_t) vec2_vec_t;

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
    vec2_vec_t    xpoints;
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

static void compute_vo_edges(struct cp_ent ent, struct cp_ent neighb,
                             vec2_t *out_xz_right, vec2_t *out_xz_left)
{
    vec2_t ent_to_nb, right;
    PFM_Vec2_Sub(&neighb.xz_pos, &ent.xz_pos, &ent_to_nb);
    PFM_Vec2_Normal(&ent_to_nb, &ent_to_nb);

    right = (vec2_t){-ent_to_nb.raw[1], ent_to_nb.raw[0]};
    PFM_Vec2_Scale(&right, neighb.radius + ent.radius + CLEARPATH_BUFFER_RADIUS, &right);

    vec2_t right_tangent, left_tangent;
    PFM_Vec2_Add(&neighb.xz_pos, &right, &right_tangent);
    PFM_Vec2_Sub(&neighb.xz_pos, &right, &left_tangent);

    PFM_Vec2_Sub(&right_tangent, &ent.xz_pos, out_xz_right);
    PFM_Vec2_Normal(out_xz_right, out_xz_right);
    assert(fabs(PFM_Vec2_Len(out_xz_right) - 1.0f) < EPSILON);

    PFM_Vec2_Sub(&left_tangent, &ent.xz_pos, out_xz_left);
    PFM_Vec2_Normal(out_xz_left, out_xz_left);
    assert(fabs(PFM_Vec2_Len(out_xz_left) - 1.0f) < EPSILON);
}

static struct VO compute_vo(struct cp_ent ent, struct cp_ent neighb)
{
    struct VO ret;
    compute_vo_edges(ent, neighb, &ret.xz_right_side, &ret.xz_left_side);
    PFM_Vec2_Add(&ent.xz_pos, &neighb.xz_vel, &ret.xz_apex);
    return ret;
}

static struct RVO compute_rvo(struct cp_ent ent, struct cp_ent neighb)
{
    struct RVO ret;
    compute_vo_edges(ent, neighb, &ret.xz_right_side, &ret.xz_left_side);

    vec2_t apex_off;
    PFM_Vec2_Add(&ent.xz_vel, &neighb.xz_vel, &apex_off);
    PFM_Vec2_Scale(&apex_off, 0.5f, &apex_off);
    PFM_Vec2_Add(&ent.xz_pos, &apex_off, &ret.xz_apex);

    return ret;
}

static struct HRVO compute_hrvo(struct cp_ent ent, struct cp_ent neighb)
{
    struct HRVO ret;
    struct RVO rvo = compute_rvo(ent, neighb);
    struct line_2d l1, l2;
    vec2_t intersec_point;

    vec2_t centerline;
    PFM_Vec2_Add(&rvo.xz_left_side, &rvo.xz_right_side, &centerline);

    vec2_t vo_apex;
    PFM_Vec2_Add(&ent.xz_pos, &neighb.xz_vel, &vo_apex);

    float det = (centerline.x * ent.xz_vel.y) - (centerline.y * ent.xz_vel.x);
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

static size_t compute_all_vos(struct cp_ent ent, cp_ent_vec_t stat_neighbs, 
                              struct VO *out)
{
    size_t ret = 0; 

    for(struct cp_ent *nb = stat_neighbs.a; 
        nb < stat_neighbs.a + kv_size(stat_neighbs); nb++) {
        
        out[ret++] = compute_vo(ent, *nb);
    }

    return ret;
}

static size_t compute_all_hrvos(struct cp_ent ent, cp_ent_vec_t dyn_neighbs, 
                                struct HRVO *out)
{
    size_t ret = 0; 

    for(struct cp_ent *nb = dyn_neighbs.a; 
        nb < dyn_neighbs.a + kv_size(dyn_neighbs); nb++) {
        
        out[ret++] = compute_hrvo(ent, *nb);
    }

    return ret;
}

/* Points exactly 'on' the boundary will be considered as 'not inside' of the PCR for our purposes. */
static bool inside_pcr(const struct line_2d *vo_lr_pairs, size_t n_rays, vec2_t test)
{
    const float test_point_x = test.raw[0];
    const float test_point_z = test.raw[1];

    const float test_norm_x = test_point_x / PFM_Vec2_Len(&test);
    const float test_norm_z = test_point_z / PFM_Vec2_Len(&test);

    assert(n_rays % 2 == 0);
    for(int i = 0; i < n_rays; i+=2) {

        assert(fabs(PFM_Vec2_Len(&vo_lr_pairs[i + 0].dir) - 1.0f) < EPSILON);
        const float left_dir_x = vo_lr_pairs[i + 0].dir.raw[0];
        const float left_dir_z = vo_lr_pairs[i + 0].dir.raw[1];

        vec2_t point_to_test;
        PFM_Vec2_Sub(&test, (vec2_t*)&vo_lr_pairs[i + 0].point, &point_to_test);
        PFM_Vec2_Normal(&point_to_test, &point_to_test);

        float left_det = (point_to_test.raw[1] * left_dir_x) - (point_to_test.raw[0] * left_dir_z);
        bool left_of_vo = (left_det < EPSILON);

        if(left_of_vo)
            continue;

        assert(fabs(PFM_Vec2_Len(&vo_lr_pairs[i + 1].dir) - 1.0f) < EPSILON);
        const float right_dir_x = vo_lr_pairs[i + 1].dir.raw[0];
        const float right_dir_z = vo_lr_pairs[i + 1].dir.raw[1];

        PFM_Vec2_Sub(&test, (vec2_t*)&vo_lr_pairs[i + 1].point, &point_to_test);
        PFM_Vec2_Normal(&point_to_test, &point_to_test);

        float right_det = (point_to_test.raw[1] * right_dir_x) - (point_to_test.raw[0] * right_dir_z);
        bool right_of_vo = (right_det > -EPSILON);

        if(right_of_vo)
            continue;

        assert(!left_of_vo && !right_of_vo);
        return true;
    }

    return false;
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

static size_t compute_vo_xpoints(struct line_2d *rays, size_t n_rays,
                                 vec2_vec_t *inout)
{
    size_t ret = 0;

    for(int i = 0; i < n_rays; i++) {
        for(int j = 0; j < n_rays; j++) {

            if(i == j) 
                continue;

            vec2_t isec_point, candidate_vel;
            if(!C_RayRayIntersection2D(rays[i], rays[j], &isec_point))
                continue;

            if(inside_pcr(rays, n_rays, isec_point))
                continue;

            kv_push(vec2_t, *inout, isec_point);
            ret++;
        }
    }

    return ret;
}

static size_t compute_vdes_proj_points(struct line_2d *rays, size_t n_rays,
                                       vec2_t des_v, vec2_vec_t *inout)
{
    vec2_t proj;
    size_t ret = 0;

    for(int i = 0; i < n_rays; i++) {
    
        assert(fabs(PFM_Vec2_Len(&rays[i].dir) - 1.0f) < EPSILON);

        float len = PFM_Vec2_Dot(&rays[i].dir, &des_v);
        PFM_Vec2_Scale(&rays[i].dir, len, &proj);
        PFM_Vec2_Add(&rays[i].point, &proj, &proj);

        if(!inside_pcr(rays, n_rays, proj)) {
        
            kv_push(vec2_t, *inout, proj);
            ret++;
        }
    }

    return ret;
}

static vec2_t compute_vnew(const vec2_vec_t *outside_points, vec2_t des_v, vec2_t ent_xz_pos)
{
    float min_dist = INFINITY, len;
    vec2_t ret;

    for(int i = 0; i < kv_size(*outside_points); i++) {

        /* The points are in worldspace coordinates. Convert them to the entity's 
         * local space to get the adimissible velocities. */
        vec2_t curr = kv_A(*outside_points, i), diff;
        PFM_Vec2_Sub(&curr, &ent_xz_pos, &curr);

        PFM_Vec2_Sub(&des_v, &curr, &diff);
        if((len = PFM_Vec2_Len(&diff)) < min_dist) {

            min_dist = len;
            ret = curr;
        }
    }

    assert(min_dist < INFINITY);
    return ret;
}

static void remove_furthest(vec2_t xz_pos, cp_ent_vec_t *dyn_inout, cp_ent_vec_t *stat_inout)
{
    float max_dist = -INFINITY;
    cp_ent_vec_t *del_vec = NULL;
    int del_idx;

    for(int i = 0; i < 2; i++) {
    
        cp_ent_vec_t *curr_vec = (i == 0) ? dyn_inout : stat_inout;
        for(int j = 0; j < kv_size(*curr_vec); j++) {
        
            float len;
            vec2_t diff;
            struct cp_ent *ent = &kv_A(*curr_vec, j);

            PFM_Vec2_Sub(&xz_pos, &ent->xz_pos, &diff);
            if((len = PFM_Vec2_Len(&diff)) > max_dist) {
                max_dist = len; 
                del_vec = curr_vec;
                del_idx = j;
            }
        }
    }

    if(max_dist > -INFINITY) {
        kv_del(vec2_t, *del_vec, del_idx);
    }
}

/* Save the combined HRVO of the first selected entity for debug rendering */
static bool should_save_debug(uint32_t ent_uid)
{
    struct sval setting;
    ss_e status = Settings_Get("pf.debug.show_first_sel_combined_hrvo", &setting);
    assert(status == SS_OKAY);

    if(!setting.as_bool)
        return false;

    enum selection_type seltype;
    const pentity_kvec_t *sel = G_Sel_Get(&seltype);

    if(kv_size(*sel) == 0)
        return false; 

    return true;
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

    vec2_t apexes[n_vos];
    vec2_t left_rays[n_vos];
    vec2_t right_rays[n_vos];

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
    R_GL_DrawCombinedHRVO(apexes, left_rays, right_rays, n_vos, map);
    R_GL_DrawSelectionCircle(cpent->xz_pos, CLEARPATH_NEIGHBOUR_RADIUS, 0.5f, yellow, map);

    mat4x4_t ident;
    PFM_Mat4x4_Identity(&ident);

    vec3_t origin_pos = (vec3_t){
        cpent->xz_pos.raw[0], 
        M_HeightAtPoint(map, cpent->xz_pos) + 5.0f, 
        cpent->xz_pos.raw[1]
    };

    vec2_t des_v = s_debug_saved.ent_des_v;
    vec3_t des_vel_dir = (vec3_t){des_v.raw[0], 0.0f, des_v.raw[1]};
    PFM_Vec3_Normal(&des_vel_dir, &des_vel_dir);
    R_GL_DrawRay(origin_pos, des_vel_dir, &ident, blue, PFM_Vec2_Len(&des_v) * MOVE_TICK_RES);

    vec2_t v_new = s_debug_saved.v_new;
    vec3_t vel_dir = (vec3_t){v_new.raw[0], 0.0f, v_new.raw[1]};
    PFM_Vec3_Normal(&vel_dir, &vel_dir);
    R_GL_DrawRay(origin_pos, vel_dir, &ident, green, PFM_Vec2_Len(&v_new) * MOVE_TICK_RES);

    for(int i = 0; i < kv_size(s_debug_saved.xpoints); i++) {
        vec2_t xp = kv_A(s_debug_saved.xpoints, i);
        R_GL_DrawSelectionCircle(kv_A(s_debug_saved.xpoints, i), 1.0f, 1.0f, green, map);
    }

    char strbuff[256];
    strcpy(strbuff, "Desired Velocity in PCR:");
    strcat(strbuff, s_debug_saved.des_v_in_pcr ? "true" : "false");
    struct rgba text_color = s_debug_saved.des_v_in_pcr ? (struct rgba){255, 0, 0, 255}
                                                        : (struct rgba){0, 255, 0, 255};
    UI_DrawText(strbuff, (struct rect){5,5,200,50}, text_color);
}

static bool clearpath_new_velocity(struct cp_ent cpent,
                                   uint32_t ent_uid,
                                   vec2_t ent_des_v,
                                   const cp_ent_vec_t dyn_neighbs,
                                   const cp_ent_vec_t stat_neighbs,
                                   vec2_t *out)
{
    struct HRVO dyn_hrvos[kv_size(dyn_neighbs)];
    struct VO stat_vos[kv_size(stat_neighbs)];

    size_t n_hrvos = compute_all_hrvos(cpent, dyn_neighbs, dyn_hrvos);
    size_t n_vos = compute_all_vos(cpent, stat_neighbs, stat_vos);

    assert(n_hrvos == kv_size(dyn_neighbs));
    assert(n_vos == kv_size(stat_neighbs));

    /* Following the ClearPath approach, which is applicable to many variations 
     * of velocity obstacles, we represent the combined hybrid reciprocal velocity 
     * obstacle as a union of line segments. 
     */
    const size_t n_rays = (n_hrvos + n_vos) * 2;
    struct line_2d rays[n_rays];
    rays_repr(dyn_hrvos, n_hrvos, stat_vos, n_vos, rays);

    if(should_save_debug(ent_uid)) {

        size_t nsaved_hrvos = n_hrvos <= MAX_SAVED_VOS ? n_hrvos : MAX_SAVED_VOS;
        memcpy(s_debug_saved.hrvos, dyn_hrvos, nsaved_hrvos * sizeof(struct HRVO));
        s_debug_saved.n_hrvos = nsaved_hrvos;

        size_t nsaved_vos = n_vos <= MAX_SAVED_VOS ? n_vos : MAX_SAVED_VOS;
        memcpy(s_debug_saved.vos, stat_vos, nsaved_vos * sizeof(struct VO));
        s_debug_saved.n_vos = nsaved_vos;

        kv_reset(s_debug_saved.xpoints);

        s_debug_saved.cpent = cpent;
        s_debug_saved.ent_des_v = ent_des_v;
        s_debug_saved.v_new = ent_des_v;
        s_debug_saved.valid = true;
    }

    vec2_t des_v_ws;
    PFM_Vec2_Add(&cpent.xz_pos, &ent_des_v, &des_v_ws);
    if(!inside_pcr(rays, n_rays, des_v_ws)) {

        s_debug_saved.des_v_in_pcr = false;
        *out = ent_des_v;
        return true;
    }

    vec2_vec_t xpoints;
    kv_init(xpoints);

    /* The line segments are intersected pairwise and the intersection points 
     * inside the combined hybrid reciprocal velocity obstacle are discarded. 
     * The remaining intersection points are permissible new velocities on the 
     * boundary of the combined hybrid reciprocal velocity obstacle.
     */
    compute_vo_xpoints(rays, n_rays, &xpoints); 

    /* In addition we project the preferred velocity (des_v) on to the line 
     * segments (xz_left_side and xz_right_side of each hrvo) and also retain 
     * those points that are outside the combined hybrid reciprocal velocity 
     * obstacle.
     */
    compute_vdes_proj_points(rays, n_rays, ent_des_v, &xpoints);

    if(kv_size(xpoints) == 0) {
        return false;    
    }

    vec2_t ret = compute_vnew(&xpoints, ent_des_v, cpent.xz_pos);

    if(should_save_debug(ent_uid)) {
    
        kv_copy(vec2_t, s_debug_saved.xpoints, xpoints);
        s_debug_saved.v_new = ret;
        s_debug_saved.des_v_in_pcr = true;
    }

    kv_destroy(xpoints);
    *out = ret;
    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void G_ClearPath_Init(const struct map *map)
{
    E_Global_Register(EVENT_RENDER_3D, on_render_3d, (struct map*)map, 
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    kv_init(s_debug_saved.xpoints);
}

void G_ClearPath_Shutdown(void)
{
    E_Global_Unregister(EVENT_RENDER_3D, on_render_3d);
    kv_destroy(s_debug_saved.xpoints);
}

vec2_t G_ClearPath_NewVelocity(struct cp_ent cpent,
                               uint32_t ent_uid,
                               vec2_t ent_des_v,
                               cp_ent_vec_t dyn_neighbs,
                               cp_ent_vec_t stat_neighbs)
{
    do{
        vec2_t ret;
        bool found = clearpath_new_velocity(cpent, ent_uid, ent_des_v, dyn_neighbs, stat_neighbs, &ret);
        if(found)
            return ret;

        remove_furthest(cpent.xz_pos, &dyn_neighbs, &stat_neighbs);

    }while(kv_size(dyn_neighbs) > 0 && kv_size(stat_neighbs) > 0);

    return (vec2_t){0.0f, 0.0f};
}

