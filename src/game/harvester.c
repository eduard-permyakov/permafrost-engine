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

#include "harvester.h"
#include "../lib/public/mpool.h"
#include "../lib/public/khash.h"
#include "../lib/public/pf_string.h"

#include <stddef.h>
#include <assert.h>

static void *hmalloc(size_t size);
static void *hcalloc(size_t n, size_t size);
static void *hrealloc(void *ptr, size_t size);
static void  hfree(void *ptr);

#undef kmalloc
#undef kcalloc
#undef krealloc
#undef kfree

#define kmalloc  hmalloc
#define kcalloc  hcalloc
#define krealloc hrealloc
#define kfree    hfree

KHASH_MAP_INIT_STR(int, int)

struct hstate{
    kh_int_t *gather_speeds;    /* How much of each resource the entity gets each cycle */
    kh_int_t *max_carry;        /* The maximum amount of each resource the entity can carry */
    kh_int_t *curr_carry;       /* The amount of each resource the entity currently holds */
};

typedef char buff_t[512];
typedef char smallbuff_t[128];

MPOOL_TYPE(buff, buff_t)
MPOOL_PROTOTYPES(static, buff, buff_t)
MPOOL_IMPL(static, buff, buff_t)

MPOOL_TYPE(smallbuff, smallbuff_t)
MPOOL_PROTOTYPES(static, smallbuff, smallbuff_t)
MPOOL_IMPL(static, smallbuff, smallbuff_t)

#undef kmalloc
#undef kcalloc
#undef krealloc
#undef kfree

#define kmalloc  malloc
#define kcalloc  calloc
#define krealloc realloc
#define kfree    free

KHASH_MAP_INIT_INT(state, struct hstate)
KHASH_MAP_INIT_INT(str, mp_ref_t)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static mp_buff_t       s_mpool;
static khash_t(str)   *s_stridx;
static mp_smallbuff_t  s_stringpool;
static khash_t(state) *s_entity_state_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void *hmalloc(size_t size)
{
    mp_ref_t ref = mp_buff_alloc(&s_mpool);
    if(ref == 0)
        return NULL;
    return mp_buff_entry(&s_mpool, ref);
}

static void *hcalloc(size_t n, size_t size)
{
    void *ret = hmalloc(n * size);
    if(!ret)
        return NULL;
    memset(ret, 0, n * size);
    return ret;
}

static void *hrealloc(void *ptr, size_t size)
{
    if(size <= sizeof(buff_t))
        return ptr;
    return NULL;
}

static void hfree(void *ptr)
{
    if(!ptr)
        return;
    mp_ref_t ref = mp_buff_ref(&s_mpool, ptr);
    mp_buff_free(&s_mpool, ref);
}

static const char *string_intern(const char *key)
{
    khint_t hash = kh_str_hash_func(key);
    khiter_t k = kh_get(str, s_stridx, hash);

    if(k != kh_end(s_entity_state_table)) {
        mp_ref_t ref = kh_value(s_stridx, k);
        return (const char *)mp_smallbuff_entry(&s_stringpool, ref);
    }

    mp_ref_t ref = mp_smallbuff_alloc(&s_stringpool);
    if(ref == 0)
        return NULL;

    int status;
    k = kh_put(str, s_stridx, hash, &status);
    if(status == -1) {
        mp_smallbuff_free(&s_stringpool, ref);
        return NULL;
    }
    assert(status == 1);

    if(strlen(key) > sizeof(smallbuff_t)-1) {
        kh_del(str, s_stridx, k);
        mp_smallbuff_free(&s_stringpool, ref);
        return NULL;
    }

    char *ret = (char *)mp_smallbuff_entry(&s_stringpool, ref);
    pf_strlcpy(ret, key, sizeof(smallbuff_t));
    return ret;
}

static struct hstate *hstate_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;

    return &kh_value(s_entity_state_table, k);
}

static bool hstate_set(uint32_t uid, struct hstate hs)
{
    int status;
    khiter_t k = kh_put(state, s_entity_state_table, uid, &status);
    if(status == -1 || status == 0)
        return false;
    kh_value(s_entity_state_table, k) = hs;
    return true;
}

static void hstate_remove(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
}

static bool hstate_init(struct hstate *hs)
{
    hs->gather_speeds = kh_init(int);
    hs->max_carry = kh_init(int);
    hs->curr_carry = kh_init(int);

    if(!hs->gather_speeds || !hs->max_carry || !hs->curr_carry) {
    
        kh_destroy(int, hs->gather_speeds);
        kh_destroy(int, hs->max_carry);
        kh_destroy(int, hs->curr_carry);
        return false;
    }
    return true;
}

static void hstate_destroy(struct hstate *hs)
{
    kh_destroy(int, hs->gather_speeds);
    kh_destroy(int, hs->max_carry);
    kh_destroy(int, hs->curr_carry);
}

static bool hstate_set_key(khash_t(int) *table, const char *name, int val)
{
    const char *key = string_intern(name);
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

static bool hstate_get_key(khash_t(int) *table, const char *name, int *out)
{
    const char *key = string_intern(name);
    if(!key)
        return false;

    khiter_t k = kh_get(int, table, key);
    if(k == kh_end(table))
        return false;
    *out = kh_value(table, k);
    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Harvester_Init(void)
{
    mp_buff_init(&s_mpool);
    mp_smallbuff_init(&s_stringpool);

    if(!mp_buff_reserve(&s_mpool, 4096 * 3))
        goto fail_mpool; 
    if(!mp_smallbuff_reserve(&s_stringpool, 512))
        goto fail_stringpool; 
    if(!(s_stridx = kh_init(str)))
        goto fail_stridx;
    if(!(s_entity_state_table = kh_init(state)))
        goto fail_table;

    return true;

fail_table:
    kh_destroy(str, s_stridx);
fail_stridx:
    mp_smallbuff_destroy(&s_stringpool);
fail_stringpool:
    mp_buff_destroy(&s_mpool);
fail_mpool:
    return false;
}

void G_Harvester_Shutdown(void)
{
    kh_destroy(state, s_entity_state_table);
    kh_destroy(str, s_stridx);
    mp_smallbuff_destroy(&s_stringpool);
    mp_buff_destroy(&s_mpool);
}

bool G_Harvester_AddEntity(uint32_t uid)
{
    struct hstate hs;
    if(!hstate_init(&hs))
        return false;
    if(!hstate_set(uid, hs))
        return false;
    return true;
}

void G_Harvester_RemoveEntity(uint32_t uid)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    hstate_destroy(hs);
    hstate_remove(uid);
}

bool G_Harvester_SetGatherSpeed(uint32_t uid, const char *rname, int speed)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return hstate_set_key(hs->gather_speeds, rname, speed);
}

int G_Harvester_GetGatherSpeed(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_GATHER_SPEED;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hstate_get_key(hs->gather_speeds, rname, &ret);
    return ret;
}

bool G_Harvester_SetMaxCarry(uint32_t uid, const char *rname, int max)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return hstate_set_key(hs->max_carry, rname, max);
}

int G_Harvester_GetMaxCarry(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_MAX_CARRY;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hstate_get_key(hs->max_carry, rname, &ret);
    return ret;
}

bool G_Harvester_SetCurrCarry(uint32_t uid, const char *rname, int curr)
{
    struct hstate *hs = hstate_get(uid);
    assert(hs);
    return hstate_set_key(hs->curr_carry, rname, curr);
}

int G_Harvester_GetCurrCarry(uint32_t uid, const char *rname)
{
    int ret = 0;
    struct hstate *hs = hstate_get(uid);
    assert(hs);

    hstate_get_key(hs->curr_carry, rname, &ret);
    return ret;
}

