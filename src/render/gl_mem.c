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

#define MEM_FILE_SYS MEM_SYS_RENDER
#define MEM_FILE_SUB MEM_SUB_RENDER_GL_MEM

#include "gl_mem.h"
#include "public/render.h"

#include <string.h>

#include "../mem.h"
#include "../lib/public/khash.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_MEM)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_MEM)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_MEM)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

#ifndef NDEBUG

struct gpu_alloc{
    int64_t  bytes;
    uint16_t sys;
    uint16_t sub;
};

KHASH_MAP_INIT_INT64(gpualloc, struct gpu_alloc)

static struct{
    int64_t bytes;
    int64_t count;
}s_gpu[GPU_MEM_SYS_COUNT][GPU_MEM_SUB_COUNT];

static khash_t(gpualloc) *s_gpu_objs;

#endif /* !NDEBUG */

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

#ifndef NDEBUG

/* key namespaces in the high bits:
 * 0 = texture (+mip level),
 * 1 = renderbuffer,
 * 2 = bufferobject.
 * id occupies bits 8..39, level the low byte.
 */
static uint64_t gpu_key(int type, GLuint id, int level)
{
    return ((uint64_t)type << 40) | ((uint64_t)id << 8) | (uint64_t)(level & 0xff);
}

static int gpu_texel_bytes(GLenum ifmt)
{
    switch(ifmt) {
    case GL_R8: case GL_RED:                return 1;
    case GL_RG8: case GL_DEPTH_COMPONENT16: return 2;
    case GL_RGBA16F:                        return 8;
    case GL_RGBA32F:                        return 16;
    default:                                return 4; /* RGB(A)8, R32x, depth, ... */
    }
}

static GLenum gpu_tex_binding(GLenum target)
{
    switch(target) {
    case GL_TEXTURE_1D:       return GL_TEXTURE_BINDING_1D;
    case GL_TEXTURE_3D:       return GL_TEXTURE_BINDING_3D;
    case GL_TEXTURE_2D_ARRAY: return GL_TEXTURE_BINDING_2D_ARRAY;
    case GL_TEXTURE_CUBE_MAP: return GL_TEXTURE_BINDING_CUBE_MAP;
    default:                  return GL_TEXTURE_BINDING_2D;
    }
}

static GLenum gpu_buf_binding(GLenum target)
{
    switch(target) {
    case GL_ELEMENT_ARRAY_BUFFER:  return GL_ELEMENT_ARRAY_BUFFER_BINDING;
    case GL_UNIFORM_BUFFER:        return GL_UNIFORM_BUFFER_BINDING;
    case GL_SHADER_STORAGE_BUFFER: return GL_SHADER_STORAGE_BUFFER_BINDING;
    case GL_TEXTURE_BUFFER:        return GL_TEXTURE_BUFFER_BINDING;
    case GL_PIXEL_UNPACK_BUFFER:   return GL_PIXEL_UNPACK_BUFFER_BINDING;
    case GL_PIXEL_PACK_BUFFER:     return GL_PIXEL_PACK_BUFFER_BINDING;
    case GL_DRAW_INDIRECT_BUFFER:  return GL_DRAW_INDIRECT_BUFFER_BINDING;
    case GL_COPY_WRITE_BUFFER:     return GL_COPY_WRITE_BUFFER_BINDING;
    default:                       return GL_ARRAY_BUFFER_BINDING;
    }
}

static void gpu_track(uint64_t key, int sys, int sub, int64_t bytes)
{
    if(!s_gpu_objs)
        s_gpu_objs = kh_init(gpualloc);

    khiter_t k = kh_get(gpualloc, s_gpu_objs, key);
    if(k != kh_end(s_gpu_objs)) {
        struct gpu_alloc *old = &kh_val(s_gpu_objs, k);
        s_gpu[old->sys][old->sub].bytes -= old->bytes;
        s_gpu[old->sys][old->sub].count -= 1;
    }else{
        int ret;
        k = kh_put(gpualloc, s_gpu_objs, key, &ret);
    }
    kh_val(s_gpu_objs, k) = (struct gpu_alloc){bytes, (uint16_t)sys, (uint16_t)sub};
    s_gpu[sys][sub].bytes += bytes;
    s_gpu[sys][sub].count += 1;
}

static void gpu_untrack(uint64_t key)
{
    if(!s_gpu_objs)
        return;
    khiter_t k = kh_get(gpualloc, s_gpu_objs, key);
    if(k == kh_end(s_gpu_objs))
        return;
    struct gpu_alloc *a = &kh_val(s_gpu_objs, k);
    s_gpu[a->sys][a->sub].bytes -= a->bytes;
    s_gpu[a->sys][a->sub].count -= 1;
    kh_del(gpualloc, s_gpu_objs, k);
}

#endif /* !NDEBUG */

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

#ifndef NDEBUG

void R_GL_MemTrack_TexImage2D(int sys, GLenum target, GLint level, GLint ifmt,
                              GLsizei w, GLsizei h, GLint border, GLenum fmt,
                              GLenum type, const void *pix)
{
    GLint id = 0;
    glGetIntegerv(gpu_tex_binding(target), &id);
    if(id > 0)
        gpu_track(gpu_key(0, (GLuint)id, level), sys, GPU_MEM_SUB_TEXTURE,
            (int64_t)w * h * gpu_texel_bytes((GLenum)ifmt));
    glTexImage2D(target, level, ifmt, w, h, border, fmt, type, pix);
}

void R_GL_MemTrack_TexImage3D(int sys, GLenum target, GLint level, GLint ifmt,
                              GLsizei w, GLsizei h, GLsizei d, GLint border,
                              GLenum fmt, GLenum type, const void *pix)
{
    GLint id = 0;
    glGetIntegerv(gpu_tex_binding(target), &id);
    if(id > 0)
        gpu_track(gpu_key(0, (GLuint)id, level), sys, GPU_MEM_SUB_TEXTURE,
            (int64_t)w * h * d * gpu_texel_bytes((GLenum)ifmt));
    glTexImage3D(target, level, ifmt, w, h, d, border, fmt, type, pix);
}

void R_GL_MemTrack_TexStorage2D(int sys, GLenum target, GLsizei levels,
                                GLenum ifmt, GLsizei w, GLsizei h)
{
    GLint id = 0;
    glGetIntegerv(gpu_tex_binding(target), &id);
    if(id > 0) {
        int bpt = gpu_texel_bytes(ifmt);
        for(GLsizei l = 0; l < levels; l++) {
            GLsizei lw = w >> l, lh = h >> l;
            if(lw < 1) lw = 1;
            if(lh < 1) lh = 1;
            gpu_track(gpu_key(0, (GLuint)id, l), sys, GPU_MEM_SUB_TEXTURE,
                (int64_t)lw * lh * bpt);
        }
    }
    glTexStorage2D(target, levels, ifmt, w, h);
}

void R_GL_MemTrack_RenderbufferStorage(int sys, GLenum target, GLenum ifmt,
                                       GLsizei w, GLsizei h)
{
    GLint id = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &id);
    if(id > 0)
        gpu_track(gpu_key(1, (GLuint)id, 0), sys, GPU_MEM_SUB_RENDERBUFFER,
            (int64_t)w * h * gpu_texel_bytes(ifmt));
    glRenderbufferStorage(target, ifmt, w, h);
}

void R_GL_MemTrack_BufferData(int sys, GLenum target, GLsizeiptr size,
                              const void *data, GLenum usage)
{
    GLint id = 0;
    glGetIntegerv(gpu_buf_binding(target), &id);
    if(id > 0)
        gpu_track(gpu_key(2, (GLuint)id, 0), sys, GPU_MEM_SUB_BUFFEROBJECT,
            (int64_t)size);
    glBufferData(target, size, data, usage);
}

void R_GL_MemTrack_BufferStorage(int sys, GLenum target, GLsizeiptr size,
                                 const void *data, GLbitfield flags)
{
    GLint id = 0;
    glGetIntegerv(gpu_buf_binding(target), &id);
    if(id > 0)
        gpu_track(gpu_key(2, (GLuint)id, 0), sys, GPU_MEM_SUB_BUFFEROBJECT,
            (int64_t)size);
    glBufferStorage(target, size, data, flags);
}

void R_GL_MemTrack_GenerateMipmap(int sys, GLenum target)
{
    GLint id = 0;
    glGetIntegerv(gpu_tex_binding(target), &id);
    if(id > 0 && s_gpu_objs) {
        khiter_t k = kh_get(gpualloc, s_gpu_objs, gpu_key(0, (GLuint)id, 0));
        if(k != kh_end(s_gpu_objs)) {
            /* a full mip chain adds ~1/3 of the base level; book it at level 31 */
            int64_t base = kh_val(s_gpu_objs, k).bytes;
            gpu_track(gpu_key(0, (GLuint)id, 31), sys, GPU_MEM_SUB_TEXTURE, base / 3);
        }
    }
    glGenerateMipmap(target);
}

void R_GL_MemTrack_DeleteTextures(GLsizei n, const GLuint *ids)
{
    for(GLsizei i = 0; i < n; i++)
        for(int l = 0; l < 32; l++)
            gpu_untrack(gpu_key(0, ids[i], l));
    glDeleteTextures(n, ids);
}

void R_GL_MemTrack_DeleteRenderbuffers(GLsizei n, const GLuint *ids)
{
    for(GLsizei i = 0; i < n; i++)
        gpu_untrack(gpu_key(1, ids[i], 0));
    glDeleteRenderbuffers(n, ids);
}

void R_GL_MemTrack_DeleteBuffers(GLsizei n, const GLuint *ids)
{
    for(GLsizei i = 0; i < n; i++)
        gpu_untrack(gpu_key(2, ids[i], 0));
    glDeleteBuffers(n, ids);
}

#endif /* !NDEBUG */

const char *gpu_mem_sys_name(int sys)
{
    static const char *names[] = {
        [GPU_MEM_SYS_UNKNOWN]        = "unknown",
        [GPU_MEM_SYS_GL_ANIM]        = "anim",
        [GPU_MEM_SYS_GL_BATCH]       = "batch",
        [GPU_MEM_SYS_GL_FOLIAGE]     = "foliage",
        [GPU_MEM_SYS_GL_IMAGE_QUILT] = "image_quilt",
        [GPU_MEM_SYS_GL_MINIMAP]     = "minimap",
        [GPU_MEM_SYS_GL_MOVEMENT]    = "movement",
        [GPU_MEM_SYS_GL_POSITION]    = "position",
        [GPU_MEM_SYS_GL_RENDER]      = "render",
        [GPU_MEM_SYS_GL_RINGBUFFER]  = "ringbuffer",
        [GPU_MEM_SYS_GL_SHADOWS]     = "shadows",
        [GPU_MEM_SYS_GL_SKYBOX]      = "skybox",
        [GPU_MEM_SYS_GL_SPRITE]      = "sprite",
        [GPU_MEM_SYS_GL_STATUSBAR]   = "statusbar",
        [GPU_MEM_SYS_GL_SWAPCHAIN]   = "swapchain",
        [GPU_MEM_SYS_GL_TERRAIN]     = "terrain",
        [GPU_MEM_SYS_GL_TEXTURE]     = "texture",
        [GPU_MEM_SYS_GL_TILE]        = "tile",
        [GPU_MEM_SYS_GL_UI]          = "ui",
        [GPU_MEM_SYS_GL_WATER]       = "water",
    };
    if(sys < 0 || sys >= GPU_MEM_SYS_COUNT)
        return NULL;
    return names[sys];
}

const char *gpu_mem_sub_name(int sub)
{
    switch(sub) {
    case GPU_MEM_SUB_TEXTURE:      return "texture";
    case GPU_MEM_SUB_RENDERBUFFER: return "renderbuffer";
    case GPU_MEM_SUB_BUFFEROBJECT: return "bufferobject";
    default:                       return NULL;
    }
}

void R_GL_GetMemoryAccounting(struct gpu_mem_accounting *out)
{
    memset(out, 0, sizeof(*out));
#ifndef NDEBUG
    for(int s = 0; s < GPU_MEM_SYS_COUNT; s++) {
        for(int b = 0; b < GPU_MEM_SUB_COUNT; b++) {
            out->sub_bytes[s][b] = s_gpu[s][b].bytes;
            out->sub_count[s][b] = s_gpu[s][b].count;
            out->sys_bytes[s]   += s_gpu[s][b].bytes;
            out->sys_count[s]   += s_gpu[s][b].count;
        }
    }
#endif
}
