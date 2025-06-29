/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2025 Eduard Permyakov 
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

#include "render_private.h"
#include "gl_anim.h"
#include "gl_mesh.h"
#include "gl_vertex.h"
#include "gl_shader.h"
#include "gl_material.h"
#include "gl_assert.h"
#include "gl_perf.h"
#include "gl_state.h"
#include "gl_texture.h"
#include "gl_render.h"
#include "public/render.h"
#include "../main.h"
#include "../entity.h"

#define MAX(a, b)               ((a) > (b) ? (a) : (b))
#define DEFAULT_POSE_BUFF_SIZE  (16*1024*1024)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static GLuint s_pose_buff_vbo = 0;
static GLuint s_pose_buff_tex = 0;

static size_t s_pose_buff_used = 0;
static size_t s_pose_buff_size = DEFAULT_POSE_BUFF_SIZE;

/* Scratch buffer for posting extended joint data to shaders.
 */
static GLuint s_joint_buff_ubo = 0;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void realloc_vbo(size_t oldsize, size_t newsize)
{
    GL_PERF_ENTER();

    /* Generate a new VBO */
    GLuint new_buffer;
    glGenBuffers(1, &new_buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, new_buffer);
    glBufferData(GL_TEXTURE_BUFFER, newsize, NULL, GL_STATIC_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);

    /* Copy existing data from the old VBO */
    glBindBuffer(GL_COPY_READ_BUFFER, s_pose_buff_vbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, new_buffer);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, oldsize);
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

    /* Delete the old VBO */
    glDeleteBuffers(1, &s_pose_buff_vbo);
    s_pose_buff_vbo = new_buffer;
    s_pose_buff_size = newsize;

    /* Upate the buffer texture */
    glBindTexture(GL_TEXTURE_BUFFER, s_pose_buff_tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, s_pose_buff_vbo);
    glBindTexture(GL_TEXTURE_BUFFER, 0);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_AnimInit(void)
{
    ASSERT_IN_RENDER_THREAD();

    glGenBuffers(1, &s_pose_buff_vbo);
    glBindBuffer(GL_TEXTURE_BUFFER, s_pose_buff_vbo);
    glBufferData(GL_TEXTURE_BUFFER, DEFAULT_POSE_BUFF_SIZE, NULL, GL_STATIC_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);

    glGenTextures(1, &s_pose_buff_tex);
    glBindTexture(GL_TEXTURE_BUFFER, s_pose_buff_tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, s_pose_buff_vbo);
    glBindTexture(GL_TEXTURE_BUFFER, 0);

    GL_ASSERT_OK();
    return true;
}

void R_GL_AnimShutdown(void)
{
    ASSERT_IN_RENDER_THREAD();

    glDeleteTextures(1, &s_pose_buff_tex);
    glDeleteBuffers(1, &s_pose_buff_vbo);
    s_pose_buff_vbo = 0;
    s_pose_buff_tex = 0;
    s_pose_buff_used = 0;
    s_pose_buff_size = DEFAULT_POSE_BUFF_SIZE;
}

void R_GL_AnimAppendData(GLfloat *data, size_t *size)
{
    ASSERT_IN_RENDER_THREAD();

    assert(s_pose_buff_size >= s_pose_buff_used);
    size_t left = s_pose_buff_size - s_pose_buff_used;

    if(left < *size) {
        size_t newsize = MAX(s_pose_buff_size * 2, s_pose_buff_size + *size);
        realloc_vbo(s_pose_buff_size, newsize);
    }

    glBindBuffer(GL_TEXTURE_BUFFER, s_pose_buff_vbo);
    glBufferSubData(GL_TEXTURE_BUFFER, s_pose_buff_used, *size, data);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    s_pose_buff_used += *size;

    GL_ASSERT_OK();
}

void R_GL_AnimBindPoseBuff(void)
{
    ASSERT_IN_RENDER_THREAD();

    glActiveTexture(POSE_BUFF_TUNINT);
    glBindTexture(GL_TEXTURE_BUFFER, s_pose_buff_tex);

    GL_ASSERT_OK();
}

void R_GL_AnimSetUniforms(mat4x4_t *inv_bind_poses, mat4x4_t *curr_poses, 
                          mat4x4_t *normal_mat, const size_t *count)
{
    ASSERT_IN_RENDER_THREAD();

    R_GL_StateSet(GL_U_POSEBUFF, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = POSE_BUFF_TUNINT - GL_TEXTURE0
    });

    bool extended = (*count > MAX_JOINTS);
    R_GL_StateSet(GL_U_EXTENDED_JOINTS, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = (int)extended 
    });
    if(extended) {

        if(0 == s_joint_buff_ubo) {
            glGenBuffers(1, &s_joint_buff_ubo);
            glBindBuffer(GL_UNIFORM_BUFFER, s_joint_buff_ubo);
            glBufferData(GL_UNIFORM_BUFFER, sizeof(mat4x4_t) * MAX_JOINTS_EXTENDED * 2, NULL, 
                GL_STATIC_DRAW);
        }
        glBindBuffer(GL_UNIFORM_BUFFER, s_joint_buff_ubo);

        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(mat4x4_t) * MAX_JOINTS_EXTENDED, curr_poses);
        glBufferSubData(GL_UNIFORM_BUFFER, sizeof(mat4x4_t) * MAX_JOINTS_EXTENDED, 
            sizeof(mat4x4_t) * MAX_JOINTS_EXTENDED, inv_bind_poses);

        glBindBufferBase(GL_UNIFORM_BUFFER, 0, s_joint_buff_ubo);
        R_GL_StateSetBlockBinding(GL_U_JOINTS_BUFF, 0);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

    }else{
        R_GL_StateSetArray(GL_U_INV_BIND_MATS, UTYPE_MAT4, *count, inv_bind_poses);
        R_GL_StateSetArray(GL_U_CURR_POSE_MATS, UTYPE_MAT4, *count, curr_poses);
        R_GL_StateSet(GL_U_NORMAL_MAT, (struct uval){
            .type = UTYPE_MAT4, 
            .val.as_mat4 = *normal_mat
        });
    }

    GL_ASSERT_OK();
}

