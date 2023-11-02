/* 
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2023 Eduard Permyakov 
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

#include "public/stalloc.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool stalloc_init(struct memstack *st)
{
    st->head = malloc(sizeof(struct st_mem));
    if(!st->head)
        return false;

    st->head->next = NULL;
    st->top = st->head->raw;
    st->tail = st->head;
    return true;
}

void stalloc_destroy(struct memstack *st)
{
    struct st_mem *curr = st->head, *tmp;
    while(curr) {
        tmp = curr->next;
        free(curr);
        curr = tmp;
    }
    memset(st, 0, sizeof(*st));
}

void *stalloc(struct memstack *st, size_t size)
{
    /* align to the size of the largest builtin type,
     * and zero out the padding bytes */
    const size_t aligned_size = (size + (sizeof(intmax_t) - 1)) & ~(sizeof(intmax_t) - 1);
    const size_t align_pad = aligned_size - size;

    unsigned char *curr_end = st->tail->raw + MEMBLOCK_SZ;
    size_t curr_left = curr_end - (unsigned char*)st->top;
    assert(curr_left <= MEMBLOCK_SZ);

    if(aligned_size > MEMBLOCK_SZ)
        return NULL;

    if(curr_left >= aligned_size) {
        void *ret = st->top;
        st->top = (unsigned char*)st->top + aligned_size; 

        assert((((uintptr_t)ret) & (sizeof(intmax_t)-1)) == 0);
        memset(((char*)ret) + aligned_size - align_pad, 0, align_pad);
        return ret;
    }

    st->tail->next = malloc(sizeof(struct st_mem));
    if(!st->tail->next)
        return NULL;

    st->tail = st->tail->next;
    st->tail->next = NULL;

    void *ret = st->tail->raw;
    st->top = st->tail->raw + aligned_size;

    assert((((uintptr_t)ret) & (sizeof(intmax_t)-1)) == 0);
    memset(((char*)ret) + aligned_size - align_pad, 0, align_pad);
    return ret;
}

void stalloc_clear(struct memstack *st)
{
    /* Don't free the very first memblock */
    struct st_mem *curr = st->head->next, *tmp;
    while(curr) {
        tmp = curr->next;
        free(curr);
        curr = tmp;
    }

    st->head->next = NULL;
    st->top = st->head->raw;
    st->tail = st->head;
}

bool sstalloc_init(struct smemstack *st)
{
    st->top = st->mem;
    memset(&st->extra, 0, sizeof(st->extra));
    return true;
}

void sstalloc_destroy(struct smemstack *st)
{
    if(st->top)
        return;
    stalloc_destroy(&st->extra);
}

void *sstalloc(struct smemstack *st, size_t size)
{
    /* align to the size of the largest builtin type,
     * and zero out the padding bytes */
    const size_t aligned_size = (size + (sizeof(intmax_t) - 1)) & ~(sizeof(intmax_t) - 1);
    const size_t align_pad = aligned_size - size;

    if(!st->top)
        return stalloc(&st->extra, size);

    size_t local_left = STATIC_BUFF_SZ - ((unsigned char*)st->top - st->mem);
    if(local_left >= aligned_size) {
        void *ret = st->top;
        st->top = (unsigned char*)st->top + aligned_size;

        assert((((uintptr_t)ret) & (sizeof(intmax_t)-1)) == 0);
        memset(((char*)ret) + aligned_size - align_pad, 0, align_pad);
        return ret;
    }

    if(!stalloc_init(&st->extra))
        return NULL;

    st->top = NULL;
    return stalloc(&st->extra, size);
}

void sstalloc_clear(struct smemstack *st)
{
    if(!st->top)
        stalloc_destroy(&st->extra);
    st->top = st->mem;
}

