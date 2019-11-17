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

#ifndef LRU_CACHE_H
#define LRU_CACHE_H

#include "mpool.h"
#include "khash.h"

#include <stdint.h>
#include <stdbool.h>

/***********************************************************************************************/

#define LRU_CACHE_TYPE(name, type)                                                              \
                                                                                                \
    typedef struct lru_##name##_node_s {                                                        \
        mp_ref_t next;                                                                          \
        mp_ref_t prev;                                                                          \
        khint64_t key;                                                                          \
        type entry;                                                                             \
    } lru_##name##_node_t;                                                                      \
                                                                                                \
    MPOOL_TYPE(name, lru_##name##_node_t)                                                       \
	__KHASH_TYPE(name, khint64_t, mp_ref_t) 								                    \
                                                                                                \
    typedef struct lru_##name##_s {                                                             \
        size_t         capacity;                                                                \
        size_t         used;                                                                    \
        mp_ref_t       ilru_head;                                                               \
        mp_ref_t       ilru_tail;                                                               \
        khash_t(name) *key_node_table;                                                          \
        mp(name)       node_pool;                                                               \
        /* Optional hook to clean up entries' resources before eviction */                      \
        void           (*on_evict)(type *victim);                                               \
    } lru_##name##_t;

/***********************************************************************************************/

#define lru(name)                                                                               \
    lru_##name##_t

#define lru_node(name)                                                                          \
    lru_##name##_node_t

/***********************************************************************************************/

#define LRU_CACHE_PROTOTYPES(scope, name, type)                                                 \
                                                                                                \
    MPOOL_PROTOTYPES(scope, name, lru_node(name))                                               \
	__KHASH_PROTOTYPES(name, khint64_t, mp_ref_t)                                               \
                                                                                                \
    static void _lru_##name##_reference(lru(name) *lru, mp_ref_t ref);                          \
    scope  bool  lru_##name##_init     (lru(name) *lru, size_t capacity,                        \
                                        void (*on_evict)(type *victim));                        \
    scope  void  lru_##name##_destroy  (lru(name) *lru);                                        \
    scope  void  lru_##name##_clear    (lru(name) *lru);                                        \
    scope  bool  lru_##name##_get      (lru(name) *lru, uint64_t key, type *out);               \
    /* Returned pointer is invalidated when new entries are added; it should not be cached  */  \
    scope  const type *lru_##name##_at (lru(name) *lru, uint64_t key);                          \
    scope  bool  lru_##name##_contains (lru(name) *lru, uint64_t key);                          \
    scope  void  lru_##name##_put      (lru(name) *lru, uint64_t key, const type *in);          \

/***********************************************************************************************/

#define LRU_CACHE_IMPL(scope, name, type)                                                       \
                                                                                                \
    MPOOL_IMPL(static, name, lru_node(name))                                                    \
    __KHASH_IMPL(name, extern, khint64_t, mp_ref_t, 1, kh_int_hash_func, kh_int_hash_equal)     \
                                                                                                \
    static void _lru_##name##_reference(lru(name) *lru, mp_ref_t ref)                           \
    {                                                                                           \
        if(ref == lru->ilru_head)                                                               \
            return;                                                                             \
                                                                                                \
        lru_node(name) *node = mp_##name##_entry(&lru->node_pool, ref);                         \
        lru_node(name) *prev = node->prev ? mp_##name##_entry(&lru->node_pool, node->prev)      \
                                          : NULL;                                               \
        lru_node(name) *next = node->next ? mp_##name##_entry(&lru->node_pool, node->next)      \
                                          : NULL;                                               \
        lru_node(name) *old_head = mp_##name##_entry(&lru->node_pool, lru->ilru_head);          \
        mp_ref_t old_tail_prev = mp_##name##_entry(&lru->node_pool, lru->ilru_tail)->prev;      \
                                                                                                \
        /* Remove the node from the list */                                                     \
        if(prev)                                                                                \
            prev->next = node->next;                                                            \
        if(next)                                                                                \
            next->prev = node->prev;                                                            \
                                                                                                \
        /* Then make it the new head */                                                         \
        old_head->prev = ref;                                                                   \
        node->next = lru->ilru_head;                                                            \
        node->prev = 0;                                                                         \
        lru->ilru_head = ref;                                                                   \
                                                                                                \
        /* In the case where we removed the tail, set a new one */                              \
        if(ref == lru->ilru_tail) {                                                             \
            lru->ilru_tail = old_tail_prev;                                                     \
            mp_##name##_entry(&lru->node_pool, old_tail_prev)->next = 0;                        \
        }                                                                                       \
    }                                                                                           \
                                                                                                \
    scope bool lru_##name##_init(lru(name) *lru, size_t capacity,                               \
                                     void (*on_evict)(type *victim))                            \
    {                                                                                           \
        memset(lru, 0, sizeof(*lru));                                                           \
        lru->key_node_table = kh_init(name);                                                    \
        if(!lru->key_node_table)                                                                \
            return false;                                                                       \
        kh_resize(name, lru->key_node_table, capacity);                                         \
                                                                                                \
        mp_##name##_init(&lru->node_pool);                                                      \
        if(!mp_##name##_reserve(&lru->node_pool, capacity)) {                                   \
            kh_destroy(name, lru->key_node_table);                                              \
            return false;                                                                       \
        }                                                                                       \
        lru->capacity = capacity;                                                               \
        lru->on_evict = on_evict;                                                               \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope void lru_##name##_destroy(lru(name) *lru)                                             \
    {                                                                                           \
        lru_##name##_clear(lru);                                                                \
        kh_destroy(name, lru->key_node_table);                                                  \
        mp_##name##_destroy(&lru->node_pool);                                                   \
        memset(lru, 0, sizeof(*lru));                                                           \
    }                                                                                           \
                                                                                                \
    scope void lru_##name##_clear(lru(name) *lru)                                               \
    {                                                                                           \
        uint64_t key;                                                                           \
        mp_ref_t curr;                                                                          \
        kh_foreach(lru->key_node_table, key, curr, {                                            \
                                                                                                \
            if(!lru->on_evict)                                                                  \
                continue;                                                                       \
            lru_node(name) *vict = &lru->node_pool.pool[curr].entry;                            \
            lru->on_evict(&vict->entry);                                                        \
        });                                                                                     \
                                                                                                \
        kh_clear(name, lru->key_node_table);                                                    \
        lru->ilru_head = 0;                                                                     \
        lru->ilru_tail = 0;                                                                     \
        lru->used = 0;                                                                          \
    }                                                                                           \
                                                                                                \
    scope bool lru_##name##_get(lru(name) *lru, uint64_t key, type *out)                        \
    {                                                                                           \
        khiter_t k;                                                                             \
        if((k = kh_get(name, lru->key_node_table, key)) == kh_end(lru->key_node_table))         \
            return false;                                                                       \
                                                                                                \
        mp_ref_t ref = kh_val(lru->key_node_table, k);                                          \
        lru_node(name) *mpn = mp_##name##_entry(&lru->node_pool, ref);                          \
                                                                                                \
        *out = mpn->entry;                                                                      \
        _lru_##name##_reference(lru, ref);                                                      \
        return true;                                                                            \
    }                                                                                           \
                                                                                                \
    scope const type *lru_##name##_at(lru(name) *lru, uint64_t key)                             \
    {                                                                                           \
        khiter_t k;                                                                             \
        if((k = kh_get(name, lru->key_node_table, key)) == kh_end(lru->key_node_table))         \
            return NULL;                                                                        \
                                                                                                \
        mp_ref_t ref = kh_val(lru->key_node_table, k);                                          \
        lru_node(name) *mpn = mp_##name##_entry(&lru->node_pool, ref);                          \
                                                                                                \
        _lru_##name##_reference(lru, ref);                                                      \
        return &mpn->entry;                                                                     \
    }                                                                                           \
                                                                                                \
    scope bool lru_##name##_contains(lru(name) *lru, uint64_t key)                              \
    {                                                                                           \
        return (lru_##name##_at(lru, key) != NULL);                                             \
    }                                                                                           \
                                                                                                \
    scope void lru_##name##_put(lru(name) *lru, uint64_t key, const type *in)                   \
    {                                                                                           \
        khiter_t k;                                                                             \
        if((k = kh_get(name, lru->key_node_table, key)) == kh_end(lru->key_node_table)) {       \
            /* There is no existing entry for this key */                                       \
                                                                                                \
            mp_ref_t new_ref = 0;                                                               \
            lru_node(name) *new_node = NULL;                                                    \
            if(lru->used == 0) {                                                                \
                                                                                                \
                new_ref = mp_##name##_alloc(&lru->node_pool);                                   \
                new_node = mp_##name##_entry(&lru->node_pool, new_ref);                         \
                new_node->prev = 0;                                                             \
                new_node->next = 0;                                                             \
                lru->ilru_head = lru->ilru_tail = new_ref;                                      \
                ++(lru->used);                                                                  \
                                                                                                \
            }else if(lru->used == lru->capacity) {                                              \
                                                                                                \
                lru_node(name) *vict = mp_##name##_entry(&lru->node_pool, lru->ilru_tail);      \
                if(lru->on_evict)                                                               \
                    lru->on_evict(&vict->entry);                                                \
                                                                                                \
                /* Remember to delete the victim's key */                                       \
                k = kh_get(name, lru->key_node_table, vict->key);                               \
                kh_del(name, lru->key_node_table, k);                                           \
                                                                                                \
                new_ref = lru->ilru_tail;                                                       \
                new_node = vict;                                                                \
                _lru_##name##_reference(lru, new_ref);                                          \
                                                                                                \
            }else {                                                                             \
                                                                                                \
                new_ref = mp_##name##_alloc(&lru->node_pool);                                   \
                new_node = mp_##name##_entry(&lru->node_pool, new_ref);                         \
                lru_node(name) *old_head = mp_##name##_entry(&lru->node_pool, lru->ilru_head);  \
                                                                                                \
                new_node->prev = 0;                                                             \
                new_node->next = lru->ilru_head;                                                \
                lru->ilru_head = new_ref;                                                       \
                old_head->prev = new_ref;                                                       \
                ++(lru->used);                                                                  \
            }                                                                                   \
                                                                                                \
            new_node->entry = *in;                                                              \
            new_node->key = key;                                                                \
                                                                                                \
            int ret;                                                                            \
            k = kh_put(name, lru->key_node_table, key, &ret);                                   \
            kh_value(lru->key_node_table, k) = new_ref;                                         \
                                                                                                \
        }else {                                                                                 \
            /* There is an existing entry for this key - overwrite it and reference it */       \
                                                                                                \
            mp_ref_t ref = kh_val(lru->key_node_table, k);                                      \
            lru_node(name) *mpn = mp_##name##_entry(&lru->node_pool, ref);                      \
                                                                                                \
            if(lru->on_evict)                                                                   \
                lru->on_evict(&mpn->entry);                                                     \
                                                                                                \
            mpn->entry = *in;                                                                   \
            _lru_##name##_reference(lru, ref);                                                  \
        }                                                                                       \
    }

#endif

