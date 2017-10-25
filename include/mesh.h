#ifndef MESH_H
#define MESH_H

#include <pf_math.h>
#include <texture.h>

struct vertex;

struct mesh{
    GLint           VBO;
    struct vertex  *vbuff;
    struct texture  tex;
};

#endif
