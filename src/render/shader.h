#ifndef SHADER_H
#define SHADER_H

#include <GL/glew.h>

#include <stdbool.h>

bool  R_Shader_InitAll(const char *base_path);
GLint R_Shader_GetProgForName(const char *name);

#endif
