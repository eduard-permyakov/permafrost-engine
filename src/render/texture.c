/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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

#include "texture.h"
#include "gl_uniforms.h"
#include "gl_assert.h"
#include "../lib/public/stb_image.h"

#include <string.h>
#include <assert.h>

#define MAX_NUM_TEXTURE  2048
#define MAX_TEX_NAME_LEN 32


struct texture_resource{
    char                     name[MAX_TEX_NAME_LEN];
    GLint                    texture_id;
    struct texture_resource *next_free;
    struct texture_resource *prev_free;
    bool                     free;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct texture_resource  s_tex_resources[MAX_NUM_TEXTURE];
static struct texture_resource *s_free_head = &s_tex_resources[0];


/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool r_texture_gl_init(const char *path, GLuint *out)
{
    GLuint ret;
    int width, height, nr_channels;
    unsigned char *data;
    
    data = stbi_load(path, &width, &height, &nr_channels, 0);
    if(!data)
        goto fail_load;

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &ret);
    glBindTexture(GL_TEXTURE_2D, ret);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    if(nr_channels != 3 && nr_channels != 4)
        goto fail_format;

    GLint format = (nr_channels == 3) ? GL_RGB :
                                        GL_RGBA;
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

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

void R_Texture_Init(void)
{
    for(int i = 0; i < MAX_NUM_TEXTURE; i++) {

        struct texture_resource *res = &s_tex_resources[i];

        res->free = true;
        memset(res->name, 0, sizeof(res->name));

        if(i + 1 < MAX_NUM_TEXTURE) {
         
            struct texture_resource *next = &s_tex_resources[i+1];

            res->next_free = next;
            next->prev_free = res;
        }
    }
}

bool R_Texture_GetForName(const char *name, GLuint *out)
{
    for(int i = 0; i < MAX_NUM_TEXTURE; i++) {

        struct texture_resource *curr = &s_tex_resources[i];

        if(!strcmp(name, curr->name) && !curr->free) {
            *out = curr->texture_id;
            return true;
        }
    }

    return false;
}

bool R_Texture_Load(const char *basedir, const char *name, GLuint *out)
{
    if(!s_free_head)
        return false;

    struct texture_resource *alloc = s_free_head;
    alloc->free = false;

    s_free_head = alloc->next_free;
    if(s_free_head)
        s_free_head->prev_free = NULL;

    assert( strlen(name) < MAX_TEX_NAME_LEN );
    strcpy(alloc->name, name);

    char texture_path[512], texture_path_maps[512];

    if(basedir) {
    
        assert( strlen(basedir) + strlen(name) < sizeof(texture_path) );

        strcpy(texture_path, basedir);
        strcat(texture_path, "/");
        strcat(texture_path, name);
    }else{
        texture_path[0] = '\0';
    }

    extern const char *g_basepath;
    strcpy(texture_path_maps, g_basepath);
    strcat(texture_path_maps, "assets/map_textures/");
    strcat(texture_path_maps, name);

    GLuint ret;
    if(!r_texture_gl_init(texture_path, &ret)
    && !r_texture_gl_init(texture_path_maps, &ret))
        goto fail;

    alloc->texture_id = ret;
    *out = ret;

    GL_ASSERT_OK();
    return true;

fail:
    return false;
}

bool R_Texture_AddExisting(const char *name, GLuint id)
{
    if(!s_free_head)
        return false;

    struct texture_resource *alloc = s_free_head;
    alloc->free = false;

    s_free_head = alloc->next_free;
    if(s_free_head)
        s_free_head->prev_free = NULL;

    assert( strlen(name) < MAX_TEX_NAME_LEN );
    strcpy(alloc->name, name);

    alloc->texture_id = id;
    return true;
}

void R_Texture_Free(const char *name)
{
    for(int i = 0; i < MAX_NUM_TEXTURE - 1; i++) {

        struct texture_resource *curr = &s_tex_resources[i];

        if(!strcmp(name, curr->name) && !curr->free) {

            glDeleteTextures(1, &curr->texture_id);
            curr->free = true;

            struct texture_resource *tmp = s_free_head;
            s_free_head = curr;
            s_free_head->next_free = tmp;
            if(tmp)
                tmp->prev_free = s_free_head;
            break;
        }
    }
    GL_ASSERT_OK();
}

void R_Texture_GL_Activate(const struct texture *text, GLuint shader_prog)
{
    GLuint sampler_loc;

    switch(text->tunit) {
    case GL_TEXTURE0:  sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE0);  break;
    case GL_TEXTURE1:  sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE1);  break;
    case GL_TEXTURE2:  sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE2);  break;
    case GL_TEXTURE3:  sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE3);  break;
    case GL_TEXTURE4:  sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE4);  break;
    case GL_TEXTURE5:  sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE5);  break;
    case GL_TEXTURE6:  sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE6);  break;
    case GL_TEXTURE7:  sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE7);  break;
    case GL_TEXTURE8:  sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE8);  break;
    case GL_TEXTURE9:  sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE9);  break;
    case GL_TEXTURE10: sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE10); break;
    case GL_TEXTURE11: sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE11); break;
    case GL_TEXTURE12: sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE12); break;
    case GL_TEXTURE13: sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE13); break;
    case GL_TEXTURE14: sampler_loc = glGetUniformLocation(shader_prog, GL_U_TEXTURE14); break;

    default: assert(0);
    }

    glActiveTexture(text->tunit);
    glBindTexture(GL_TEXTURE_2D, text->id);
    glUniform1i(sampler_loc, text->tunit - GL_TEXTURE0);

    GL_ASSERT_OK();
}

