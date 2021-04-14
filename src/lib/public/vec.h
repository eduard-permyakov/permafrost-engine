/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2020 Eduard Permyakov 
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

#ifndef VEC_H
#define VEC_H

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/***********************************************************************************************/

#define VEC_TYPE(name, type)                                                                    \
                                                                                                \
    typedef struct vec_##name##_s {                                                             \
        size_t size;                                                                            \
        size_t capacity;                                                                        \
        type *array;                                                                            \
        void *(*vrealloc)(void *ptr, size_t size);                                              \
        void  (*vfree)(void *ptr);                                                              \
    } vec_##name##_t;

/***********************************************************************************************/

#define vec(name)                                                                               \
    vec_##name##_t

/***********************************************************************************************/

#define vec_size(vec)                                                                           \
    ((vec)->size)

#define vec_AT(vec, i)                                                                          \
    ((vec)->array[i])

/***********************************************************************************************/

#define VEC_PROTOTYPES(scope, name, type)                                                       \
                                                                                                \
    scope void vec_##name##_init    (vec(name) *vec);                                           \
    scope void vec_##name##_init_alloc(vec(name) *vec,                                          \
                                       void *(*vrealloc)(void *ptr, size_t size),               \
                                       void (*vfree)(void *ptr));                               \
    scope bool vec_##name##_resize  (vec(name) *vec, size_t new_cap);                           \
    scope void vec_##name##_destroy (vec(name) *vec);                                           \
    scope bool vec_##name##_push    (vec(name) *vec, type in);                                  \
    scope bool vec_##name##_del     (vec(name) *vec, int del_idx);                              \
    scope int  vec_##name##_indexof (vec(name) *vec, type t, bool (*comparator)());             \
    scope void vec_##name##_reset   (vec(name) *vec);                                           \
    scope bool vec_##name##_copy    (vec(name) *dst, vec(name) *src);                           \
    scope type vec_##name##_pop     (vec(name) *vec);                                           \
    scope bool vec_##name##_concat  (vec(name) *a, vec(name) *b);                               \
    scope void vec_##name##_subtract(vec(name) *a, vec(name) *b, bool (*comparator)());

/***********************************************************************************************/

#define VEC_IMPL(scope, name, type)                                                             \
                                                                                                \
    scope void vec_##name##_init(vec(name) *vec)                                                \
    {                                                                                           \
        vec->size = 0;                                                                          \
        vec->capacity = 0;                                                                      \
        vec->array = NULL;                                                                      \
        vec->vrealloc = realloc;                                                                \
        vec->vfree = free;                                                                      \
    }                                                                                           \
                                                                                                \
    scope void vec_##name##_init_alloc(vec(name) *vec,                                          \
                                       void *(*vrealloc)(void *ptr, size_t size),               \
                                       void (*vfree)(void *ptr))                                \
    {                                                                                           \
        vec->size = 0;                                                                          \
        vec->capacity = 0;                                                                      \
        vec->array = NULL;                                                                      \
        vec->vrealloc = vrealloc;                                                               \
        vec->vfree = vfree;                                                                     \
    }                                                                                           \
                                                                                                \
    scope bool vec_##name##_resize(vec(name) *vec, size_t new_cap)                              \
    {                                                                                           \
        if(vec->capacity >= new_cap)                                                            \
            return true;                                                                        \
                                                                                                \
        type *new_array = vec->vrealloc(vec->array, new_cap * sizeof(type));                    \
        if(!new_array)                                                                          \
            return false;                                                                       \
                                                                                                \
        vec->array = new_array;                                                                 \
        vec->capacity = new_cap;                                                                \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope void vec_##name##_destroy(vec(name) *vec)                                             \
    {                                                                                           \
        vec->vfree(vec->array);                                                                 \
        memset(vec, 0, sizeof(*vec));                                                           \
    }                                                                                           \
                                                                                                \
    scope bool vec_##name##_push(vec(name) *vec, type in)                                       \
    {                                                                                           \
        if(vec->size == vec->capacity                                                           \
        && !vec_##name##_resize(vec, (vec->size == 0 ? 16 : vec->size * 2) * sizeof(type)))     \
            return false;                                                                       \
                                                                                                \
        vec->array[vec->size++] = in;                                                           \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool vec_##name##_del(vec(name) *vec, int del_idx)                                    \
    {                                                                                           \
        if(del_idx < 0 || del_idx >= vec->size)                                                 \
            return false;                                                                       \
                                                                                                \
        vec->array[del_idx] = vec->array[--vec->size];                                          \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope int vec_##name##_indexof(vec(name) *vec, type t, bool (*comparator)())                \
    {                                                                                           \
        int ret = -1;                                                                           \
        for(int i = 0; i < vec->size; i++) {                                                    \
            if(comparator(&vec->array[i], &t)) {                                                \
                ret = i;                                                                        \
                break;                                                                          \
            }                                                                                   \
        }                                                                                       \
        return ret;                                                                             \
    }                                                                                           \
                                                                                                \
    scope void vec_##name##_reset(vec(name) *vec)                                               \
    {                                                                                           \
        vec->size = 0;                                                                          \
    }                                                                                           \
                                                                                                \
    scope bool vec_##name##_copy(vec(name) *dst, vec(name) *src)                                \
    {                                                                                           \
        if(!vec_##name##_resize(dst, vec_size(src)))                                            \
            return false;                                                                       \
                                                                                                \
        memcpy(dst->array, src->array, src->size * sizeof(type));                               \
        dst->size = src->size;                                                                  \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope type vec_##name##_pop(vec(name) *vec)                                                 \
    {                                                                                           \
        return vec->array[--vec->size];                                                         \
    }                                                                                           \
                                                                                                \
    scope bool vec_##name##_concat(vec(name) *a, vec(name) *b)                                  \
    {                                                                                           \
        size_t total = a->size + b->size;                                                       \
        if(total == 0)                                                                          \
            return true;                                                                        \
        if(!vec_##name##_resize(a, total))                                                      \
            return false;                                                                       \
                                                                                                \
        memcpy(a->array + a->size, b->array, b->size * sizeof(type));                           \
        a->size = total;                                                                        \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope void vec_##name##_subtract(vec(name) *a, vec(name) *b, bool (*comparator)())          \
    {                                                                                           \
        for(int i = 0; i < b->size; i++) {                                                      \
            int idx = vec_##name##_indexof(a, vec_AT(b, i), comparator);                        \
            if(idx >= 0) {                                                                      \
                vec_##name##_del(a, idx);                                                       \
            }                                                                                   \
        }                                                                                       \
    }                                                                                           \

#endif

