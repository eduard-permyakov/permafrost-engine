/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2023 Eduard Permyakov 
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

#include "public/string_intern.h"
#include "public/pf_string.h"

#include <string.h>

MPOOL_PROTOTYPES(static, strbuff, strbuff_t)
MPOOL_IMPL(static, strbuff, strbuff_t)

__KHASH_IMPL(stridx, static, khint32_t, mp_ref_t, 1, kh_int_hash_func, kh_int_hash_equal)

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool si_init(mp_strbuff_t *pool, khash_t(stridx) **index, size_t size)
{
    mp_strbuff_init(pool, true);

    if(!mp_strbuff_reserve(pool, size))
        goto fail_stringpool; 
    if(!(*index = kh_init(stridx)))
        goto fail_stridx;
    if(0 != kh_resize(stridx, *index, size))
        goto fail_resize;

    return true;

fail_resize:
    kh_destroy(stridx, *index);
fail_stridx:
    mp_strbuff_destroy(pool);
fail_stringpool:
    return false;
}

const char *si_intern(const char *str, mp_strbuff_t *pool, khash_t(stridx) *index)
{
    khint_t hash = kh_str_hash_func(str);
    khiter_t k = kh_get(stridx, index, hash);

    if(k != kh_end(index)) {
        mp_ref_t ref = kh_value(index, k);
        return (const char *)mp_strbuff_entry(pool, ref);
    }

    mp_ref_t ref = mp_strbuff_alloc(pool);
    if(ref == 0)
        return NULL;

    int status;
    k = kh_put(stridx, index, hash, &status);
    if(status == -1) {
        mp_strbuff_free(pool, ref);
        return NULL;
    }
    assert(status == 1);
    kh_value(index, k) = ref;

    if(strlen(str) > sizeof(strbuff_t)-1) {
        kh_del(stridx, index, k);
        mp_strbuff_free(pool, ref);
        return NULL;
    }

    char *ret = (char *)mp_strbuff_entry(pool, ref);
    pf_strlcpy(ret, str, sizeof(strbuff_t));
    return ret;
}

void si_shutdown(mp_strbuff_t *pool, khash_t(stridx) *index)
{
    kh_destroy(stridx, index);
    mp_strbuff_destroy(pool);
}

void si_clear(mp_strbuff_t *pool, khash_t(stridx) *index)
{
    kh_clear(stridx, index);
    mp_strbuff_clear(pool);
}

