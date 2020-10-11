/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#include "storage_site.h"
#include "../lib/public/mpool.h"
#include "../lib/public/khash.h"
#include "../lib/public/string_intern.h"

#include <assert.h>

static void *pmalloc(size_t size);
static void *pcalloc(size_t n, size_t size);
static void *prealloc(void *ptr, size_t size);
static void  pfree(void *ptr);

#undef kmalloc
#undef kcalloc
#undef krealloc
#undef kfree

#define kmalloc  pmalloc
#define kcalloc  pcalloc
#define krealloc prealloc
#define kfree    pfree

KHASH_MAP_INIT_STR(int, int)

struct ss_state{
    kh_int_t *capacity;
    kh_int_t *curr;
};

typedef char buff_t[512];

MPOOL_TYPE(buff, buff_t)
MPOOL_PROTOTYPES(static, buff, buff_t)
MPOOL_IMPL(static, buff, buff_t)

#undef kmalloc
#undef kcalloc
#undef krealloc
#undef kfree

#define kmalloc  malloc
#define kcalloc  calloc
#define krealloc realloc
#define kfree    free

KHASH_MAP_INIT_INT(state, struct ss_state)
KHASH_MAP_INIT_STR(res, int)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static mp_buff_t        s_mpool;
static khash_t(stridx) *s_stridx;
static mp_strbuff_t     s_stringpool;
static khash_t(state)  *s_entity_state_table;
static khash_t(res)    *s_global_resource_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void *pmalloc(size_t size)
{
    mp_ref_t ref = mp_buff_alloc(&s_mpool);
    if(ref == 0)
        return NULL;
    return mp_buff_entry(&s_mpool, ref);
}

static void *pcalloc(size_t n, size_t size)
{
    void *ret = pmalloc(n * size);
    if(!ret)
        return NULL;
    memset(ret, 0, n * size);
    return ret;
}

static void *prealloc(void *ptr, size_t size)
{
    if(size <= sizeof(buff_t))
        return ptr;
    return NULL;
}

static void pfree(void *ptr)
{
    if(!ptr)
        return;
    mp_ref_t ref = mp_buff_ref(&s_mpool, ptr);
    mp_buff_free(&s_mpool, ref);
}

static struct ss_state *ss_state_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;

    return &kh_value(s_entity_state_table, k);
}

static bool ss_state_set(uint32_t uid, struct ss_state hs)
{
    int status;
    khiter_t k = kh_put(state, s_entity_state_table, uid, &status);
    if(status == -1 || status == 0)
        return false;
    kh_value(s_entity_state_table, k) = hs;
    return true;
}

static void ss_state_remove(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
}

static bool ss_state_init(struct ss_state *hs)
{
    hs->capacity = kh_init(int);
    hs->curr = kh_init(int);

    if(!hs->capacity || !hs->curr) {
    
        kh_destroy(int, hs->capacity);
        kh_destroy(int, hs->curr);
        return false;
    }
    return true;
}

static void ss_state_destroy(struct ss_state *hs)
{
    kh_destroy(int, hs->capacity);
    kh_destroy(int, hs->curr);
}

static bool ss_state_set_key(khash_t(int) *table, const char *name, int val)
{
    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(int, table, key);
    if(k != kh_end(table)) {
        kh_value(table, k) = val;
        return true;
    }

    int status;
    k = kh_put(int, table, key, &status);
    if(status == -1)
        return false;

    assert(status == 1);
    kh_value(table, k) = val;
    return true;
}

static bool ss_state_get_key(khash_t(int) *table, const char *name, int *out)
{
    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(int, table, key);
    if(k == kh_end(table))
        return false;
    *out = kh_value(table, k);
    return true;
}

static bool update_res_delta(const char *rname, int delta)
{
    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(res, s_global_resource_table, key);
    if(k != kh_end(s_global_resource_table)) {
        int val = kh_value(s_global_resource_table, k);
        val += delta;
        kh_value(s_global_resource_table, k) = val;
        return true;
    }

    int status;
    k = kh_put(res, s_global_resource_table, key, &status);
    if(status == -1)
        return false;

    assert(status == 1);
    kh_value(s_global_resource_table, k) = delta;
    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_StorageSite_Init(void)
{
    mp_buff_init(&s_mpool);

    if(!mp_buff_reserve(&s_mpool, 4096 * 3))
        goto fail_mpool; 
    if(!(s_entity_state_table = kh_init(state)))
        goto fail_table;
    if(!(s_global_resource_table = kh_init(res)))
        goto fail_res;
    if(!si_init(&s_stringpool, &s_stridx, 512))
        goto fail_strintern;

    return true;

fail_strintern:
    kh_destroy(res, s_global_resource_table);
fail_res:
    kh_destroy(state, s_entity_state_table);
fail_table:
    mp_buff_destroy(&s_mpool);
fail_mpool:
    return false;
}

void G_StorageSite_Shutdown(void)
{
    si_shutdown(&s_stringpool, s_stridx);
    kh_destroy(res, s_global_resource_table);
    kh_destroy(state, s_entity_state_table);
    mp_buff_destroy(&s_mpool);
}

bool G_StorageSite_AddEntity(uint32_t uid)
{
    struct ss_state ss;
    if(!ss_state_init(&ss))
        return false;
    if(!ss_state_set(uid, ss))
        return false;
    return true;
}

void G_StorageSite_RemoveEntity(uint32_t uid)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);
    ss_state_destroy(ss);
    ss_state_remove(uid);
}

bool G_StorageSite_SetCapacity(uint32_t uid, const char *rname, int max)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);
    return ss_state_set_key(ss->capacity, rname, max);
}

int G_StorageSite_GetCapacity(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_CAPACITY;
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    ss_state_get_key(ss->capacity, rname, &ret);
    return ret;
}

bool G_StorageSite_SetCurr(uint32_t uid, const char *rname, int curr)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    int prev = 0;
    ss_state_get_key(ss->curr, rname, &prev);
    int delta = curr - prev;
    update_res_delta(rname, delta);

    return ss_state_set_key(ss->curr, rname, curr);
}

int G_StorageSite_GetCurr(uint32_t uid, const char *rname)
{
    int ret = 0;
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    ss_state_get_key(ss->curr, rname, &ret);
    return ret;
}

int G_StorageSite_GetTotal(const char *rname)
{
    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return 0;

    khiter_t k = kh_get(res, s_global_resource_table, key);
    if(k == kh_end(s_global_resource_table))
        return 0;

    return kh_value(s_global_resource_table, k);
}

