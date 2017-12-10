#ifndef MESH_H
#define MESH_H

#include "../pf_math.h"
#include "texture.h"

struct vertex;

struct face{
    GLint vertex_indeces[3];
};

struct mesh{
    unsigned        num_verts;
    GLuint          VBO;
    GLuint          VAO;
    struct vertex  *vbuff;
    unsigned        num_faces;
    GLint           EBO;
    struct face    *ebuff;
};

#endif
