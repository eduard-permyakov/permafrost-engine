/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2023 Eduard Permyakov 
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
#define MEM_FILE_SUB MEM_SUB_RENDER_GL_TERRAIN

#include "gl_render.h"
#include "gl_texture.h"
#include "gl_shader.h"
#include "gl_ringbuffer.h"
#include "gl_assert.h"

#define GPU_MEM_FILE_SYS GPU_MEM_SYS_GL_TERRAIN
#include "gl_mem.h"
#include "gl_state.h"
#include "gl_perf.h"
#include "gl_image_quilt.h"
#include "render_private.h"
#include "public/render.h"
#include "../config.h"
#include "../main.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../perf/public/mem.h"
#include "../lib/public/noise.h"

#include <assert.h>
#include <string.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_TERRAIN)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_TERRAIN)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_TERRAIN)

#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define HEIGHT_MAP_RES  (2048)
#define SPLAT_MAP_RES   (1024)

struct gl_heightmap{
    GLuint buffer;
    GLuint tex_buff;
};

struct gl_splatmap{
    GLuint buffer;
    GLuint tex_buff;
};

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

static size_t                 s_num_arrays;
static uint32_t               s_materials_hash;
static struct texture_arr     s_map_textures[4];
static struct texture_arr     s_map_normals[4];
static bool                   s_map_ctx_active = false;
static struct gl_ring        *s_fog_ring;
static struct gl_heightmap    s_heightmap;
static struct gl_splatmap     s_splatmap;
static struct map_resolution  s_res;
static struct bake_ctx        s_bake;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void create_height_map(void)
{
    ASSERT_IN_RENDER_THREAD();
    
    size_t buffsize = sizeof(float) * HEIGHT_MAP_RES * HEIGHT_MAP_RES;
    float *data = PF_MALLOC(buffsize);
    if(!data)
        return;

    Noise_GenerateOctavePerlinTile2D(HEIGHT_MAP_RES, HEIGHT_MAP_RES, 1/128.0, 4, 0.5, data);
    Noise_Normalize2D(HEIGHT_MAP_RES, HEIGHT_MAP_RES, data);

    glGenBuffers(1, &s_heightmap.buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, s_heightmap.buffer);
    glBufferStorage(GL_TEXTURE_BUFFER, buffsize, data, 0);

    glGenTextures(1, &s_heightmap.tex_buff);
    glBindTexture(GL_TEXTURE_BUFFER, s_heightmap.tex_buff);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, s_heightmap.buffer);

    PF_FREE(data);
    GL_ASSERT_OK();
}

static void create_splat_map(void)
{
    ASSERT_IN_RENDER_THREAD();
    
    size_t buffsize = sizeof(float) * SPLAT_MAP_RES * SPLAT_MAP_RES;
    float *data = PF_MALLOC(buffsize);
    if(!data)
        return;

    Noise_GenerateOctavePerlinTile2D(SPLAT_MAP_RES, SPLAT_MAP_RES, 1/128.0, 4, 0.5, data);

    glGenBuffers(1, &s_splatmap.buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, s_splatmap.buffer);
    glBufferStorage(GL_TEXTURE_BUFFER, buffsize, data, 0);

    glGenTextures(1, &s_splatmap.tex_buff);
    glBindTexture(GL_TEXTURE_BUFFER, s_splatmap.tex_buff);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, s_splatmap.buffer);

    PF_FREE(data);
    GL_ASSERT_OK();
}

static uint32_t materials_hash_adler32(const char map_texfiles[][256], size_t num_textures) 
{
    uint32_t s1 = 1;
    uint32_t s2 = 0;

    size_t i = 0;
    while(num_textures-- > 0) {

        const char *cursor = map_texfiles[i++];
        while(*cursor) {
            s1 = (s1 + *cursor) % 65521;
            s2 = (s2 + s1) % 65521;
            cursor++;
        }
    }
    return (s2 << 16) | s1;
}

static int mip_levels(int res)
{
    int nlevels = 1;
    while(res > 1) {
        res >>= 1;
        nlevels++;
    }
    return nlevels;
}

static void bake_free_targets(void)
{
    if(!s_bake.array)
        return;

    glDeleteTextures(1, &s_bake.array);
    glDeleteTextures(1, &s_bake.scratch);
    glDeleteRenderbuffers(1, &s_bake.depth_rb);
    s_bake.array = 0;
    s_bake.scratch = 0;
    s_bake.depth_rb = 0;

    if(s_bake.block_tex) {
        glDeleteTextures(1, &s_bake.block_tex);
        glDeleteBuffers(1, &s_bake.pbo);
        s_bake.block_tex = 0;
        s_bake.pbo = 0;
    }
}

static void bake_alloc_block_targets(void)
{
    int bdim = s_bake.res / 4;

    glGenTextures(1, &s_bake.block_tex);
    glBindTexture(GL_TEXTURE_2D, s_bake.block_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, bdim, bdim, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, s_bake.block_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_bake.block_tex, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glGenBuffers(1, &s_bake.pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_bake.pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (size_t)bdim * bdim * 16, NULL, GL_STREAM_COPY);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

/* Must be called with the bake's texture unit active and the FBO binding saved */
static void bake_alloc_targets(void)
{
    int res = s_bake.res;
    s_bake.nlevels = mip_levels(res);
    GLenum ifmt = s_bake.compressed ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_RGBA8;

    glGenTextures(1, &s_bake.array);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_bake.array);
    for(int lvl = 0; lvl < s_bake.nlevels; lvl++) {
        int dim = MAX(res >> lvl, 1);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, lvl, ifmt, dim, dim, CONFIG_TERRAIN_BAKE_LAYERS,
            0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, s_bake.nlevels - 1);
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

    glGenTextures(1, &s_bake.scratch);
    glBindTexture(GL_TEXTURE_2D, s_bake.scratch);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, res, res, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_2D);

    glGenRenderbuffers(1, &s_bake.depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, s_bake.depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, res, res);

    glBindFramebuffer(GL_FRAMEBUFFER, s_bake.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_bake.scratch, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_bake.depth_rb);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    if(s_bake.compressed) {
        bake_alloc_block_targets();
    }

    glBindTexture(GL_TEXTURE_2D_ARRAY, s_bake.array);
    GL_ASSERT_OK();
}

/* Encode a level of the scratch bake, bound as GL_TEXTURE_2D on the active unit */
static void bake_encode_level(int lvl, int layer)
{
    int dim = MAX(s_bake.res >> lvl, 1);
    int bdim = (dim + 3) / 4;

    R_GL_StatePushRenderTarget(s_bake.block_fbo);
    glViewport(0, 0, bdim, bdim);

    R_GL_StateSet(GL_U_BC3_SRC_TEX, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = TERRAIN_BAKE_TUNIT - GL_TEXTURE0
    });
    R_GL_StateSet(GL_U_BC3_SRC_LEVEL, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = lvl
    });
    R_GL_Shader_InstallProg(s_bake.prog_bc3);
    glBindVertexArray(s_bake.empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_bake.pbo);
    glReadPixels(0, 0, bdim, bdim, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    R_GL_StatePopRenderTarget();

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, s_bake.pbo);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_bake.array);
    glCompressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl, 0, 0, layer, dim, dim, 1,
        GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, bdim * bdim * 16, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

static void bake_copy_level(int lvl, int layer)
{
    int dim = MAX(s_bake.res >> lvl, 1);

    if(GLEW_ARB_copy_image) {
        glCopyImageSubData(s_bake.scratch, GL_TEXTURE_2D, lvl, 0, 0, 0,
                           s_bake.array, GL_TEXTURE_2D_ARRAY, lvl, 0, 0, layer, dim, dim, 1);
        return;
    }

    R_GL_StatePushRenderTarget(s_bake.copy_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_bake.scratch, lvl);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_bake.array);
    glCopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl, 0, 0, layer, 0, 0, dim, dim);
    R_GL_StatePopRenderTarget();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_MapInit(const char map_texfiles[][256], const size_t *num_textures, 
                  const struct map_resolution *res)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    size_t nchunks = res->chunk_w * res->chunk_h;
    static bool init_once_done = false;

    /* Even though maps can change dynamically, most of our OpenGL resources
     * can be re-used between different maps, so initialize them just once. 
     */
    if(!init_once_done) {

        create_height_map();
        create_splat_map();

        s_num_arrays = R_GL_Texture_ArrayMakeMapWangTileset(map_texfiles,
            *num_textures, s_map_textures, s_map_normals, GL_TEXTURE0);
        s_materials_hash = materials_hash_adler32(map_texfiles, *num_textures);

        init_once_done = true;
    }else{

        uint32_t new_materials_hash = materials_hash_adler32(map_texfiles, *num_textures);
        if(new_materials_hash != s_materials_hash) {

            for(int i = 0; i < s_num_arrays; i++) {
                R_GL_Texture_ArrayFree(s_map_textures[i]);
                R_GL_Texture_ArrayFree(s_map_normals[i]);
            }
            s_num_arrays = R_GL_Texture_ArrayMakeMapWangTileset(map_texfiles,
                *num_textures, s_map_textures, s_map_normals, GL_TEXTURE0);
            s_materials_hash = new_materials_hash;
        }
    }

    s_fog_ring = R_GL_RingbufferInit(
        nchunks * TILES_PER_CHUNK_WIDTH * TILES_PER_CHUNK_HEIGHT * 3, RING_UBYTE);
    assert(s_fog_ring);

    R_GL_StateSet(GL_U_HEIGHT_MAP, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = HEIGHT_MAP_TUNIT - GL_TEXTURE0
    });

    R_GL_StateSet(GL_U_SPLAT_MAP, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = SPLAT_MAP_TUNIT - GL_TEXTURE0
    });

    R_GL_StateSet(GL_U_SKYBOX, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = SKYBOX_TUNIT - GL_TEXTURE0
    });

    R_GL_StateSet(GL_U_MAP_RES, (struct uval){
        .type = UTYPE_IVEC4,
        .val.as_ivec4[0] = res->chunk_w, 
        .val.as_ivec4[1] = res->chunk_h,
        .val.as_ivec4[2] = res->tile_w,
        .val.as_ivec4[3] = res->tile_h
    });

    s_res = *res;
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_MapUpdateFog(void *buff, const size_t *size)
{
    GL_PERF_ENTER();
    R_GL_RingbufferPush(s_fog_ring, buff, *size);
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_MapShutdown(void)
{
    /* Leave the mao textures and heightmaps. They can be
     * reused between different maps 
     */
    R_GL_RingbufferDestroy(s_fog_ring);
}

/* Push a fully 'visible' field into the ringbuffer. Must be followed
 * with a matching R_GL_MapInvalidate to consume the fence. */
void R_GL_MapUpdateFogClear(void)
{
    size_t size = s_res.chunk_w * s_res.chunk_h * s_res.tile_w * s_res.tile_h;
    void *buff = PF_MALLOC(size);
    memset(buff, 0x2, size);
    R_GL_RingbufferPush(s_fog_ring, buff, size);
    PF_FREE(buff);
}

void R_GL_MapBegin(const bool *shadows, const vec2_t *pos,
                   size_t *num_splats, const struct splatmap *splatmap)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();
    GL_PERF_PUSH_GROUP(0, "map");
    assert(!s_map_ctx_active);

    R_GL_StateSet(GL_U_MAP_POS, (struct uval){
        .type = UTYPE_VEC2,
        .val.as_vec2 = *pos
    });

    GLint splatbuff[MAX_MAP_TEXTURES];
    for(int i = 0; i < MAX_MAP_TEXTURES; i++) {
        splatbuff[i] = -1;
    }
    for(int i = 0; i < *num_splats; i++) {
        GLint base_idx = splatmap->splats[i].base_mat_idx;
        GLint accent_idx = splatmap->splats[i].accent_mat_idx;

        assert(base_idx < MAX_MAP_TEXTURES);
        assert(accent_idx < MAX_MAP_TEXTURES);

        splatbuff[base_idx] = accent_idx;
    }
    R_GL_StateSetArray(GL_U_SPLATS, UTYPE_INT, MAX_MAP_TEXTURES, splatbuff);
    R_GL_SetShadowsEnabled(shadows);

    GLuint shader_prog = R_GL_Shader_GetProgForName("terrain-shadowed");
    assert(shader_prog != -1);
    R_GL_Shader_InstallProg(shader_prog);

    for(int i = 0; i < s_num_arrays; i++) {
        R_GL_Texture_BindArray(&s_map_textures[i], shader_prog);
        R_GL_Texture_BindArrayNormal(&s_map_normals[i], shader_prog);
    }
    R_GL_RingbufferBindLast(s_fog_ring, GL_TEXTURE5, shader_prog, "visbuff");
    R_GL_TerrainBakeBind();

    glActiveTexture(HEIGHT_MAP_TUNIT);
    glBindTexture(GL_TEXTURE_BUFFER, s_heightmap.tex_buff);

    glActiveTexture(SPLAT_MAP_TUNIT);
    glBindTexture(GL_TEXTURE_BUFFER, s_splatmap.tex_buff);

    R_GL_SkyboxBind();

    s_map_ctx_active = true;
    GL_PERF_RETURN_VOID();
}

void R_GL_MapEnd(void)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    assert(s_map_ctx_active);
    s_map_ctx_active = false;

    GL_PERF_POP_GROUP();
    GL_PERF_RETURN_VOID();
}

void R_GL_MapInvalidate(void)
{
    GL_PERF_ENTER();
    R_GL_RingbufferSyncLast(s_fog_ring);
    GL_PERF_RETURN_VOID();
}

void R_GL_MapFogBindLast(GLuint tunit, GLuint shader_prog, const char *uname)
{
    R_GL_RingbufferBindLast(s_fog_ring, tunit, shader_prog, uname);
}

bool R_GL_TerrainBakeInit(void)
{
    ASSERT_IN_RENDER_THREAD();

    s_bake = (struct bake_ctx){0};
    s_bake.res = 2048;
    s_bake.compressed = GLEW_EXT_texture_compression_s3tc;
    s_bake.prog_src = R_GL_Shader_GetProgForName("terrain-bake-src");
    s_bake.prog_bc3 = R_GL_Shader_GetProgForName("terrain-bake-bc3");
    if(s_bake.prog_src == -1 || s_bake.prog_bc3 == -1)
        return false;

    glGenFramebuffers(1, &s_bake.fbo);
    glGenFramebuffers(1, &s_bake.copy_fbo);
    glGenFramebuffers(1, &s_bake.block_fbo);
    glGenVertexArrays(1, &s_bake.empty_vao);
    GL_ASSERT_OK();
    return true;
}

void R_GL_TerrainBakeShutdown(void)
{
    ASSERT_IN_RENDER_THREAD();

    glActiveTexture(TERRAIN_BAKE_TUNIT);
    bake_free_targets();
    glDeleteFramebuffers(1, &s_bake.fbo);
    glDeleteFramebuffers(1, &s_bake.copy_fbo);
    glDeleteFramebuffers(1, &s_bake.block_fbo);
    glDeleteVertexArrays(1, &s_bake.empty_vao);
}

void R_GL_TerrainBakeBind(void)
{
    ASSERT_IN_RENDER_THREAD();

    glActiveTexture(TERRAIN_BAKE_TUNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_bake.array);
    R_GL_StateSet(GL_U_BAKE_TEX, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = TERRAIN_BAKE_TUNIT - GL_TEXTURE0
    });
}

void R_GL_TerrainBakeConfigure(const int *res, const bool *compress)
{
    ASSERT_IN_RENDER_THREAD();

    glActiveTexture(TERRAIN_BAKE_TUNIT);
    bake_free_targets();
    s_bake.res = *res;
    s_bake.compressed = *compress && GLEW_EXT_texture_compression_s3tc;
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
    if(!s_bake.array) {
        bake_alloc_targets();
    }

    int cell = s_bake.res / CONFIG_TERRAIN_BAKE_SLICE_GRID;
    glBindFramebuffer(GL_FRAMEBUFFER, s_bake.fbo);
    glViewport(0, 0, s_bake.res, s_bake.res);
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
        R_GL_DrawChunkTops(slice->chunk_rprivates[i], &slice->chunk_models[i], s_bake.prog_src);
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
    assert(s_bake.array);
    assert(*layer >= 0 && *layer < CONFIG_TERRAIN_BAKE_LAYERS);

    GLint saved_viewport[4];
    glGetIntegerv(GL_VIEWPORT, saved_viewport);

    glActiveTexture(TERRAIN_BAKE_TUNIT);
    glBindTexture(GL_TEXTURE_2D, s_bake.scratch);
    glGenerateMipmap(GL_TEXTURE_2D);

    for(int lvl = 0; lvl < s_bake.nlevels; lvl++) {
        if(s_bake.compressed) {
            bake_encode_level(lvl, *layer);
        }else{
            bake_copy_level(lvl, *layer);
        }
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_bake.array);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}
