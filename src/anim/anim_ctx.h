#ifndef ANIM_CTX_H
#define ANIM_CTX_H

#include <stddef.h>

struct anim_ctx{
    const struct anim_clip *active;
    const struct anim_clip *idle;
    enum anim_mode          mode; 
    unsigned                key_fps;
    int                     curr_frame;
    uint32_t                curr_frame_start_ticks;
};

#endif
