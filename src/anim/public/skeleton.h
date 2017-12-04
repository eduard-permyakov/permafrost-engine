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
