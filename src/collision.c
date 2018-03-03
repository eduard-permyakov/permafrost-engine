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

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define EPSILON (1.0f / 1000000.0f)

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
    float t = (PFM_Vec3_Dot(&n, &ray_origin) + d) / n_dot_ray_dir;

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
    PFM_Vec3_Cross(&edge2, &vp1, &C);

    if(PFM_Vec3_Dot(&n, &C) < 0.0f) {
        return false;
    }

    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool C_RayIntersectsAABB(vec3_t ray_origin, vec3_t ray_dir, struct aabb aabb, float *out_t)
{
     float t1 = fabs( (aabb.x_min - ray_origin.x) / ray_dir.x );
     float t2 = fabs( (aabb.x_max - ray_origin.x) / ray_dir.x );
     float t3 = fabs( (aabb.y_min - ray_origin.y) / ray_dir.y );
     float t4 = fabs( (aabb.y_max - ray_origin.y) / ray_dir.y );
     float t5 = fabs( (aabb.z_min - ray_origin.z) / ray_dir.z );
     float t6 = fabs( (aabb.z_max - ray_origin.z) / ray_dir.z );
    
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
    for(int i = 0; i < n; n += 3) {
        
        if(ray_triangle_intersect(ray_origin, ray_dir, tribuff + n))
            return true;
    }

    return false;
}

