/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2023 Eduard Permyakov 
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
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
 *
 */

#ifndef GL_ASSERT_H
#define GL_ASSERT_H

#include <assert.h>
#include <stdio.h>

#ifndef NDEBUG

#ifdef _MSC_VER
#include "../lib/public/windows.h"
#define PRINT(_text) OutputDebugString(_text)
#else
#define PRINT(_text) fprintf(stderr, "%s", (_text))
#endif

/* Report but never abort: a stray error must not take the engine down, and
 * draining the whole error queue here stops it being misattributed to a
 * later checkpoint. Reports are capped per call site to keep a recurring
 * per-frame error from flooding the log.
 */
#define GL_ASSERT_MAX_REPORTS (8)

#define GL_ASSERT_OK()                                          \
    do {                                                        \
        static int s_nreported_here;                            \
        GLenum error;                                           \
        while((error = glGetError()) != GL_NO_ERROR) {          \
            if(s_nreported_here > GL_ASSERT_MAX_REPORTS)        \
                continue;                                       \
            char buff[512];                                     \
            if(s_nreported_here++ == GL_ASSERT_MAX_REPORTS) {   \
                snprintf(buff, sizeof(buff),                    \
                    "%s:%d further OpenGL errors suppressed\n", \
                    __FILE__, __LINE__);                        \
            }else{                                              \
                snprintf(buff, sizeof(buff),                    \
                    "%s:%d OpenGL error: %x\n",                 \
                    __FILE__, __LINE__, error);                 \
            }                                                   \
            PRINT(buff);                                        \
        }                                                       \
    }while(0)

#else

#define PRINT(...) 
#define GL_ASSERT_OK() /* no-op */

#endif

#endif
