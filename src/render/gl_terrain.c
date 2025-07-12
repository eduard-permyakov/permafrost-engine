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

#include "gl_render.h"
#include "gl_texture.h"
#include "gl_shader.h"
#include "gl_ringbuffer.h"
#include "gl_assert.h"
#include "gl_state.h"
#include "gl_perf.h"
#include "gl_image_quilt.h"
#include "render_private.h"
#include "../main.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/mem.h"
#include "../lib/public/noise.h"

#include <assert.h>
#include <string.h>

#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))
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

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static size_t                 s_num_arrays;
static uint32_t               s_materials_hash;
static struct texture_arr     s_map_textures[4];
static bool                   s_map_ctx_active = false;
static struct gl_ring        *s_fog_ring;
static struct gl_heightmap    s_heightmap;
static struct gl_splatmap     s_splatmap;
static struct map_resolution  s_res;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void create_height_map(void)
{
    ASSERT_IN_RENDER_THREAD();
    
    size_t buffsize = sizeof(float) * HEIGHT_MAP_RES * HEIGHT_MAP_RES;
    float *data = malloc(buffsize);
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
    float *data = malloc(buffsize);
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
            *num_textures, s_map_textures, GL_TEXTURE0);
        s_materials_hash = materials_hash_adler32(map_texfiles, *num_textures);

        init_once_done = true;
    }else{

        uint32_t new_materials_hash = materials_hash_adler32(map_texfiles, *num_textures);
        if(new_materials_hash != s_materials_hash) {

            for(int i = 0; i < s_num_arrays; i++) {
                R_GL_Texture_ArrayFree(s_map_textures[i]);
            }
            s_num_arrays = R_GL_Texture_ArrayMakeMapWangTileset(map_texfiles, 
                *num_textures, s_map_textures, GL_TEXTURE0);
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
    void *buff = malloc(size);
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
    }
    R_GL_RingbufferBindLast(s_fog_ring, GL_TEXTURE5, shader_prog, "visbuff");

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

