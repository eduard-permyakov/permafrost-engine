#ifndef VERTEX_H
#define VERTEX_H

#include <GL/gl.h>

struct joint_weight{
    GLfloat joint_idx;
    GLfloat weight;
};

struct vertex{
    vec3_t              pos;
    vec2_t              uv;
    struct joint_weight weights[4];
};

#endif
