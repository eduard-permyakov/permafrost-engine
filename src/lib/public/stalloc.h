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

#ifndef STALLOC_H
#define STALLOC_H

#include <stddef.h>
#include <stdbool.h>


#define STATIC_BUFF_SZ (512*1024)
#define MEMBLOCK_SZ    (64*1024*1024)

/* The memstack allows variable-sized allocations from larger pre-allocated 
 * blocks. The point is to reduce ovehead of 'malloc' and 'free' when wanting
 * to make many small allocations. 
 *
 * The memblocks are chained in a linked list. When one memblock is exhausted,
 * another one is allocated from the OS and appended to it. The purpose is to 
 * allow arbitrary many allocations without needing to invalidate pointers to
 * prior allocations, which would be required with a 'realloc'-based approach. 
 *
 * The allocations cannot be freed in arbitrary order. The API provides only a
 * means to clear all the allocations at once. Hence, this allocator is good 
 * for cases where all allocations will have the same lifetime (ex. a single
 * frame).
 */

struct st_mem{
    struct st_mem *next;
    unsigned char  raw[MEMBLOCK_SZ];
};

struct memstack{
    struct st_mem *head;
    struct st_mem *tail;
    void          *top; /* Empty Ascending stack */
};

bool  stalloc_init(struct memstack *st);
void  stalloc_destroy(struct memstack *st);

void *stalloc(struct memstack *st, size_t size);
void  stalloc_clear(struct memstack *st);

/* The smemstack is just like the memstack, except that the first 'STATIC_BUFF_SZ' 
 * bytes of allocations will be from the local 'mem' buffer, which can be declared 
 * on the stack or in static storage.
 */

struct smemstack{
    unsigned char    mem[STATIC_BUFF_SZ];
    void            *top; /* Empty Ascending stack; when NULL, extra.top is the TOS */
    struct memstack  extra;
};

bool  sstalloc_init(struct smemstack *st);
void  sstalloc_destroy(struct smemstack *st);

void *sstalloc(struct smemstack *st, size_t size);
void  sstalloc_clear(struct smemstack *st);

#endif

