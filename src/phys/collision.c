/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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

#include "public/collision.h"
#include <assert.h>
#include <float.h>

#define MIN(a, b)     ((a) < (b) ? (a) : (b))
#define MAX(a, b)     ((a) > (b) ? (a) : (b))
#define MAX3(a, b, c) (MAX(MAX(a, b), c))
#define ARR_SIZE(a)   (sizeof(a)/sizeof(a[0]))

#define SWAP(type, a, b) \
    do{ \
        type tmp = a; \
        a = b; \
        b = tmp; \
    }while(0)

#define EPSILON (1.0f/1024.0f)


struct range{
    float begin, end;
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/*
 * Based on the algorithm outlined here:
 * https://www.scratchapixel.com/lessons/3d-basic-rendering/ray-tracing-rendering-a-triangle/ray-triangle-intersection-geometric-solution
 */
static bool ray_triangle_intersect(vec3_t ray_origin, vec3_t ray_dir, vec3_t tri_verts[3], float *out_t)
{
    /* Compute plane's normal */
    vec3_t v0v1;
    vec3_t v0v2;

    PFM_Vec3_Sub(&tri_verts[1], &tri_verts[0], &v0v1);
    PFM_Vec3_Sub(&tri_verts[2], &tri_verts[0], &v0v2);

    vec3_t n;
    PFM_Vec3_Cross(&v0v1, &v0v2, &n);

    float n_dot_ray_dir = PFM_Vec3_Dot(&n, &ray_dir);
    if(fabs(n_dot_ray_dir) < EPSILON) {
        /* Ray is parallel to the plane of the triangle */ 
        return false;
    }

    float d = PFM_Vec3_Dot(&n, &tri_verts[0]);
    float t = (d - PFM_Vec3_Dot(&n, &ray_origin)) / n_dot_ray_dir;

    if(t < 0.0f) {
        /* Triangle is behind the ray */ 
        return false;
    }

    /* P is the point of intersection of the ray
     * and the plane of the triangle. */
    vec3_t P;
    vec3_t delta;

    PFM_Vec3_Scale(&ray_dir, t, &delta);
    PFM_Vec3_Add(&ray_origin, &delta, &P);

    /* Lastly, perform the inside-outside test. */

    vec3_t C;

    vec3_t edge0, edge1, edge2;
    vec3_t vp0, vp1, vp2;

    PFM_Vec3_Sub(&tri_verts[1], &tri_verts[0], &edge0);
    PFM_Vec3_Sub(&P, &tri_verts[0], &vp0);
    PFM_Vec3_Cross(&edge0, &vp0, &C);

    if(PFM_Vec3_Dot(&n, &C) < 0.0f) {
        return false;
    }

    PFM_Vec3_Sub(&tri_verts[2], &tri_verts[1], &edge1);
    PFM_Vec3_Sub(&P, &tri_verts[1], &vp1);
    PFM_Vec3_Cross(&edge1, &vp1, &C);

    if(PFM_Vec3_Dot(&n, &C) < 0.0f) {
        return false;
    }

    PFM_Vec3_Sub(&tri_verts[0], &tri_verts[2], &edge2);
    PFM_Vec3_Sub(&P, &tri_verts[2], &vp2);
    PFM_Vec3_Cross(&edge2, &vp2, &C);

    if(PFM_Vec3_Dot(&n, &C) < 0.0f) {
        return false;
    }

    *out_t = t;
    return true;
}

static float plane_point_signed_distance(const struct plane *plane, vec3_t point)
{
    vec3_t diff;
    PFM_Vec3_Sub(&point, (vec3_t*)&plane->point, &diff);
    return PFM_Vec3_Dot(&diff, (vec3_t*)&plane->normal);
}

static float arr_min(float *array, size_t size)
{
    assert(size > 0);

    float min = array[0];
    for(int i = 1; i < size; i++) {
            
        if(array[i] < min)
           min = array[i]; 
    }

    return min;
}

static float arr_max(float *array, size_t size)
{
    assert(size > 0);

    float max = array[0];
    for(int i = 1; i < size; i++) {
            
        if(array[i] > max)
           max = array[i]; 
    }

    return max;
}

static bool ranges_overlap(struct range *a, struct range *b)
{
    float imin = MAX(a->begin, b->begin);
    float imax = MIN(a->end, b->end);
    if(imax < imin) {
        return false;
    }else{
        return true;
    }
}

static bool point_in_range(float point, struct range *r)
{
    return (point >= r->begin && point <= r->end);
}

static bool separating_axis_exists(vec3_t axis, const struct frustum *frustum, const vec3_t cuboid_corners[8])
{
    struct range frust_range, cuboid_range;

    float frust_axis_dots[8];
    float cuboid_axis_dots[8];

    const vec3_t *frust_points[8] = {&frustum->ntl, &frustum->ntr, &frustum->nbl, &frustum->nbr,
                                     &frustum->ftl, &frustum->ftr, &frustum->fbl, &frustum->fbr};

    for(int i = 0; i < 8; i++)
        frust_axis_dots[i] = PFM_Vec3_Dot((vec3_t*)frust_points[i], &axis);

    for(int i = 0; i < 8; i++)
        cuboid_axis_dots[i] = PFM_Vec3_Dot((vec3_t*)&cuboid_corners[i], &axis);

    frust_range = (struct range){arr_min(frust_axis_dots, 8), arr_max(frust_axis_dots, 8)}; 
    cuboid_range  = (struct range){arr_min(cuboid_axis_dots, 8),  arr_max(cuboid_axis_dots, 8)};

    return !ranges_overlap(&frust_range, &cuboid_range);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

/* Useful information about frustums here:
 * http://cgvr.informatik.uni-bremen.de/teaching/cg_literatur/lighthouse3d_view_frustum_culling/index.html
 * Note that our engine's coordinate system is left-handed.
 */
void C_MakeFrustum(vec3_t pos, vec3_t up, vec3_t front, 
                   GLfloat aspect_ratio, GLfloat fov_rad, 
                   GLfloat near_dist, GLfloat far_dist,
                   struct frustum *out)
{
    const float near_height = 2 * tan(fov_rad/2.0f) * near_dist;
    const float near_width = near_height * aspect_ratio;

    const float far_height = 2 * tan(fov_rad/2.0f) * far_dist;
    const float far_width = far_height * aspect_ratio;

    vec3_t tmp;
    vec3_t cam_right;
    PFM_Vec3_Cross(&up, (vec3_t*)&front, &cam_right);
    PFM_Vec3_Normal(&cam_right, &cam_right);

    vec3_t nc = pos;
    PFM_Vec3_Scale(&front, near_dist, &tmp);
    PFM_Vec3_Add(&nc, &tmp, &nc);

    vec3_t fc = pos;
    PFM_Vec3_Scale(&front, far_dist, &tmp);
    PFM_Vec3_Add(&nc, &tmp, &fc);

    vec3_t up_half_hfar;
    PFM_Vec3_Scale(&up, far_height/2.0f, &up_half_hfar);

    vec3_t right_half_wfar;
    PFM_Vec3_Scale(&cam_right, far_width/2.0f, &right_half_wfar);

    vec3_t up_half_hnear;
    PFM_Vec3_Scale(&up, near_height/2.0f, &up_half_hnear);

    vec3_t right_half_wnear;
    PFM_Vec3_Scale(&cam_right, near_width/2.0f, &right_half_wnear);


    /* Far Top Left corner */
    PFM_Vec3_Add(&fc, &up_half_hfar, &tmp);
    PFM_Vec3_Sub(&tmp, &right_half_wfar, &out->ftl);

    /* Far Top Right corner */
    PFM_Vec3_Add(&fc, &up_half_hfar, &tmp);
    PFM_Vec3_Add(&tmp, &right_half_wfar, &out->ftr);

    /* Far Bottom Left corner */
    PFM_Vec3_Sub(&fc, &up_half_hfar, &tmp);
    PFM_Vec3_Sub(&tmp, &right_half_wfar, &out->fbl);

    /* Far Bottom Right corner */
    PFM_Vec3_Sub(&fc, &up_half_hfar, &tmp);
    PFM_Vec3_Add(&tmp, &right_half_wfar, &out->fbr);

    /* Near Top Left corner */
    PFM_Vec3_Add(&nc, &up_half_hnear, &tmp);
    PFM_Vec3_Sub(&tmp, &right_half_wnear, &out->ntl);

    /* Near Top Right corner */
    PFM_Vec3_Add(&nc, &up_half_hnear, &tmp);
    PFM_Vec3_Add(&tmp, &right_half_wnear, &out->ntr);

    /* Near Bottom Left corner */
    PFM_Vec3_Sub(&nc, &up_half_hnear, &tmp); 
    PFM_Vec3_Sub(&tmp, &right_half_wnear, &out->nbl);

    /* Near Bottom right corner */
    PFM_Vec3_Sub(&nc, &up_half_hnear, &tmp);
    PFM_Vec3_Add(&tmp, &right_half_wnear, &out->nbr);


    /* Near plane */
    out->nearp.point = nc;
    out->nearp.normal = front;

    /* Far plane */
    vec3_t negative_dir;
    PFM_Vec3_Scale(&front, -1.0f, &negative_dir);

    out->farp.point = fc;
    out->farp.normal = negative_dir;

    /* Right plane */
    vec3_t p_to_near_right_edge;
    PFM_Vec3_Scale(&cam_right, near_width / 2.0f, &tmp);
    PFM_Vec3_Add(&nc, &tmp, &tmp);
    PFM_Vec3_Sub(&tmp, &pos, &p_to_near_right_edge);
    PFM_Vec3_Normal(&p_to_near_right_edge, &p_to_near_right_edge);

    out->right.point = pos;
    PFM_Vec3_Cross(&p_to_near_right_edge, &up, &out->right.normal);

    /* Left plane */
    vec3_t p_to_near_left_edge;
    PFM_Vec3_Scale(&cam_right, near_width / 2.0f, &tmp);
    PFM_Vec3_Sub(&nc, &tmp, &tmp);
    PFM_Vec3_Sub(&tmp, &pos, &p_to_near_left_edge);
    PFM_Vec3_Normal(&p_to_near_left_edge, &p_to_near_left_edge);

    out->left.point = pos;
    PFM_Vec3_Cross(&up, &p_to_near_left_edge, &out->left.normal);

    /* Top plane */
    vec3_t p_to_near_top_edge;
    PFM_Vec3_Scale(&up, near_height / 2.0f, &tmp);
    PFM_Vec3_Add(&nc, &tmp, &tmp);
    PFM_Vec3_Sub(&tmp, &pos, &p_to_near_top_edge);
    PFM_Vec3_Normal(&p_to_near_top_edge, &p_to_near_top_edge);

    out->top.point = pos;
    PFM_Vec3_Cross(&cam_right, &p_to_near_top_edge, &out->top.normal);

    /* Bot plane */
    vec3_t p_to_near_bot_edge;
    PFM_Vec3_Scale(&up, near_height / 2.0f, &tmp);
    PFM_Vec3_Sub(&nc, &tmp, &tmp);
    PFM_Vec3_Sub(&tmp, &pos, &p_to_near_bot_edge);
    PFM_Vec3_Normal(&p_to_near_bot_edge, &p_to_near_bot_edge);

    out->bot.point = pos;
    PFM_Vec3_Cross(&p_to_near_bot_edge, &cam_right, &out->bot.normal);
}

bool C_RayIntersectsAABB(vec3_t ray_origin, vec3_t ray_dir, struct aabb aabb, float *out_t)
{
     float t1 = (aabb.x_min - ray_origin.x) / ray_dir.x;
     float t2 = (aabb.x_max - ray_origin.x) / ray_dir.x;
     float t3 = (aabb.y_min - ray_origin.y) / ray_dir.y;
     float t4 = (aabb.y_max - ray_origin.y) / ray_dir.y;
     float t5 = (aabb.z_min - ray_origin.z) / ray_dir.z;
     float t6 = (aabb.z_max - ray_origin.z) / ray_dir.z;
    
     float tmin = MAX(MAX(MIN(t1, t2), MIN(t3, t4)), MIN(t5, t6));
     float tmax = MIN(MIN(MAX(t1, t2), MAX(t3, t4)), MAX(t5, t6));
    
     /* Ray (line) is intersecting AABB, but the whole AABB is behind us */
     if (tmax < 0) {
        return false;
     }
    
     /* Ray doesn't intersect AABB */
     if (tmin > tmax) {
        return false;
     }
   
     *out_t = tmin;
     return true;
}

bool C_RayIntersectsOBB(vec3_t ray_origin, vec3_t ray_dir, struct obb obb, float *out_t)
{
    float tmin = 0;
    float tmax = FLT_MAX;

    vec3_t d;
    PFM_Vec3_Sub(&obb.center, &ray_origin, &d);

    for (int i = 0; i < 3; ++i) {

        GLfloat DA = PFM_Vec3_Dot(&ray_dir, &obb.axes[i]); 
        GLfloat dA = PFM_Vec3_Dot(&d, &obb.axes[i]);

        if(fabs(DA) < EPSILON) {

            /* Ray is parallel to the slabs - check that the ray origin is inside the slab. */
            if(fabs(dA - tmin * DA) > obb.half_lengths[i] || fabs(dA - tmax * DA) > obb.half_lengths[i])
                return false;
        }else {
            /* Otherwise find the entry and exit points into the slab. For there to be intersection, the 
             * time range between entry and exit must overlap with the previously found time range. */

            GLfloat es = (DA > 0.0) ? obb.half_lengths[i] : -obb.half_lengths[i];
            GLfloat invDA = 1.0 / DA;

            GLfloat t1 = (dA - es) * invDA;
            GLfloat t2 = (dA + es) * invDA;

            if(t1 > tmin) tmin = t1;
            if(t2 < tmax) tmax = t2;
            if(tmin > tmax) return false;
        }
    }
    
    *out_t = tmin;
    return true;
}

bool C_RayIntersectsTriMesh(vec3_t ray_origin, vec3_t ray_dir, vec3_t *tribuff, size_t n, float *out_t)
{
    float tmin = FLT_MAX;
    bool intersec = false;

    assert(n % 3 == 0);
    for(int i = 0; i < n; i += 3) {
        
        float t;
        if(ray_triangle_intersect(ray_origin, ray_dir, tribuff + i, &t)) {
            intersec = true;
            tmin = MIN(tmin, t);
        }
    }

    *out_t = tmin;
    return intersec;
}

bool C_RayIntersectsPlane(vec3_t ray_origin, vec3_t ray_dir, struct plane plane, float *out_t)
{
    float denom = PFM_Vec3_Dot(&ray_dir, &plane.normal);
    if(fabs(denom) > EPSILON) {

        vec3_t rp; 
        PFM_Vec3_Sub(&plane.point, &ray_origin, &rp);
        float t = PFM_Vec3_Dot(&rp, &plane.normal) / denom;
        if(t >= 0.0f) {
            *out_t = t;
            return true;
        }
    }

    return false;
}

bool C_PointInsideOBB(vec3_t point, struct obb obb)
{
    /* Project the point (relative to the OBB origin) onto each of the 
     * three OBB axes and check that it is within half length range on 
     * either side */
    for(int i = 0; i < 3; i++) {

        vec3_t axis = obb.axes[i];
        float halflen = obb.half_lengths[i];

        vec3_t relative;
        PFM_Vec3_Sub(&point, &obb.center, &relative);

        float dot = PFM_Vec3_Dot(&relative, &axis);
        float comp = dot / PFM_Vec3_Len(&axis);

        if(fabs(comp) > halflen)
            return false;
    }
    return true;
}

bool C_LineSegIntersectsOBB(vec3_t begin, vec3_t end, struct obb obb)
{
    vec3_t delta;
    PFM_Vec3_Sub(&begin, &end, &delta);

    if(PFM_Vec3_Len(&delta) < EPSILON)
        return C_PointInsideOBB(begin, obb);

    vec3_t dir;
    PFM_Vec3_Normal(&delta, &dir);
    float t;

    if(!C_RayIntersectsOBB(begin, dir, obb, &t))
        return false;

    return (t >= 0 && t <= PFM_Vec3_Len(&delta));
}

enum volume_intersec_type C_FrustrumPointIntersectionFast(const struct frustum *frustum, vec3_t point)
{
    const struct plane *planes[] = {&frustum->top, &frustum->bot, &frustum->left, 
                                    &frustum->right, &frustum->nearp, &frustum->farp};

    for(int i = 0; i < ARR_SIZE(planes); i++) {
   
        if(plane_point_signed_distance(planes[i], point) < 0.0f)
            return VOLUME_INTERSEC_OUTSIDE;
    }

    return VOLUME_INTERSEC_INSIDE;
}

/* Based on the algorithm outlined here:
 * http://cgvr.informatik.uni-bremen.de/teaching/cg_literatur/lighthouse3d_view_frustum_culling/index.html
 */
enum volume_intersec_type C_FrustumAABBIntersectionFast(const struct frustum *frustum, const struct aabb *aabb)
{
    const struct plane *planes[] = {&frustum->top, &frustum->bot, &frustum->left, 
                                    &frustum->right, &frustum->nearp, &frustum->farp};

    const vec3_t corners[8] = {
        (vec3_t){aabb->x_min, aabb->y_min, aabb->z_min},
        (vec3_t){aabb->x_min, aabb->y_min, aabb->z_max},
        (vec3_t){aabb->x_min, aabb->y_max, aabb->z_min},
        (vec3_t){aabb->x_min, aabb->y_max, aabb->z_max},
        (vec3_t){aabb->x_max, aabb->y_min, aabb->z_min},
        (vec3_t){aabb->x_max, aabb->y_min, aabb->z_max},
        (vec3_t){aabb->x_max, aabb->y_max, aabb->z_min},
        (vec3_t){aabb->x_max, aabb->y_max, aabb->z_max},
    };

    for(int i = 0; i < ARR_SIZE(planes); i++) {

        int corners_in = 0, corners_out = 0;

        /* Break as soon as we know the box has corners both inside and outside 
         * the frustum */
        for(int k = 0; k < 8 && (corners_in == 0 || corners_out == 0); k++) {
        
            if(plane_point_signed_distance(planes[i], corners[k]) < 0.0f)
                corners_out++;
            else
                corners_in++;
        }

        /* All corners are outside */
        if(corners_in == 0)
            return VOLUME_INTERSEC_OUTSIDE;
        else if(corners_out > 0)
            return VOLUME_INTERSEC_INTERSECTION;
    }

    return VOLUME_INTERSEC_INSIDE;
}

enum volume_intersec_type C_FrustumOBBIntersectionFast(const struct frustum *frustum, const struct obb *obb)
{
    const struct plane *planes[] = {&frustum->top, &frustum->bot, &frustum->left, 
                                    &frustum->right, &frustum->nearp, &frustum->farp};

    for(int i = 0; i < ARR_SIZE(planes); i++) {

        int corners_in = 0, corners_out = 0;

        /* Break as soon as we know the box has corners both inside and outside 
         * the frustum */
        for(int k = 0; k < 8 && (corners_in == 0 || corners_out == 0); k++) {
        
            if(plane_point_signed_distance(planes[i], obb->corners[k]) < 0.0f)
                corners_out++;
            else
                corners_in++;
        }

        /* All corners are outside */
        if(corners_in == 0)
            return VOLUME_INTERSEC_OUTSIDE;
        else if(corners_out > 0)
            return VOLUME_INTERSEC_INTERSECTION;
    }

    return VOLUME_INTERSEC_INSIDE;
}

bool C_FrustumAABBIntersectionExact(const struct frustum *frustum, const struct aabb *aabb)
{
    vec3_t aabb_axes[3] = {
        (vec3_t){1.0f, 0.0f, 0.0f}, 
        (vec3_t){0.0f, 1.0f, 0.0f}, 
        (vec3_t){0.0f, 0.0f, 1.0f}
    };

    vec3_t aabb_corners[8] = {
        (vec3_t){aabb->x_min, aabb->y_min, aabb->z_min},
        (vec3_t){aabb->x_min, aabb->y_min, aabb->z_max},
        (vec3_t){aabb->x_min, aabb->y_max, aabb->z_min},
        (vec3_t){aabb->x_min, aabb->y_max, aabb->z_max},
        (vec3_t){aabb->x_max, aabb->y_min, aabb->z_min},
        (vec3_t){aabb->x_max, aabb->y_min, aabb->z_max},
        (vec3_t){aabb->x_max, aabb->y_max, aabb->z_min},
        (vec3_t){aabb->x_max, aabb->y_max, aabb->z_max},
    };

    for(int i = 0; i < ARR_SIZE(aabb_axes); i++) {
    
        if(separating_axis_exists(aabb_axes[i], frustum, aabb_corners))
            return false;
    }

    vec3_t frust_axes[6] = {
        frustum->nearp.normal,
        frustum->farp.normal,
        frustum->top.normal,
        frustum->bot.normal,
        frustum->left.normal,
        frustum->right.normal
    };

    for(int i = 0; i < ARR_SIZE(frust_axes); i++) {
    
        if(separating_axis_exists(frust_axes[i], frustum, aabb_corners)) 
            return false;
    }

    vec3_t frust_edges[6];
    PFM_Vec3_Sub((vec3_t*)&frustum->ntr, (vec3_t*)&frustum->ntl, &frust_edges[0]);
    PFM_Vec3_Sub((vec3_t*)&frustum->ntl, (vec3_t*)&frustum->nbl, &frust_edges[1]);
    PFM_Vec3_Sub((vec3_t*)&frustum->ftl, (vec3_t*)&frustum->ntl, &frust_edges[2]);
    PFM_Vec3_Sub((vec3_t*)&frustum->ftr, (vec3_t*)&frustum->ntr, &frust_edges[3]);
    PFM_Vec3_Sub((vec3_t*)&frustum->fbr, (vec3_t*)&frustum->nbr, &frust_edges[4]);
    PFM_Vec3_Sub((vec3_t*)&frustum->fbl, (vec3_t*)&frustum->nbl, &frust_edges[5]);

    /* For AABBs, edge axes and normals are the same */
    size_t ncrosses = 0;
    vec3_t edge_cross_products[ARR_SIZE(aabb_axes) * ARR_SIZE(frust_edges)];

    for(int i = 0; i < ARR_SIZE(aabb_axes); i++) {
        for(int j = 0; j < ARR_SIZE(frust_edges); j++) {

            PFM_Vec3_Cross(&aabb_axes[i], &frust_edges[j], &edge_cross_products[ncrosses]);
            if(PFM_Vec3_Len(&edge_cross_products[ncrosses]) > EPSILON) {
                PFM_Vec3_Normal(&edge_cross_products[ncrosses], &edge_cross_products[ncrosses]);
                ncrosses++;
            }
        }
    }

    for(int i = 0; i < ncrosses; i++) {
    
        if(separating_axis_exists(edge_cross_products[i], frustum, aabb_corners)) {
            return false;
        }
    }

    return true;
}

bool C_FrustumOBBIntersectionExact(const struct frustum *frustum, const struct obb *obb)
{
    for(int i = 0; i < ARR_SIZE(obb->axes); i++) {
    
        if(separating_axis_exists(obb->axes[i], frustum, obb->corners))
            return false;
    }

    /* Near and far planes assumed to be parallel */
    vec3_t frust_normals[5] = {
        frustum->farp.normal,
        frustum->top.normal,
        frustum->bot.normal,
        frustum->left.normal,
        frustum->right.normal
    };

    for(int i = 0; i < ARR_SIZE(frust_normals); i++) {
    
        if(separating_axis_exists(frust_normals[i], frustum, obb->corners))
            return false;
    }

    vec3_t frust_edges[6];
    PFM_Vec3_Sub((vec3_t*)&frustum->ntr, (vec3_t*)&frustum->ntl, &frust_edges[0]);
    PFM_Vec3_Sub((vec3_t*)&frustum->ntl, (vec3_t*)&frustum->nbl, &frust_edges[1]);
    PFM_Vec3_Sub((vec3_t*)&frustum->ftl, (vec3_t*)&frustum->ntl, &frust_edges[2]);
    PFM_Vec3_Sub((vec3_t*)&frustum->ftr, (vec3_t*)&frustum->ntr, &frust_edges[3]);
    PFM_Vec3_Sub((vec3_t*)&frustum->fbr, (vec3_t*)&frustum->nbr, &frust_edges[4]);
    PFM_Vec3_Sub((vec3_t*)&frustum->fbl, (vec3_t*)&frustum->nbl, &frust_edges[5]);

    /* For OBBs, edge axes and normals are the same */
    size_t ncrosses = 0;
    vec3_t edge_cross_products[ARR_SIZE(obb->axes) * ARR_SIZE(frust_edges)];

    for(int i = 0; i < ARR_SIZE(obb->axes); i++) {
        for(int j = 0; j < ARR_SIZE(frust_edges); j++) {

            PFM_Vec3_Cross((vec3_t*)&obb->axes[i], &frust_edges[j], &edge_cross_products[ncrosses]);
            if(PFM_Vec3_Len(&edge_cross_products[ncrosses]) > EPSILON) {
                PFM_Vec3_Normal(&edge_cross_products[ncrosses], &edge_cross_products[ncrosses]);
                ncrosses++;
            }
        }
    }

    for(int i = 0; i < ncrosses; i++) {
    
        if(separating_axis_exists(edge_cross_products[i], frustum, obb->corners))
            return false;
    }

    return true;
}

bool C_PointInsideRect2D(vec2_t point, vec2_t a, vec2_t b, vec2_t c, vec2_t d)
{
    vec2_t ap, ab, ad;
    PFM_Vec2_Sub(&point, &a, &ap);
    PFM_Vec2_Sub(&b, &a, &ab);
    PFM_Vec2_Sub(&d, &a, &ad);

    float ap_dot_ab = PFM_Vec2_Dot(&ap, &ab); 
    float ap_dot_ad = PFM_Vec2_Dot(&ap, &ad);

    return (ap_dot_ab >= 0.0f && ap_dot_ab <= PFM_Vec2_Dot(&ab, &ab))
        && (ap_dot_ad >= 0.0f && ap_dot_ad <= PFM_Vec2_Dot(&ad, &ad));
}

bool C_PointInsideTriangle2D(vec2_t point, vec2_t a, vec2_t b, vec2_t c)
{
    vec2_t v0, v1, v2;
    PFM_Vec2_Sub(&c, &a, &v0);
    PFM_Vec2_Sub(&b, &a, &v1);
    PFM_Vec2_Sub(&point, &a, &v2);
    
    GLfloat dot00 = PFM_Vec2_Dot(&v0, &v0);
    GLfloat dot01 = PFM_Vec2_Dot(&v0, &v1);
    GLfloat dot02 = PFM_Vec2_Dot(&v0, &v2);
    GLfloat dot11 = PFM_Vec2_Dot(&v1, &v1);
    GLfloat dot12 = PFM_Vec2_Dot(&v1, &v2);
    
    /* Compute barycentric coordinates */
    GLfloat inv_denom = 1.0f / (dot00 * dot11 - dot01 * dot01);
    GLfloat u = (dot11 * dot02 - dot01 * dot12) * inv_denom;
    GLfloat v = (dot00 * dot12 - dot01 * dot02) * inv_denom;
    
    return (u >= 0.0f) && (v >= 0.0f) && (u + v < 1.0f);
}

bool C_PointInsideCircle2D(vec2_t point, vec2_t origin, float radius)
{
    vec2_t delta;
    PFM_Vec2_Sub(&point, &origin, &delta);
    return (PFM_Vec2_Len(&delta) <= radius);
}

bool C_LineLineIntersection(struct line_seg_2d l1, struct line_seg_2d l2, vec2_t *out_xz)
{
    float s1_x, s1_z, s2_x, s2_z;
    s1_x = l1.bx - l1.ax;     s1_z = l1.bz - l1.az;
    s2_x = l2.bx - l2.ax;     s2_z = l2.bz - l2.az;
    
    float s, t;
    s = (-s1_z * (l1.ax - l2.ax) + s1_x * (l1.az - l2.az)) / (-s2_x * s1_z + s1_x * s2_z);
    t = ( s2_x * (l1.az - l2.az) - s2_z * (l1.ax - l2.ax)) / (-s2_x * s1_z + s1_x * s2_z);
    
    if (s >= 0 && s <= 1 && t >= 0 && t <= 1) {
        /* Intersection detected */
        if (out_xz) {
            out_xz->raw[0] = l1.ax + (t * s1_x);
            out_xz->raw[1] = l1.az + (t * s1_z);
        }
        return true;
    }
    
    return false; /* No intersection */
}

bool C_InfiniteLineIntersection(struct line_2d l1, struct line_2d l2, vec2_t *out_xz)
{
    float l1_slope = fabs(l1.dir.raw[0]) < EPSILON ? NAN : (l1.dir.raw[1] / l1.dir.raw[0]);
    float l2_slope = fabs(l2.dir.raw[0]) < EPSILON ? NAN : (l2.dir.raw[1] / l2.dir.raw[0]);

    /* Lines are parallel or coincident */
    if(fabs(l1_slope - l2_slope) < EPSILON)
        return false;

    if(isnan(l1_slope) && !isnan(l2_slope)) {
    
        out_xz->raw[0] = l1.point.raw[0];
        out_xz->raw[1] = (l1.point.raw[0] - l2.point.raw[0]) * l2_slope + l2.point.raw[1];

    }else if(!isnan(l1_slope) && isnan(l2_slope)) {

        out_xz->raw[0] = l2.point.raw[0];
        out_xz->raw[1] = (l2.point.raw[0] - l1.point.raw[0]) * l1_slope + l2.point.raw[1];
    
    }else{
    
        out_xz->raw[0] = (l1_slope * l1.point.raw[0] - l2_slope * l2.point.raw[0] 
            + l2.point.raw[1] - l1.point.raw[1]) / (l1_slope - l2_slope);
        out_xz->raw[1] = l2_slope * (out_xz->raw[0] - l2.point.raw[0]) + l2.point.raw[1];
    }

    return true;
}

bool C_RayRayIntersection2D(struct line_2d l1, struct line_2d l2, vec2_t *out_xz)
{
    vec2_t intersec_point;
    if(!C_InfiniteLineIntersection(l1, l2, &intersec_point))
        return false;

    /* Now check if the intersection point is within the bounds of both rays */
    if((intersec_point.raw[0] - l1.point.raw[0]) / l1.dir.raw[0] < 0.0f)
        return false;

    if((intersec_point.raw[1] - l1.point.raw[1]) / l1.dir.raw[1] < 0.0f)
        return false;

    if((intersec_point.raw[0] - l2.point.raw[0]) / l2.dir.raw[0] < 0.0f)
        return false;

    if((intersec_point.raw[1] - l2.point.raw[1]) / l2.dir.raw[1] < 0.0f)
        return false;

    *out_xz = intersec_point;
    return true;
}

int C_LineBoxIntersection(struct line_seg_2d line, struct box bounds, vec2_t out_xz[2])
{
    int ret = 0;

    struct line_seg_2d top = (struct line_seg_2d){
        bounds.x, 
        bounds.z, 
        bounds.x - bounds.width,
        bounds.z
    };

    struct line_seg_2d bot = (struct line_seg_2d){
        bounds.x, 
        bounds.z + bounds.height, 
        bounds.x - bounds.width,
        bounds.z + bounds.height
    };

    struct line_seg_2d left = (struct line_seg_2d){
        bounds.x, 
        bounds.z, 
        bounds.x,
        bounds.z + bounds.height
    };

    struct line_seg_2d right = (struct line_seg_2d){
        bounds.x - bounds.width, 
        bounds.z, 
        bounds.x - bounds.width,
        bounds.z + bounds.height
    };

    if(C_LineLineIntersection(line, top, out_xz + ret))
        ret++;

    if(C_LineLineIntersection(line, bot, out_xz + ret))
        ret++;

    if(C_LineLineIntersection(line, left, out_xz + ret))
        ret++;

    if(C_LineLineIntersection(line, right, out_xz + ret))
        ret++;

    assert(ret >= 0 && ret <= 2);
    return ret;
}

bool C_BoxPointIntersection(float px, float pz, struct box bounds)
{
    return (px <= bounds.x && px >= bounds.x - bounds.width)
        && (pz >= bounds.z && pz <= bounds.z + bounds.height);
}

float C_PointLineSegmentShortestDist(vec2_t point, struct line_seg_2d seg)
{
    float len_sqrt = pow(seg.bz - seg.az, 2) + pow(seg.bx - seg.ax, 2);
    if(len_sqrt < EPSILON) {
        return sqrt(pow(seg.az - point.z, 2) + pow(seg.ax - point.x, 2));
    }

    /* Consider the line extending the segment, parameterized as a + t * (b - a). 
     * We find the projection of the point onto this line. 
     */
    vec2_t a = (vec2_t){seg.ax, seg.az}, b = (vec2_t){seg.bx, seg.bz};
    vec2_t dir;
    PFM_Vec2_Sub(&b, &a, &dir);

    vec2_t to;
    PFM_Vec2_Sub(&point, &a, &to);
    float t = PFM_Vec2_Dot(&to, &dir) / len_sqrt;

    /* We clamp t from [0, 1] to handle points outside the segment 
     */
    t = MAX(MIN(t, 1.0f), 0.0f);

    vec2_t proj, delta;
    PFM_Vec2_Scale(&dir, t, &delta);
    PFM_Vec2_Add(&a, &delta, &proj);

    return sqrt(pow(proj.z - point.z, 2) + pow(proj.x - point.x, 2));
}

bool C_LineCircleIntersection(struct line_seg_2d line, vec2_t center_xz, float radius, float *out_t)
{
    float cx = center_xz.raw[0];
    float cz = center_xz.raw[1];

    float dx = line.bx - line.ax;
    float dz = line.bz - line.az;

    float A = pow(dx, 2) + pow(dz, 2);
    float B = 2 * (dx * (line.ax - cx) + dz * (line.az - cz));
    float C = pow(line.ax - cx, 2) + pow(line.az - cz, 2) - pow(radius, 2);
    float det = pow(B, 2) - (4 * A * C);

    /* No real solutions */
    float t;
    if(det < 0.0f || A < EPSILON) {

        return false;
    /* One real solution */
    }else if(det == 0.0f){
        
        t = -B / (2 * A);
    /* Two real solutions */
    }else{

        float t1 = (-B + sqrt(det)) / (2 * A);
        float t2 = (-B - sqrt(det)) / (2 * A);
        t = MIN(t1, t2);
    }

    if(t < 0.0f || t > 1.0f)
        return false;

    *out_t = t;
    return true;
}

bool C_CircleRectIntersection(vec2_t center, float radius, struct box rect)
{
    vec2_t corners[4] = {
        rect.x - rect.width, rect.z,
        rect.x,              rect.z,
        rect.x,              rect.z + rect.height,
        rect.x - rect.width, rect.z + rect.height,
    };
    if(C_PointInsideRect2D(center, corners[0], corners[1], corners[2], corners[3]))
        return true;

    for(int i = 0; i < ARR_SIZE(corners); i++) {
        vec2_t delta;
        PFM_Vec2_Sub(&corners[i], &center, &delta);
        if(PFM_Vec2_Len(&delta) <= radius)
            return true;
    }

    struct line_seg_2d edges[4] = {
        (struct line_seg_2d){corners[0].x, corners[0].z, corners[1].x, corners[1].z},
        (struct line_seg_2d){corners[1].x, corners[1].z, corners[2].x, corners[2].z},
        (struct line_seg_2d){corners[2].x, corners[2].z, corners[3].x, corners[3].z},
        (struct line_seg_2d){corners[3].x, corners[3].z, corners[0].x, corners[0].z},
    };
    for(int i = 0; i < ARR_SIZE(edges); i++) {
        if(C_LineCircleIntersection(edges[i], center, radius, &(float){0}))
            return true;
    }
    return false;
}

bool C_RectRectIntersection(struct box a, struct box b)
{
    struct range ax = (struct range){a.x - a.width, a.x};
    struct range az = (struct range){a.z, a.z + a.height};
    struct range bx = (struct range){b.x - b.width, b.x};
    struct range bz = (struct range){b.z, b.z + b.height};

    return (ranges_overlap(&ax, &bx) && ranges_overlap(&az, &bz));
}

