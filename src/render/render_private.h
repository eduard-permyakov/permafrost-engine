#ifndef RENDER_PRIVATE_H
#define RENDER_PRIVATE_H

#include "mesh.h"

struct render_private{
    struct mesh      mesh;
    size_t           num_materials;
    struct material *materials;
    GLint            shader_prog;
};

#endif
