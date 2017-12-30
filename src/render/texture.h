/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

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
void R_Texture_GL_Activate(const struct texture *text, GLuint shader_prog);

#endif
