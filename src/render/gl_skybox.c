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
#include "gl_shader.h"
#include "gl_perf.h"
#include "gl_state.h"
#include "../main.h"
#include "../camera.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/stb_image.h"

#include <GL/glew.h>
#include <assert.h>
#include <string.h>


#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))

struct skybox{
    GLuint cubemap;
    GLuint VAO;
    GLuint VBO;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct skybox s_skybox;

static const vec3_t s_cube_verts[] = {
    { 10.0f,  10.0f, -10.0f},
    { 10.0f, -10.0f, -10.0f},
    {-10.0f, -10.0f, -10.0f},
    {-10.0f, -10.0f, -10.0f},
    {-10.0f,  10.0f, -10.0f},
    { 10.0f,  10.0f, -10.0f},

    { 10.0f, -10.0f,  10.0f},
    { 10.0f, -10.0f, -10.0f},
    { 10.0f,  10.0f, -10.0f},
    { 10.0f,  10.0f, -10.0f},
    { 10.0f,  10.0f,  10.0f},
    { 10.0f, -10.0f,  10.0f},

    {-10.0f, -10.0f, -10.0f},
    {-10.0f, -10.0f,  10.0f},
    {-10.0f,  10.0f,  10.0f},
    {-10.0f,  10.0f,  10.0f},
    {-10.0f,  10.0f, -10.0f},
    {-10.0f, -10.0f, -10.0f},

    { 10.0f, -10.0f,  10.0f},
    { 10.0f,  10.0f,  10.0f},
    {-10.0f,  10.0f,  10.0f},
    {-10.0f,  10.0f,  10.0f},
    {-10.0f, -10.0f,  10.0f},
    { 10.0f, -10.0f,  10.0f},

    { 10.0f,  10.0f, -10.0f},
    {-10.0f,  10.0f, -10.0f},
    {-10.0f,  10.0f,  10.0f},
    {-10.0f,  10.0f,  10.0f},
    { 10.0f,  10.0f,  10.0f},
    { 10.0f,  10.0f, -10.0f},

    { 10.0f, -10.0f, -10.0f},
    { 10.0f, -10.0f,  10.0f},
    {-10.0f, -10.0f, -10.0f},
    {-10.0f, -10.0f, -10.0f},
    { 10.0f, -10.0f,  10.0f},
    {-10.0f, -10.0f,  10.0f}
};

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

    /* Generate cube mesh */
    glGenVertexArrays(1, &s_skybox.VAO);
    glBindVertexArray(s_skybox.VAO);

    glGenBuffers(1, &s_skybox.VBO);
    glBindBuffer(GL_ARRAY_BUFFER, s_skybox.VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);

    size_t buffsize = sizeof(s_cube_verts);
    glBufferData(GL_ARRAY_BUFFER, buffsize, s_cube_verts, GL_STATIC_DRAW);

    GL_ASSERT_OK();
}

void R_GL_SkyboxBind(void)
{
    ASSERT_IN_RENDER_THREAD();

    glActiveTexture(SKYBOX_TUNIT);
    glBindTexture(GL_TEXTURE_CUBE_MAP, s_skybox.cubemap);

    GL_ASSERT_OK();
}

void R_GL_DrawSkybox(const struct camera *cam)
{
    ASSERT_IN_RENDER_THREAD();
    GL_PERF_PUSH_GROUP(0, "skybox");

    mat4x4_t view_mat;
    Camera_MakeViewMat(cam, &view_mat);

    mat4x4_t identity;
    PFM_Mat4x4_Identity(&identity);

    /* Remove the translation component from the view matrix */
    mat4x4_t view_dir_mat;
    view_dir_mat = (mat4x4_t){
        .cols[0][0] = view_mat.cols[0][0],   
        .cols[1][0] = view_mat.cols[1][0],    
        .cols[2][0] = view_mat.cols[2][0],    
        .cols[3][0] = 0,

        .cols[0][1] = view_mat.cols[0][1],   
        .cols[1][1] = view_mat.cols[1][1],    
        .cols[2][1] = view_mat.cols[2][1],    
        .cols[3][1] = 0,

        .cols[0][2] = view_mat.cols[0][2],   
        .cols[1][2] = view_mat.cols[1][2],    
        .cols[2][2] = view_mat.cols[2][2],    
        .cols[3][2] = 0,

        .cols[0][3] = 0,                     
        .cols[1][3] = 0,                      
        .cols[2][3] = 0,                      
        .cols[3][3] = 1
    };

    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = identity
    });

    R_GL_StateSet(GL_U_VIEW_ROT_MAT, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = view_dir_mat
    });

    GLint old_cull_face_mode;
    glGetIntegerv(GL_CULL_FACE_MODE, &old_cull_face_mode);
    GLint old_depth_func_mode;
    glGetIntegerv(GL_DEPTH_FUNC, &old_depth_func_mode);

    glCullFace(GL_FRONT);
    glDepthFunc(GL_LEQUAL);

    R_GL_Shader_Install("skybox");
    glBindVertexArray(s_skybox.VAO);
    glDrawArrays(GL_TRIANGLES, 0, 36);

    glDepthMask(old_depth_func_mode);
    glCullFace(old_cull_face_mode);

    GL_PERF_POP_GROUP();
    GL_ASSERT_OK();
}

void R_GL_DrawSkyboxScaled(const struct camera *cam, float *map_width, float *map_height)
{
    ASSERT_IN_RENDER_THREAD();
    GL_PERF_PUSH_GROUP(0, "skybox");

    float scale = MAX(*map_width, *map_height) / (10.0f * 2.0f);
    mat4x4_t model;
    PFM_Mat4x4_MakeScale(scale, scale, scale, &model);

    mat4x4_t view;
    Camera_MakeViewMat(cam, &view);

    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = model
    });

    R_GL_StateSet(GL_U_VIEW_ROT_MAT, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = view
    });

    GLint old_cull_face_mode;
    glGetIntegerv(GL_CULL_FACE_MODE, &old_cull_face_mode);
    GLint old_depth_func_mode;
    glGetIntegerv(GL_DEPTH_FUNC, &old_depth_func_mode);

    glCullFace(GL_FRONT);
    glDepthFunc(GL_LEQUAL);

    R_GL_Shader_Install("skybox");
    glBindVertexArray(s_skybox.VAO);
    glDrawArrays(GL_TRIANGLES, 0, 36);

    glDepthMask(old_depth_func_mode);
    glCullFace(old_cull_face_mode);

    GL_PERF_POP_GROUP();
    GL_ASSERT_OK();
}

void R_GL_SkyboxFree(void)
{
    ASSERT_IN_RENDER_THREAD();

    glDeleteTextures(1, &s_skybox.cubemap);
    glDeleteVertexArrays(1, &s_skybox.VAO);
    glDeleteBuffers(1, &s_skybox.VBO);

    GL_ASSERT_OK();
}

