#ifndef ENTITY_H
#define ENTITY_H

#include "pf_math.h"

#define ENTITY_NAME_LEN 32

struct entity{
    unsigned  id;
    char      name[ENTITY_NAME_LEN];
    mat4x4_t  model_matrix;
    void     *render_private;
    void     *anim_private;
    char      mem[];
};

#endif
