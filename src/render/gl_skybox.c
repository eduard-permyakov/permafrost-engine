/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2024 Eduard Permyakov 
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
#include "gl_assert.h"
#include "../main.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/stb_image.h"

#include <GL/glew.h>
#include <assert.h>


#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))

struct skybox{
    GLuint cubemap;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct skybox s_skybox;

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_SkyboxLoad(const char *dir, const char *extension)
{
    ASSERT_IN_RENDER_THREAD();

    glGenTextures(1, &s_skybox.cubemap);
    glBindTexture(GL_TEXTURE_CUBE_MAP, s_skybox.cubemap);

    const struct face{
        const char *name;
        GLuint target;
    }faces[6] = {
        {"right",   GL_TEXTURE_CUBE_MAP_POSITIVE_X},
        {"left",    GL_TEXTURE_CUBE_MAP_NEGATIVE_X},
        {"top",     GL_TEXTURE_CUBE_MAP_POSITIVE_Y},
        {"bottom",  GL_TEXTURE_CUBE_MAP_NEGATIVE_Y},
        {"back",    GL_TEXTURE_CUBE_MAP_POSITIVE_Z},
        {"front",   GL_TEXTURE_CUBE_MAP_NEGATIVE_Z}
    };

    for(int i = 0; i < ARR_SIZE(faces); i++) {

        char filename[256];
        pf_snprintf(filename, sizeof(filename), "%s/%s/%s.%s",
            g_basepath, dir, faces[i].name, extension);

        int width, height, nr_channels;
        static const unsigned char black[3] = {0};
        bool loaded = true;
        const unsigned char *data = stbi_load(filename, &width, &height, &nr_channels, 0);
        if(!data) {
            data = black;
            width = 1;
            height = 1;
            nr_channels = 3;
            loaded = false;
        }
        GLuint format = (nr_channels == 4) ? GL_RGBA : GL_RGB;
        glTexImage2D(faces[i].target, 0, format, width, height, 0, 
            format, GL_UNSIGNED_BYTE, data);
        if(loaded) {
            stbi_image_free((void*)data);
        }
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);  

    GL_ASSERT_OK();
}

void R_GL_SkyboxBind(void)
{
    ASSERT_IN_RENDER_THREAD();
    glActiveTexture(SKYBOX_TUNIT);
    glBindTexture(GL_TEXTURE_CUBE_MAP, s_skybox.cubemap);
    GL_ASSERT_OK();
}

