#ifndef ANIM_MESH_H
#define ANIM_MESH_H

#include "public/skeleton.h"
#include "../pf_math.h"

#include <stddef.h>

#define ANIM_NAME_LEN  32

struct SQT{
    vec3_t scale;
    quat_t quat_rotation;
    vec3_t trans;
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
    unsigned          num_anims;
    struct skeleton   skel;
    struct anim_clip *anims;
};

#endif
