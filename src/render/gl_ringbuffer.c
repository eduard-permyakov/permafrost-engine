/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#include "gl_ringbuffer.h"
#include "../lib/public/pf_string.h"

#include <stdlib.h>
#include <string.h>

//TODO: use ARB_buffer_storage if it's available

/* Support triple-buffering efficiently (i.e. 3 slots in the ring) */
#define RING_SIZE (3)

struct gl_ring{
    int    iwrite;
    size_t elem_size;
    /* The buffer object backing the ringbuffer */
    GLuint VBO;
    /* The texture buffer object associated with the VBO - 
     * for exposing the buffer to shaders. */
    GLuint tex_buff;
    /* Fences are to make sure we don't overwrite the next 
     * part of the buffer before it's consumed by the GPU.
     * With 3+ buffers, we should never stall on a fence, but 
     * better to stay squeaky clean. */
    GLsync fences[RING_SIZE];
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

struct gl_ring *R_GL_RingbufferInit(size_t elem_size)
{
    struct gl_ring *ret = malloc(sizeof(struct gl_ring));
    if(!ret)
        return NULL;

    glGenBuffers(1, &ret->VBO);
    glGenTextures(1, &ret->tex_buff);

    glBindBuffer(GL_ARRAY_BUFFER, ret->VBO);
    glBufferData(ret->VBO, elem_size * RING_SIZE, NULL, GL_STREAM_DRAW);

    ret->iwrite = 0;
    ret->elem_size = elem_size;
    memset(&ret->fences, 0, sizeof(ret->fences));
    return ret;
}

void R_GL_RingbufferDestroy(struct gl_ring *ring)
{
    glDeleteBuffers(1, &ring->VBO);
    glDeleteTextures(1, &ring->tex_buff);
    free(ring);
}

bool R_GL_RingbufferPush(struct gl_ring *buff, void *data)
{
    if(buff->fences[buff->iwrite]) {
    
        GLenum result = glClientWaitSync(buff->fences[buff->iwrite], 0, ((uint64_t)10) * 1000 * 1000 * 1000);
        glDeleteSync(buff->fences[buff->iwrite]);
        if(result == GL_TIMEOUT_EXPIRED || result == GL_WAIT_FAILED)
            return false;
    }

    glBindBuffer(GL_ARRAY_BUFFER, buff->VBO);
    void *ptr = glMapBufferRange(GL_ARRAY_BUFFER, buff->iwrite * buff->elem_size, buff->elem_size,
        GL_MAP_READ_BIT | GL_MAP_COHERENT_BIT | GL_MAP_UNSYNCHRONIZED_BIT);

    memcpy(ptr, data, buff->elem_size);
    glUnmapBuffer(GL_ARRAY_BUFFER);
    return true;
}

void R_GL_RingbufferSubmit(struct gl_ring *buff, GLuint shader_prog, const char *uname)
{
    buff->fences[buff->iwrite] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glUseProgram(shader_prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_BUFFER, buff->tex_buff);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGB8UI, buff->VBO);

    char uname_offset[128];
    pf_snprintf(uname_offset, sizeof(uname_offset), "%s_offset", uname);

    GLuint loc = glGetUniformLocation(shader_prog, uname);
    glUniform1i(loc, buff->tex_buff);

    loc = glGetUniformLocation(shader_prog, uname_offset);
    glUniform1i(loc, buff->iwrite * buff->elem_size);
}

