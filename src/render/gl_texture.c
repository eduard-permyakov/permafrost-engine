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

#include "gl_texture.h"
#include "gl_state.h"
#include "gl_assert.h"
#include "gl_material.h"
#include "../lib/public/stb_image.h"
#include "../lib/public/stb_image_resize.h"
#include "../lib/public/khash.h"
#include "../lib/public/pf_string.h"
#include "../config.h"
#include "../main.h"

#include <string.h>
#include <assert.h>
#include <math.h>


#define LOD_BIAS (-0.5f)

#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define MAX3(a, b, c)   (MAX((a), MAX((b), (c))))
#define ARR_SIZE(a)     (sizeof(a)/sizeof((a)[0]))

KHASH_MAP_INIT_STR(tex, GLuint)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(tex) *s_name_tex_table;
static GLuint        s_null_tex;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool texture_gl_init(const char *path, GLuint *out)
{
    ASSERT_IN_RENDER_THREAD();

    GLuint ret;
    int width, height, nr_channels;
    unsigned char *data;
    
    data = stbi_load(path, &width, &height, &nr_channels, 0);
    if(!data)
        goto fail_load;

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &ret);
    glBindTexture(GL_TEXTURE_2D, ret);

    if(nr_channels != 3 && nr_channels != 4)
        goto fail_format;

    GLint format = (nr_channels == 3) ? GL_RGB : GL_RGBA;
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, LOD_BIAS);

    stbi_image_free(data);
    *out = ret;
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;

fail_format:
    stbi_image_free(data);
fail_load:
    glBindTexture(GL_TEXTURE_2D, 0);
    return false;
}

static void texture_make_null(GLuint *out)
{
    ASSERT_IN_RENDER_THREAD();

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, out);
    glBindTexture(GL_TEXTURE_2D, *out);

    unsigned char data[] = {0, 0, 0};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, LOD_BIAS);

    glBindTexture(GL_TEXTURE_2D, 0);
    GL_ASSERT_OK();
}

static size_t texture_arr_num_mip_levels(GLuint tex)
{
    int max_lvl;
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glGetTexParameteriv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, &max_lvl);

    int w, h, d;
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &h);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_DEPTH, &d);

    size_t nmips = 1 + floor(log2(MAX3(w, h, d)));

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return MIN(max_lvl + 1, nmips);
}

static bool texture_write_ppm(const char* filename, const unsigned char *data, int width, int height)
{
    FILE* file = fopen(filename, "wb");
    if (!file)
        return false;

    fprintf(file, "P6\n%d %d\n%d\n", width, height, 255);
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {

            unsigned char color[3];
            color[0] = data[3 * i * width + 3 * j + 0];
            color[1] = data[3 * i * width + 3 * j + 1];
            color[2] = data[3 * i * width + 3 * j + 2];
            fwrite(color, 1, 3, file);
        }
    }

    fclose(file);
    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_Texture_Init(void)
{
    ASSERT_IN_RENDER_THREAD();

    s_name_tex_table = kh_init(tex);
    texture_make_null(&s_null_tex);

    return (s_name_tex_table != NULL);
}

void R_GL_Texture_Shutdown(void)
{
    ASSERT_IN_RENDER_THREAD();

    const char *key;
    GLuint curr;

    kh_foreach(s_name_tex_table, key, curr, {
        glDeleteTextures(1, &curr); 
        free((void*)key);
    });
    kh_destroy(tex, s_name_tex_table);
    glDeleteTextures(1, &s_null_tex); 
}

bool R_GL_Texture_GetForName(const char *basedir, const char *name, GLuint *out)
{
    ASSERT_IN_RENDER_THREAD();

    char qualname[512];
    pf_snprintf(qualname, sizeof(qualname), "%s/%s", basedir, name);

    khiter_t k;
    if((k = kh_get(tex, s_name_tex_table, qualname)) == kh_end(s_name_tex_table)) {
        *out = s_null_tex;
        return false;
    }

    *out = kh_val(s_name_tex_table, k);
    return true;
}

bool R_GL_Texture_Load(const char *basedir, const char *name, GLuint *out)
{
    ASSERT_IN_RENDER_THREAD();

    GLuint ret;
    khiter_t k;
    char texture_path[512], texture_path_maps[512];

    if((k = kh_get(tex, s_name_tex_table, name)) != kh_end(s_name_tex_table))
        goto fail;

    if(basedir) {
        pf_snprintf(texture_path, sizeof(texture_path), "%s/%s", basedir, name);
    }else{
        texture_path[0] = '\0';
    }
    pf_snprintf(texture_path_maps, sizeof(texture_path_maps), "%s/assets/map_textures/%s", g_basepath, name);

    if(!texture_gl_init(texture_path, &ret)
    && !texture_gl_init(texture_path_maps, &ret))
        goto fail;

    int put_ret;
    k = kh_put(tex, s_name_tex_table, pf_strdup(texture_path), &put_ret);
    assert(put_ret != -1 && put_ret != 0);
    kh_value(s_name_tex_table, k) = ret;

    *out = ret;
    GL_ASSERT_OK();
    return true;

fail:
    return false;
}

bool R_GL_Texture_AddExisting(const char *name, GLuint id)
{
    ASSERT_IN_RENDER_THREAD();

    khiter_t k;
    if((k = kh_get(tex, s_name_tex_table, name)) != kh_end(s_name_tex_table))
        return false;

    int put_ret;
    k = kh_put(tex, s_name_tex_table, pf_strdup(name), &put_ret);
    assert(put_ret != -1 && put_ret != 0);
    kh_value(s_name_tex_table, k) = id;
    return true;
}

void R_GL_Texture_Free(const char *basedir, const char *name)
{
    ASSERT_IN_RENDER_THREAD();

    char qualname[512];
    if(basedir) {
        pf_snprintf(qualname, sizeof(qualname), "%s/%s", basedir, name);
    }else{
        pf_strlcpy(qualname, name, sizeof(qualname));
    }

    khiter_t k;
    if((k = kh_get(tex, s_name_tex_table, qualname)) != kh_end(s_name_tex_table)) {

        GLuint id = kh_val(s_name_tex_table, k);
        glDeleteTextures(1, &id);
        free((void*)kh_key(s_name_tex_table, k));
        kh_del(tex, s_name_tex_table, k);
    }

    GL_ASSERT_OK();
}

void R_GL_Texture_Bind(const struct texture *text, GLuint shader_prog)
{
    ASSERT_IN_RENDER_THREAD();

    glActiveTexture(text->tunit);
    glBindTexture(GL_TEXTURE_2D, text->id);
    GLint sampler = text->tunit - GL_TEXTURE0;

    const char *uname_table[] = {
        GL_U_TEXTURE0,
        GL_U_TEXTURE1,
        GL_U_TEXTURE2,
        GL_U_TEXTURE3,
        GL_U_TEXTURE4,
        GL_U_TEXTURE5,
        GL_U_TEXTURE6,
        GL_U_TEXTURE7,
        GL_U_TEXTURE8,
        GL_U_TEXTURE9,
        GL_U_TEXTURE10,
        GL_U_TEXTURE11,
        GL_U_TEXTURE12,
        GL_U_TEXTURE13,
        GL_U_TEXTURE14,
        GL_U_TEXTURE15,
    };

    assert(sampler >= 0 && sampler < sizeof(uname_table)/sizeof(uname_table[0]));
    const char *uname = uname_table[sampler];

    R_GL_StateSet(uname, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = sampler
    });
    R_GL_StateInstall(uname, shader_prog);

    GL_ASSERT_OK();
}

void R_GL_Texture_Dump(const struct texture *text, const char *filename)
{
    glBindTexture(GL_TEXTURE_2D, text->id);

    GLint width, height, iformat, level = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_INTERNAL_FORMAT, &iformat);

    unsigned char* data = malloc(width * height * 3);
    if(!data)
        return;

    bool integer = false;
    switch(iformat) {
    case GL_RGB8I:
    case GL_RGB8UI:
    case GL_RGB16I:
    case GL_RGB16UI:
    case GL_RGB32I:
    case GL_RGB32UI:
    case GL_RGBA8I:
    case GL_RGBA8UI:
    case GL_RGBA16I:
    case GL_RGBA16UI:
    case GL_RGBA32I:
    case GL_RGBA32UI:
        integer = true;
    }

    GLuint format = integer ? GL_RGB_INTEGER : GL_RGB;
    glGetTexImage(GL_TEXTURE_2D, level, format, GL_UNSIGNED_BYTE, data);
    texture_write_ppm(filename, data, width, height);

    glBindTexture(GL_TEXTURE_2D, 0);
    free(data);
}

void R_GL_Texture_DumpArray(const struct texture_arr *arr, const char *base)
{
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr->id);

    GLint width, height, depth, iformat;
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_DEPTH, &depth);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_INTERNAL_FORMAT, &iformat);

    unsigned char *data = malloc(width * height * 3);
    if (!data)
        return;

    bool integer = false;
    switch(iformat) {
    case GL_RGB8I:
    case GL_RGB8UI:
    case GL_RGB16I:
    case GL_RGB16UI:
    case GL_RGB32I:
    case GL_RGB32UI:
    case GL_RGBA8I:
    case GL_RGBA8UI:
    case GL_RGBA16I:
    case GL_RGBA16UI:
    case GL_RGBA32I:
    case GL_RGBA32UI:
        integer = true;
    }
    GLuint format = integer ? GL_RGB_INTEGER : GL_RGB;

    for(int i = 0; i < depth; i++) {
    
        glGetTextureSubImage(arr->id, 0, 0, 0, i, width, height, 1, 
            format, GL_UNSIGNED_BYTE, width * height * 3, data);

        char filename[512];
        pf_snprintf(filename, sizeof(filename), "%s-%d.ppm", base, i);
        texture_write_ppm(filename, data, width, height);
    }

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    free(data);
}

void R_GL_Texture_ArrayAlloc(size_t num_elems, struct texture_arr *out, GLuint tunit)
{
    ASSERT_IN_RENDER_THREAD();

    glActiveTexture(tunit);
    out->tunit = tunit;
    glGenTextures(1, &out->id);
    glBindTexture(GL_TEXTURE_2D_ARRAY, out->id);

    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 
        CONFIG_ARR_TEX_RES, CONFIG_ARR_TEX_RES, num_elems, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_LOD_BIAS, LOD_BIAS);

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void R_GL_Texture_ArrayCopyElem(struct texture_arr *dst, int dst_idx, struct texture_arr *src, int src_idx)
{
    GLuint fbo;
    if(!GLEW_ARB_copy_image) {
        glGenFramebuffers(1, &fbo);    
    }
    GL_ASSERT_OK();

    for(int i = 0; i < texture_arr_num_mip_levels(dst->id); i++) {

        size_t mip_res = pow(0.5f, i) * CONFIG_ARR_TEX_RES;
    
        if(GLEW_ARB_copy_image) {
            glCopyImageSubData(src->id, GL_TEXTURE_2D_ARRAY, i, 0, 0, src_idx,
                               dst->id, GL_TEXTURE_2D_ARRAY, i, 0, 0, dst_idx,
                               mip_res, mip_res, 1);
            GL_ASSERT_OK();
        }else{

            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, src->id, i, src_idx);
            glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, dst->id, i, dst_idx);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glDrawBuffer(GL_COLOR_ATTACHMENT1);

            assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
            glBlitFramebuffer(0, 0, mip_res, mip_res, 0, 0, mip_res, mip_res, 
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            GL_ASSERT_OK();
        }
    }

    if(!GLEW_ARB_copy_image) {
        glDeleteFramebuffers(1, &fbo);    
    }
}

void R_GL_Texture_ArrayMake(const struct material *mats, size_t num_mats, 
                            struct texture_arr *out, GLuint tunit)
{
    ASSERT_IN_RENDER_THREAD();

    glActiveTexture(tunit);
    out->tunit = tunit;
    glGenTextures(1, &out->id);
    glBindTexture(GL_TEXTURE_2D_ARRAY, out->id);

    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 
        CONFIG_ARR_TEX_RES, CONFIG_ARR_TEX_RES, num_mats, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    for(int i = 0; i < num_mats; i++) {

        if(mats[i].texture.id == 0)
            continue;

        glBindTexture(GL_TEXTURE_2D, mats[i].texture.id);

        int w, h;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);

        GLubyte *orig_data = malloc(w * h * 4);
        if(!orig_data) {
            continue;
        }
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, orig_data);

        GLubyte *resized_data = malloc(CONFIG_ARR_TEX_RES * CONFIG_ARR_TEX_RES * 4);
        if(!resized_data) {
            free(orig_data);
            continue;
        }
        int res = stbir_resize_uint8(orig_data, w, h, 0, resized_data, CONFIG_ARR_TEX_RES, CONFIG_ARR_TEX_RES, 0, 4);
        if(res != 1) {
            free(orig_data);
            free(resized_data);
            continue;
        }

        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, CONFIG_ARR_TEX_RES, 
            CONFIG_ARR_TEX_RES, 1, GL_RGBA, GL_UNSIGNED_BYTE, resized_data);
        free(orig_data);
        free(resized_data);
    }

    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_LOD_BIAS, LOD_BIAS);

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    GL_ASSERT_OK();
}

void R_GL_Texture_ArrayMakeMap(const char texnames[][256], size_t num_textures, 
                               struct texture_arr *out, GLuint tunit)
{
    ASSERT_IN_RENDER_THREAD();

    glActiveTexture(tunit);
    out->tunit = tunit;
    glGenTextures(1, &out->id);
    glBindTexture(GL_TEXTURE_2D_ARRAY, out->id);

    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 
        CONFIG_TILE_TEX_RES, CONFIG_TILE_TEX_RES, num_textures, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    for(int i = 0; i < num_textures; i++) {

        char path[512];
        pf_snprintf(path, sizeof(path), "%s/assets/map_textures/%s", g_basepath, texnames[i]);

        int width, height, nr_channels;
        unsigned char *orig_data = stbi_load(path, &width, &height, &nr_channels, 0);
        if(orig_data) {
        
            GLubyte *resized_data = malloc(CONFIG_TILE_TEX_RES * CONFIG_TILE_TEX_RES * 3);
            if(!resized_data)
                continue;

            int res = stbir_resize_uint8(orig_data, width, height, 0, resized_data, CONFIG_TILE_TEX_RES, CONFIG_TILE_TEX_RES, 0, 3);
            assert(1 == res);

            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, CONFIG_TILE_TEX_RES, 
                CONFIG_TILE_TEX_RES, 1, GL_RGB, GL_UNSIGNED_BYTE, resized_data);
            stbi_image_free(orig_data);
            free(resized_data);
        }else{

            GLubyte *data = malloc(CONFIG_TILE_TEX_RES * CONFIG_TILE_TEX_RES * 3);
            if(!data)
                continue;

            memset(data, 0, CONFIG_TILE_TEX_RES * CONFIG_TILE_TEX_RES * 3);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, CONFIG_TILE_TEX_RES, 
                CONFIG_TILE_TEX_RES, 1, GL_RGB, GL_UNSIGNED_BYTE, data);
            free(data);
        }
    }

    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_LOD_BIAS, LOD_BIAS);

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    GL_ASSERT_OK();
}

void R_GL_Texture_ArrayFree(struct texture_arr array)
{
    glDeleteTextures(1, &array.id);
}

void R_GL_Texture_BindArray(const struct texture_arr *arr, GLuint shader_prog)
{
    ASSERT_IN_RENDER_THREAD();

    int idx = (arr->tunit - GL_TEXTURE0);
    const char *unit_name[] = {
        GL_U_TEX_ARRAY0,
        GL_U_TEX_ARRAY1,
        GL_U_TEX_ARRAY2,
        GL_U_TEX_ARRAY3,
    };
    assert(idx >= 0 && idx < ARR_SIZE(unit_name));

    glActiveTexture(arr->tunit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr->id);

    R_GL_StateSet(unit_name[idx], (struct uval){
        .type = UTYPE_INT,
        .val.as_int = idx
    });
    R_GL_StateInstall(unit_name[idx], shader_prog);

    GL_ASSERT_OK();
}

void R_GL_Texture_GetSize(GLuint texid, int *out_w, int *out_h, int *out_d)
{
    ASSERT_IN_RENDER_THREAD();

    glBindTexture(GL_TEXTURE_2D, texid);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, out_w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, out_h);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_DEPTH, out_d);

    glBindTexture(GL_TEXTURE_2D, 0);
    GL_ASSERT_OK();
}

void R_GL_Texture_GetOrLoad(const char *basedir, const char *name, GLuint *out)
{
    ASSERT_IN_RENDER_THREAD();

    if(R_GL_Texture_GetForName(basedir, name, out))
        return;

    R_GL_Texture_Load(basedir, name, out);
}

