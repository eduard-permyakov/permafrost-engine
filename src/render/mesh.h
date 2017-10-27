#ifndef MESH_H
#define MESH_H

#include "../pf_math.h"
#include "texture.h"

struct vertex;

struct face{
    GLint vertex_indeces[3];
};

struct mesh{
    GLint           VBO;
    struct vertex  *vbuff;
    struct texture  tex;
    GLint           EBO;
    struct face    *ebuff;
};

#endif
