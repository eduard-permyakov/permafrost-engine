/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#ifndef GL_PERF_H
#define GL_PERF_H

#include "../perf.h"
#include <GL/glew.h>

#include "gl_assert.h"

#ifndef NDEBUG

extern bool g_trace_gpu;

#define GL_GPU_PERF_PUSH(name)                  \
    do{                                         \
        if(!g_trace_gpu)                        \
            break;                              \
        GLuint cookie;                          \
        glGenQueries(1, &cookie);               \
        glQueryCounter(cookie, GL_TIMESTAMP);   \
        GL_ASSERT_OK(); \
        Perf_PushGPU(name, cookie);             \
    }while(0)

#define GL_GPU_PERF_POP()                       \
    do{                                         \
        if(!g_trace_gpu)                        \
            break;                              \
        GLuint cookie;                          \
        glGenQueries(1, &cookie);               \
        glQueryCounter(cookie, GL_TIMESTAMP);   \
        GL_ASSERT_OK(); \
        Perf_PopGPU(cookie);                    \
    }while(0)

#define GL_PERF_ENTER()                         \
    do{                                         \
        Perf_Push(__func__);                    \
        GL_GPU_PERF_PUSH(__func__);             \
    }while(0)

#define GL_PERF_RETURN(...)                     \
    do{                                         \
        Perf_Pop();                             \
        GL_GPU_PERF_POP();                      \
        return (__VA_ARGS__);                   \
    }while(0)

#define GL_PERF_RETURN_VOID()                   \
    do{                                         \
        Perf_Pop();                             \
        GL_GPU_PERF_POP();                      \
        return;                                 \
    }while(0)

#define GL_PERF_PUSH_GROUP(id, message)                     \
    do{                                                     \
        if(GLEW_KHR_debug) {                                \
            glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION,   \
                id, strlen(message), message);              \
        }                                                   \
    }while(0)

#define GL_PERF_POP_GROUP()                                 \
    do{                                                     \
        if(GLEW_KHR_debug) {                                \
            glPopDebugGroup();                              \
        }                                                   \
    }while(0)
    

#else

#define GL_GPU_PERF_PUSH(name)
#define GL_GPU_PERF_POP()

#define GL_PERF_ENTER()
#define GL_PERF_RETURN(...) do {return (__VA_ARGS__); } while(0)
#define GL_PERF_RETURN_VOID(...) do { return; } while(0)

#define GL_PERF_PUSH_GROUP(id, message) /* No-op */
#define GL_PERF_POP_GROUP() /* No-op */

#endif //NDEBUG

#endif

