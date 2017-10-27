#ifndef ANIM_MESH_H
#define ANIM_MESH_H

#include "../pf_math.h"
#include <stddef.h>

#define JOINT_NAME_LEN 32
#define ANIM_NAME_LEN  32

struct SQT{
    vec3_t scale;
    quat_t quat_rotation;
    vec3_t trans;
};

struct joint{
    char     name[JOINT_NAME_LEN];
    int      parent_idx;
    mat4x4_t inv_bind_pose;
};

struct skeleton{
    size_t        num_joints;
    struct joint *joints;
};

struct anim_sample{
    struct SQT *local_joint_poses;
};

struct anim_clip{
    char                name[ANIM_NAME_LEN];
    struct skeleton    *skel;
    unsigned            num_frames;
    float               key_fps;
    struct anim_sample *samples;
};

struct anim_data{
    unsigned           num_anims;
    struct skeleton    skel;
    struct anim_clip *anims;
};

#endif
