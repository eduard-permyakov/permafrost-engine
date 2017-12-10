#ifndef VERTEX_H
#define VERTEX_H

#include <GL/gl.h>

struct vertex{
    vec3_t  pos;
    vec2_t  uv;
    GLint   material_idx;
    GLint   joint_indices[4];
    GLfloat weights[4];
};

#endif
