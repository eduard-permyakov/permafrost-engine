/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2019 Eduard Permyakov 
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

#ifndef QUEUE_H
#define QUEUE_H

#include <stddef.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/***********************************************************************************************/

#define QUEUE_TYPE(name, type)                                                                  \
                                                                                                \
    typedef struct queue_##name##_s {                                                           \
        size_t capacity;                                                                        \
        size_t size;                                                                            \
        int ihead;                                                                              \
        int itail;                                                                              \
        type *mem;                                                                              \
    } queue_##name##_t;                                                                         \

/***********************************************************************************************/

#define queue(name)                                                                             \
    queue_##name##_t

#define queue_size(queue)                                                                       \
    ((queue).size)

/***********************************************************************************************/

#define QUEUE_PROTOTYPES(scope, name, type)                                                     \
                                                                                                \
    static bool _queue_##name##_resize  (queue(name) *queue, size_t new_cap);                   \
    scope  bool  queue_##name##_init    (queue(name) *queue, size_t init_cap);                  \
    scope  void  queue_##name##_destroy (queue(name) *queue);                                   \
    scope  bool  queue_##name##_push    (queue(name) *queue, type *entry);                      \
    scope  bool  queue_##name##_pop     (queue(name) *queue, type *out);                        \
    scope  void  queue_##name##_clear   (queue(name) *queue);

/***********************************************************************************************/

#define QUEUE_IMPL(scope, name, type)                                                           \
                                                                                                \
    static bool _queue_##name##_resize(queue(name) *queue, size_t new_cap)                      \
    {                                                                                           \
        type *new_mem = realloc(queue->mem, sizeof(type) * new_cap);                            \
        if(!new_mem)                                                                            \
            return false;                                                                       \
                                                                                                \
        if(queue->ihead > queue->itail) {                                                       \
            /*                       */                                                         \
            /* +-----+ <--mem    ^   */                                                         \
            /* |     |          top  */                                                         \
            /* |     |           |   */                                                         \
            /* +-----+ <--tail   |   */                                                         \
            /* +-----+           v   */                                                         \
            /* |     |               */                                                         \
            /* |     |               */                                                         \
            /* +-----+ <--head   ^   */                                                         \
            /* +-----+           |   */                                                         \
            /* |     |          bot  */                                                         \
            /* +-----+           v   */                                                         \
            /* | new |               */                                                         \
            /*                       */                                                         \
            size_t top = queue->itail + 1;                                                      \
            size_t bot = queue->capacity - queue->ihead;                                        \
            assert(top + bot == queue->size);                                                   \
                                                                                                \
            type tmp[top];                                                                      \
            memcpy(tmp, new_mem, sizeof(type) * top);                                           \
            memmove(new_mem, new_mem + queue->ihead, sizeof(type) * bot);                       \
            memcpy(new_mem + bot, tmp, sizeof(type) * top);                                     \
                                                                                                \
            queue->ihead = 0;                                                                   \
            queue->itail = top + bot - 1;                                                       \
        }                                                                                       \
                                                                                                \
        queue->mem = new_mem;                                                                   \
        queue->capacity = new_cap;                                                              \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool queue_##name##_init(queue(name) *queue, size_t init_cap)                         \
    {                                                                                           \
        memset(queue, 0, sizeof(*queue));                                                       \
        queue->itail = -1;                                                                      \
        return _queue_##name##_resize(queue, init_cap);                                         \
    }                                                                                           \
                                                                                                \
    scope void queue_##name##_destroy(queue(name) *queue)                                       \
    {                                                                                           \
        free(queue->mem);                                                                       \
        memset(queue, 0, sizeof(*queue));                                                       \
    }                                                                                           \
                                                                                                \
    scope bool queue_##name##_push(queue(name) *queue, type *entry)                             \
    {                                                                                           \
        if(queue->size == queue->capacity) {                                                    \
            if(!_queue_##name##_resize(queue, queue->capacity ? queue->capacity * 2 : 32))      \
                return false;                                                                   \
        }                                                                                       \
                                                                                                \
        ++queue->itail;                                                                         \
        if(queue->itail >= queue->capacity) {                                                   \
            queue->itail = 0;  /* Wrap around back to top */                                    \
        }                                                                                       \
                                                                                                \
        queue->mem[queue->itail] = *entry;                                                      \
        ++queue->size;                                                                          \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool queue_##name##_pop(queue(name) *queue, type *out)                                \
    {                                                                                           \
        if(queue->size == 0)                                                                    \
            return false;                                                                       \
                                                                                                \
        *out = queue->mem[queue->ihead];                                                        \
        ++queue->ihead;                                                                         \
        if(queue->ihead >= queue->capacity) {                                                   \
            queue->ihead = 0;  /* Wrap around back to top */                                    \
        }                                                                                       \
                                                                                                \
        --queue->size;                                                                          \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope void queue_##name##_clear(queue(name) *queue)                                         \
    {                                                                                           \
        queue->itail = -1;                                                                      \
        queue->ihead = 0;                                                                       \
        queue->size = 0;                                                                        \
    }

#endif

