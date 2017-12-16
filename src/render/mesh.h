#ifndef MESH_H
#define MESH_H

#include "../pf_math.h"

struct vertex;

struct mesh{
    unsigned       num_verts;
    GLuint         VBO;
    GLuint         VAO;
    struct vertex *vbuff;
};

#endif
