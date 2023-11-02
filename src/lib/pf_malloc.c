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

#include "public/pf_malloc.h"

#include <stdint.h>
#include <assert.h>
#include <stdlib.h>

/* The maximum number of discrete allocations from 
 * a single slab. */
#define MAX_HEAP_SZ 512

#define SWAP_PTRS(a, b)                                 \
    do{                                                 \
        a = (void*) ((uintptr_t)a ^ (uintptr_t)b);      \
        b = (void*) ((uintptr_t)a ^ (uintptr_t)b);      \
        a = (void*) ((uintptr_t)a ^ (uintptr_t)b);      \
    }while(0)

#define SWAP(a, b)                                      \
    do{                                                 \
        a = a ^ b;                                      \
        b = a ^ b;                                      \
        a = a ^ b;                                      \
    }while(0)

#define MEMBLOCK_MEM(mblock)            ((void*)((char*)(mblock) + ALIGNED(sizeof(struct memblock))))
#define MEM_MEMBLOCK(mem)               ((struct memblock*)((char*)(mem) - ALIGNED(sizeof(struct memblock))))

#define CAN_COALESE_WITH_NEXT(mblock)   ((mblock)->next && (mblock)->next->free)
#define CAN_COALESE_WITH_PREV(mblock)   ((mblock)->prev && (mblock)->prev->free)

#define ALIGNED(size)                   (((size) + (sizeof(intmax_t) - 1)) & ~(sizeof(intmax_t) - 1))

struct memblock{
    bool             free;
    size_t           size;
    size_t           offset;
    int              index;
    struct memblock *next, *prev; /* Doubly-linked list of adjacent blocks used for coalesing */
};

struct memheap{
    struct memblock *blocks[MAX_HEAP_SZ + 1]; /* 0th element empty; top at index 1 */
    size_t           nblocks;
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static size_t align(size_t val, size_t alignment)
{
    size_t pad = (alignment - (val % alignment)) % alignment;
    return val + pad;
}

static size_t align_down(size_t val, size_t alignment)
{
    size_t aval = align(val, alignment);
    aval = aval == val ? aval : aval - alignment;
    return aval;
}
 
static void heap_remove(struct memheap *heap, unsigned i)
{
    heap->blocks[i] = heap->blocks[heap->nblocks--];

    unsigned left, right, curr = i;
    while((left = curr << 1, right = left + 1)) {
    
        bool hasleft = left <= heap->nblocks;
        bool hasright = right <= heap->nblocks;
        unsigned max;

        if(!hasright)
            break;

        if(!hasleft) {
            max = right;
        }else {
            max = heap->blocks[left]->size > heap->blocks[right]->size ? left : right;
        }

        if(heap->blocks[max]->size > heap->blocks[curr]->size) {
            SWAP_PTRS(heap->blocks[max], heap->blocks[curr]);
            SWAP(heap->blocks[max]->index, heap->blocks[curr]->index);
            curr = max;
        }else{
            break;
        }
    }
}

static void heap_insert(struct memheap *heap, struct memblock *new)
{
    heap->blocks[++heap->nblocks] = new;
    new->index = heap->nblocks;

    unsigned parent, curr = heap->nblocks;
    while((parent = curr >> 1)) {

        if(heap->blocks[parent]->size < heap->blocks[curr]->size) {
            SWAP_PTRS(heap->blocks[parent], heap->blocks[curr]);
            SWAP(heap->blocks[parent]->index, heap->blocks[curr]->index);
        }else{
            break;
        }
        curr = parent;
    }
}

static struct memblock *heap_split_block(struct memheap *heap, unsigned i, size_t newsize)
{
    struct memblock *top = heap->blocks[i]; 
    newsize = ALIGNED(newsize);

    struct memblock *new = (struct memblock*)(
        (char*)MEMBLOCK_MEM(top)
        + (top->size - newsize - ALIGNED(sizeof(struct memblock)))
    );
    new->size = newsize;
    new->offset = top->offset + (top->size - newsize);
    new->free = false;
    top->size -= (ALIGNED(sizeof(struct memblock)) + newsize);

    /* Re-insert the block we just resized into the heap */
    heap_remove(heap, i);
    heap_insert(heap, top);

    new->prev = top;
    new->next = top->next;
    top->next = new;

    heap_insert(heap, new);
    return new;
}

static void heap_coalese_blocks(struct memheap *heap, unsigned ifirst, unsigned inext)
{
    struct memblock *first = heap->blocks[ifirst];
    struct memblock *next = heap->blocks[inext];

    struct memblock *pre = first->prev;
    struct memblock *post = next->next;

    first->size += (ALIGNED(sizeof(struct memblock)) + next->size);
    heap_remove(heap, inext);
    first->prev = pre;
    first->next = post;
}

static struct memblock *meta_split_block_aligned(struct memheap *heap, unsigned i, 
                                                 size_t newsize, size_t newalign)
{
    struct memblock *top = heap->blocks[i]; 
    newsize = align(newsize, newalign);

    struct memblock *new = top + 1;
    new->size = newsize + (top->offset + (top->size - newsize)) % newalign;
    new->offset = align_down(top->offset + (top->size - newsize), newalign);
    new->free = false;
    top->size = new->offset - top->offset;

    /* Re-insert the block we just resized into the heap */
    heap_remove(heap, i);
    heap_insert(heap, top);

    new->prev = top;
    new->next = top->next;
    top->next = new;

    heap_insert(heap, new);
    return new;
}

static void meta_coalese_blocks(struct memheap *heap, unsigned ifirst, unsigned inext)
{
    struct memblock *first = heap->blocks[ifirst];
    struct memblock *next = heap->blocks[inext];

    struct memblock *pre = first->prev;
    struct memblock *post = next->next;

    first->size += next->size;
    heap_remove(heap, inext);
    first->prev = pre;
    first->next = post;
}

static struct memblock *meta_block_for_offset(struct memheap *heap, size_t offset)
{
    int low = 1;
    int high = heap->nblocks;
    int mid = (low + high) / 2;

    while(heap->blocks[mid]->offset != offset) {

        if(heap->blocks[mid]->offset > offset) {
            high = mid - 1;
        }else{
            low = mid + 1;
        }
        mid = (low + high) / 2;
    }

    return heap->blocks[mid];
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool pf_malloc_init(void *slab, size_t size)
{
    struct memheap *heap = (struct memheap*)slab;
    struct memblock *head = (struct memblock*)((char*)heap + ALIGNED(sizeof(struct memheap)));
    if(size < ALIGNED(sizeof(*heap)) + ALIGNED(sizeof(*head)))
        return false;

    head->size = ALIGNED(size) - ALIGNED(sizeof(struct memheap)) - ALIGNED(sizeof(struct memblock));
    head->offset = ALIGNED(sizeof(struct memheap)) + ALIGNED(sizeof(struct memblock));
    head->free = true;
    head->next = NULL;
    head->prev = NULL;

    heap->blocks[1] = head;
    heap->nblocks = 1;
    return true;
}

void *pf_malloc(void *slab, size_t size)
{
    struct memheap *heap = (struct memheap*)slab;
    struct memblock *top = heap->blocks[1];

    if(!top->free || size > top->size || heap->nblocks == MAX_HEAP_SZ) {
        return NULL;
    }

    if(top->size - size <= ALIGNED(sizeof(struct memblock))) {
        top->free = false;
        return top;
    }

    return MEMBLOCK_MEM(heap_split_block(heap, 1, size));
}

void pf_free(void *slab, void *ptr)
{
    struct memheap *heap = (struct memheap*)slab;
    struct memblock *mem = MEM_MEMBLOCK(ptr);

    if(CAN_COALESE_WITH_NEXT(mem)) {
        mem->free = true;
        heap_coalese_blocks(heap, mem->index, mem->next->index);
    }
    if(CAN_COALESE_WITH_PREV(mem)) {
        mem->prev->free = true;
        heap_coalese_blocks(heap, mem->prev->index, mem->index);
    }
}

void *pf_metamalloc_init(size_t size)
{
    size_t metasize = sizeof(struct memheap) + MAX_HEAP_SZ * sizeof(struct memblock);
    struct memheap *heap = malloc(metasize);
    if(!heap)
        return NULL;

    struct memblock *head = (struct memblock*)((char*)heap + ALIGNED(sizeof(struct memheap)));
    head->size = size;
    head->offset = 0;
    head->free = true;
    head->next = NULL;
    head->prev = NULL;

    heap->blocks[1] = head;
    heap->nblocks = 1;
    return heap;
}

void pf_metamalloc_destroy(void *meta)
{
    free(meta);
}

int pf_metamalloc(void *meta, size_t size)
{
    struct memheap *heap = (struct memheap*)meta;
    struct memblock *top = heap->blocks[1];

    if(!top->free || size > top->size || heap->nblocks == MAX_HEAP_SZ) {
        return -1;
    }

    if(top->size == size) {
        top->free = false;
        return top->offset;
    }

    return meta_split_block_aligned(heap, 1, size, sizeof(intmax_t))->offset;
}

int pf_metamemalign(void *meta, size_t alignment, size_t size)
{
    struct memheap *heap = (struct memheap*)meta;
    struct memblock *top = heap->blocks[1];

    if(!top->free || heap->nblocks == MAX_HEAP_SZ) {
        return -1;
    }

    size_t pad = align(top->offset, alignment) - top->offset;
    if(top->size + pad < size) {
        return -1;
    }

    if(top->size + pad == size) {
        top->free = false;
        top->offset += pad;
        if(top->prev)
            top->prev->size += pad;
        return top->offset;
    }

    int ret = meta_split_block_aligned(heap, 1, size, alignment)->offset;
    return ret;
}

void pf_metafree(void *meta, size_t offset)
{
    struct memheap *heap = (struct memheap*)meta;
    struct memblock *mem = meta_block_for_offset(heap, offset);

    if(CAN_COALESE_WITH_NEXT(mem)) {
        mem->free = true;
        meta_coalese_blocks(heap, mem->index, mem->next->index);
    }
    if(CAN_COALESE_WITH_PREV(mem)) {
        mem->prev->free = true;
        meta_coalese_blocks(heap, mem->prev->index, mem->index);
    }
}

