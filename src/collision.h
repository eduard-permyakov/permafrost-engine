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

#ifndef COLLISION_H
#define COLLISION_H

#include "pf_math.h"
#include <stdbool.h>

struct aabb{
    float x_min, x_max;
    float y_min, y_max;
    float z_min, z_max;
};

struct plane{
    vec3_t point;
    vec3_t normal;
};

struct frustum{
    struct plane near, far;
    struct plane top, bot;
    struct plane left, right;
    /* The 8 corners of the frustum are used for precise collision 
     * detection using Separating Axis Theorem. */
    vec3_t ntl, ntr, nbl, nbr;
    vec3_t ftl, ftr, fbl, fbr;
};

enum volume_intersec_type{
    VOLUME_INTERSEC_INSIDE,
    VOLUME_INTERSEC_OUTSIDE,
    VOLUME_INTERSEC_INTERSECTION,
};

bool C_RayIntersectsAABB(vec3_t ray_origin, vec3_t ray_dir, struct aabb aabb, float *out_t);
bool C_RayIntersectsTriMesh(vec3_t ray_origin, vec3_t ray_dir, vec3_t *tribuff, size_t n);
bool C_RayIntersectsPlane(vec3_t ray_origin, vec3_t ray_dir, struct plane plane, float *out_t);

/* Note that the following 2 routines do not give precise results. They are fast checks but 
 * sometimes give false positives. Howvever, this is still suitable for view frustum culling 
 * in some cases. */
enum volume_intersec_type C_FrustumPointIntersection(const struct frustum *frustum, vec3_t point);
enum volume_intersec_type C_FrustumAABBIntersection (const struct frustum *frustum, const struct aabb *aabb);

bool C_FrustumAABBIntersectionExact(const struct frustum *frustum, const struct aabb *aabb);

/* Note that the following assumes that AB is parallel to CD and BC is parallel to AD
 */
bool C_PointInsideScreenRect(vec2_t point, vec2_t a, vec2_t b, vec2_t c, vec2_t d);

#endif
