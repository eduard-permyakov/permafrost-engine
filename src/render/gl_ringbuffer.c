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
#include "gl_assert.h"
#include "../perf.h"
#include "../lib/public/pf_string.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

//TODO: use ARB_buffer_storage if it's available

/* How many discrete sets of data (guarded by fences) the buffer can hold */
#define NMAXMARKERS     (16)
#define TIMEOUT_NSEC    (((uint64_t)10) * 1000 * 1000 * 1000)

/* */
enum mode{
    MODE_UNSYNCHRONIZED_VBO,
    MODE_PERSISTENT_MAPPED_BUFFER,
};

struct marker{
    size_t begin;
    size_t end;
};

struct gl_ring{
    size_t pos;
    size_t size;
    /* The buffer object backing the ringbuffer */
    GLuint VBO;
    /* The texture buffer object associated with the VBO - 
     * for exposing the buffer to shaders. */
    GLuint tex_buff;
    /* Fences are to make sure we don't overwrite the next 
     * part of the buffer before it's consumed by the GPU. */
    GLsync fences[NMAXMARKERS];
    /* The markers hold the buffer positions guarded by the fences */
    size_t nmarkers;
    size_t imark_head, imark_tail;
    struct marker markers[NMAXMARKERS];
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool ring_wait_one(struct gl_ring *ring)
{
    PERF_ENTER();

    if(ring->nmarkers == 0)
        PERF_RETURN(false);

    GLenum result = glClientWaitSync(ring->fences[ring->imark_tail], 0, TIMEOUT_NSEC);
    glDeleteSync(ring->fences[ring->imark_tail]);
    ring->fences[ring->imark_tail] = 0;
    ring->imark_tail = (ring->imark_tail + 1) % NMAXMARKERS;
    ring->nmarkers--;

    if(result == GL_TIMEOUT_EXPIRED || result == GL_WAIT_FAILED)
        PERF_RETURN(false);

    PERF_RETURN(true);
}

static bool ring_section_free(const struct gl_ring *ring, size_t size)
{
    if(ring->nmarkers == NMAXMARKERS)
        return false;
    if(ring->nmarkers == 0)
        return true;

    size_t begin = ring->markers[ring->imark_tail].begin;
    size_t end = ring->markers[ring->imark_head].end;

    if(end < begin) { /* wrap around */
        return (begin - end) >= size;
    }else{
        size_t end_size = ring->size - end;
        size_t start_size = ring->pos;
        return (end_size + start_size) >= size;
    }
}

static void ring_notify_shaders(const struct gl_ring *ring, GLuint *shader_progs, 
                                size_t nshaders, const char *uname)
{
    char uname_offset[128];
    pf_snprintf(uname_offset, sizeof(uname_offset), "%s_offset", uname);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_BUFFER, ring->tex_buff);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R8, ring->VBO);

    for(int i = 0; i < nshaders; i++) {

        GLuint shader_prog = shader_progs[i];
        glUseProgram(shader_prog);

        GLuint loc = glGetUniformLocation(shader_prog, uname);
        glUniform1i(loc, ring->tex_buff);

        loc = glGetUniformLocation(shader_prog, uname_offset);
        glUniform1i(loc, ring->pos);
    }
}

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

struct gl_ring *R_GL_RingbufferInit(size_t size)
{
    struct gl_ring *ret = malloc(sizeof(struct gl_ring));
    if(!ret)
        return NULL;

    glGenBuffers(1, &ret->VBO);
    glGenTextures(1, &ret->tex_buff);

    glBindBuffer(GL_ARRAY_BUFFER, ret->VBO);
    glBufferData(GL_ARRAY_BUFFER, size, NULL, GL_STREAM_DRAW);

    ret->pos = 0;
    ret->size = size;
    ret->imark_head = 0;
    ret->imark_tail = 0;
    ret->nmarkers = 0;
    memset(&ret->fences, 0, sizeof(ret->fences));
    memset(&ret->markers, 0, sizeof(ret->fences));

    GL_ASSERT_OK();
    return ret;
}

void R_GL_RingbufferDestroy(struct gl_ring *ring)
{
    while(ring->nmarkers) {
        ring_wait_one(ring);
    }
    glDeleteBuffers(1, &ring->VBO);
    glDeleteTextures(1, &ring->tex_buff);
    free(ring);
}

bool R_GL_RingbufferPush(struct gl_ring *ring, void *data, size_t size,
                         GLuint *shader_progs, size_t nshaders, const char *uname)
{
    assert(!ring->nmarkers || ring->fences[ring->imark_tail] > 0);

    if(size > ring->size)
        return false;

    while(!ring_section_free(ring, size)) {
        if(!ring_wait_one(ring))
            return false;
    }

    ring_notify_shaders(ring, shader_progs, nshaders, uname);
    glBindBuffer(GL_ARRAY_BUFFER, ring->VBO);

    size_t left = ring->size - ring->pos;
    size_t old_pos = ring->pos;

    if(size <= left) {
        void *ptr = glMapBufferRange(GL_ARRAY_BUFFER, ring->pos, size,
            GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        memcpy(ptr, data, size);
        ring->pos = ring->pos + size;
    }else{
        size_t start = size - left;
        if(left > 0) {
            void *ptr = glMapBufferRange(GL_ARRAY_BUFFER, ring->pos, left,
                GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
            memcpy(ptr, data, left);
            glUnmapBuffer(GL_ARRAY_BUFFER);
        }

        /* wrap around */
        void *ptr = glMapBufferRange(GL_ARRAY_BUFFER, 0, start,
            GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        memcpy(ptr, data, start);
        ring->pos = start;
    }

    glUnmapBuffer(GL_ARRAY_BUFFER);
    ring->imark_head = (ring->imark_head + 1) % NMAXMARKERS;
    ring->markers[ring->imark_head] = (struct marker){old_pos, ring->pos};

    if(!ring->nmarkers)
        ring->imark_tail = ring->imark_head;

    ring->nmarkers++;

    GL_ASSERT_OK();
    return true;
}

void R_GL_RingbufferSyncLast(struct gl_ring *ring)
{
    assert(ring->nmarkers);
    ring->fences[ring->imark_head] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

