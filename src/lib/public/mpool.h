/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019 Eduard Permyakov 
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

#ifndef MPOOL_H
#define MPOOL_H

#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

/* Hold on to objects by their handles. Unlike pointers, they don't need to be */
/* invalidated when a realloc takes place. */
typedef uint16_t mp_ref_t;

/***********************************************************************************************/

#define MPOOL_TYPE(name, type)                                                                  \
                                                                                                \
    typedef struct mp_##name##_node_s {                                                         \
        mp_ref_t inext_free;                                                                    \
        type entry;                                                                             \
    } mp_##name##_node_t;                                                                       \
                                                                                                \
    typedef struct mp_##name##_s {                                                              \
        size_t capacity;                                                                        \
        size_t num_allocd;                                                                      \
        mp_ref_t ifree_head;                                                                    \
        mp_##name##_node_t *pool;                                                               \
    } mp_##name##_t;                                                                            \

/***********************************************************************************************/

#define mp(name)                                                                                \
    mp_##name##_t

/***********************************************************************************************/

#define MPOOL_PROTOTYPES(scope, name, type)                                                     \
                                                                                                \
    scope void     mp_##name##_init   (mp(name) *mp);                                           \
    scope bool     mp_##name##_reserve(mp(name) *mp, size_t new_cap);                           \
    scope void     mp_##name##_destroy(mp(name) *mp);                                           \
    scope mp_ref_t mp_##name##_alloc  (mp(name) *mp);                                           \
    scope void     mp_##name##_free   (mp(name) *mp, mp_ref_t ref);                             \
    /* The entryory pointer may invalidated when a new allocation is filled by the mempool. */  \
    /* For this reason, cache the reference but not the pointer. */                             \
    scope type    *mp_##name##_entry  (mp(name) *mp, mp_ref_t ref);                             \
    scope void     mp_##name##_clear  (mp(name) *mp);

/***********************************************************************************************/

#define MPOOL_IMPL(scope, name, type)                                                           \
                                                                                                \
    scope void mp_##name##_init(mp(name) *mp)                                                   \
    {                                                                                           \
        memset(mp, 0, sizeof(*mp));                                                             \
    }                                                                                           \
                                                                                                \
    scope bool mp_##name##_reserve(mp(name) *mp, size_t new_cap)                                \
    {                                                                                           \
        size_t old_cap = mp->capacity;                                                          \
        if(new_cap <= old_cap)                                                                  \
            return true;                                                                        \
                                                                                                \
        mp_##name##_node_t *new_entry = realloc(mp->pool,                                       \
            (new_cap + 1) * sizeof(mp_##name##_node_t));                                        \
        if(!new_entry)                                                                          \
            return false;                                                                       \
                                                                                                \
        for(int i = old_cap; i < new_cap; ++i) {                                                \
            new_entry[i].inext_free = i + 1;                                                    \
        }                                                                                       \
        new_entry[new_cap].inext_free = 0;                                                      \
                                                                                                \
        if(!mp->ifree_head) {                                                                   \
            /* Skip the first node - index 0 is used as NULL */                                 \
            mp->ifree_head = 1;                                                                 \
        }else {                                                                                 \
            /* Append at the front */                                                           \
            new_entry[new_cap].inext_free = mp->ifree_head;                                     \
            mp->ifree_head = old_cap + 1;                                                       \
        }                                                                                       \
                                                                                                \
        mp->pool = new_entry;                                                                   \
        mp->capacity = new_cap;                                                                 \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope void mp_##name##_destroy(mp(name) *mp)                                                \
    {                                                                                           \
        free(mp->pool);                                                                         \
        memset(mp, 0, sizeof(*mp));                                                             \
    }                                                                                           \
                                                                                                \
    scope mp_ref_t mp_##name##_alloc(mp(name) *mp)                                              \
    {                                                                                           \
        if(mp->num_allocd == mp->capacity) {                                                    \
            if(!mp_##name##_reserve(mp, mp->capacity ? mp->capacity * 2 : 32))                  \
                return 0;                                                                       \
        }                                                                                       \
                                                                                                \
        assert(mp->ifree_head > 0);                                                             \
        mp_ref_t ret = mp->ifree_head;                                                          \
                                                                                                \
        mp->ifree_head = mp->pool[mp->ifree_head].inext_free;                                   \
        ++mp->num_allocd;                                                                       \
        return ret;                                                                             \
    }                                                                                           \
                                                                                                \
    scope void mp_##name##_free(mp(name) *mp, mp_ref_t ref)                                     \
    {                                                                                           \
        if(!ref)                                                                                \
            return;                                                                             \
        assert(mp->num_allocd > 0);                                                             \
        assert(ref <= mp->capacity);                                                            \
                                                                                                \
        mp->pool[ref].inext_free = mp->ifree_head;                                              \
        mp->ifree_head = ref;                                                                   \
        --mp->num_allocd;                                                                       \
    }                                                                                           \
                                                                                                \
    scope type *mp_##name##_entry(mp(name) *mp, mp_ref_t ref)                                   \
    {                                                                                           \
        return &mp->pool[ref].entry;                                                            \
    }                                                                                           \
                                                                                                \
    scope void mp_##name##_clear(mp(name) *mp)                                                  \
    {                                                                                           \
        if(mp->capacity == 0)                                                                   \
            return;                                                                             \
        mp->num_allocd = 0;                                                                     \
        mp->ifree_head = 1;                                                                     \
                                                                                                \
        for(int i = 1; i < mp->capacity; ++i) {                                                 \
            mp->pool[i].inext_free = i + 1;                                                     \
        }                                                                                       \
        mp->pool[mp->capacity].inext_free = 0;                                                  \
    }

#endif

