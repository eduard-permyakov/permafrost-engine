#ifndef ANIM_DATA_H
#define ANIM_DATA_H

#include "public/skeleton.h"
#include "../pf_math.h"

#include <stddef.h>

#define ANIM_NAME_LEN  32

struct anim_sample{
    struct SQT *local_joint_poses;
};

struct anim_clip{
    char                name[ANIM_NAME_LEN];
    struct skeleton    *skel;
    unsigned            num_frames;
    struct anim_sample *samples;
};

struct anim_data{
    unsigned          num_anims;
    struct skeleton   skel;
    struct anim_clip *anims;
};

#endif
