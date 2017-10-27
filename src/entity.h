#ifndef ENTITY_H
#define ENTITY_H

#include "pf_math.h"
#include "render/mesh.h"
#include "anim/anim_data.h"

#define ENTITY_NAME_LEN 32

struct entity{
    unsigned         id;
    char             name[ENTITY_NAME_LEN];
    struct mesh      mesh;    
    struct anim_data anim_data;
    mat4x4_t         model_matrix;
    char             mem[];
};

#endif
