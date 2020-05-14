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

#ifndef GL_RINGBUFFER_H
#define GL_RINGBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <GL/glew.h>

/* The ringbuffer is used for efficient submission of streamed data
 * to the GPU. The key principle is using a manually synchronized buffer,
 * (or Persistent Mapped Buffer, if available) and filling up 1 section 
 * of it every frame. The data is exposed to a shader via a pair of uniforms: 
 * 
 *     1. uname (usamplerBuffer)
 *     2. uname_offset (int)
 * 
 * So long as there is sufficient room in the buffer, this should allow
 * for the GPU to use one section of the buffer, while the CPU is filling
 * another with the next frame's data, all without implicit synchronization
 * and minimal state changes.
 *
 * Usage: 
 *
 *   ring = R_GL_RingbufferInit(...);
 *   for each frame:
 *       R_GL_RingbufferPush(ring, ...);
 *       R_GL_RingbufferBindLast(ring, ...);
 *       // queue the GL draw commands touching buffered data
 *       R_GL_RingbufferSyncLast(ring, ...);
 *   R_GL_RingbufferDestroy(ring);
 * 
 */
struct gl_ring;

struct gl_ring *R_GL_RingbufferInit(size_t size);
void            R_GL_RingbufferDestroy(struct gl_ring *ring);
bool            R_GL_RingbufferPush(struct gl_ring *ring, void *data, size_t size);
void            R_GL_RingbufferBindLast(struct gl_ring *ring, GLuint tunit, GLuint shader_prog, const char *uname);
void            R_GL_RingbufferSyncLast(struct gl_ring *ring);

#endif

