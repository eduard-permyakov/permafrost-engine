/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2020 Eduard Permyakov 
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


#define LOD_BIAS (-0.5f)

KHASH_MAP_INIT_STR(tex, GLuint)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(tex) *s_name_tex_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool r_texture_gl_init(const char *path, GLuint *out)
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
    return true;

fail_format:
    stbi_image_free(data);
fail_load:
    return false;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_Texture_Init(void)
{
    ASSERT_IN_RENDER_THREAD();

    s_name_tex_table = kh_init(tex);
    return (s_name_tex_table != NULL);
}

bool R_GL_Texture_GetForName(const char *basedir, const char *name, GLuint *out)
{
    ASSERT_IN_RENDER_THREAD();

    char qualname[512];
    pf_snprintf(qualname, sizeof(qualname), "%s/%s", basedir, name);

    khiter_t k;
    if((k = kh_get(tex, s_name_tex_table, qualname)) == kh_end(s_name_tex_table))
        return false;

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

    if(!r_texture_gl_init(texture_path, &ret)
    && !r_texture_gl_init(texture_path_maps, &ret))
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

void R_GL_Texture_Free(const char *name)
{
    ASSERT_IN_RENDER_THREAD();

    khiter_t k;
    if((k = kh_get(tex, s_name_tex_table, name)) != kh_end(s_name_tex_table)) {

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
}

void R_GL_Texture_ArraySetElem(struct texture_arr *arr, int idx, GLuint id)
{
    glBindTexture(GL_TEXTURE_2D, id);

    int w, h;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);

    GLubyte *orig_data = malloc(w * h * 4);
    if(!orig_data) {
        arr->id = 0;
        return;
    }
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, orig_data);

    GLubyte resized_data[CONFIG_ARR_TEX_RES * CONFIG_ARR_TEX_RES * 4];
    int res = stbir_resize_uint8(orig_data, w, h, 0, resized_data, CONFIG_ARR_TEX_RES, CONFIG_ARR_TEX_RES, 0, 4);
    if(res != 1) {
        free(orig_data);
        arr->id = 0;
        return;
    }

    free(orig_data);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, idx, CONFIG_ARR_TEX_RES, 
        CONFIG_ARR_TEX_RES, 1, GL_RGBA, GL_UNSIGNED_BYTE, resized_data);
}

void R_GL_Texture_ArrayGenMipmap(struct texture_arr *arr)
{
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr->id);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_LOD_BIAS, LOD_BIAS);
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
        R_GL_Texture_ArraySetElem(out, i, mats[i].texture.id);
    }

    R_GL_Texture_ArrayGenMipmap(out);
    GL_ASSERT_OK();
}

bool R_GL_Texture_ArrayMakeMap(const char texnames[][256], size_t num_textures, 
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
        if(!orig_data)
            goto fail_load;

        GLubyte resized_data[CONFIG_TILE_TEX_RES * CONFIG_TILE_TEX_RES * 3];
        int res = stbir_resize_uint8(orig_data, width, height, 0, resized_data, CONFIG_TILE_TEX_RES, CONFIG_TILE_TEX_RES, 0, 3);

        assert(1 == res);
        stbi_image_free(orig_data);

        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, CONFIG_TILE_TEX_RES, 
            CONFIG_TILE_TEX_RES, 1, GL_RGB, GL_UNSIGNED_BYTE, resized_data);
    }

    R_GL_Texture_ArrayGenMipmap(out);
    GL_ASSERT_OK();
    return true;

fail_load:
    glDeleteTextures(1, &out->id);
    return false;
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

    glActiveTexture(arr->tunit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr->id);

    R_GL_StateSet(unit_name[idx], (struct uval){
        .type = UTYPE_INT,
        .val.as_int = idx
    });
    R_GL_StateInstall(unit_name[idx], shader_prog);

    GL_ASSERT_OK();
}

void R_GL_Texture_GetSize(GLuint texid, int *out_w, int *out_h)
{
    ASSERT_IN_RENDER_THREAD();

    glBindTexture(GL_TEXTURE_2D, texid);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, out_w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, out_h);
    GL_ASSERT_OK();
}

void R_GL_Texture_GetOrLoad(const char *basedir, const char *name, GLuint *out)
{
    ASSERT_IN_RENDER_THREAD();

    if(R_GL_Texture_GetForName(basedir, name, out))
        return;

    R_GL_Texture_Load(basedir, name, out);
}

