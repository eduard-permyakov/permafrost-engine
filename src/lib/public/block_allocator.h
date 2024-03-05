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
 
#ifndef BLOCK_ALLOCATOR_H
#define BLOCK_ALLOCATOR_H

#include "vec.h"

VEC_TYPE(block, void*)
VEC_IMPL(static inline, block, void*)

struct block_allocator{
    vec_block_t blockstack;
    size_t      blocksize;
};

static inline void block_alloc_init(struct block_allocator *alloc, size_t size, size_t init_capacity)
{
    vec_block_init(&alloc->blockstack);
    alloc->blocksize = size;

    for(int i = 0; i < init_capacity; i++) {
        void *block = malloc(size);
        vec_block_push(&alloc->blockstack, block);
    }
}

static inline void block_alloc_destroy(struct block_allocator *alloc)
{
    for(int i = 0; i < vec_size(&alloc->blockstack); i++) {
        void *block = vec_AT(&alloc->blockstack, i);
        free(block);
    }
    vec_block_destroy(&alloc->blockstack);
}

static inline void *block_alloc(struct block_allocator *alloc)
{
    if(vec_size(&alloc->blockstack) > 0)
        return vec_block_pop(&alloc->blockstack);
    return malloc(alloc->blocksize);
}

static inline void block_free(struct block_allocator *alloc, void *block)
{
    vec_block_push(&alloc->blockstack, block);
}

#endif

