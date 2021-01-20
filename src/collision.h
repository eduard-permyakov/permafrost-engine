/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2020 Eduard Permyakov 
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

struct obb{
    vec3_t center;
    vec3_t axes[3];
    float  half_lengths[3];
    vec3_t corners[8];
};

enum volume_intersec_type{
    VOLUME_INTERSEC_INSIDE,
    VOLUME_INTERSEC_OUTSIDE,
    VOLUME_INTERSEC_INTERSECTION,
};

void C_MakeFrustum(vec3_t pos, vec3_t up, vec3_t front, 
                   GLfloat aspect_ratio, GLfloat fov_rad, 
                   GLfloat near_dist, GLfloat far_dist,
                   struct frustum *out);

bool C_RayIntersectsAABB(vec3_t ray_origin, vec3_t ray_dir, struct aabb aabb, float *out_t);
bool C_RayIntersectsOBB (vec3_t ray_origin, vec3_t ray_dir, struct obb obb,   float *out_t);
bool C_RayIntersectsTriMesh(vec3_t ray_origin, vec3_t ray_dir, vec3_t *tribuff, size_t n, float *out_t);
bool C_RayIntersectsPlane(vec3_t ray_origin, vec3_t ray_dir, struct plane plane, float *out_t);

/* Note that the following routines do not give precise results. They are fast checks but 
 * sometimes give false positives. Howvever, this is still suitable for view frustum culling 
 * in some cases. */
enum volume_intersec_type C_FrustumPointIntersectionFast(const struct frustum *frustum, vec3_t point);
enum volume_intersec_type C_FrustumAABBIntersectionFast (const struct frustum *frustum, const struct aabb *aabb);
enum volume_intersec_type C_FrustumOBBIntersectionFast  (const struct frustum *frustum, const struct obb *obb);

bool C_FrustumAABBIntersectionExact(const struct frustum *frustum, const struct aabb *aabb);
bool C_FrustumOBBIntersectionExact(const struct frustum *frustum, const struct obb *obb);

/* Note that the following assumes that AB is parallel to CD and BC is parallel to AD
 */
bool C_PointInsideRect2D(vec2_t point, vec2_t a, vec2_t b, vec2_t c, vec2_t d);
bool C_PointInsideTriangle2D(vec2_t point, vec2_t a, vec2_t b, vec2_t c);
bool C_PointInsideCircle2D(vec2_t point, vec2_t origin, float radius);


struct box{
    float x, z; 
    float width, height;
};

struct line_seg_2d{
    float ax, az; 
    float bx, bz;
};

struct line_2d{
    vec2_t point; 
    vec2_t dir; 
};

bool  C_LineLineIntersection(struct line_seg_2d l1, struct line_seg_2d l2, vec2_t *out_xz);
int   C_LineBoxIntersection (struct line_seg_2d line, struct box bounds, vec2_t out_xz[2]);
bool  C_BoxPointIntersection(float px, float pz, struct box bounds);
float C_PointLineSegmentShortestDist(vec2_t point, struct line_seg_2d seg);
/* 'out_t' gets set to the value corresponding to the _closest_ intersection in the 
 * case that there is more than one intersection. */
bool  C_LineCircleIntersection(struct line_seg_2d line, vec2_t center_xz, float radius, float *out_t);
bool  C_InfiniteLineIntersection(struct line_2d l1, struct line_2d l2, vec2_t *out_xz);
bool  C_RayRayIntersection2D(struct line_2d l1, struct line_2d l2, vec2_t *out_xz);

bool  C_CircleRectIntersection(vec2_t center, float radius, struct box rect);
bool  C_RectRectIntersection(struct box a, struct box b);

#endif

