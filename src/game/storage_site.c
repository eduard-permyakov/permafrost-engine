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

#include "storage_site.h"
#include "game_private.h"
#include "selection.h"
#include "../sched.h"
#include "../ui.h"
#include "../event.h"
#include "../settings.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/khash.h"
#include "../lib/public/mpool_allocator.h"
#include "../lib/public/string_intern.h"
#include "../lib/public/attr.h"

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

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

KHASH_MAP_INIT_STR(int, int)

struct ss_state{
    kh_int_t              *capacity;
    kh_int_t              *curr;
    kh_int_t              *desired;
    struct ss_delta_event  last_change;
    /* Alternative capacity/desired parameters that 
     *can be turned on/off */
    bool                   use_alt;
    kh_int_t              *alt_capacity;
    kh_int_t              *alt_desired;
    /* Flag to inform harvesters not to take anything 
     * from this site */
    bool                   do_not_take;
};

typedef char buff_t[512];

MPOOL_ALLOCATOR_TYPE(buff, buff_t)
MPOOL_ALLOCATOR_PROTOTYPES(static, buff, buff_t)
MPOOL_ALLOCATOR_IMPL(static, buff, buff_t)

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

static mpa_buff_t       s_mpool;
static khash_t(stridx) *s_stridx;
static mp_strbuff_t     s_stringpool;
static khash_t(state)  *s_entity_state_table;
static khash_t(res)    *s_global_resource_tables[MAX_FACTIONS];
static khash_t(res)    *s_global_capacity_tables[MAX_FACTIONS];

static struct nk_style_item s_bg_style = {0};
static struct nk_color      s_border_clr = {0};
static struct nk_color      s_font_clr = {0};
static bool                 s_show_ui = true;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void *pmalloc(size_t size)
{
    if(size > sizeof(buff_t))
        return NULL;
    return mpa_buff_alloc(&s_mpool);
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
    mpa_buff_free(&s_mpool, ptr);
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

static void ss_state_destroy(struct ss_state *hs)
{
    kh_destroy(int, hs->capacity);
    kh_destroy(int, hs->curr);
    kh_destroy(int, hs->desired);

    kh_destroy(int, hs->alt_capacity);
    kh_destroy(int, hs->alt_desired);
}

static bool ss_state_init(struct ss_state *hs)
{
    hs->capacity = kh_init(int);
    hs->curr = kh_init(int);
    hs->desired = kh_init(int);
    hs->alt_capacity = kh_init(int);
    hs->alt_desired = kh_init(int);

    if(!hs->capacity || !hs->curr || !hs->desired
    || !hs->alt_capacity || !hs->alt_desired) {

        ss_state_destroy(hs);
        return false;
    }

    hs->last_change = (struct ss_delta_event){0};
    hs->use_alt = false;
    hs->do_not_take = false;
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
    khash_t(int) *table = (hs->use_alt) ? hs->alt_capacity : hs->capacity;

    kh_foreach(table, key, amount, {
        if(ret == maxout)
            break;
        if(amount == 0)
            continue;
        out[ret++] = key;
    });

    qsort(out, ret, sizeof(char*), compare_keys);
    return ret;
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

static bool update_res_delta(const char *rname, int delta, int faction_id)
{
    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(res, s_global_resource_tables[faction_id], key);
    if(k != kh_end(s_global_resource_tables[faction_id])) {
        int val = kh_value(s_global_resource_tables[faction_id], k);
        val += delta;
        kh_value(s_global_resource_tables[faction_id], k) = val;
        return true;
    }

    int status;
    k = kh_put(res, s_global_resource_tables[faction_id], key, &status);
    if(status == -1)
        return false;

    assert(status == 1);
    kh_value(s_global_resource_tables[faction_id], k) = delta;
    return true;
}

static bool update_cap_delta(const char *rname, int delta, int faction_id)
{
    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return false;

    khiter_t k = kh_get(res, s_global_capacity_tables[faction_id], key);
    if(k != kh_end(s_global_capacity_tables[faction_id])) {
        int val = kh_value(s_global_capacity_tables[faction_id], k);
        val += delta;
        kh_value(s_global_capacity_tables[faction_id], k) = val;
        return true;
    }

    int status;
    k = kh_put(res, s_global_capacity_tables[faction_id], key, &status);
    if(status == -1)
        return false;

    assert(status == 1);
    kh_value(s_global_capacity_tables[faction_id], k) = delta;
    return true;
}

static void constrain_desired(struct ss_state *ss, const char *rname)
{
    int cap = 0, desired = 0;
    ss_state_get_key(ss->capacity, rname, &cap);
    ss_state_get_key(ss->desired, rname, &desired);

    desired = MIN(desired, cap);
    desired = MAX(desired, 0);
    ss_state_set_key(ss->desired, rname, desired);
}

static void on_update_ui(void *user, void *event)
{
    if(!s_show_ui)
        return;

    struct sval ui_setting;
    ss_e status = Settings_Get("pf.game.storage_site_ui_mode", &ui_setting);
    assert(status == SS_OKAY);

    if(ui_setting.as_int == SS_UI_SHOW_NEVER)
        return;

    uint32_t key;
    struct ss_state curr;
    struct nk_context *ctx = UI_GetContext();

    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background, s_bg_style);
    nk_style_push_color(ctx, &ctx->style.window.border_color, s_border_clr);

    kh_foreach(s_entity_state_table, key, curr, {

        if(ui_setting.as_int == SS_UI_SHOW_SELECTED && !G_Sel_IsSelected(key))
            continue;

        struct obb obb;
        Entity_CurrentOBB(key, &obb, true);
        if(!G_Fog_ObjExplored(G_GetPlayerControlledFactions(), key, &obb))
            continue;

        char name[256];
        pf_snprintf(name, sizeof(name), "__storage_site__.%x", key);

        const vec2_t vres = (vec2_t){1920, 1080};
        const vec2_t adj_vres = UI_ArAdjustedVRes(vres);

        vec2_t ss_pos = Entity_TopScreenPos(key, adj_vres.x, adj_vres.y);
        khash_t(int) *table = (curr.use_alt) ? curr.alt_capacity : curr.capacity;

        const int width = 224;
        const int height = MIN(kh_size(table), 16) * 20 + 4;
        const vec2_t pos = (vec2_t){ss_pos.x - width/2, ss_pos.y + 20};
        const int flags = NK_WINDOW_NOT_INTERACTIVE | NK_WINDOW_BORDER | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR;

        struct rect adj_bounds = UI_BoundsForAspectRatio(
            (struct rect){pos.x, pos.y, width, height}, 
            vres, adj_vres, ANCHOR_DEFAULT
        );

        const char *names[16];
        size_t nnames = ss_get_keys(&curr, names, ARR_SIZE(names));

        if(nnames == 0)
            continue;

        if(nk_begin_with_vres(ctx, name, 
            (struct nk_rect){adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h}, 
            flags, (struct nk_vec2i){adj_vres.x, adj_vres.y})) {

            for(int i = 0; i < nnames; i++) {

                int capacity = curr.use_alt ? G_StorageSite_GetAltCapacity(key, names[i]) 
                                            : G_StorageSite_GetCapacity(key, names[i]);
                int desired = curr.use_alt ? G_StorageSite_GetAltDesired(key, names[i]) 
                                           : G_StorageSite_GetDesired(key, names[i]);

                char curr[5], cap[5], des[7];
                pf_snprintf(curr, sizeof(curr), "%4d", G_StorageSite_GetCurr(key, names[i]));
                pf_snprintf(cap, sizeof(cap), "%4d", capacity);
                pf_snprintf(des, sizeof(des), "(%4d)", desired);

                nk_layout_row_begin(ctx, NK_DYNAMIC, 16, 2);
                nk_layout_row_push(ctx, 0.30f);
                nk_label_colored(ctx, names[i], NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

                nk_layout_row_push(ctx, 0.20f);
                nk_label_colored(ctx, curr, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

                nk_layout_row_push(ctx, 0.05f);
                nk_label_colored(ctx, "/", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

                nk_layout_row_push(ctx, 0.20f);
                nk_label_colored(ctx, cap, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

                nk_layout_row_push(ctx, 0.30f);
                nk_label_colored(ctx, des, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
            }
        }
        nk_end(ctx);
    });

    nk_style_pop_style_item(ctx);
    nk_style_pop_color(ctx);
}

static bool save_color(struct nk_color clr, SDL_RWops *stream)
{
    struct attr clr_r = (struct attr){
        .type = TYPE_INT,
        .val.as_int = clr.r
    };
    CHK_TRUE_RET(Attr_Write(stream, &clr_r, "clr_r"));

    struct attr clr_g = (struct attr){
        .type = TYPE_INT,
        .val.as_int = clr.g
    };
    CHK_TRUE_RET(Attr_Write(stream, &clr_g, "clr_g"));

    struct attr clr_b = (struct attr){
        .type = TYPE_INT,
        .val.as_int = clr.b
    };
    CHK_TRUE_RET(Attr_Write(stream, &clr_b, "clr_b"));

    struct attr clr_a = (struct attr){
        .type = TYPE_INT,
        .val.as_int = clr.a
    };
    CHK_TRUE_RET(Attr_Write(stream, &clr_a, "clr_a"));

    return true;
}

static bool load_color(struct nk_color *out, SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    out->r = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    out->g = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    out->b = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    out->a = attr.val.as_int;

    return true;
}

static bool save_global_resources(int i, SDL_RWops *stream)
{
    struct attr num_global_resources = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_global_resource_tables[i])
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_global_resources, "num_global_resources"));

    const char *resource_key;
    int resource_amount;

    kh_foreach(s_global_resource_tables[i], resource_key, resource_amount, {
    
        struct attr resource_key_attr = (struct attr){ .type = TYPE_STRING, };
        pf_strlcpy(resource_key_attr.val.as_string, resource_key, sizeof(resource_key_attr.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &resource_key_attr, "resource_key"));

        struct attr resource_amount_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = resource_amount
        };
        CHK_TRUE_RET(Attr_Write(stream, &resource_amount_attr, "resource_amount"));
    });
    return true;
}

static bool load_global_resources(int i, SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_resources  = attr.val.as_int;

    for(int j = 0; j < num_resources; j++) {

        struct attr keyattr;
        CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
        CHK_TRUE_RET(keyattr.type == TYPE_STRING);
        const char *key = si_intern(keyattr.val.as_string, &s_stringpool, s_stridx);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int val = attr.val.as_int;

        khiter_t k = kh_get(res, s_global_resource_tables[i], key);
        if(k == kh_end(s_global_resource_tables[i])) {
            k = kh_put(res, s_global_resource_tables[i], key, &(int){0});
        }
        kh_value(s_global_resource_tables[i], k) = val;
    }
    return true;
}

static bool save_global_capacities(int i, SDL_RWops *stream)
{
    struct attr num_global_capacities = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_global_capacity_tables[i])
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_global_capacities, "num_global_capacities"));

    const char *capacity_key;
    int capacity_amount;

    kh_foreach(s_global_capacity_tables[i], capacity_key, capacity_amount, {
    
        struct attr capacity_key_attr = (struct attr){ .type = TYPE_STRING, };
        pf_strlcpy(capacity_key_attr.val.as_string, capacity_key, sizeof(capacity_key_attr.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &capacity_key_attr, "capacity_key"));

        struct attr capacity_amount_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = capacity_amount
        };
        CHK_TRUE_RET(Attr_Write(stream, &capacity_amount_attr, "capacity_amount"));
    });
    return true;
}

static bool load_global_capacities(int i, SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_capacities  = attr.val.as_int;

    for(int j = 0; j < num_capacities; j++) {

        struct attr keyattr;
        CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
        CHK_TRUE_RET(keyattr.type == TYPE_STRING);
        const char *key = si_intern(keyattr.val.as_string, &s_stringpool, s_stridx);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int val = attr.val.as_int;

        khiter_t k = kh_get(res, s_global_capacity_tables[i], key);
        if(k == kh_end(s_global_capacity_tables[i])) {
            k = kh_put(res, s_global_capacity_tables[i], key, &(int){0});
        }
        kh_value(s_global_capacity_tables[i], k) = val;
    }
    return true;
}

static bool storage_site_ui_mode_validate(const struct sval *val)
{
    if(val->type != ST_TYPE_INT)
        return false;
    if(val->as_int < 0 || val->as_int > SS_UI_SHOW_NEVER)
        return false;
    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_StorageSite_Init(void)
{
    mpa_buff_init(&s_mpool, 1024, 0);

    if(!mpa_buff_reserve(&s_mpool, 1024))
        goto fail_mpool; 
    if(!(s_entity_state_table = kh_init(state)))
        goto fail_table;

    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(!(s_global_resource_tables[i] = kh_init(res))) {
            for(--i; i; i--)
                kh_destroy(res, s_global_resource_tables[i]);
            goto fail_res;
        }
    }

    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(!(s_global_capacity_tables[i] = kh_init(res))) {
            for(--i; i; i--)
                kh_destroy(res, s_global_capacity_tables[i]);
            goto fail_cap;
        }
    }

    if(!si_init(&s_stringpool, &s_stridx, 512))
        goto fail_strintern;

    struct nk_context ctx;
    nk_style_default(&ctx);

    s_bg_style = ctx.style.window.fixed_background;
    s_border_clr = ctx.style.window.border_color;
    s_font_clr = ctx.style.text.color;

    ss_e status;
    (void)status;
    status = Settings_Create((struct setting){
        .name = "pf.game.storage_site_ui_mode",
        .val = (struct sval) {
            .type = ST_TYPE_INT,
            .as_int = SS_UI_SHOW_ALWAYS
        },
        .prio = 0,
        .validate = storage_site_ui_mode_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    E_Global_Register(EVENT_UPDATE_UI, on_update_ui, NULL, G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);
    return true;

fail_strintern:
    for(int i = 0; i < MAX_FACTIONS; i++)
        kh_destroy(res, s_global_capacity_tables[i]);
fail_cap:
    for(int i = 0; i < MAX_FACTIONS; i++)
        kh_destroy(res, s_global_resource_tables[i]);
fail_res:
    kh_destroy(state, s_entity_state_table);
fail_table:
    mpa_buff_destroy(&s_mpool);
fail_mpool:
    return false;
}

void G_StorageSite_Shutdown(void)
{
    E_Global_Unregister(EVENT_UPDATE_UI, on_update_ui);

    for(int i = 0; i < MAX_FACTIONS; i++)
        kh_destroy(res, s_global_capacity_tables[i]);
    for(int i = 0; i < MAX_FACTIONS; i++)
        kh_destroy(res, s_global_resource_tables[i]);

    si_shutdown(&s_stringpool, s_stridx);
    kh_destroy(state, s_entity_state_table);
    mpa_buff_destroy(&s_mpool);
}

void G_StorageSite_ClearState(void)
{
    G_StorageSite_Shutdown();
    G_StorageSite_Init();
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

    const char *key;
    int amount;

    kh_foreach(ss->curr, key, amount, {
        update_res_delta(key, -amount, G_GetFactionID(uid));
    });

    khash_t(int) *cap = ss->use_alt ? ss->alt_capacity : ss->capacity;
    kh_foreach(cap, key, amount, {
        update_cap_delta(key, -amount, G_GetFactionID(uid));
    });

    ss_state_destroy(ss);
    ss_state_remove(uid);
}

bool G_StorageSite_IsSaturated(uint32_t uid)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    const char *key;
    int amount;
    khash_t(int) *table = ss->use_alt ? ss->alt_capacity : ss->capacity;

    kh_foreach(table, key, amount, {
        int curr = G_StorageSite_GetCurr(uid, key);
        if(curr < amount)
            return false;
    });
    return true;
}

bool G_StorageSite_SetCapacity(uint32_t uid, const char *rname, int max)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    int prev = 0;
    ss_state_get_key(ss->curr, rname, &prev);
    int delta = max - prev;

    if(!ss->use_alt) {
        update_cap_delta(rname, delta, G_GetFactionID(uid));
    }

    bool ret = ss_state_set_key(ss->capacity, rname, max);
    constrain_desired(ss, rname);
    return ret;
}

int G_StorageSite_GetCapacity(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_CAPACITY;
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    khash_t(int) *table = (ss->use_alt) ? ss->alt_capacity : ss->capacity;
    ss_state_get_key(table, rname, &ret);
    return ret;
}

bool G_StorageSite_SetCurr(uint32_t uid, const char *rname, int curr)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    int cap = 0;
    khash_t(int) *table = (ss->use_alt) ? ss->alt_capacity : ss->capacity;
    ss_state_get_key(table, rname, &cap);

    if(curr > cap)
        return false;
    if(curr < 0)
        return false;

    int prev = 0;
    ss_state_get_key(ss->curr, rname, &prev);
    int delta = curr - prev;
    update_res_delta(rname, delta, G_GetFactionID(uid));

    if(delta) {
        ss->last_change = (struct ss_delta_event){
            .name = si_intern(rname, &s_stringpool, s_stridx),
            .delta = delta
        };
        E_Entity_Notify(EVENT_STORAGE_SITE_AMOUNT_CHANGED, uid, &ss->last_change, ES_ENGINE);
    }

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

bool G_StorageSite_SetDesired(uint32_t uid, const char *rname, int des)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);
    bool ret = ss_state_set_key(ss->desired, rname, des);
    constrain_desired(ss, rname);
    return ret;
}

int G_StorageSite_GetDesired(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_CAPACITY;
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    ss_state_get_key(ss->desired, rname, &ret);
    return ret;
}

int G_StorageSite_GetPlayerStored(const char *rname)
{
    int ret = 0;
    uint16_t pfacs = G_GetPlayerControlledFactions();

    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(!(pfacs & (0x1 << i)))
            continue;
        khiter_t k = kh_get(res, s_global_resource_tables[i], rname);
        if(k == kh_end(s_global_resource_tables[i]))
            continue;
        ret += kh_value(s_global_resource_tables[i], k);
    }
    return ret;
}

int G_StorageSite_GetPlayerCapacity(const char *rname)
{
    int ret = 0;
    uint16_t pfacs = G_GetPlayerControlledFactions();

    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(!(pfacs & (0x1 << i)))
            continue;
        khiter_t k = kh_get(res, s_global_capacity_tables[i], rname);
        if(k == kh_end(s_global_capacity_tables[i]))
            continue;
        ret += kh_value(s_global_capacity_tables[i], k);
    }
    return ret;
}

int G_StorageSite_GetStorableResources(uint32_t uid, size_t maxout, const char *out[])
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);
    return ss_get_keys(ss, out, maxout);
}

void G_StorageSite_SetFontColor(const struct nk_color *clr)
{
    s_font_clr = *clr;
}

void G_StorageSite_SetBorderColor(const struct nk_color *clr)
{
    s_border_clr = *clr;
}

void G_StorageSite_SetBackgroundStyle(const struct nk_style_item *style)
{
    s_bg_style = *style;
}

void G_StorageSite_SetShowUI(bool show)
{
    s_show_ui = show;
}

bool G_StorageSite_GetDoNotTake(uint32_t uid)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);
    return ss->do_not_take;
}

void G_StorageSite_SetDoNotTake(uint32_t uid, bool on)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);
    ss->do_not_take = on;
}

void G_StorageSite_SetUseAlt(uint32_t uid, bool use)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    if(use == ss->use_alt)
        return;

    const char *key;
    int amount;

    if(use) {
        kh_foreach(ss->capacity, key, amount, {
            update_cap_delta(key, -amount, G_GetFactionID(uid));
        });
        kh_foreach(ss->alt_capacity, key, amount, {
            update_cap_delta(key, amount, G_GetFactionID(uid));
        });
    }else{
        kh_foreach(ss->alt_capacity, key, amount, {
            update_cap_delta(key, -amount, G_GetFactionID(uid));
        });
        kh_foreach(ss->capacity, key, amount, {
            update_cap_delta(key, amount, G_GetFactionID(uid));
        });
    }
    ss->use_alt = use;
}

bool G_StorageSite_GetUseAlt(uint32_t uid)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);
    return ss->use_alt;
}

void G_StorageSite_ClearAlt(uint32_t uid)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    if(ss->use_alt) {
        const char *key;
        int amount;

        kh_foreach(ss->alt_capacity, key, amount, {
            update_cap_delta(key, -amount, G_GetFactionID(uid));
        });
    }

    kh_clear(int, ss->alt_capacity);
    kh_clear(int, ss->alt_desired);
}

void G_StorageSite_ClearCurr(uint32_t uid)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    const char *key;
    int amount;

    kh_foreach(ss->alt_capacity, key, amount, {
        update_cap_delta(key, -amount, G_GetFactionID(uid));
    });

    kh_clear(int, ss->curr);
}

bool G_StorageSite_SetAltCapacity(uint32_t uid, const char *rname, int max)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    int prev = 0;
    ss_state_get_key(ss->curr, rname, &prev);
    int delta = max - prev;

    if(ss->use_alt) {
        update_cap_delta(rname, delta, G_GetFactionID(uid));
    }

    bool ret = ss_state_set_key(ss->alt_capacity, rname, max);
    constrain_desired(ss, rname);
    return ret;
}

int G_StorageSite_GetAltCapacity(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_CAPACITY;
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    ss_state_get_key(ss->alt_capacity, rname, &ret);
    return ret;
}

bool G_StorageSite_SetAltDesired(uint32_t uid, const char *rname, int des)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);
    bool ret = ss_state_set_key(ss->alt_desired, rname, des);
    constrain_desired(ss, rname);
    return ret;
}

int G_StorageSite_GetAltDesired(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_CAPACITY;
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    ss_state_get_key(ss->alt_desired, rname, &ret);
    return ret;
}

void G_StorageSite_UpdateFaction(uint32_t uid, int oldfac, int newfac)
{
    struct ss_state *ss = ss_state_get(uid);
    if(!ss)
        return;

    const char *key;
    int amount;

    khash_t(int) *cap = ss->use_alt ? ss->alt_capacity : ss->capacity;
    kh_foreach(cap, key, amount, {
        update_cap_delta(key, -amount, oldfac);
        update_cap_delta(key,  amount, newfac);
    });

    kh_foreach(ss->curr, key, amount, {
        update_res_delta(key, -amount, oldfac);
        update_res_delta(key,  amount, newfac);
    });
}

bool G_StorageSite_Desires(uint32_t uid, const char *rname)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);
    khash_t(int) *des = ss->use_alt ? ss->alt_desired  : ss->desired;

    int rdes, rcurr = 0;
    if(!ss_state_get_key(des, rname, &rdes))
        return false;

    ss_state_get_key(ss->curr, rname, &rcurr);
    return (rdes > rcurr);
}

bool G_StorageSite_SaveState(struct SDL_RWops *stream)
{
    struct attr num_ents = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_entity_state_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_ents, "num_ents"));
    Sched_TryYield();

    uint32_t key;
    struct ss_state curr;

    kh_foreach(s_entity_state_table, key, curr, {

        struct attr uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = key
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid, "uid"));

        struct attr use_alt = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.use_alt
        };
        CHK_TRUE_RET(Attr_Write(stream, &use_alt, "use_alt"));

        struct attr num_capacity = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr.capacity)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_capacity, "num_capacity"));

        const char *cap_key;
        int cap_amount;
        kh_foreach(curr.capacity, cap_key, cap_amount, {
        
            struct attr cap_key_attr = (struct attr){ .type = TYPE_STRING, };
            pf_strlcpy(cap_key_attr.val.as_string, cap_key, sizeof(cap_key_attr.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &cap_key_attr, "cap_key"));

            struct attr cap_amount_attr = (struct attr){
                .type = TYPE_INT,
                .val.as_int = cap_amount
            };
            CHK_TRUE_RET(Attr_Write(stream, &cap_amount_attr, "cap_amount"));
        });

        struct attr num_curr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr.curr)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_curr, "num_curr"));

        const char *curr_key;
        int curr_amount;
        kh_foreach(curr.curr, curr_key, curr_amount, {
        
            struct attr curr_key_attr = (struct attr){ .type = TYPE_STRING, };
            pf_strlcpy(curr_key_attr.val.as_string, curr_key, sizeof(curr_key_attr.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &curr_key_attr, "curr_key"));

            struct attr curr_amount_attr = (struct attr){
                .type = TYPE_INT,
                .val.as_int = curr_amount
            };
            CHK_TRUE_RET(Attr_Write(stream, &curr_amount_attr, "curr_amount"));
        });

        struct attr num_desired = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr.desired)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_desired, "num_desired"));

        const char *desired_key;
        int desired_amount;
        kh_foreach(curr.desired, desired_key, desired_amount, {
        
            struct attr desired_key_attr = (struct attr){ .type = TYPE_STRING, };
            pf_strlcpy(desired_key_attr.val.as_string, desired_key, sizeof(desired_key_attr.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &desired_key_attr, "desired_key"));

            struct attr desired_amount_attr = (struct attr){
                .type = TYPE_INT,
                .val.as_int = desired_amount
            };
            CHK_TRUE_RET(Attr_Write(stream, &desired_amount_attr, "desired_amount"));
        });

        struct attr num_alt_cap = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr.alt_capacity)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_alt_cap, "num_alt_cap"));

        const char *alt_cap_key;
        int alt_cap_amount;
        kh_foreach(curr.alt_capacity, alt_cap_key, alt_cap_amount, {
        
            struct attr alt_cap_key_attr = (struct attr){ .type = TYPE_STRING, };
            pf_strlcpy(alt_cap_key_attr.val.as_string, alt_cap_key, sizeof(alt_cap_key_attr.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &alt_cap_key_attr, "alt_cap_key"));

            struct attr alt_cap_amount_attr = (struct attr){
                .type = TYPE_INT,
                .val.as_int = alt_cap_amount
            };
            CHK_TRUE_RET(Attr_Write(stream, &alt_cap_amount_attr, "alt_cap_amount"));
        });

        struct attr num_alt_desired = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr.alt_desired)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_alt_desired, "num_alt_desired"));

        const char *alt_desired_key;
        int alt_desired_amount;
        kh_foreach(curr.alt_desired, alt_desired_key, alt_desired_amount, {
        
            struct attr alt_desired_key_attr = (struct attr){ .type = TYPE_STRING, };
            pf_strlcpy(alt_desired_key_attr.val.as_string, alt_desired_key, sizeof(alt_desired_key_attr.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &alt_desired_key_attr, "alt_desired_key"));

            struct attr alt_desired_amount_attr = (struct attr){
                .type = TYPE_INT,
                .val.as_int = alt_desired_amount
            };
            CHK_TRUE_RET(Attr_Write(stream, &alt_desired_amount_attr, "alt_desired_amount"));
        });

        struct attr do_not_take = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.do_not_take
        };
        CHK_TRUE_RET(Attr_Write(stream, &do_not_take, "do_not_take"));
        Sched_TryYield();
    });

    /* save global resource/capacity tables 
     */
    for(int i = 0; i < MAX_FACTIONS; i++) {
    
        CHK_TRUE_RET(save_global_resources(i, stream));
        CHK_TRUE_RET(save_global_capacities(i, stream));
        Sched_TryYield();
    }

    /* save UI style 
     */
    struct attr bg_style_type = (struct attr){
        .type = TYPE_INT,
        .val.as_int = s_bg_style.type
    };
    CHK_TRUE_RET(Attr_Write(stream, &bg_style_type, "bg_style_type"));
    Sched_TryYield();

    switch(s_bg_style.type) {
    case NK_STYLE_ITEM_COLOR: {

        CHK_TRUE_RET(save_color(s_bg_style.data.color, stream));
        break;
    }
    case NK_STYLE_ITEM_TEXPATH: {

        struct attr bg_texpath = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(bg_texpath.val.as_string, s_bg_style.data.texpath, sizeof(bg_texpath.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &bg_texpath, "bg_texpath"));
        break;
    }
    default: assert(0);
    }

    CHK_TRUE_RET(save_color(s_border_clr, stream));
    CHK_TRUE_RET(save_color(s_font_clr, stream));
    Sched_TryYield();

    struct attr ui_shown = (struct attr){
        .type = TYPE_BOOL,
        .val.as_bool = s_show_ui
    };
    CHK_TRUE_RET(Attr_Write(stream, &ui_shown, "ui_shown"));
    Sched_TryYield();

    return true;
}

bool G_StorageSite_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_ents = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_ents; i++) {

        uint32_t uid;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uid = attr.val.as_int;
        CHK_TRUE_RET(G_EntityExists(attr.val.as_int));

        struct ss_state *ss = ss_state_get(uid);
        CHK_TRUE_RET(ss);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        ss->use_alt = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const size_t num_capacity = attr.val.as_int;

        for(int i = 0; i < num_capacity; i++) {
        
            struct attr keyattr;
            CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
            CHK_TRUE_RET(keyattr.type == TYPE_STRING);
            const char *key = keyattr.val.as_string;

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            int val = attr.val.as_int;

            G_StorageSite_SetCapacity(uid, key, val);
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const size_t num_curr = attr.val.as_int;

        for(int i = 0; i < num_curr; i++) {
        
            struct attr keyattr;
            CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
            CHK_TRUE_RET(keyattr.type == TYPE_STRING);
            const char *key = keyattr.val.as_string;

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            int val = attr.val.as_int;

            G_StorageSite_SetCurr(uid, key, val);
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const size_t num_desired = attr.val.as_int;

        for(int i = 0; i < num_desired; i++) {
        
            struct attr keyattr;
            CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
            CHK_TRUE_RET(keyattr.type == TYPE_STRING);
            const char *key = keyattr.val.as_string;

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            int val = attr.val.as_int;

            G_StorageSite_SetDesired(uid, key, val);
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const size_t num_alt_capacity = attr.val.as_int;

        for(int i = 0; i < num_alt_capacity; i++) {
        
            struct attr keyattr;
            CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
            CHK_TRUE_RET(keyattr.type == TYPE_STRING);
            const char *key = keyattr.val.as_string;

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            int val = attr.val.as_int;

            G_StorageSite_SetAltCapacity(uid, key, val);
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const size_t num_alt_desired = attr.val.as_int;

        for(int i = 0; i < num_alt_desired; i++) {
        
            struct attr keyattr;
            CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
            CHK_TRUE_RET(keyattr.type == TYPE_STRING);
            const char *key = keyattr.val.as_string;

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            int val = attr.val.as_int;

            G_StorageSite_SetAltDesired(uid, key, val);
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        ss->do_not_take = attr.val.as_bool;
        Sched_TryYield();
    }

    /* load global resource/capacity tables 
     */
    for(int i = 0; i < MAX_FACTIONS; i++) {

        CHK_TRUE_RET(load_global_resources(i, stream));
        CHK_TRUE_RET(load_global_capacities(i, stream));
        Sched_TryYield();
    }

    /* load UI style 
     */
    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    s_bg_style.type = attr.val.as_int;

    switch(s_bg_style.type) {
    case NK_STYLE_ITEM_COLOR: {

        CHK_TRUE_RET(load_color(&s_bg_style.data.color, stream));
        break;
    }
    case NK_STYLE_ITEM_TEXPATH: {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        pf_strlcpy(s_bg_style.data.texpath, attr.val.as_string, sizeof(s_bg_style.data.texpath));
        break;
    }
    default: 
        return false;
    }

    CHK_TRUE_RET(load_color(&s_border_clr, stream));
    CHK_TRUE_RET(load_color(&s_font_clr, stream));

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_BOOL);
    s_show_ui = attr.val.as_bool;
    Sched_TryYield();

    return true;
}

