/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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

#ifndef GL_ASSERT_H
#define GL_ASSERT_H

#include <assert.h>
#include <stdio.h>

#define GL_ASSERT_OK()                                  \
    do {                                                \
        GLenum error = glGetError();                    \
        if(error != GL_NO_ERROR)                        \
            fprintf(stderr, "%s:%d OpenGL error: %x\n", \
            __FILE__, __LINE__, error);                 \
        assert(error == GL_NO_ERROR);                   \
    }while(0)

#endif
