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


#include "gl_render.h"
#include "gl_shader.h"
#include "gl_state.h"
#include "gl_texture.h"
#include "gl_assert.h"
#include "gl_perf.h"
#include "render_private.h"
#include "public/render.h"
#include "../map/public/map.h"
#include "../config.h"
#include "../main.h"

#include <assert.h>

#define GPU_MEM_FILE_SYS GPU_MEM_SYS_GL_TERRAIN
#include "gl_mem.h"

#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))
#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#define MAX(a, b)   ((a) > (b) ? (a) : (b))

/* Modest anisotropy is enough: the RTS camera foreshortens the ground by at most
 * a factor of about two, and the layers are otherwise sampled near 1:1.
 */
#define BAKE_ANISOTROPY (4.0f)

struct bake_ctx{
    int    res;
    bool   compressed;
    int    nlevels;
    /* One layer per resident chunk, allocated on the first bake */
    GLuint array;
    /* A chunk is rendered here, mipmapped, then copied into its layer */
    GLuint scratch;
    GLuint depth_rb;
    GLuint fbo;
    GLuint copy_fbo;
    /* Compressed layers: a level is encoded to BC3 blocks in an integer texture,
     * read into the PBO and uploaded from there, all on the GPU.
     */
    GLuint block_tex;
    GLuint block_fbo;
    GLuint pbo;
    GLuint empty_vao;
    GLint  prog_src;
    GLint  prog_bc3;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct bake_ctx s_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static int mip_levels(int res)
{
    int nlevels = 1;
    while(res > 1) {
        res >>= 1;
        nlevels++;
    }
    return nlevels;
}

static void free_targets(void)
{
    if(!s_ctx.array)
        return;

    glDeleteTextures(1, &s_ctx.array);
    glDeleteTextures(1, &s_ctx.scratch);
    glDeleteRenderbuffers(1, &s_ctx.depth_rb);
    s_ctx.array = 0;
    s_ctx.scratch = 0;
    s_ctx.depth_rb = 0;

    if(s_ctx.block_tex) {
        glDeleteTextures(1, &s_ctx.block_tex);
        glDeleteBuffers(1, &s_ctx.pbo);
        s_ctx.block_tex = 0;
        s_ctx.pbo = 0;
    }
}

static void alloc_block_targets(void)
{
    int bdim = s_ctx.res / 4;

    glGenTextures(1, &s_ctx.block_tex);
    glBindTexture(GL_TEXTURE_2D, s_ctx.block_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, bdim, bdim, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, s_ctx.block_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_ctx.block_tex, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glGenBuffers(1, &s_ctx.pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_ctx.pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (size_t)bdim * bdim * 16, NULL, GL_STREAM_COPY);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

/* Must be called with the bake's texture unit active and the FBO binding saved */
static void alloc_targets(void)
{
    int res = s_ctx.res;
    s_ctx.nlevels = mip_levels(res);
    GLenum ifmt = s_ctx.compressed ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_RGBA8;

    glGenTextures(1, &s_ctx.array);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_ctx.array);
    for(int lvl = 0; lvl < s_ctx.nlevels; lvl++) {
        int dim = MAX(res >> lvl, 1);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, lvl, ifmt, dim, dim, CONFIG_TERRAIN_BAKE_LAYERS,
            0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, s_ctx.nlevels - 1);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if(GLEW_EXT_texture_filter_anisotropic) {
        GLfloat max_aniso = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_aniso);
        glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY_EXT, MIN(BAKE_ANISOTROPY, max_aniso));
    }else{
        glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_LOD_BIAS, LOD_BIAS);
    }

    glGenTextures(1, &s_ctx.scratch);
    glBindTexture(GL_TEXTURE_2D, s_ctx.scratch);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, res, res, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_2D);

    glGenRenderbuffers(1, &s_ctx.depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, s_ctx.depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, res, res);

    glBindFramebuffer(GL_FRAMEBUFFER, s_ctx.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_ctx.scratch, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_ctx.depth_rb);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    if(s_ctx.compressed) {
        alloc_block_targets();
    }

    glBindTexture(GL_TEXTURE_2D_ARRAY, s_ctx.array);
    GL_ASSERT_OK();
}

/* Encode a level of the scratch bake, bound as GL_TEXTURE_2D on the active unit */
static void encode_level(int lvl, int layer)
{
    int dim = MAX(s_ctx.res >> lvl, 1);
    int bdim = (dim + 3) / 4;

    R_GL_StatePushRenderTarget(s_ctx.block_fbo);
    glViewport(0, 0, bdim, bdim);

    R_GL_StateSet(GL_U_BC3_SRC_TEX, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = TERRAIN_BAKE_TUNIT - GL_TEXTURE0
    });
    R_GL_StateSet(GL_U_BC3_SRC_LEVEL, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = lvl
    });
    R_GL_Shader_InstallProg(s_ctx.prog_bc3);
    glBindVertexArray(s_ctx.empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_ctx.pbo);
    glReadPixels(0, 0, bdim, bdim, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    R_GL_StatePopRenderTarget();

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, s_ctx.pbo);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_ctx.array);
    glCompressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl, 0, 0, layer, dim, dim, 1,
        GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, bdim * bdim * 16, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

static void copy_level(int lvl, int layer)
{
    int dim = MAX(s_ctx.res >> lvl, 1);

    if(GLEW_ARB_copy_image) {
        glCopyImageSubData(s_ctx.scratch, GL_TEXTURE_2D, lvl, 0, 0, 0,
                           s_ctx.array, GL_TEXTURE_2D_ARRAY, lvl, 0, 0, layer, dim, dim, 1);
        return;
    }

    R_GL_StatePushRenderTarget(s_ctx.copy_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_ctx.scratch, lvl);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_ctx.array);
    glCopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl, 0, 0, layer, 0, 0, dim, dim);
    R_GL_StatePopRenderTarget();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_TerrainBakeInit(void)
{
    ASSERT_IN_RENDER_THREAD();

    s_ctx = (struct bake_ctx){0};
    s_ctx.res = 2048;
    s_ctx.compressed = GLEW_EXT_texture_compression_s3tc;
    s_ctx.prog_src = R_GL_Shader_GetProgForName("terrain-bake-src");
    s_ctx.prog_bc3 = R_GL_Shader_GetProgForName("terrain-bake-bc3");
    if(s_ctx.prog_src == -1 || s_ctx.prog_bc3 == -1)
        return false;

    glGenFramebuffers(1, &s_ctx.fbo);
    glGenFramebuffers(1, &s_ctx.copy_fbo);
    glGenFramebuffers(1, &s_ctx.block_fbo);
    glGenVertexArrays(1, &s_ctx.empty_vao);
    GL_ASSERT_OK();
    return true;
}

void R_GL_TerrainBakeShutdown(void)
{
    ASSERT_IN_RENDER_THREAD();

    glActiveTexture(TERRAIN_BAKE_TUNIT);
    free_targets();
    glDeleteFramebuffers(1, &s_ctx.fbo);
    glDeleteFramebuffers(1, &s_ctx.copy_fbo);
    glDeleteFramebuffers(1, &s_ctx.block_fbo);
    glDeleteVertexArrays(1, &s_ctx.empty_vao);
}

void R_GL_TerrainBakeBind(void)
{
    ASSERT_IN_RENDER_THREAD();

    glActiveTexture(TERRAIN_BAKE_TUNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_ctx.array);
    R_GL_StateSet(GL_U_BAKE_TEX, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = TERRAIN_BAKE_TUNIT - GL_TEXTURE0
    });
}

void R_GL_TerrainBakeConfigure(const int *res, const bool *compress)
{
    ASSERT_IN_RENDER_THREAD();

    glActiveTexture(TERRAIN_BAKE_TUNIT);
    free_targets();
    s_ctx.res = *res;
    s_ctx.compressed = *compress && GLEW_EXT_texture_compression_s3tc;
    GL_ASSERT_OK();
}

void R_GL_TerrainBakeSlice(const struct terrain_bake_slice *slice)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    GLint saved_viewport[4];
    GLint saved_fb, saved_rb;
    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fb);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &saved_rb);

    const char *saved_unames[] = {
        GL_U_VIEW, GL_U_VIEW_POS, GL_U_PROJECTION, GL_U_SHADOWS_ON
    };
    struct uval saved_uvals[ARR_SIZE(saved_unames)];
    bool have_uval[ARR_SIZE(saved_unames)];
    for(int i = 0; i < ARR_SIZE(saved_unames); i++) {
        have_uval[i] = R_GL_StateGet(saved_unames[i], &saved_uvals[i]);
    }

    glActiveTexture(TERRAIN_BAKE_TUNIT);
    if(!s_ctx.array) {
        alloc_targets();
    }

    int cell = s_ctx.res / CONFIG_TERRAIN_BAKE_SLICE_GRID;
    glBindFramebuffer(GL_FRAMEBUFFER, s_ctx.fbo);
    glViewport(0, 0, s_ctx.res, s_ctx.res);
    glEnable(GL_SCISSOR_TEST);
    glScissor((slice->slice % CONFIG_TERRAIN_BAKE_SLICE_GRID) * cell, 
              (slice->slice / CONFIG_TERRAIN_BAKE_SLICE_GRID) * cell, cell, cell);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);

    vec3_t eye = (vec3_t){0.0f, 0.0f, 0.0f};
    R_GL_SetViewMatAndPos(&slice->frame.view, &eye);
    R_GL_SetProj(&slice->frame.proj);
    R_GL_StateSet(GL_U_BAKE_VIEW_DIR, (struct uval){
        .type = UTYPE_VEC3,
        .val.as_vec3 = slice->view_dir
    });

    const bool shadows = false;
    size_t num_splats = slice->num_splats;
    R_GL_MapBegin(&shadows, &slice->map_pos, &num_splats, &slice->splatmap);
    for(int i = 0; i < slice->num_chunks; i++) {
        R_GL_DrawChunkTops(slice->chunk_rprivates[i], &slice->chunk_models[i], s_ctx.prog_src);
    }
    R_GL_MapEnd();

    glDisable(GL_SCISSOR_TEST);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, saved_fb);
    glBindRenderbuffer(GL_RENDERBUFFER, saved_rb);

    for(int i = 0; i < ARR_SIZE(saved_unames); i++) {
        if(have_uval[i]) {
            R_GL_StateSet(saved_unames[i], saved_uvals[i]);
        }
    }

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_TerrainBakeFinish(const int *layer)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();
    assert(s_ctx.array);
    assert(*layer >= 0 && *layer < CONFIG_TERRAIN_BAKE_LAYERS);

    GLint saved_viewport[4];
    glGetIntegerv(GL_VIEWPORT, saved_viewport);

    glActiveTexture(TERRAIN_BAKE_TUNIT);
    glBindTexture(GL_TEXTURE_2D, s_ctx.scratch);
    glGenerateMipmap(GL_TEXTURE_2D);

    for(int lvl = 0; lvl < s_ctx.nlevels; lvl++) {
        if(s_ctx.compressed) {
            encode_level(lvl, *layer);
        }else{
            copy_level(lvl, *layer);
        }
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_ctx.array);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}
