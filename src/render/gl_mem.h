/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
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

#ifndef GL_MEM_H
#define GL_MEM_H

/* Per-file GPU memory accounting. Each render translation unit defines
 * GPU_MEM_FILE_SYS to its own system before including this header; the wrapper
 * macros below then redirect the GL allocation/deletion calls through the
 * tracking functions, tallying bytes into [system][subsystem] counters. The
 * counters are maintained on the render thread and snapshotted by
 * R_GL_GetMemoryAccounting.
 */

#include <stdint.h>
#include <GL/glew.h>

enum gpu_mem_sys{
    GPU_MEM_SYS_UNKNOWN = 0,
    GPU_MEM_SYS_GL_ANIM,
    GPU_MEM_SYS_GL_BATCH,
    GPU_MEM_SYS_GL_FOLIAGE,
    GPU_MEM_SYS_GL_IMAGE_QUILT,
    GPU_MEM_SYS_GL_MINIMAP,
    GPU_MEM_SYS_GL_MOVEMENT,
    GPU_MEM_SYS_GL_POSITION,
    GPU_MEM_SYS_GL_RENDER,
    GPU_MEM_SYS_GL_RINGBUFFER,
    GPU_MEM_SYS_GL_SHADOWS,
    GPU_MEM_SYS_GL_SKYBOX,
    GPU_MEM_SYS_GL_SPRITE,
    GPU_MEM_SYS_GL_STATUSBAR,
    GPU_MEM_SYS_GL_SWAPCHAIN,
    GPU_MEM_SYS_GL_TERRAIN,
    GPU_MEM_SYS_GL_TEXTURE,
    GPU_MEM_SYS_GL_TILE,
    GPU_MEM_SYS_GL_UI,
    GPU_MEM_SYS_GL_WATER,
    GPU_MEM_SYS_COUNT
};

enum gpu_mem_sub{
    GPU_MEM_SUB_TEXTURE = 0,
    GPU_MEM_SUB_RENDERBUFFER,
    GPU_MEM_SUB_BUFFEROBJECT,
    GPU_MEM_SUB_COUNT
};

struct gpu_mem_accounting{
    int64_t sys_bytes[GPU_MEM_SYS_COUNT];
    int64_t sys_count[GPU_MEM_SYS_COUNT];
    int64_t sub_bytes[GPU_MEM_SYS_COUNT][GPU_MEM_SUB_COUNT];
    int64_t sub_count[GPU_MEM_SYS_COUNT][GPU_MEM_SUB_COUNT];
};

const char *gpu_mem_sys_name(int sys);
const char *gpu_mem_sub_name(int sub);

#ifndef NDEBUG

void R_GL_MemTrack_TexImage2D(int sys, GLenum target, GLint level, GLint ifmt,
                              GLsizei w, GLsizei h, GLint border, GLenum fmt,
                              GLenum type, const void *pix);
void R_GL_MemTrack_TexImage3D(int sys, GLenum target, GLint level, GLint ifmt,
                              GLsizei w, GLsizei h, GLsizei d, GLint border,
                              GLenum fmt, GLenum type, const void *pix);
void R_GL_MemTrack_TexStorage2D(int sys, GLenum target, GLsizei levels,
                                GLenum ifmt, GLsizei w, GLsizei h);
void R_GL_MemTrack_RenderbufferStorage(int sys, GLenum target, GLenum ifmt,
                                       GLsizei w, GLsizei h);
void R_GL_MemTrack_BufferData(int sys, GLenum target, GLsizeiptr size,
                              const void *data, GLenum usage);
void R_GL_MemTrack_BufferStorage(int sys, GLenum target, GLsizeiptr size,
                                 const void *data, GLbitfield flags);
void R_GL_MemTrack_GenerateMipmap(int sys, GLenum target);
void R_GL_MemTrack_DeleteTextures(GLsizei n, const GLuint *ids);
void R_GL_MemTrack_DeleteRenderbuffers(GLsizei n, const GLuint *ids);
void R_GL_MemTrack_DeleteBuffers(GLsizei n, const GLuint *ids);

#ifdef GPU_MEM_FILE_SYS

/* Several of these are GLEW function-pointer macros, so undef before redefining. */
#undef glTexImage2D
#define glTexImage2D(t, l, ifmt, w, h, b, f, ty, p)    \
    R_GL_MemTrack_TexImage2D(GPU_MEM_FILE_SYS, (t), (l), (ifmt), (w), (h), (b), (f), (ty), (p))

#undef glTexImage3D
#define glTexImage3D(t, l, ifmt, w, h, d, b, f, ty, p) \
    R_GL_MemTrack_TexImage3D(GPU_MEM_FILE_SYS, (t), (l), (ifmt), (w), (h), (d), (b), (f), (ty), (p))

#undef glTexStorage2D
#define glTexStorage2D(t, lv, ifmt, w, h)              \
    R_GL_MemTrack_TexStorage2D(GPU_MEM_FILE_SYS, (t), (lv), (ifmt), (w), (h))

#undef glRenderbufferStorage
#define glRenderbufferStorage(t, ifmt, w, h)           \
    R_GL_MemTrack_RenderbufferStorage(GPU_MEM_FILE_SYS, (t), (ifmt), (w), (h))

#undef glBufferData
#define glBufferData(t, s, d, u)                       \
    R_GL_MemTrack_BufferData(GPU_MEM_FILE_SYS, (t), (s), (d), (u))

#undef glBufferStorage
#define glBufferStorage(t, s, d, fl)                   \
    R_GL_MemTrack_BufferStorage(GPU_MEM_FILE_SYS, (t), (s), (d), (fl))

#undef glGenerateMipmap
#define glGenerateMipmap(t)                            \
    R_GL_MemTrack_GenerateMipmap(GPU_MEM_FILE_SYS, (t))

#undef glDeleteTextures
#define glDeleteTextures(n, ids)      R_GL_MemTrack_DeleteTextures((n), (ids))

#undef glDeleteRenderbuffers
#define glDeleteRenderbuffers(n, ids) R_GL_MemTrack_DeleteRenderbuffers((n), (ids))

#undef glDeleteBuffers
#define glDeleteBuffers(n, ids)       R_GL_MemTrack_DeleteBuffers((n), (ids))

#endif /* GPU_MEM_FILE_SYS */

#endif /* !NDEBUG */

#endif
