#ifndef TEXTURE_H
#define TEXTURE_H

#include <GL/glew.h>
#include <stdbool.h>

struct texture{
    GLuint id;
    GLuint tunit;
};

void R_Texture_Init(void);
bool R_Texture_GetForName(const char *name, GLuint *out);
bool R_Texture_Load(const char *basedir, const char *name, GLuint *out);
void R_Texture_Free(const char *name);
void R_Texture_GL_Activate(const struct texture *text);

#endif
