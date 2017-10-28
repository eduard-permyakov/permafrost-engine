#ifndef ANIM_CTX_H
#define ANIM_CTX_H

#include <stddef.h>

enum anim_mode{
    ANIM_TYPE_ONCE,
    ANIM_TYPE_LOOP
};

struct anim_ctx{
    struct anim_clip *active;
    enum anim_mode    mode; 
    int               curr_frame;
    uint32_t          curr_frame_start_ticks;
};

#endif
