#ifndef MATERIAL_H
#define MATERIAL_H

#include "../pf_math.h"

struct material{
    GLfloat ambient_intensity;    
    vec3_t  diffuse_clr;
    vec3_t  specular_clr;
    GLuint  tex_id;
};

#endif
