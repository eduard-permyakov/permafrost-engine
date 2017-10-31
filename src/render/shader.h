#ifndef SHADER_H
#define SHADER_H

#include <GL/glew.h>

#include <stdbool.h>

bool  Shader_InitAll(void);
GLint Shader_GetProgForName(const char *name);

#endif
