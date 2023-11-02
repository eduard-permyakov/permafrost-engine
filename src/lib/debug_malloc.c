/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2021-2023 Eduard Permyakov 
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

#include "../config.h"

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <stddef.h>

#include <SDL.h>

#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define ALIGNED(val, align) (((val) + ((align) - 1)) & ~((align) - 1))
#define HEADER(ptr)         (((intmax_t*)(ptr))    - 3)
#define PTR(header)         (((intmax_t*)(header)) + 3)

/* The debug allocator is available for Linux builds. It overrides 
 * the glibc allocator functions (simply compile the functions - the 
 * linker will use them intead of the glibc weak symbols), allowing 
 * adding of debug/tracing code. Currently there are is some metadata 
 * added before and after the allocated block to catch certain categories 
 * of overwrites/underwrites. Careful with adding code here - it must both 
 * be threadsafe and not make use of 'malloc' - not an easy task.
 *
 * Relink with the '-lmcheck' linker flag to also force some additional 
 * heap consistency checks in glibc.
 *
 * The MMAP allocator will get all memory via mmap instead of malloc,
 * also making an effort to cycle through the virtual address space.
 * As the page permissions will be changed at 'free' time, this will
 * cause a fault to occur if some client code tries to dereference a
 * dangling pointer. This allocator is both slower and more wasteful
 * of memory, but extremely handy in catching use-after-free bugs.
 * 
 * When using the MMAP allocator, you will likely need to increase the
 * number of allowed per-process virtual mappings. This can be done 
 * with the following command:
 * 
 *     sysctl -w vm.max_map_count=xxxxxx
 */

#if CONFIG_USE_DEBUG_ALLOCATOR
#if defined(__linux__) && !defined(NDEBUG)

#pragma GCC push_options
#pragma GCC optimize ("O0")

#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <mcheck.h>
#include <sys/mman.h>

extern void *__libc_malloc(size_t size);
extern void *__libc_realloc(void *ptr, size_t size);
extern void *__libc_calloc(size_t num, size_t size);
extern void  __libc_free(void *ptr);
extern void *__libc_memalign(size_t alignment, size_t size);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static uintptr_t       s_page_base = 65536;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static inline void check(void *header)
{
    intmax_t *allocd = header;
    if(allocd[2] != 0xDEADBEEF)
        goto fail;

    size_t allocsize = allocd[0];
    char data[] = {0xDE, 0xAD, 0xBE, 0xEF};

    void *footer = ((char*)header + allocsize + 3 * sizeof(intmax_t));
    if(0 != memcmp(footer, data, sizeof(data)))
        goto fail;

#if !CONFIG_DEBUG_ALLOC_MMAP
    mcheck_check_all();
#endif
    return;

fail:
    abort();
}

/* The footer location may not necessarily be aligned, so write it byte-by-byte */
static void write_footer(void *footer)
{
    char data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    memcpy(footer, data, sizeof(data));
}

static void trap(void)
{
    volatile int e = errno;
    volatile int *zero = NULL;
    *zero = 0x1;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void *malloc(size_t size)
{
    size_t newsize = size + sizeof(intmax_t) * 4;

#if CONFIG_DEBUG_ALLOC_MMAP
    size_t page_size = sysconf(_SC_PAGESIZE);
    intmax_t *header = mmap((void*)s_page_base, newsize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    s_page_base += ceil(newsize / (float)page_size) * page_size;

    if(header == (void*)-1) {
        trap();
        return NULL;
    }
#else
    intmax_t *header = __libc_malloc(newsize);
    if(!header)
        return NULL;
#endif

    header[0] = size;
    header[1] = (uintptr_t)header;
    header[2] = 0xDEADBEEF;
    write_footer(((char*)header) + newsize - sizeof(intmax_t));

    check(header);
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

#if CONFIG_DEBUG_ALLOC_MMAP
        size_t page_size = sysconf(_SC_PAGESIZE);
        intmax_t *newheader = mmap((void*)s_page_base, newsize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        s_page_base += ceil(newsize / (float)page_size) * page_size;

        if(newheader == (void*)-1) {
            trap();
            return NULL;
        }

        size_t cpysize = MIN(newsize, header[0]);
        memcpy(PTR(newheader), PTR(header), cpysize);
        munmap((void*)header[1], header[0]);
#else
        intmax_t *newheader = __libc_realloc((void*)header[1], newsize);
        if(!newheader)
            return NULL;
#endif

        newheader[0] = size;
        newheader[1] = (uintptr_t)newheader;
        newheader[2] = 0xDEADBEEF;
        write_footer(((char*)newheader) + newsize - sizeof(intmax_t));

        check(newheader);
        return PTR(newheader);
    }
    return malloc(size);
}

void free(void *ptr)
{
    if(ptr) {
        intmax_t *header = HEADER(ptr);
        check(header);
#if CONFIG_DEBUG_ALLOC_MMAP
        munmap((void*)header[1], header[0]);
#else
        __libc_free((void*)header[1]);
#endif
    }
}

void *memalign(size_t alignment, size_t size)
{
    alignment = MAX(alignment, sizeof(intmax_t));
    size_t asize = ALIGNED(size, alignment) + 4 * alignment;
    assert(asize % alignment == 0);

#if CONFIG_DEBUG_ALLOC_MMAP
    size_t page_size = sysconf(_SC_PAGESIZE);
    char *allocd = mmap((void*)s_page_base, asize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    s_page_base += ceil(asize / (float)page_size) * page_size;

    if(allocd == (void*)-1) {
        trap();
        return NULL;
    }
#else
    char *allocd = __libc_memalign(alignment, asize);
    if(!allocd)
        return NULL;
#endif

    char *ret = (void*)ALIGNED((uintptr_t)allocd, alignment) + 3 * alignment;
    assert(((uintptr_t)ret) == ALIGNED(((uintptr_t)ret), alignment));

    *((intmax_t*)(ret - 3 * sizeof(intmax_t))) = size;
    *((intmax_t*)(ret - 2 * sizeof(intmax_t))) = (uintptr_t)allocd;
    *((intmax_t*)(ret - 1 * sizeof(intmax_t))) = 0xDEADBEEF;
    write_footer(ret + size);

    check(HEADER(ret));
    return ret;
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

#pragma GCC pop_options

#endif
#endif //CONFIG_USE_DEBUG_ALLOCATOR

