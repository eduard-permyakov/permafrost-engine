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

#ifndef MPOOL_ALLOCATOR_H
#define MPOOL_ALLOCATOR_H

#include "mpool.h"

#include <stdio.h>

/* The pool allocator is essentially a list of memory pools with a bit of additional 
 * bookkeeping. The advantage over using just a single memory pool is  that we can 
 * hand out raw memory pointers while allowing the pool size to grow dynamically.
 */

/***********************************************************************************************/

#define MPOOL_ALLOCATOR_TYPE(name, type)                                                        \
                                                                                                \
    MPOOL_TYPE(name, type)                                                                      \
                                                                                                \
    typedef struct mpa_##name##_s {                                                             \
        size_t chunk_size;                                                                      \
        size_t num_chunks;                                                                      \
        size_t max_chunks; /* 0 for 'unlimited' */                                              \
        size_t first_free_chunk;                                                                \
        size_t capacity;                                                                        \
        size_t size;                                                                            \
        mp_##name##_t *chunks;                                                                  \
    } mpa_##name##_t;

/***********************************************************************************************/

#define mpa(name)                                                                               \
    mpa_##name##_t

/***********************************************************************************************/

#define MPOOL_ALLOCATOR_PROTOTYPES(scope, name, type)                                           \
                                                                                                \
    MPOOL_PROTOTYPES(static inline, name, type)                                                 \
                                                                                                \
    scope bool  mpa_##name##_init   (mpa(name) *mpa, size_t chunk_size, size_t max_chunks);     \
    scope bool  mpa_##name##_reserve(mpa(name) *mpa, size_t new_cap);                           \
    scope void  mpa_##name##_destroy(mpa(name) *mpa);                                           \
    scope void *mpa_##name##_alloc  (mpa(name) *mpa);                                           \
    scope void  mpa_##name##_free   (mpa(name) *mpa, void *mem);                                \
    scope void  mpa_##name##_clear  (mpa(name) *mpa);                                           \

/***********************************************************************************************/

#define MPOOL_ALLOCATOR_IMPL(scope, name, type)                                                 \
                                                                                                \
    MPOOL_IMPL(static inline, name, type)                                                       \
                                                                                                \
    scope bool mpa_##name##_init(mpa(name) *mpa, size_t chunk_size, size_t max_chunks)          \
    {                                                                                           \
        mpa->chunks = malloc(sizeof(mp_##name##_t));                                            \
        if(!mpa->chunks)                                                                        \
            return false;                                                                       \
                                                                                                \
        mp_##name##_init(mpa->chunks, false);                                                   \
        if(!mp_##name##_reserve(mpa->chunks, chunk_size))                                       \
            return false;                                                                       \
                                                                                                \
        mpa->chunk_size = chunk_size;                                                           \
        mpa->max_chunks = max_chunks;                                                           \
        mpa->num_chunks = 1;                                                                    \
        mpa->first_free_chunk = 0;                                                              \
        mpa->capacity = chunk_size;                                                             \
        mpa->size = 0;                                                                          \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool mpa_##name##_reserve(mpa(name) *mpa, size_t new_cap)                             \
    {                                                                                           \
        while(mpa->capacity < new_cap) {                                                        \
                                                                                                \
            if(mpa->max_chunks != 0 && mpa->num_chunks == mpa->max_chunks)                      \
                return false;                                                                   \
            mp_##name##_t *newchunks = realloc(mpa->chunks,                                     \
                (mpa->num_chunks + 1) * sizeof(mp_##name##_t));                                 \
            if(!newchunks)                                                                      \
                return false;                                                                   \
            mp_##name##_init(newchunks + mpa->num_chunks, false);                               \
            if(!mp_##name##_reserve(newchunks + mpa->num_chunks, mpa->chunk_size))              \
                return false;                                                                   \
            mpa->chunks = newchunks;                                                            \
            mpa->num_chunks += 1;                                                               \
            mpa->capacity += mpa->chunk_size;                                                   \
        }                                                                                       \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope void mpa_##name##_destroy(mpa(name) *mpa)                                             \
    {                                                                                           \
        for(int i = 0; i < mpa->num_chunks; i++) {                                              \
            mp_##name##_destroy(&mpa->chunks[i]);                                               \
        }                                                                                       \
        free(mpa->chunks);                                                                      \
    }                                                                                           \
                                                                                                \
    scope void *mpa_##name##_alloc(mpa(name) *mpa)                                              \
    {                                                                                           \
        if(mpa->capacity == mpa->size) {                                                        \
            if(!mpa_##name##_reserve(mpa, mpa->capacity + mpa->chunk_size))                     \
                return NULL;                                                                    \
            mpa->first_free_chunk = mpa->num_chunks - 1;                                        \
        }                                                                                       \
                                                                                                \
        mp_##name##_t *pool = &mpa->chunks[mpa->first_free_chunk];                              \
        while(pool->num_allocd == pool->capacity) {                                             \
            mpa->first_free_chunk++;                                                            \
            pool = &mpa->chunks[mpa->first_free_chunk];                                         \
        }                                                                                       \
                                                                                                \
        mp_ref_t ref = mp_##name##_alloc(pool);                                                 \
        assert(ref > 0 && ref <= mpa->chunk_size);                                              \
        mpa->size++;                                                                            \
        return mp_##name##_entry(pool, ref);                                                    \
    }                                                                                           \
                                                                                                \
    scope void mpa_##name##_free(mpa(name) *mpa, void *mem)                                     \
    {                                                                                           \
        if(mem == NULL)                                                                         \
            return;                                                                             \
                                                                                                \
        bool found = false;                                                                     \
        for(int i = 0; i < mpa->num_chunks; i++) {                                              \
            mp_##name##_t *pool = &mpa->chunks[i];                                              \
            uintptr_t base = (uintptr_t)(pool->pool + 1);                                       \
            uintptr_t limit = (uintptr_t)(pool->pool + pool->capacity + 1);                     \
            uintptr_t umem = (uintptr_t)mem;                                                    \
            if(umem >= base && umem < limit) {                                                  \
                mp_ref_t ref = mp_##name##_ref(pool, mem);                                      \
                assert(ref > 0 && ref <= mpa->chunk_size);                                      \
                mp_##name##_free(pool, ref);                                                    \
                mpa->size--;                                                                    \
                found = true;                                                                   \
                if(i < mpa->first_free_chunk) {                                                 \
                    mpa->first_free_chunk = i;                                                  \
                }                                                                               \
            }                                                                                   \
        }                                                                                       \
        assert(found);                                                                          \
    }                                                                                           \
                                                                                                \
    scope void mpa_##name##_clear(mpa(name) *mpa)                                               \
    {                                                                                           \
        for(int i = 0; i < mpa->num_chunks; i++) {                                              \
            mp_##name##_clear(&mpa->chunks[i]);                                                 \
        }                                                                                       \
        mpa->size = 0;                                                                          \
        mpa->first_free_chunk = 0;                                                              \
    }                                                                                           \

#endif

