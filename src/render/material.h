#ifndef MATERIAL_H
#define MATERIAL_H

#include "../pf_math.h"
#include "texture.h"

struct material{
    GLfloat        ambient_intensity;    
    vec3_t         diffuse_clr;
    vec3_t         specular_clr;
    struct texture texture;
    char           texname[32];
};

#endif
