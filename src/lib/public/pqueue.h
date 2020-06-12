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
    } pq_##name##_t;                                                                            \

/***********************************************************************************************/

#define pq(name)                                                                                \
    pq_##name##_t

/***********************************************************************************************/

#define pq_size(pqueue)                                                                         \
    ((pqueue)->size)

/***********************************************************************************************/

#define PQUEUE_PROTOTYPES(scope, name, type)                                                    \
                                                                                                \
    scope void pq_##name##_init    (pq(name) *pqueue);                                          \
    scope void pq_##name##_destroy (pq(name) *pqueue);                                          \
    scope bool pq_##name##_push    (pq(name) *pqueue, float in_prio, type in);                  \
    scope bool pq_##name##_pop     (pq(name) *pqueue, type *out);                               \
    scope bool pq_##name##_contains(pq(name) *pqueue, type t);                                  \
    scope bool pq_##name##_reserve (pq(name) *pqueue, size_t cap);                              \

/***********************************************************************************************/

#define PQUEUE_IMPL(scope, name, type)                                                          \
                                                                                                \
    scope void pq_##name##_init(pq(name) *pqueue)                                               \
    {                                                                                           \
        pqueue->nodes = NULL;                                                                   \
        pqueue->capacity = 0;                                                                   \
        pqueue->size = 0;                                                                       \
    }                                                                                           \
                                                                                                \
    scope void pq_##name##_destroy(pq(name) *pqueue)                                            \
    {                                                                                           \
        free(pqueue->nodes);                                                                    \
    }                                                                                           \
                                                                                                \
    scope bool pq_##name##_push(pq(name) *pqueue, float in_prio, type in)                       \
    {                                                                                           \
        if(pqueue->size + 1 >= pqueue->capacity) {                                              \
                                                                                                \
            pqueue->capacity = pqueue->capacity ? pqueue->capacity * 2 : 32;                    \
            pqueue->nodes = realloc(pqueue->nodes,                                              \
                pqueue->capacity * sizeof(pq_##name##_node_t));                                 \
            if(!pqueue->nodes)                                                                  \
                return false;                                                                   \
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
                                                                                                \
        int curr_idx = 1;                                                                       \
        while(curr_idx != pqueue->size + 1) {                                                   \
                                                                                                \
            int target_idx = pqueue->size + 1;                                                  \
            int left_child_idx = curr_idx * 2;                                                  \
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
            pqueue->nodes[curr_idx] = pqueue->nodes[target_idx];                                \
            curr_idx = target_idx;                                                              \
        }                                                                                       \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope bool pq_##name##_contains(pq(name) *pqueue, type t)                                   \
    {                                                                                           \
        for(int i = 0; i < pqueue->size; i++) {                                                 \
            if(0 == memcmp(&pqueue->nodes[i].data, &t, sizeof(t)))                              \
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
            pqueue->nodes = realloc(pqueue->nodes,                                              \
                pqueue->capacity * sizeof(pq_##name##_node_t));                                 \
            if(!pqueue->nodes)                                                                  \
                return false;                                                                   \
        }                                                                                       \
        return true;                                                                            \
    }

#endif

