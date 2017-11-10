#ifndef SKELETON_H
#define SKELETON_H

#include "../../pf_math.h"

#define JOINT_NAME_LEN 32

struct joint{
    char     name[JOINT_NAME_LEN];
    int      parent_idx;
    mat4x4_t inv_bind_pose;
};

struct skeleton{
    size_t        num_joints;
    struct joint *joints;
};

#endif
