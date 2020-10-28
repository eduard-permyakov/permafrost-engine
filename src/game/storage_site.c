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
#include "game_private.h"
#include "../ui.h"
#include "../event.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/pf_string.h"
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

#define ARR_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MAX(a, b)   ((a) > (b) ? (a) : (b))
#define MIN(a, b)   ((a) < (b) ? (a) : (b))

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
    if(!ptr)
        return pmalloc(size);
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

static int compare_keys(const void *a, const void *b)
{
    char *stra = *(char**)a;
    char *strb = *(char**)b;
    return strcmp(stra, strb);
}

static size_t ss_get_keys(struct ss_state *hs, const char **out, size_t maxout)
{
    size_t ret = 0;

    const char *key;
    int amount;
    (void)amount;

    kh_foreach(hs->capacity, key, amount, {
        if(ret == maxout)
            break;
        out[ret++] = key;
    });

    qsort(out, ret, sizeof(char*), compare_keys);
    return ret;
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

static void on_update_ui(void *user, void *event)
{
    uint32_t key;
    struct ss_state curr;

    kh_foreach(s_entity_state_table, key, curr, {

        char name[256];
        pf_snprintf(name, sizeof(name), "__storage_site__.%x", key);

        const struct entity *ent = G_EntityForUID(key);
        vec2_t ss_pos = Entity_TopScreenPos(ent);

        const int width = 160;
        const int height = MIN(kh_size(curr.capacity), 16) * 22;
        const vec2_t pos = (vec2_t){ss_pos.x - width/2, ss_pos.y + 20};
        const int flags = NK_WINDOW_NOT_INTERACTIVE | NK_WINDOW_BORDER | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR;

        const vec2_t vres = (vec2_t){1920, 1080};
        const vec2_t adj_vres = UI_ArAdjustedVRes(vres);

        struct rect adj_bounds = UI_BoundsForAspectRatio(
            (struct rect){pos.x, pos.y, width, height}, 
            vres, adj_vres, ANCHOR_DEFAULT
        );

        const char *names[16];
        size_t nnames = ss_get_keys(&curr, names, ARR_SIZE(names));
        struct nk_context *ctx = UI_GetContext();

        if(nk_begin_with_vres(ctx, name, 
            (struct nk_rect){adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h}, 
            flags, (struct nk_vec2i){adj_vres.x, adj_vres.y})) {

            for(int i = 0; i < nnames; i++) {

                char status[256];
                pf_snprintf(status, sizeof(status), "%4d/%4d", 
                    G_StorageSite_GetCurr(key, names[i]), G_StorageSite_GetCapacity(key, names[i]));

                nk_layout_row_begin(ctx, NK_DYNAMIC, 16, 2);
                nk_layout_row_push(ctx, 0.5f);
                nk_label_colored(ctx, names[i], NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, 
                    (struct nk_color){255, 0, 0, 255});
                nk_layout_row_push(ctx, 0.5f);
                nk_label_colored(ctx, status, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, 
                    (struct nk_color){255, 0, 0, 255});
                nk_layout_row_end(ctx);
            }
        }
        nk_end(ctx);
    });
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

    E_Global_Register(EVENT_UPDATE_UI, on_update_ui, NULL, G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);
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
    E_Global_Unregister(EVENT_UPDATE_UI, on_update_ui);

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
    if(!ss)
        return;
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

