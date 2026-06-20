/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
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

#if !defined(_WIN32)
#define _GNU_SOURCE
#endif

#define MEM_FILE_SYS MEM_SYS_LIB

#include "public/pf_cow_region.h"

#if defined(_WIN32)
#include "public/windows.h"
#include <psapi.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../lib/public/mem.h"

#if !defined(_WIN32) && !defined(MFD_CLOEXEC)
#define MFD_CLOEXEC 0x0001U
#endif

struct pf_cow_region{
    size_t size;
#if defined(_WIN32)
    HANDLE mapping;
#else
    int    fd;
    int    pagemap_fd; /* /proc/self/pagemap, for per-page dirty detection */
#endif
    void  *canonical;  /* shared mapping: the published state, read by readers */
    void  *writer;     /* private copy-on-write mapping: mutated by the writer  */
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static size_t cow_page_size(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    long ret = sysconf(_SC_PAGESIZE);
    return (ret > 0) ? (size_t)ret : 4096;
#endif
}

static size_t cow_round_up(size_t n, size_t mult)
{
    return ((n + mult - 1) / mult) * mult;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

struct pf_cow_region *pf_cow_create(size_t size)
{
    if(size == 0)
        return NULL;

    struct pf_cow_region *region = PF_MALLOC_TAGGED(sizeof(*region), MEM_SYS_LIB, 0);
    if(!region)
        return NULL;

    memset(region, 0, sizeof(*region));
    region->size = cow_round_up(size, cow_page_size());
#if !defined(_WIN32)
    region->fd = -1;
    region->pagemap_fd = -1;
#endif

#if defined(_WIN32)
    region->mapping = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        (DWORD)(((uint64_t)region->size) >> 32), (DWORD)(region->size & 0xffffffffu), NULL);
    if(!region->mapping)
        goto fail;
    region->canonical = MapViewOfFile(region->mapping, FILE_MAP_WRITE, 0, 0, region->size);
    if(!region->canonical)
        goto fail;
    region->writer = MapViewOfFile(region->mapping, FILE_MAP_COPY, 0, 0, region->size);
    if(!region->writer)
        goto fail;
#else
    region->fd = memfd_create("pf_cow", MFD_CLOEXEC);
    if(region->fd == -1)
        goto fail;
    if(ftruncate(region->fd, (off_t)region->size) == -1)
        goto fail;
    region->canonical = mmap(NULL, region->size, PROT_READ | PROT_WRITE,
        MAP_SHARED, region->fd, 0);
    if(region->canonical == MAP_FAILED) {
        region->canonical = NULL;
        goto fail;
    }
    region->writer = mmap(NULL, region->size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE, region->fd, 0);
    if(region->writer == MAP_FAILED) {
        region->writer = NULL;
        goto fail;
    }
    /* Enables per-page publish; if it fails, publish falls back to whole ranges. */
    region->pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
#endif
    return region;

fail:
    pf_cow_destroy(region);
    return NULL;
}

void pf_cow_destroy(struct pf_cow_region *region)
{
    if(!region)
        return;

#if defined(_WIN32)
    if(region->writer)
        UnmapViewOfFile(region->writer);
    if(region->canonical)
        UnmapViewOfFile(region->canonical);
    if(region->mapping)
        CloseHandle(region->mapping);
#else
    if(region->writer)
        munmap(region->writer, region->size);
    if(region->canonical)
        munmap(region->canonical, region->size);
    if(region->fd != -1)
        close(region->fd);
    if(region->pagemap_fd != -1)
        close(region->pagemap_fd);
#endif
    PF_FREE(region);
}

size_t pf_cow_size(const struct pf_cow_region *region)
{
    return region->size;
}

void *pf_cow_writer_base(struct pf_cow_region *region)
{
    return region->writer;
}

const void *pf_cow_reader_base(const struct pf_cow_region *region)
{
    return region->canonical;
}

/* Commit the pages in the writer span [pstart, pend) that the writer privately
 * modified (copy-on-write) to the canonical, leaving pages still shared with the
 * canonical untouched.
 *
 * The OS reports which pages are private: on Linux via the pagemap bits (an
 * unwritten page still shares the canonical's page); on Windows via
 * QueryWorkingSetEx (a written copy-on-write page is no longer Shared).
 *
 * On Linux the committed pages are then dropped (MADV_DONTNEED) so they re-alias
 * the canonical and the next write to them faults fresh; on Windows there is no
 * cheap per-page reset for a copy-on-write view, so committed pages stay private
 * and are re-copied next publish.
 */
static bool cow_commit_dirty_pages(struct pf_cow_region *region, size_t pstart, size_t pend)
{
    size_t ps = cow_page_size();
    char *wbase = region->writer;
    char *cbase = region->canonical;

#if defined(_WIN32)
    PSAPI_WORKING_SET_EX_INFORMATION info[256];
    HANDLE proc = GetCurrentProcess();
    for(size_t base = pstart; base < pend; base += 256 * ps) {
        size_t npages = (pend - base) / ps;
        if(npages > 256)
            npages = 256;
        for(size_t k = 0; k < npages; k++)
            info[k].VirtualAddress = wbase + base + k * ps;
        if(!QueryWorkingSetEx(proc, info, (DWORD)(npages * sizeof(info[0])))) {
            memcpy(cbase + base, wbase + base, npages * ps);
            continue;
        }
        size_t run = (size_t)-1;
        for(size_t k = 0; k <= npages; k++) {
            bool dirty = (k < npages)
                      && info[k].VirtualAttributes.Valid
                      && !info[k].VirtualAttributes.Shared;
            if(dirty && run == (size_t)-1) {
                run = base + k * ps;
            }else if(!dirty && run != (size_t)-1) {
                size_t rend = base + k * ps;
                memcpy(cbase + run, wbase + run, rend - run);
                run = (size_t)-1;
            }
        }
    }
    return true;
#else
    if(region->pagemap_fd < 0) {
        memcpy(cbase + pstart, wbase + pstart, pend - pstart);
        return madvise(wbase + pstart, pend - pstart, MADV_DONTNEED) == 0;
    }
    uint64_t batch[256];
    for(size_t base = pstart; base < pend; base += 256 * ps) {
        size_t npages = (pend - base) / ps;
        if(npages > 256)
            npages = 256;
        off_t pmoff = (off_t)(((uintptr_t)wbase + base) / ps) * sizeof(uint64_t);
        ssize_t got = pread(region->pagemap_fd, batch, npages * sizeof(uint64_t), pmoff);
        if(got != (ssize_t)(npages * sizeof(uint64_t))) {
            memcpy(cbase + base, wbase + base, npages * ps);
            if(madvise(wbase + base, npages * ps, MADV_DONTNEED) != 0)
                return false;
            continue;
        }
        size_t run = (size_t)-1;
        for(size_t k = 0; k <= npages; k++) {
            /* Bit 56 = exclusively mapped, bit 62 = swapped: either marks a page
             * the writer privately copied. */
            bool dirty = (k < npages)
                      && (batch[k] & ((1ULL << 56) | (1ULL << 62)));
            if(dirty && run == (size_t)-1) {
                run = base + k * ps;
            }else if(!dirty && run != (size_t)-1) {
                size_t rend = base + k * ps;
                memcpy(cbase + run, wbase + run, rend - run);
                if(madvise(wbase + run, rend - run, MADV_DONTNEED) != 0)
                    return false;
                run = (size_t)-1;
            }
        }
    }
    return true;
#endif
}

bool pf_cow_publish(struct pf_cow_region *region, const struct cow_range *dirty, size_t ndirty)
{
    size_t ps = cow_page_size();
    for(size_t i = 0; i < ndirty; i++) {

        size_t off = dirty[i].off;
        size_t len = dirty[i].len;

        if(off > region->size || len > region->size - off)
            return false;
        if(len == 0)
            continue;

        size_t pstart = off & ~(ps - 1);
        size_t pend = (off + len + ps - 1) & ~(ps - 1);
        if(pend > region->size)
            pend = region->size;

        if(!cow_commit_dirty_pages(region, pstart, pend))
            return false;
    }
    return true;
}

