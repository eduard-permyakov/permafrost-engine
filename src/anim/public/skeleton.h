/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017 Eduard Permyakov 
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

#ifndef SKELETON_H
#define SKELETON_H

#include "../../pf_math.h"

#define JOINT_NAME_LEN 32

struct SQT{
    vec3_t scale;
    quat_t quat_rotation;
    vec3_t trans;
};

struct joint{
    char       name[JOINT_NAME_LEN];
    int        parent_idx;
    vec3_t     tip;
};

struct skeleton{
    size_t        num_joints;
    struct joint *joints;
    /* Transformation from the parent joint's space to 
     * the local joint space. In the case of a root 
     * bone, the trasformation from object space to the 
     * local joint space. 
     */
    struct SQT   *bind_sqts;
    /* joint space to object space */
    mat4x4_t     *inv_bind_poses;
};

#endif
