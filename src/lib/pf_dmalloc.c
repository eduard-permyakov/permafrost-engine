/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2021 Eduard Permyakov 
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

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <stddef.h>

#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define ALIGNED(val, align) (((val) + ((align) - 1)) & ~((align) - 1))
#define HEADER(ptr)         (((intmax_t*)ptr)    - 3)
#define PTR(header)         (((intmax_t*)header) + 3)

#if defined(__linux__) && !defined(NDEBUG)

#include <unistd.h>
#include <errno.h>

extern void *__libc_malloc(size_t size);
extern void *__libc_realloc(void *ptr, size_t size);
extern void  __libc_free(void *ptr);
extern void *__libc_memalign(size_t alignment, size_t size);

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static inline void check(void *header)
{
    intmax_t *allocd = header;
    assert(allocd[2] == 0xDEADBEEF && "underwrite detected");
    size_t allocsize = allocd[0];

    char data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    void *footer = ((char*)header + allocsize + 3 * sizeof(intmax_t));
    assert(0 == memcmp(footer, data, sizeof(data)) && "overwrite detected");
}

/* The footer location may not necessarily be aligned, so write it byte-by-byte */
static void write_footer(void *footer)
{
    char data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    memcpy(footer, data, sizeof(data));
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void *malloc(size_t size)
{
    size_t newsize = size + sizeof(intmax_t) * 4;
    intmax_t *header = __libc_malloc(newsize);
    if(!header)
        return NULL;
    header[0] = size;
    header[1] = (uintptr_t)header;
    header[2] = 0xDEADBEEF;
    write_footer((char*)header + newsize - sizeof(intmax_t));
    return PTR(header);
}

void *calloc(size_t num, size_t size)
{
    void *ret = malloc(num * size);
    memset(ret, 0, num * size);
    return ret;
}

void *realloc(void *ptr, size_t size)
{
    if(ptr) {
        intmax_t *header = HEADER(ptr);
        check(header);

        size_t newsize = size + sizeof(intmax_t) * 4;
        intmax_t *newheader = __libc_realloc((void*)header[1], newsize);
        if(!newheader)
            return NULL;

        newheader[0] = size;
        newheader[1] = (uintptr_t)newheader;
        newheader[2] = 0xDEADBEEF;
        write_footer((char*)newheader + newsize - sizeof(intmax_t));
        return PTR(newheader);
    }
    return malloc(size);
}

void free(void *ptr)
{
    if(ptr) {
        intmax_t *header = HEADER(ptr);
        check(header);
        return __libc_free((void*)header[1]);
    }
    return __libc_free(ptr);
}

void *memalign(size_t alignment, size_t size)
{
    alignment = MAX(alignment, sizeof(intmax_t));
    size_t asize = ALIGNED(size, alignment) + 4 * alignment;
    assert(asize % alignment == 0);
    char *allocd = __libc_memalign(alignment, asize);

    *((intmax_t*)(allocd + 3 * alignment - 3 * sizeof(intmax_t))) = size;
    *((intmax_t*)(allocd + 3 * alignment - 2 * sizeof(intmax_t))) = (uintptr_t)allocd;
    *((intmax_t*)(allocd + 3 * alignment - 1 * sizeof(intmax_t))) = 0xDEADBEEF;
    write_footer(allocd + 3 * alignment + size);

    assert((uintptr_t)(allocd + 3 * alignment) == ALIGNED((uintptr_t)(allocd + 3 * alignment), alignment));
    return allocd + 3 * alignment;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    if(size == 0) {
        *memptr = NULL;
        return 0;
    }

    void *ret = memalign(alignment, size);
    if(!ret)
        return -ENOMEM;

    *memptr = ret;
    return 0;
}

void *aligned_alloc(size_t alignment, size_t size)
{
    return memalign(alignment, size);
}

void *valloc(size_t size)
{
    size_t page_size = sysconf(_SC_PAGESIZE);
    return memalign(page_size, size);
}

void *pvalloc(size_t size)
{
    size_t page_size = sysconf(_SC_PAGESIZE);
    size = ceil(size / ((float)page_size)) * page_size;
    return memalign(page_size, size);
}

size_t malloc_usable_size(void *ptr)
{
    intmax_t *header = HEADER(ptr);
    check(header);
    return header[0];
}

#endif

