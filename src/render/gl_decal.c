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
#define MEM_FILE_SUB MEM_SUB_RENDER_GL_DECAL

#include "public/render.h"
#include "render_private.h"
#include "gl_render.h"
#include "gl_shader.h"
#include "gl_state.h"
#include "gl_texture.h"
#include "gl_assert.h"

#define GPU_MEM_FILE_SYS GPU_MEM_SYS_GL_DECAL
#include "gl_mem.h"
#include "gl_perf.h"
#include "../camera/public/camera.h"
#include "../main.h"
#include "../asset/public/asset_cache.h"
#include "../lib/public/stb_image.h"
#include "../lib/public/stb_image_resize.h"
#include "../lib/public/pf_string.h"
#include "../mem.h"

#include <GL/glew.h>

#include <string.h>
#include <assert.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_DECAL)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_DECAL)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_DECAL)

#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define MIN_ARRAY_LAYERS    (4)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static GLuint  *s_chunk_vaos    = NULL;
static GLuint  *s_chunk_vbos    = NULL;
static size_t  *s_chunk_counts  = NULL;
static size_t   s_num_chunks    = 0;

static struct texture_arr s_tex = {0};
static int      s_tex_layers    = 0;
static int      s_tex_res       = 0;
static GLint    s_old_depth_func;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static int num_mip_levels(int res)
{
    int ret = 1;
    while(res > 1) {
        res /= 2;
        ret++;
    }
    return ret;
}

static void array_alloc(int layers, struct texture_arr *out)
{
    out->tunit = DECAL_TUNIT;
    glActiveTexture(DECAL_TUNIT);
    glGenTextures(1, &out->id);
    glBindTexture(GL_TEXTURE_2D_ARRAY, out->id);

    int nmips = num_mip_levels(s_tex_res);
    for(int i = 0; i < nmips; i++) {
        int res = MAX(s_tex_res >> i, 1);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, i, GL_RGBA8, res, res, layers, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }

    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, nmips - 1);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    GL_ASSERT_OK();
}

static bool array_reserve(int layer)
{
    if(layer < s_tex_layers)
        return true;

    int new_layers = MAX(s_tex_layers * 2, MIN_ARRAY_LAYERS);
    while(new_layers <= layer)
        new_layers *= 2;

    GLint max_layers;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &max_layers);
    if(layer >= max_layers)
        return false;
    new_layers = MIN(new_layers, max_layers);

    struct texture_arr new_tex;
    array_alloc(new_layers, &new_tex);

    for(int i = 0; i < s_tex_layers; i++) {
        R_GL_Texture_ArrayCopyElem(&new_tex, i, &s_tex, i, s_tex_res);
    }
    if(s_tex.id) {
        glDeleteTextures(1, &s_tex.id);
    }

    s_tex = new_tex;
    s_tex_layers = new_layers;
    GL_ASSERT_OK();
    return true;
}

/* Returns 'res' x 'res' RGBA pixels which the caller must PF_FREE */
static unsigned char *decode_image(const char *abspath, const char *relpath, 
                                   const struct decal_segment *seg)
{
    const int res = s_tex_res;
    const size_t size = (size_t)res * res * 4;
    uint64_t tag = AssetCache_SourceTag(abspath);

    char key[512];
    pf_snprintf(key, sizeof(key), "%s.seg%dof%d", relpath, seg->index, seg->count);

    unsigned char *ret = PF_MALLOC(size);
    if(!ret)
        return NULL;

    struct texture_cache tc;
    if(AssetCache_TextureLoad(key, tag, &tc)) {

        bool match = (tc.width == res && tc.height == res && tc.channels == 4);
        if(match)
            memcpy(ret, tc.pixels, size);
        AssetCache_TextureRelease(&tc);
        if(match)
            return ret;
    }

    int w, h, n;
    unsigned char *data = stbi_load(abspath, &w, &h, &n, 4);
    if(!data) {
        PF_FREE(ret);
        return NULL;
    }

    /* The segment's band of the source image */
    const unsigned char *src = data;
    int sw = w, sh = h;
    if(seg->vertical) {
        int y0 = h * seg->index / seg->count;
        sh = h * (seg->index + 1) / seg->count - y0;
        src = data + (size_t)y0 * w * 4;
    }else{
        int x0 = w * seg->index / seg->count;
        sw = w * (seg->index + 1) / seg->count - x0;
        src = data + (size_t)x0 * 4;
    }

    int ok = stbir_resize_uint8_srgb(src, sw, sh, w * 4, ret, res, res, 0, 4, 3, 0);
    stbi_image_free(data);
    if(!ok) {
        PF_FREE(ret);
        return NULL;
    }

    AssetCache_TextureStore(key, tag, &(struct texture_cache){res, res, 4, ret});
    return ret;
}

static void upload_layer(int layer, const unsigned char *pixels)
{
    const int nmips = num_mip_levels(s_tex_res);
    unsigned char *mip = PF_MALLOC((size_t)s_tex_res * s_tex_res * 4);
    if(!mip)
        return;

    glActiveTexture(DECAL_TUNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_tex.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, s_tex_res, s_tex_res, 1,
        GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    /* Alpha-weighted filtering keeps transparent texels from bleeding dark
     * fringes into the rest. Each level halves the one before it. */
    const unsigned char *src = pixels;
    unsigned char *dst = mip;
    unsigned char *spare = mip + (size_t)(s_tex_res / 2) * (s_tex_res / 2) * 4;

    for(int i = 1; i < nmips; i++) {
        int src_res = MAX(s_tex_res >> (i - 1), 1);
        int res = MAX(s_tex_res >> i, 1);
        stbir_resize_uint8_srgb(src, src_res, src_res, 0, dst, res, res, 0, 4, 3, 0);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, i, 0, 0, layer, res, res, 1,
            GL_RGBA, GL_UNSIGNED_BYTE, dst);

        src = dst;
        dst = (dst == mip) ? spare : mip;
    }

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    PF_FREE(mip);
    GL_ASSERT_OK();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_DecalMapInit(const size_t *num_chunks, const int *res)
{
    ASSERT_IN_RENDER_THREAD();

    s_num_chunks   = *num_chunks;
    s_tex_res      = *res;
    s_tex_layers   = 0;
    s_tex          = (struct texture_arr){0};

    s_chunk_vaos   = PF_CALLOC(s_num_chunks, sizeof(GLuint));
    s_chunk_vbos   = PF_CALLOC(s_num_chunks, sizeof(GLuint));
    s_chunk_counts = PF_CALLOC(s_num_chunks, sizeof(size_t));
    if(!s_chunk_vaos || !s_chunk_vbos || !s_chunk_counts)
        goto fail_alloc;
    return;

fail_alloc:
    PF_FREE(s_chunk_vaos);
    PF_FREE(s_chunk_vbos);
    PF_FREE(s_chunk_counts);
    s_chunk_vaos   = NULL;
    s_chunk_vbos   = NULL;
    s_chunk_counts = NULL;
    s_num_chunks   = 0;
}

void R_GL_DecalMapShutdown(void)
{
    ASSERT_IN_RENDER_THREAD();

    for(size_t i = 0; i < s_num_chunks; i++) {
        if(s_chunk_vbos[i])
            glDeleteBuffers(1, &s_chunk_vbos[i]);
        if(s_chunk_vaos[i])
            glDeleteVertexArrays(1, &s_chunk_vaos[i]);
    }
    if(s_tex.id) {
        glDeleteTextures(1, &s_tex.id);
    }

    PF_FREE(s_chunk_vaos);
    PF_FREE(s_chunk_vbos);
    PF_FREE(s_chunk_counts);
    s_chunk_vaos   = NULL;
    s_chunk_vbos   = NULL;
    s_chunk_counts = NULL;
    s_num_chunks   = 0;

    s_tex          = (struct texture_arr){0};
    s_tex_layers   = 0;
    s_tex_res      = 0;
    GL_ASSERT_OK();
}

void R_GL_DecalSetTexture(const int *layer, const struct decal_segment *seg, 
                          const char *abspath, const char *relpath)
{
    ASSERT_IN_RENDER_THREAD();

    if(!array_reserve(*layer))
        return;

    unsigned char *pixels = decode_image(abspath, relpath, seg);
    if(!pixels)
        return;

    upload_layer(*layer, pixels);
    PF_FREE(pixels);
}

void R_GL_DecalSetChunk(const size_t *chunk_idx, const struct decal_vert *verts,
                        const size_t *count)
{
    ASSERT_IN_RENDER_THREAD();

    size_t idx = *chunk_idx;
    size_t n = *count;
    assert(idx < s_num_chunks);

    s_chunk_counts[idx] = n;

    if(n == 0) {
        if(s_chunk_vbos[idx]) {
            glDeleteBuffers(1, &s_chunk_vbos[idx]);
            s_chunk_vbos[idx] = 0;
        }
        if(s_chunk_vaos[idx]) {
            glDeleteVertexArrays(1, &s_chunk_vaos[idx]);
            s_chunk_vaos[idx] = 0;
        }
        return;
    }

    if(!s_chunk_vaos[idx]) {

        glGenVertexArrays(1, &s_chunk_vaos[idx]);
        glGenBuffers(1, &s_chunk_vbos[idx]);

        glBindVertexArray(s_chunk_vaos[idx]);
        glBindBuffer(GL_ARRAY_BUFFER, s_chunk_vbos[idx]);

        const GLsizei stride = sizeof(struct decal_vert);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
            (void*)offsetof(struct decal_vert, pos));
        glEnableVertexAttribArray(0);

        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
            (void*)offsetof(struct decal_vert, normal));
        glEnableVertexAttribArray(1);

        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
            (void*)offsetof(struct decal_vert, uv));
        glEnableVertexAttribArray(2);

        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride,
            (void*)offsetof(struct decal_vert, layer));
        glEnableVertexAttribArray(3);

        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride,
            (void*)offsetof(struct decal_vert, alpha));
        glEnableVertexAttribArray(4);

        glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, stride,
            (void*)offsetof(struct decal_vert, highlight));
        glEnableVertexAttribArray(5);

        glBindVertexArray(0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, s_chunk_vbos[idx]);
    glBufferData(GL_ARRAY_BUFFER, n * sizeof(struct decal_vert), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GL_ASSERT_OK();
}

void R_GL_DecalBeginDraw(const struct camera *cam, const struct map_resolution *res,
                         const vec2_t *map_pos)
{
    ASSERT_IN_RENDER_THREAD();

    GL_PERF_PUSH_GROUP(0, "map_decals");

    Camera_TickFinish((struct camera*)cam);

    R_GL_StateSet(GL_U_DECAL_TEX, (struct uval){
        .type       = UTYPE_INT,
        .val.as_int = DECAL_TUNIT - GL_TEXTURE0,
    });
    R_GL_Shader_Install("decal");
    R_GL_ShadowMapBind();

    GLuint prog = R_GL_Shader_GetProgForName("decal");
    R_GL_StateSet(GL_U_MAP_RES, (struct uval){
        .type            = UTYPE_IVEC4,
        .val.as_ivec4[0] = res->chunk_w,
        .val.as_ivec4[1] = res->chunk_h,
        .val.as_ivec4[2] = res->tile_w,
        .val.as_ivec4[3] = res->tile_h,
    });
    R_GL_StateInstall(GL_U_MAP_RES, prog);
    R_GL_StateSet(GL_U_MAP_POS, (struct uval){
        .type        = UTYPE_VEC2,
        .val.as_vec2 = *map_pos,
    });
    R_GL_StateInstall(GL_U_MAP_POS, prog);
    R_GL_MapFogBindLast(GL_TEXTURE5, prog, "visbuff");

    glActiveTexture(DECAL_TUNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_tex.id);

    glGetIntegerv(GL_DEPTH_FUNC, &s_old_depth_func);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glDisable(GL_CULL_FACE);
}

void R_GL_DecalDrawChunk(const size_t *chunk_idx)
{
    ASSERT_IN_RENDER_THREAD();

    size_t idx = *chunk_idx;
    assert(idx < s_num_chunks);

    size_t n = s_chunk_counts[idx];
    if(n == 0 || !s_chunk_vaos[idx] || !s_tex.id)
        return;

    glBindVertexArray(s_chunk_vaos[idx]);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)n);
    glBindVertexArray(0);
    GL_ASSERT_OK();
}

void R_GL_DecalEndDraw(void)
{
    ASSERT_IN_RENDER_THREAD();

    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glDepthFunc(s_old_depth_func);

    glActiveTexture(DECAL_TUNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    GL_PERF_POP_GROUP();
    GL_ASSERT_OK();
}
