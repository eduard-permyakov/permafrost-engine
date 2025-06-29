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
#include "public/render.h"

#define MAX(a, b)               ((a) > (b) ? (a) : (b))
#define DEFAULT_POSE_BUFF_SIZE  (16*1024*1024)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static GLuint s_pose_buff_ssbo = 0;
static size_t s_pose_buff_used = 0;
static size_t s_pose_buff_size = DEFAULT_POSE_BUFF_SIZE;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void realloc_ssbo(size_t oldsize, size_t newsize)
{
    GL_PERF_ENTER();
    /* Generate a new SSBO */
    GLuint new_buffer;
    glGenBuffers(1, &new_buffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, new_buffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, newsize, NULL, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    /* Copy existing data from the old SSBO */
    glBindBuffer(GL_COPY_READ_BUFFER, s_pose_buff_ssbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, new_buffer);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, oldsize);
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

    /* Delete the old SSBO */
    glDeleteBuffers(1, &s_pose_buff_ssbo);
    s_pose_buff_ssbo = new_buffer;
    s_pose_buff_size = newsize;

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_AnimInit(void)
{
    glGenBuffers(1, &s_pose_buff_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_pose_buff_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, DEFAULT_POSE_BUFF_SIZE, NULL, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return true;
}

void R_GL_AnimShutdown(void)
{
    glDeleteBuffers(1, &s_pose_buff_ssbo);
    s_pose_buff_ssbo = 0;
    s_pose_buff_used = 0;
    s_pose_buff_size = DEFAULT_POSE_BUFF_SIZE;
}

void R_GL_AnimAppendData(GLfloat *data, size_t *size)
{
    assert(s_pose_buff_size >= s_pose_buff_used);
    size_t left = s_pose_buff_size - s_pose_buff_used;

    if(left < *size) {
        size_t newsize = MAX(s_pose_buff_size * 2, s_pose_buff_size + *size);
        realloc_ssbo(s_pose_buff_size, newsize);
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_pose_buff_ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, s_pose_buff_used, *size, data);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    s_pose_buff_used += *size;
}

