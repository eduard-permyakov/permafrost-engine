/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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
 */

#include "collision.h"
#include <assert.h>

#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#define MAX(a, b)   ((a) > (b) ? (a) : (b))
#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))

#define EPSILON (1.0f / 1000000.0f)


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
static bool ray_triangle_intersect(vec3_t ray_origin, vec3_t ray_dir, vec3_t tri_verts[3])
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
    if(b->begin >= a->begin && b->begin <= a->end)
        return true;

    if(b->end >= a->begin && b->end <= a->end)
        return true;

    return false;
}

static bool separating_axis_exists(vec3_t axis, const struct frustum *frustum, const struct aabb *aabb)
{
    struct range frust_range, aabb_range;

    float frust_axis_dots[8];
    float aabb_axis_dots[8];

    const vec3_t *frust_points[8] = {&frustum->ntl, &frustum->ntr, &frustum->nbl, &frustum->nbr,
                                     &frustum->ftl, &frustum->ftr, &frustum->fbl, &frustum->fbr};

    vec3_t aabb_points[8] = {
        (vec3_t){aabb->x_min, aabb->y_min, aabb->z_min},
        (vec3_t){aabb->x_min, aabb->y_min, aabb->z_max},
        (vec3_t){aabb->x_min, aabb->y_max, aabb->z_min},
        (vec3_t){aabb->x_min, aabb->y_max, aabb->z_max},
        (vec3_t){aabb->x_max, aabb->y_min, aabb->z_min},
        (vec3_t){aabb->x_max, aabb->y_min, aabb->z_max},
        (vec3_t){aabb->x_max, aabb->y_max, aabb->z_min},
        (vec3_t){aabb->x_max, aabb->y_max, aabb->z_max},
    };

    for(int i = 0; i < 8; i++)
        frust_axis_dots[i] = PFM_Vec3_Dot((vec3_t*)frust_points[i], &axis);

    for(int i = 0; i < 8; i++)
        aabb_axis_dots[i] = PFM_Vec3_Dot(&aabb_points[i], &axis);

    frust_range = (struct range){arr_min(frust_axis_dots, 8), arr_max(frust_axis_dots, 8)}; 
    aabb_range  = (struct range){arr_min(aabb_axis_dots, 8),  arr_max(aabb_axis_dots, 8)};

    return !ranges_overlap(&frust_range, &aabb_range);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

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

bool C_RayIntersectsTriMesh(vec3_t ray_origin, vec3_t ray_dir, vec3_t *tribuff, size_t n)
{
    assert(n % 3 == 0);
    for(int i = 0; i < n; i += 3) {
        
        if(ray_triangle_intersect(ray_origin, ray_dir, tribuff + i))
            return true;
    }

    return false;
}

bool C_RayIntersectsPlane(vec3_t ray_origin, vec3_t ray_dir, struct plane plane, float *out_t)
{
    float denom = PFM_Vec3_Dot(&ray_dir, &plane.normal);
    if(denom < 0.0f) {

        vec3_t rp; 
        PFM_Vec3_Sub(&plane.point, &ray_origin, &rp);
        *out_t = PFM_Vec3_Dot(&rp, &plane.normal) / denom;
        return true;
    }

    return false;
}

enum volume_intersec_type C_FrustrumPointIntersection(const struct frustum *frustum, vec3_t point)
{
    const struct plane *planes[] = {&frustum->top, &frustum->bot, &frustum->left, 
                                    &frustum->right, &frustum->near, &frustum->far};

    for(int i = 0; i < ARR_SIZE(planes); i++) {
   
        if(plane_point_signed_distance(planes[i], point) < 0.0f)
            return VOLUME_INTERSEC_OUTSIDE;
    }

    return VOLUME_INTERSEC_INSIDE;
}

/* Based on the algorithm outlined here:
 * http://cgvr.informatik.uni-bremen.de/teaching/cg_literatur/lighthouse3d_view_frustum_culling/index.html
 */
enum volume_intersec_type C_FrustumAABBIntersection(const struct frustum *frustum, const struct aabb *aabb)
{
    const struct plane *planes[] = {&frustum->top, &frustum->bot, &frustum->left, 
                                    &frustum->right, &frustum->near, &frustum->far};

    for(int i = 0; i < ARR_SIZE(planes); i++) {

        int corners_in = 0, corners_out = 0;

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

bool C_FrustumAABBIntersectionExact(const struct frustum *frustum, const struct aabb *aabb)
{
    vec3_t aabb_axes[3] = {
        (vec3_t){1.0f, 0.0f, 0.0f}, 
        (vec3_t){0.0f, 1.0f, 0.0f}, 
        (vec3_t){0.0f, 0.0f, 1.0f}
    };
    
    for(int i = 0; i < ARR_SIZE(aabb_axes); i++) {
    
        if(separating_axis_exists(aabb_axes[i], frustum, aabb))
            return false;
    }

    vec3_t frust_axes[6] = {
        frustum->near.normal,
        frustum->far.normal,
        frustum->top.normal,
        frustum->bot.normal,
        frustum->left.normal,
        frustum->right.normal
    };

    for(int i = 0; i < ARR_SIZE(frust_axes); i++) {
    
        if(separating_axis_exists(frust_axes[i], frustum, aabb)) 
            return false;
    }

    vec3_t axes_cross_products[ARR_SIZE(aabb_axes) * ARR_SIZE(frust_axes)];
    for(int i = 0; i < ARR_SIZE(aabb_axes); i++) {
        for(int j = 0; j < ARR_SIZE(frust_axes); j++)
            PFM_Vec3_Cross(&aabb_axes[i], &frust_axes[j], &axes_cross_products[i * ARR_SIZE(aabb_axes) + j]);
    }

    for(int i = 0; i < ARR_SIZE(axes_cross_products); i++) {
    
        if(separating_axis_exists(axes_cross_products[i], frustum, aabb)) 
            return false;
    }

    return true;
}

bool C_PointInsideScreenRect(vec2_t point, vec2_t a, vec2_t b, vec2_t c, vec2_t d)
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

