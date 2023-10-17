/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2020 Eduard Permyakov 
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

#ifndef PQUEUE_H
#define PQUEUE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/***********************************************************************************************/

#define PQUEUE_TYPE(name, type)                                                                 \
                                                                                                \
    typedef struct pq_##name##_node_s {                                                         \
        float priority;                                                                         \
        type data;                                                                              \
    } pq_##name##_node_t;                                                                       \
                                                                                                \
    typedef struct pq_##name##_s {                                                              \
        pq_##name##_node_t *nodes;                                                              \
        size_t capacity;                                                                        \
        size_t size;                                                                            \
        void *(*prealloc)(void *ptr, size_t size);                                              \
        void  (*pfree)(void *ptr);                                                              \
    } pq_##name##_t;                                                                            \

/***********************************************************************************************/

#define pq(name)                                                                                \
    pq_##name##_t

/***********************************************************************************************/

#define pq_size(pqueue)                                                                         \
    ((pqueue)->size)

#define pq_foreach(_pqueue, _entry, ...)                                                        \
    for(int i = 1; i <= (_pqueue)->size; i++) {                                                 \
        _entry = (_pqueue)->nodes[i].data;                                                      \
        __VA_ARGS__                                                                             \
    }                                                                                           \

/***********************************************************************************************/

#define PQUEUE_PROTOTYPES(scope, name, type)                                                    \
                                                                                                \
    static void _pq_##name##_balance (pq(name) *pqueue, int root_idx);                          \
    scope  void  pq_##name##_init    (pq(name) *pqueue);                                        \
    scope  void  pq_##name##_init_alloc(pq(name) *pqueue,                                       \
                                        void *(*prealloc)(void *ptr, size_t size),              \
                                        void (*pfree)(void *ptr));                              \
    scope  void  pq_##name##_destroy (pq(name) *pqueue);                                        \
    scope  bool  pq_##name##_push    (pq(name) *pqueue, float in_prio, type in);                \
    scope  bool  pq_##name##_pop     (pq(name) *pqueue, type *out);                             \
    scope  bool  pq_##name##_pop_matching(pq(name) *pqueue, type *out, bool pred(void *a));     \
    scope  bool  pq_##name##_peek    (pq(name) *pqueue, type *out);                             \
    scope  bool  pq_##name##_remove  (pq(name) *pqueue, int compare(void *a, void *b), type t); \
    scope  bool  pq_##name##_contains(pq(name) *pqueue, int compare(void *a, void *b), type t); \
    scope  bool  pq_##name##_reserve (pq(name) *pqueue, size_t cap);                            \
    scope  bool  pq_##name##_top_prio(pq(name) *pqueue, float *out);                            \
    scope  bool  pq_##name##_top_prio_of(pq(name) *pqueue, float *out, bool pred(void *a));     \
    scope  bool  pq_##name##_copy    (pq(name) *dst, pq(name) *src);                            \
    scope  void  pq_##name##_clear   (pq(name) *pqueue);                                        \

/***********************************************************************************************/

#define PQUEUE_IMPL(scope, name, type)                                                          \
                                                                                                \
    static void _pq_##name##_balance(pq(name) *pqueue, int root_idx)                            \
    {                                                                                           \
        while(root_idx != pqueue->size + 1) {                                                   \
                                                                                                \
            int target_idx = pqueue->size + 1;                                                  \
            int left_child_idx = root_idx * 2;                                                  \
            int right_child_idx = left_child_idx + 1;                                           \
                                                                                                \
            if(left_child_idx <= pqueue->size                                                   \
            && pqueue->nodes[left_child_idx].priority < pqueue->nodes[target_idx].priority) {   \
                target_idx = left_child_idx;                                                    \
            }                                                                                   \
                                                                                                \
            if(right_child_idx <= pqueue->size                                                  \
            && pqueue->nodes[right_child_idx].priority < pqueue->nodes[target_idx].priority) {  \
                target_idx = right_child_idx;                                                   \
            }                                                                                   \
                                                                                                \
            pqueue->nodes[root_idx] = pqueue->nodes[target_idx];                                \
            root_idx = target_idx;                                                              \
        }                                                                                       \
    }                                                                                           \
                                                                                                \
    scope void pq_##name##_init(pq(name) *pqueue)                                               \
    {                                                                                           \
        pqueue->nodes = NULL;                                                                   \
        pqueue->capacity = 0;                                                                   \
        pqueue->size = 0;                                                                       \
        pqueue->prealloc = realloc;                                                             \
        pqueue->pfree = free;                                                                   \
    }                                                                                           \
                                                                                                \
    scope void pq_##name##_init_alloc(pq(name) *pqueue,                                         \
                                      void *(*prealloc)(void *ptr, size_t size),                \
                                      void (*pfree)(void *ptr))                                 \
    {                                                                                           \
        pqueue->nodes = NULL;                                                                   \
        pqueue->capacity = 0;                                                                   \
        pqueue->size = 0;                                                                       \
        pqueue->prealloc = prealloc;                                                            \
        pqueue->pfree = pfree;                                                                  \
    }                                                                                           \
                                                                                                \
    scope void pq_##name##_destroy(pq(name) *pqueue)                                            \
    {                                                                                           \
        free(pqueue->nodes);                                                                    \
        memset(pqueue, 0, sizeof(*pqueue));                                                     \
    }                                                                                           \
                                                                                                \
    scope bool pq_##name##_push(pq(name) *pqueue, float in_prio, type in)                       \
    {                                                                                           \
        if(pqueue->size + 1 >= pqueue->capacity) {                                              \
                                                                                                \
            pqueue->capacity = pqueue->capacity ? pqueue->capacity * 2 : 32;                    \
            void *nodes = pqueue->prealloc(pqueue->nodes,                                       \
                pqueue->capacity * sizeof(pq_##name##_node_t));                                 \
            if(!nodes)                                                                          \
                return false;                                                                   \
            pqueue->nodes = nodes;                                                              \
        }                                                                                       \
                                                                                                \
        int curr_idx = pqueue->size + 1;                                                        \
        int parent_idx = curr_idx / 2;                                                          \
                                                                                                \
        while(curr_idx > 1 && pqueue->nodes[parent_idx].priority > in_prio) {                   \
            pqueue->nodes[curr_idx] = pqueue->nodes[parent_idx];                                \
            curr_idx = parent_idx;                                                              \
            parent_idx = parent_idx / 2;                                                        \
        }                                                                                       \
                                                                                                \
        pqueue->nodes[curr_idx].priority = in_prio;                                             \
        pqueue->nodes[curr_idx].data = in;                                                      \
        pqueue->size++;                                                                         \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool pq_##name##_pop(pq(name) *pqueue, type *out)                                     \
    {                                                                                           \
        if(pqueue->size == 0)                                                                   \
            return false;                                                                       \
                                                                                                \
        *out = pqueue->nodes[1].data;                                                           \
        pqueue->nodes[1] = pqueue->nodes[pqueue->size--];                                       \
        _pq_##name##_balance(pqueue, 1);                                                        \
                                                                                                \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool pq_##name##_pop_matching(pq(name) *pqueue, type *out, bool pred(void *a))        \
    {                                                                                           \
        if(pqueue->size == 0)                                                                   \
            return false;                                                                       \
                                                                                                \
        int idx = -1;                                                                           \
        for(int i = 1; i <= pqueue->size; i++) {                                                \
            if(pred(&pqueue->nodes[i].data)) {                                                  \
                idx = i;                                                                        \
                break;                                                                          \
            }                                                                                   \
        }                                                                                       \
        if(idx == -1)                                                                           \
            return false;                                                                       \
                                                                                                \
        *out = pqueue->nodes[idx].data;                                                         \
        pqueue->nodes[idx] = pqueue->nodes[pqueue->size--];                                     \
        _pq_##name##_balance(pqueue, 1);                                                        \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool pq_##name##_peek(pq(name) *pqueue, type *out)                                    \
    {                                                                                           \
        if(pqueue->size == 0)                                                                   \
            return false;                                                                       \
        *out = pqueue->nodes[1].data;                                                           \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool pq_##name##_remove(pq(name) *pqueue, int compare(void *a, void *b), type t)      \
    {                                                                                           \
        if(pqueue->size == 0)                                                                   \
            return false;                                                                       \
                                                                                                \
        int idx = -1;                                                                           \
        for(int i = 1; i <= pqueue->size; i++) {                                                \
            if(0 == compare(&pqueue->nodes[i].data, &t)) {                                      \
                idx = i;                                                                        \
                break;                                                                          \
            }                                                                                   \
        }                                                                                       \
        if(idx == -1)                                                                           \
            return false;                                                                       \
                                                                                                \
        pqueue->nodes[idx] = pqueue->nodes[pqueue->size--];                                     \
        _pq_##name##_balance(pqueue, idx);                                                      \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool pq_##name##_contains(pq(name) *pqueue, int compare(void *a, void *b), type t)    \
    {                                                                                           \
        for(int i = 1; i <= pqueue->size; i++) {                                                \
            if(0 == compare(&pqueue->nodes[i].data, &t))                                        \
                return true;                                                                    \
        }                                                                                       \
        return false;                                                                           \
    }                                                                                           \
                                                                                                \
    scope bool pq_##name##_reserve(pq(name) *pqueue, size_t cap)                                \
    {                                                                                           \
        if(pqueue->capacity < cap) {                                                            \
                                                                                                \
            pqueue->capacity = cap;                                                             \
            void *nodes = pqueue->prealloc(pqueue->nodes,                                       \
                pqueue->capacity * sizeof(pq_##name##_node_t));                                 \
            if(!nodes)                                                                          \
                return false;                                                                   \
            pqueue->nodes = nodes;                                                              \
        }                                                                                       \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool pq_##name##_top_prio(pq(name) *pqueue, float *out)                               \
    {                                                                                           \
        if(pqueue->size == 0)                                                                   \
            return false;                                                                       \
        *out = pqueue->nodes[1].priority;                                                       \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool pq_##name##_top_prio_of(pq(name) *pqueue, float *out, bool pred(void *a))        \
    {                                                                                           \
        if(pqueue->size == 0)                                                                   \
            return false;                                                                       \
                                                                                                \
        int idx = -1;                                                                           \
        for(int i = 1; i <= pqueue->size; i++) {                                                \
            if(pred(&pqueue->nodes[i].data)) {                                                  \
                idx = i;                                                                        \
                break;                                                                          \
            }                                                                                   \
        }                                                                                       \
        if(idx == -1)                                                                           \
            return false;                                                                       \
                                                                                                \
        *out = pqueue->nodes[idx].priority;                                                     \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope void pq_##name##_clear(pq(name) *pqueue)                                              \
    {                                                                                           \
        pqueue->size = 0;                                                                       \
    }                                                                                           \
                                                                                                \
    scope bool pq_##name##_copy(pq(name) *dst, pq(name) *src)                                   \
    {                                                                                           \
        if(!pq_##name##_reserve(dst, src->size))                                                \
            return false;                                                                       \
        pq_##name##_clear(dst);                                                                 \
        memcpy(dst->nodes + 1, src->nodes + 1, src->size * sizeof(pq_##name##_node_t));         \
        dst->size = src->size;                                                                  \
        return true;                                                                            \
    }                                                                                           \

#endif

