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

#define MEM_FILE_SYS MEM_SYS_GAME
#define MEM_FILE_SUB MEM_SUB_GAME_STORAGE_SITE

#include "storage_site.h"
#include "game_private.h"
#include "selection.h"
#include "../sched.h"
#include "../ui.h"
#include "../event.h"
#include "../settings.h"
#include "../main.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/khash.h"
#include "../lib/public/mpool_allocator.h"
#include "../lib/public/string_intern.h"
#include "../lib/public/attr.h"
#include "../lib/public/stalloc.h"
#include "../lib/public/stb_image.h"

#include <assert.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_STORAGE_SITE)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_STORAGE_SITE)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_STORAGE_SITE)

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
    /* Alternative capacity/desired parameters that 
     *can be turned on/off */
    bool                   use_alt;
    kh_int_t              *alt_capacity;
    kh_int_t              *alt_desired;
    /* Flags to inform harvesters not to take anything 
     * from this site */
    bool                   do_not_take_land;
    bool                   do_not_take_water;
    /* The cluster whose pool holds this site's stock, or 0. A site in a 
     * pool keeps only its share of the capacity in its own state. */
    int                    pool;
    /* For a pool's own state: how many sites share it, and the one that 
     * shows it */
    int                    nmembers;
    uint32_t               anchor;
    /* How much the requests of this site count against others' */
    int                    priority;
    /* Per resource: the reserve never given away, and what the site does 
     * with the resource (enum ss_chip) */
    kh_int_t              *keep;
    kh_int_t              *chip;
};

typedef char buff_t[512];

MPOOL_ALLOCATOR_TYPE(buff, buff_t)
MPOOL_ALLOCATOR_PROTOTYPES(static, buff, buff_t)
MPOOL_ALLOCATOR_IMPL(static, buff, buff_t)

#undef kmalloc
#undef kcalloc
#undef krealloc
#undef kfree

#define kmalloc  PF_MALLOC
#define kcalloc  PF_CALLOC
#define krealloc PF_REALLOC
#define kfree    PF_FREE

KHASH_MAP_INIT_INT(state, struct ss_state)
KHASH_MAP_INIT_STR(res, int)

/* Running totals of a resource brought into and used up from each faction's
 * economy; moving it between storage sites counts as neither */
struct flow{
    int produced[MAX_FACTIONS];
    int consumed[MAX_FACTIONS];
};

KHASH_MAP_INIT_STR(flow, struct flow)

/* How the stock of one good in a pool has been moving */
struct trend{
    const char *rname;
    int         last;
    /* Moving average of the change per second */
    float       rate;
    int         arrow;
    uint32_t    unchanged_s;
};

/* The goods trends of one pool, or of a site in none */
struct strip{
    uint32_t     stamp;
    size_t       ntrends;
    struct trend trends[16];
};

KHASH_MAP_INIT_INT64(strip, struct strip)

/* A good as the overlay strip shows it */
struct strip_good{
    const char *icon;
    char        count[8];
    float       count_w;
    int         arrow;
    bool        blocked;
};

#define GREEN_CLR (struct nk_color){22, 84, 5, 255}

#define STRIP_TEXT_CLR      (struct nk_color){235, 228, 210, 255}
#define STRIP_OUTLINE_CLR   (struct nk_color){20, 16, 12, 230}
#define STRIP_WARN_CLR      (struct nk_color){255, 150, 40, 255}
#define TREND_UP_CLR        (struct nk_color){110, 205, 80, 255}
#define TREND_DOWN_CLR      (struct nk_color){225, 70, 55, 255}
#define TREND_FLAT_CLR      (struct nk_color){185, 180, 170, 255}
#define CART_CLR            (struct nk_color){170, 120, 70, 255}

#define STRIP_MAX_SITES     (64)
#define STRIP_MAX_GOODS     (8)
#define STRIP_ICON_SZ       (22.0f)
#define STRIP_GAP           (8.0f)
#define TREND_W             (7.0f)
#define PENNANT_W           (11.0f)
#define CART_W              (13.0f)
/* Weight of each one-second sample in the moving average of a stock's change */
#define TREND_ALPHA         (0.1f)
/* Goods per second that turn the arrow on, and that it must fall back under to turn off */
#define TREND_ON            (0.05f)
#define TREND_OFF           (0.02f)
#define RECENT_CHANGE_S     (30)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static mpa_buff_t       s_mpool;
static khash_t(stridx) *s_stridx;
static mp_strbuff_t     s_stringpool;
static struct memstack  s_eventargs;
static khash_t(state)  *s_entity_state_table;
/* The pooled state of every cluster of storage sites */
static khash_t(state)  *s_pools;
static khash_t(res)    *s_global_resource_tables[MAX_FACTIONS];
static khash_t(res)    *s_global_capacity_tables[MAX_FACTIONS];
static khash_t(flow)   *s_flows;
/* Keyed by strip_key */
static khash_t(strip)  *s_strips;
static uint32_t         s_strip_stamp;

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
    kh_destroy(int, hs->keep);
    kh_destroy(int, hs->chip);
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
    hs->keep = kh_init(int);
    hs->chip = kh_init(int);

    if(!hs->capacity || !hs->curr || !hs->desired
    || !hs->alt_capacity || !hs->alt_desired
    || !hs->keep || !hs->chip) {

        ss_state_destroy(hs);
        return false;
    }

    hs->use_alt = false;
    hs->do_not_take_land = false;
    hs->do_not_take_water = false;
    hs->pool = 0;
    hs->nmembers = 0;
    hs->anchor = NULL_UID;
    hs->priority = SS_PRIORITY_NORMAL;
    return true;
}

static struct ss_state *pool_get(int pool)
{
    khiter_t k = kh_get(state, s_pools, pool);
    if(k == kh_end(s_pools))
        return NULL;
    return &kh_value(s_pools, k);
}

/* The state that a site's stock and settings live in: its cluster's pool,
 * or its own. */
static struct ss_state *ss_view(uint32_t uid)
{
    struct ss_state *ss = ss_state_get(uid);
    if(!ss || !ss->pool)
        return ss;
    struct ss_state *pool = pool_get(ss->pool);
    return pool ? pool : ss;
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

static void record_flow(int faction_id, const char *rname, int produced, int consumed)
{
    if(faction_id < 0 || faction_id >= MAX_FACTIONS)
        return;

    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return;

    khiter_t k = kh_get(flow, s_flows, key);
    if(k == kh_end(s_flows)) {
        int status;
        k = kh_put(flow, s_flows, key, &status);
        if(status == -1)
            return;
        kh_value(s_flows, k) = (struct flow){0};
    }
    kh_value(s_flows, k).produced[faction_id] += produced;
    kh_value(s_flows, k).consumed[faction_id] += consumed;
}

static int player_flow_total(const char *rname, bool produced)
{
    khiter_t k = kh_get(flow, s_flows, rname);
    if(k == kh_end(s_flows))
        return 0;

    int ret = 0;
    uint16_t pfacs = G_GetPlayerControlledFactions();
    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(!(pfacs & (0x1 << i)))
            continue;
        ret += produced ? kh_value(s_flows, k).produced[i] : kh_value(s_flows, k).consumed[i];
    }
    return ret;
}

/* Names are interned, so the same name is the same pointer */
static size_t add_unique_name(const char *name, size_t nout, size_t maxout, const char *out[])
{
    if(nout == maxout)
        return nout;
    for(size_t i = 0; i < nout; i++) {
        if(out[i] == name)
            return nout;
    }
    out[nout] = name;
    return nout + 1;
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

/* A pool is selected when any of its members is */
static bool pool_selected(uint32_t key, int pool)
{
    if(G_Sel_IsSelected(key))
        return true;
    if(!pool)
        return false;
    uint32_t members[64];
    size_t nmembers = G_Cluster_Members(pool, ARR_SIZE(members), members);
    for(int i = 0; i < nmembers; i++) {
        if(G_Sel_IsSelected(members[i]))
            return true;
    }
    return false;
}

static bool pool_hovered(uint32_t key, int pool)
{
    uint32_t hovered = G_Sel_GetHovered();
    if(hovered == NULL_UID)
        return false;
    if(hovered == key)
        return true;
    return (pool && G_Cluster_Of(hovered) == pool);
}

/* The state shown over a site: its pool's over the pool's anchor, none over
 * the pool's other members */
static struct ss_state *shown_state(uint32_t uid)
{
    struct ss_state *own = ss_state_get(uid);
    if(!own || !own->pool)
        return own;
    struct ss_state *pool = pool_get(own->pool);
    if(pool && pool->anchor != uid)
        return NULL;
    return pool ? pool : own;
}

static uint64_t strip_key(uint32_t uid)
{
    struct ss_state *own = ss_state_get(uid);
    if(own && own->pool)
        return ((uint64_t)1 << 32) | (uint32_t)own->pool;
    return uid;
}

static const struct trend *trend_find(const struct strip *strip, const char *rname)
{
    for(int i = 0; i < strip->ntrends; i++) {
        if(strip->trends[i].rname == rname)
            return &strip->trends[i];
    }
    return NULL;
}

static int trend_arrow(int arrow, float rate)
{
    if(rate > TREND_ON)
        return 1;
    if(rate < -TREND_ON)
        return -1;
    if(arrow > 0 && rate < TREND_OFF)
        return 0;
    if(arrow < 0 && rate > -TREND_OFF)
        return 0;
    return arrow;
}

static void update_trends(uint32_t uid, struct ss_state *ss)
{
    int status;
    khiter_t k = kh_put(strip, s_strips, strip_key(uid), &status);
    if(status == -1)
        return;
    struct strip *strip = &kh_value(s_strips, k);
    if(status != 0) {
        strip->ntrends = 0;
    }
    strip->stamp = s_strip_stamp;

    const char *names[16];
    size_t nnames = ss_get_keys(ss, names, ARR_SIZE(names));

    struct trend trends[16];
    for(int i = 0; i < nnames; i++) {

        int amount = 0;
        ss_state_get_key(ss->curr, names[i], &amount);

        const struct trend *old = trend_find(strip, names[i]);
        if(!old) {
            trends[i] = (struct trend){
                .rname = names[i],
                .last = amount,
                .rate = 0.0f,
                .arrow = 0,
                .unchanged_s = RECENT_CHANGE_S
            };
            continue;
        }

        int delta = amount - old->last;
        float rate = old->rate + (delta - old->rate) * TREND_ALPHA;
        trends[i] = (struct trend){
            .rname = names[i],
            .last = amount,
            .rate = rate,
            .arrow = trend_arrow(old->arrow, rate),
            .unchanged_s = delta ? 0 : MIN(old->unchanged_s + 1, RECENT_CHANGE_S)
        };
    }
    memcpy(strip->trends, trends, nnames * sizeof(struct trend));
    strip->ntrends = nnames;
}

static bool strip_shows(uint32_t uid, const char *rname, const struct trend *trend)
{
    if(G_StorageSite_Request(uid, rname) > 0)
        return true;
    if(G_StorageSite_GetKeep(uid, rname) > 0)
        return true;
    return trend && (trend->arrow != 0 || trend->unchanged_s < RECENT_CHANGE_S);
}

static const char *reason_code(enum request_status status)
{
    switch(status) {
    case REQUEST_NO_SOURCE:     return "No source";
    case REQUEST_NO_CARRIER:    return "No carrier";
    case REQUEST_UNREACHABLE:   return "Unreachable";
    case REQUEST_NOT_CONNECTED: return "Not connected";
    case REQUEST_KEPT:          return "Kept back";
    case REQUEST_PROMISED:      return "On its way";
    default:                    return NULL;
    }
}

static float text_width(const struct nk_user_font *font, const char *text)
{
    return font->width(font->userdata, font->height, text, strlen(text));
}

static void draw_strip_text(struct nk_command_buffer *canvas, const struct nk_user_font *font,
                            float x, float y, float w, const char *text, struct nk_color clr)
{
    nk_draw_text_outlined(canvas, nk_rect(x, y, w + 1, font->height), text, strlen(text),
        font, nk_rgba(0, 0, 0, 0), clr, STRIP_OUTLINE_CLR);
}

static void draw_trend(struct nk_command_buffer *canvas, float x, float cy, int arrow)
{
    if(arrow == 0) {
        nk_fill_rect(canvas, nk_rect(x - 1, cy - 2.5f, TREND_W + 2, 5), 0, STRIP_OUTLINE_CLR);
        nk_fill_rect(canvas, nk_rect(x, cy - 1.5f, TREND_W, 3), 0, TREND_FLAT_CLR);
        return;
    }
    const float dir = (arrow > 0) ? 1.0f : -1.0f;
    const float tip = cy - dir * 5.0f;
    const float base = cy + dir * 4.0f;
    nk_fill_triangle(canvas, x - 1.5f, base + dir, x + TREND_W / 2, tip - dir * 2.0f,
        x + TREND_W + 1.5f, base + dir, STRIP_OUTLINE_CLR);
    nk_fill_triangle(canvas, x, base, x + TREND_W / 2, tip, x + TREND_W, base,
        (arrow > 0) ? TREND_UP_CLR : TREND_DOWN_CLR);
}

static void draw_pennant(struct nk_command_buffer *canvas, float x, float y, int priority)
{
    struct nk_color clr;
    switch(priority) {
    case SS_PRIORITY_LOW:    clr = nk_rgba(110, 140, 190, 255); break;
    case SS_PRIORITY_URGENT: clr = nk_rgba(215, 45, 35, 255);   break;
    default:                 clr = nk_rgba(230, 175, 45, 255);  break;
    }
    nk_fill_rect(canvas, nk_rect(x - 1, y, 3, STRIP_ICON_SZ), 0, STRIP_OUTLINE_CLR);
    nk_fill_triangle(canvas, x + 1, y - 1, x + PENNANT_W + 1, y + 4, x + 1, y + 9, STRIP_OUTLINE_CLR);
    nk_fill_rect(canvas, nk_rect(x, y + 1, 1, STRIP_ICON_SZ - 2), 0, STRIP_TEXT_CLR);
    nk_fill_triangle(canvas, x + 1, y + 1, x + PENNANT_W - 1, y + 4, x + 1, y + 7, clr);
}

static void draw_cart(struct nk_command_buffer *canvas, float x, float y)
{
    nk_fill_rect(canvas, nk_rect(x - 1, y + 3, CART_W + 2, 8), 1, STRIP_OUTLINE_CLR);
    nk_fill_rect(canvas, nk_rect(x, y + 4, CART_W, 6), 0, CART_CLR);
    nk_fill_circle(canvas, nk_rect(x + 1, y + 8, 6, 6), STRIP_OUTLINE_CLR);
    nk_fill_circle(canvas, nk_rect(x + CART_W - 7, y + 8, 6, 6), STRIP_OUTLINE_CLR);
    nk_fill_circle(canvas, nk_rect(x + 2.5f, y + 9.5f, 3, 3), TREND_FLAT_CLR);
    nk_fill_circle(canvas, nk_rect(x + CART_W - 5.5f, y + 9.5f, 3, 3), TREND_FLAT_CLR);
}

/* One row over a pool's anchor: the goods that matter now, the pool's
 * priority, the carriers bound for it, and why a request of it is stuck */
static void draw_strip(struct nk_command_buffer *canvas, const struct nk_user_font *font,
                       uint32_t key, vec2_t anchor)
{
    struct ss_state *ss = shown_state(key);
    if(!ss)
        return;

    khiter_t k = kh_get(strip, s_strips, strip_key(key));
    const struct strip *strip = (k != kh_end(s_strips)) ? &kh_value(s_strips, k) : NULL;

    const char *names[16];
    size_t nnames = ss_get_keys(ss, names, ARR_SIZE(names));

    struct strip_good goods[STRIP_MAX_GOODS];
    size_t ngoods = 0;
    const char *reason = NULL;
    float width = 0.0f;

    for(int i = 0; i < nnames; i++) {

        int waited_ms;
        enum request_status status = G_Automation_RequestStatus(key, names[i], &waited_ms);
        if(!reason) {
            reason = reason_code(status);
        }

        const struct trend *trend = strip ? trend_find(strip, names[i]) : NULL;
        if(ngoods == STRIP_MAX_GOODS || !strip_shows(key, names[i], trend))
            continue;

        struct strip_good *good = &goods[ngoods++];
        good->icon = G_Resource_GetIcon(names[i]);
        pf_snprintf(good->count, sizeof(good->count), "%d", G_StorageSite_GetCurr(key, names[i]));
        good->count_w = text_width(font, good->count);
        good->arrow = trend ? trend->arrow : 0;
        good->blocked = (status != REQUEST_OK);
        width += STRIP_ICON_SZ + 2 + good->count_w + 2 + TREND_W + STRIP_GAP;
    }

    int priority = ss->priority;
    if(priority != SS_PRIORITY_NORMAL) {
        width += PENNANT_W + STRIP_GAP;
    }

    char carriers[8] = {0};
    float carriers_w = 0.0f;
    int ncarriers = G_Automation_AssignedTransporters(key);
    if(ncarriers > 0) {
        pf_snprintf(carriers, sizeof(carriers), "%d", ncarriers);
        carriers_w = text_width(font, carriers);
        width += CART_W + 3 + carriers_w + STRIP_GAP;
    }

    float reason_w = reason ? text_width(font, reason) : 0.0f;
    width += reason_w;

    if(width == 0.0f)
        return;
    if(!reason)
        width -= STRIP_GAP;

    float x = anchor.x - width / 2.0f;
    const float y = anchor.y + 20;
    const float text_y = y + (STRIP_ICON_SZ - font->height) / 2.0f;
    const float cy = y + STRIP_ICON_SZ / 2.0f;

    for(int i = 0; i < ngoods; i++) {

        const struct strip_good *good = &goods[i];
        if(good->icon) {
            nk_draw_texpath(canvas, nk_rect(x, y, STRIP_ICON_SZ, STRIP_ICON_SZ), good->icon,
                nk_rgba(255, 255, 255, 255));
        }
        x += STRIP_ICON_SZ + 2;
        draw_strip_text(canvas, font, x, text_y, good->count_w, good->count,
            good->blocked ? STRIP_WARN_CLR : STRIP_TEXT_CLR);
        x += good->count_w + 2;
        draw_trend(canvas, x, cy, good->arrow);
        x += TREND_W + STRIP_GAP;
    }

    if(priority != SS_PRIORITY_NORMAL) {
        draw_pennant(canvas, x, y, priority);
        x += PENNANT_W + STRIP_GAP;
    }

    if(ncarriers > 0) {
        draw_cart(canvas, x, y);
        x += CART_W + 3;
        draw_strip_text(canvas, font, x, text_y, carriers_w, carriers, STRIP_TEXT_CLR);
        x += carriers_w + STRIP_GAP;
    }

    if(reason) {
        draw_strip_text(canvas, font, x, text_y, reason_w, reason, STRIP_WARN_CLR);
    }
}

static void draw_strips(struct nk_context *ctx, const uint32_t *keys, size_t nkeys)
{
    const vec2_t vres = (vec2_t){1920, 1080};
    const vec2_t adj_vres = UI_ArAdjustedVRes(vres);
    struct rect adj_bounds = UI_BoundsForAspectRatio(
        (struct rect){0, 0, vres.x, vres.y},
        vres, adj_vres, ANCHOR_DEFAULT
    );

    nk_style_push_color(ctx, &ctx->style.window.background, nk_rgba(0, 0, 0, 0));
    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background,
        nk_style_item_color(nk_rgba(0, 0, 0, 0)));

    if(nk_begin_with_vres(ctx, "__storage_site_strips__",
        nk_rect(adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h),
        NK_WINDOW_NO_INPUT | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR,
        (struct nk_vec2i){adj_vres.x, adj_vres.y})) {

        /* In the normal size of whichever face the scripts left active */
        char prev[64], face[64];
        pf_strlcpy(prev, UI_GetActiveFont(), sizeof(prev));
        pf_strlcpy(face, prev, sizeof(face));
        char *suffix = strrchr(face, '.');
        if(suffix && (!strcmp(suffix, ".11") || !strcmp(suffix, ".13") || !strcmp(suffix, ".32"))) {
            *suffix = '\0';
        }
        UI_SetActiveFont(face);

        struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
        for(int i = 0; i < nkeys; i++) {
            vec2_t anchor = Entity_TopScreenPos(keys[i], adj_vres.x, adj_vres.y);
            draw_strip(canvas, ctx->style.font, keys[i], anchor);
        }
        UI_SetActiveFont(prev);
    }
    nk_end(ctx);

    nk_style_pop_color(ctx);
    nk_style_pop_style_item(ctx);
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

    struct sval strip_setting;
    status = Settings_Get("pf.game.storage_site_strip", &strip_setting);
    assert(status == SS_OKAY);

    struct sval hover_setting;
    status = Settings_Get("pf.game.storage_site_ui_on_hover", &hover_setting);
    assert(status == SS_OKAY);
    (void)status;

    uint32_t strips[STRIP_MAX_SITES];
    size_t nstrips = 0;
    const uint16_t player_factions = G_GetPlayerControlledFactions();

    uint32_t key;
    struct ss_state curr;
    struct nk_context *ctx = UI_GetContext();

    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background, s_bg_style);
    nk_style_push_color(ctx, &ctx->style.window.border_color, s_border_clr);
    nk_style_push_vec2(ctx, &ctx->style.window.padding, nk_vec2(8.0f, 16.0f));

    kh_foreach(s_entity_state_table, key, curr, {

        /* A pool is shown once, over its anchor */
        int pool_id = curr.pool;
        if(curr.pool) {
            struct ss_state *pool = pool_get(curr.pool);
            if(pool && pool->anchor != key)
                continue;
            if(pool)
                curr = *pool;
        }

        bool full = (ui_setting.as_int != SS_UI_SHOW_SELECTED)
                 || pool_selected(key, pool_id)
                 || (hover_setting.as_bool && pool_hovered(key, pool_id));
        bool strip = !full && strip_setting.as_bool
                  && (player_factions & (1 << G_GetFactionID(key)));
        if(!full && !strip)
            continue;

        struct obb obb;
        Entity_CurrentOBB(key, &obb, true);
        if(!G_Fog_ObjExplored(G_GetPlayerControlledFactions(), key, &obb))
            continue;

        if(strip) {
            if(nstrips < ARR_SIZE(strips)) {
                strips[nstrips++] = key;
            }
            continue;
        }

        char name[256];
        pf_snprintf(name, sizeof(name), "__storage_site__.%x", key);

        const vec2_t vres = (vec2_t){1920, 1080};
        const vec2_t adj_vres = UI_ArAdjustedVRes(vres);

        vec2_t ss_pos = Entity_TopScreenPos(key, adj_vres.x, adj_vres.y);
        const int width = 198;
        const int height = G_StorageSite_GetWindowHeight(key);
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
                int reserve = G_StorageSite_GetReserve(key, names[i]);
                int curr_amount = G_StorageSite_GetCurr(key, names[i]);

                char curr[5], cap[5], des[7];
                pf_snprintf(curr, sizeof(curr), "%4d", curr_amount);
                pf_snprintf(cap, sizeof(cap), "%4d", capacity);
                pf_snprintf(des, sizeof(des), "(%4d)", reserve);

                nk_layout_row_begin(ctx, NK_STATIC, 16, 5);
                nk_layout_row_push(ctx, 16);
                const char *icon = G_Resource_GetIcon(names[i]);
                if(icon) {
                    nk_image_texpath(ctx, icon);
                }else{
                    nk_label_colored(ctx, "?", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
                }

                nk_layout_row_push(ctx, 40);
                nk_label_colored(ctx, curr, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, 
                    (curr_amount > 0) ? GREEN_CLR : s_font_clr);

                nk_layout_row_push(ctx, 10);
                nk_label_colored(ctx, "/", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

                nk_layout_row_push(ctx, 40);
                nk_label_colored(ctx, cap, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

                nk_layout_row_push(ctx, 68);
                nk_label_colored(ctx, des, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
            }
        }
        nk_end(ctx);
    });

    nk_style_pop_vec2(ctx);
    nk_style_pop_style_item(ctx);
    nk_style_pop_color(ctx);

    if(nstrips > 0) {
        draw_strips(ctx, strips, nstrips);
    }
}

static void on_1hz_tick(void *user, void *event)
{
    uint32_t key;
    struct ss_state curr;
    (void)curr;

    s_strip_stamp++;
    kh_foreach(s_entity_state_table, key, curr, {
        struct ss_state *ss = shown_state(key);
        if(ss) {
            update_trends(key, ss);
        }
    });

    for(khiter_t k = kh_begin(s_strips); k != kh_end(s_strips); k++) {
        if(kh_exist(s_strips, k) && kh_value(s_strips, k).stamp != s_strip_stamp) {
            kh_del(strip, s_strips, k);
        }
    }
}

static void on_update(void *user, void *event)
{
    stalloc_clear(&s_eventargs);
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

static bool save_nine_patch(struct nk_nine_slice_texpath *slice, SDL_RWops *stream)
{
    struct attr bg_texpath = (struct attr){ .type = TYPE_STRING };
    pf_strlcpy(bg_texpath.val.as_string, slice->texpath, 
        sizeof(bg_texpath.val.as_string));
    CHK_TRUE_RET(Attr_Write(stream, &bg_texpath, "bg_texpath"));

    struct attr bg_left = (struct attr){
        .type = TYPE_INT,
        .val.as_int = slice->l
    };
    CHK_TRUE_RET(Attr_Write(stream, &bg_left, "bg_left"));

    struct attr bg_right = (struct attr){
        .type = TYPE_INT,
        .val.as_int = slice->r
    };
    CHK_TRUE_RET(Attr_Write(stream, &bg_right, "bg_right"));

    struct attr bg_top = (struct attr){
        .type = TYPE_INT,
        .val.as_int = slice->t
    };
    CHK_TRUE_RET(Attr_Write(stream, &bg_top, "bg_top"));

    struct attr bg_bot = (struct attr){
        .type = TYPE_INT,
        .val.as_int = slice->b
    };
    CHK_TRUE_RET(Attr_Write(stream, &bg_bot, "bg_bot"));

    return true;
}

static bool load_nine_patch(struct nk_nine_slice_texpath *out, SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_STRING);
    pf_strlcpy(out->texpath, attr.val.as_string, sizeof(out->texpath));

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    out->l = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    out->r = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    out->t = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    out->b = attr.val.as_int;

    char path[512];
    pf_snprintf(path, sizeof(path), "%s/%s", g_basepath, out->texpath);

    int x, y, components;
    int status = stbi_info(path, &x, &y, &components);
    if(status != 1)
        return false;

    out->w = x;
    out->h = y;
    out->region[0] = 0;
    out->region[1] = 0;
    out->region[2] = x;
    out->region[3] = y;

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

static bool save_flows(SDL_RWops *stream)
{
    struct attr num_flows = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_flows)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_flows, "num_flows"));

    const char *key;
    struct flow curr;

    kh_foreach(s_flows, key, curr, {

        struct attr key_attr = (struct attr){ .type = TYPE_STRING, };
        pf_strlcpy(key_attr.val.as_string, key, sizeof(key_attr.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &key_attr, "flow_key"));

        for(int i = 0; i < MAX_FACTIONS; i++) {

            struct attr produced = (struct attr){
                .type = TYPE_INT,
                .val.as_int = curr.produced[i]
            };
            CHK_TRUE_RET(Attr_Write(stream, &produced, "produced"));

            struct attr consumed = (struct attr){
                .type = TYPE_INT,
                .val.as_int = curr.consumed[i]
            };
            CHK_TRUE_RET(Attr_Write(stream, &consumed, "consumed"));
        }
    });
    return true;
}

static bool load_flows(SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_flows = attr.val.as_int;

    for(int j = 0; j < num_flows; j++) {

        struct attr keyattr;
        CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
        CHK_TRUE_RET(keyattr.type == TYPE_STRING);
        const char *key = si_intern(keyattr.val.as_string, &s_stringpool, s_stridx);
        CHK_TRUE_RET(key);

        struct flow val;
        for(int i = 0; i < MAX_FACTIONS; i++) {

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            val.produced[i] = attr.val.as_int;

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            val.consumed[i] = attr.val.as_int;
        }

        int status;
        khiter_t k = kh_put(flow, s_flows, key, &status);
        CHK_TRUE_RET(status != -1);
        kh_value(s_flows, k) = val;
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

static bool bool_setting_validate(const struct sval *val)
{
    return (val->type == ST_TYPE_BOOL);
}

/* Throws 'amount' of a resource out of storage onto the ground by the 
 * site, for the scripts to leave lying there to be picked up. Picking it up 
 * again is not production, so the spill is not consumption either. */
static void spill(uint32_t uid, const char *rname, int amount)
{
    if(amount <= 0)
        return;

    int faction = G_GetFactionID(uid);
    update_res_delta(rname, -amount, faction);

    vec2_t pos = G_Pos_GetXZ(uid);
    struct ss_spill_event *event = stalloc(&s_eventargs, sizeof(struct ss_spill_event));
    *event = (struct ss_spill_event){
        .x = pos.x,
        .z = pos.z,
        .faction_id = faction,
        .name = si_intern(rname, &s_stringpool, s_stridx),
        .amount = amount
    };
    E_Global_Notify(EVENT_RESOURCES_SPILLED, event, ES_ENGINE);
}

static bool save_table(khash_t(int) *table, const char *name, SDL_RWops *stream)
{
    struct attr num = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num, name));

    const char *key;
    int amount;
    kh_foreach(table, key, amount, {

        struct attr key_attr = (struct attr){ .type = TYPE_STRING, };
        pf_strlcpy(key_attr.val.as_string, key, sizeof(key_attr.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &key_attr, "key"));

        struct attr amount_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = amount
        };
        CHK_TRUE_RET(Attr_Write(stream, &amount_attr, "amount"));
    });
    return true;
}

static bool load_table(khash_t(int) *table, SDL_RWops *stream)
{
    struct attr attr;
    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num = attr.val.as_int;

    for(int i = 0; i < num; i++) {

        struct attr keyattr;
        CHK_TRUE_RET(Attr_Parse(stream, &keyattr, true));
        CHK_TRUE_RET(keyattr.type == TYPE_STRING);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        CHK_TRUE_RET(ss_state_set_key(table, keyattr.val.as_string, attr.val.as_int));
    }
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
    if(!(s_pools = kh_init(state)))
        goto fail_pools;

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

    if(!stalloc_init(&s_eventargs))
        goto fail_eventargs;

    if(!(s_flows = kh_init(flow)))
        goto fail_flows;

    if(!(s_strips = kh_init(strip)))
        goto fail_strips;

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

    status = Settings_Create((struct setting){
        .name = "pf.game.storage_site_strip",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = true
        },
        .prio = 0,
        .validate = bool_setting_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.game.storage_site_ui_on_hover",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false
        },
        .prio = 0,
        .validate = bool_setting_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    E_Global_Register(EVENT_UPDATE_UI, on_update_ui, NULL, G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);
    E_Global_Register(EVENT_UPDATE_START, on_update, NULL, G_RUNNING);
    E_Global_Register(EVENT_1HZ_TICK, on_1hz_tick, NULL, G_RUNNING);
    s_show_ui = true;
    return true;

fail_strips:
    kh_destroy(flow, s_flows);
fail_flows:
    stalloc_destroy(&s_eventargs);
fail_eventargs:
    si_shutdown(&s_stringpool, s_stridx);
fail_strintern:
    for(int i = 0; i < MAX_FACTIONS; i++)
        kh_destroy(res, s_global_capacity_tables[i]);
fail_cap:
    for(int i = 0; i < MAX_FACTIONS; i++)
        kh_destroy(res, s_global_resource_tables[i]);
fail_res:
    kh_destroy(state, s_pools);
fail_pools:
    kh_destroy(state, s_entity_state_table);
fail_table:
    mpa_buff_destroy(&s_mpool);
fail_mpool:
    return false;
}

void G_StorageSite_Shutdown(void)
{
    E_Global_Unregister(EVENT_1HZ_TICK, on_1hz_tick);
    E_Global_Unregister(EVENT_UPDATE_START, on_update);
    E_Global_Unregister(EVENT_UPDATE_UI, on_update_ui);

    for(int i = 0; i < MAX_FACTIONS; i++)
        kh_destroy(res, s_global_capacity_tables[i]);
    for(int i = 0; i < MAX_FACTIONS; i++)
        kh_destroy(res, s_global_resource_tables[i]);

    kh_destroy(strip, s_strips);
    kh_destroy(flow, s_flows);
    stalloc_destroy(&s_eventargs);
    si_shutdown(&s_stringpool, s_stridx);
    kh_destroy(state, s_pools);
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

    if(ss->pool) {
        G_StorageSite_PoolLeave(uid, ss->pool, false);
    }

    const char *key;
    int amount;

    kh_foreach(ss->curr, key, amount, {
        update_res_delta(key, -amount, G_GetFactionID(uid));
        record_flow(G_GetFactionID(uid), key, 0, amount);
    });

    khash_t(int) *cap = ss->use_alt ? ss->alt_capacity : ss->capacity;
    kh_foreach(cap, key, amount, {
        update_cap_delta(key, -amount, G_GetFactionID(uid));
    });

    ss_state_destroy(ss);
    ss_state_remove(uid);
}

void G_StorageSite_PoolJoin(uint32_t uid, int pool_id)
{
    struct ss_state *ss = ss_state_get(uid);
    if(!ss || ss->pool == pool_id)
        return;
    assert(!ss->use_alt);

    struct ss_state *pool = pool_get(pool_id);
    if(!pool) {
        struct ss_state newpool;
        if(!ss_state_init(&newpool))
            return;
        int status;
        khiter_t k = kh_put(state, s_pools, pool_id, &status);
        if(status == -1) {
            ss_state_destroy(&newpool);
            return;
        }
        newpool.do_not_take_land = ss->do_not_take_land;
        newpool.do_not_take_water = ss->do_not_take_water;
        newpool.anchor = uid;
        newpool.priority = ss->priority;
        kh_value(s_pools, k) = newpool;
        pool = &kh_value(s_pools, k);
    }

    const char *key;
    int amount;

    kh_foreach(ss->capacity, key, amount, {
        int prev = 0;
        ss_state_get_key(pool->capacity, key, &prev);
        ss_state_set_key(pool->capacity, key, prev + amount);
    });
    /* The stock moves into the pool: the faction's totals stay the same */
    kh_foreach(ss->curr, key, amount, {
        int prev = 0;
        ss_state_get_key(pool->curr, key, &prev);
        ss_state_set_key(pool->curr, key, prev + amount);
    });
    kh_clear(int, ss->curr);
    kh_foreach(ss->desired, key, amount, {
        int prev = 0;
        ss_state_get_key(pool->desired, key, &prev);
        ss_state_set_key(pool->desired, key, MAX(prev, amount));
    });
    kh_foreach(ss->keep, key, amount, {
        int prev = 0;
        ss_state_get_key(pool->keep, key, &prev);
        ss_state_set_key(pool->keep, key, MAX(prev, amount));
    });
    kh_foreach(ss->chip, key, amount, {
        if(!ss_state_get_key(pool->chip, key, &(int){0})) {
            ss_state_set_key(pool->chip, key, amount);
        }
    });

    pool->nmembers++;
    ss->pool = pool_id;
}

void G_StorageSite_PoolLeave(uint32_t uid, int pool_id, bool died)
{
    struct ss_state *ss = ss_state_get(uid);
    if(!ss || ss->pool != pool_id)
        return;
    struct ss_state *pool = pool_get(pool_id);
    ss->pool = 0;
    if(!pool)
        return;

    const char *key;
    int amount;

    kh_foreach(ss->capacity, key, amount, {
        int prev = 0;
        ss_state_get_key(pool->capacity, key, &prev);
        ss_state_set_key(pool->capacity, key, MAX(prev - amount, 0));
    });
    pool->nmembers--;

    if(pool->nmembers <= 0) {
        /* The last site takes back what is left */
        kh_foreach(pool->curr, key, amount, {
            ss_state_set_key(ss->curr, key, amount);
        });
        ss_state_destroy(pool);
        kh_del(state, s_pools, kh_get(state, s_pools, pool_id));
        return;
    }

    /* What the remaining sites cannot hold goes with the one leaving, which 
     * spills it if it is falling */
    kh_foreach(pool->curr, key, amount, {
        int cap = 0;
        ss_state_get_key(pool->capacity, key, &cap);
        int over = amount - cap;
        if(over <= 0)
            continue;
        ss_state_set_key(pool->curr, key, cap);
        if(died) {
            spill(uid, key, over);
        }else{
            int own_cap = 0;
            ss_state_get_key(ss->capacity, key, &own_cap);
            int keep = MIN(over, own_cap);
            ss_state_set_key(ss->curr, key, keep);
            spill(uid, key, over - keep);
        }
    });
    kh_foreach(pool->desired, key, amount, {
        constrain_desired(pool, key);
    });
}

void G_StorageSite_PoolSetAnchor(int pool_id, uint32_t anchor)
{
    struct ss_state *pool = pool_get(pool_id);
    if(!pool)
        return;
    pool->anchor = anchor;
}

void G_StorageSite_OnDeath(uint32_t uid)
{
    struct ss_state *ss = ss_state_get(uid);
    if(!ss)
        return;

    const char *key;
    int amount;
    kh_foreach(ss->curr, key, amount, {
        spill(uid, key, amount);
    });
    kh_clear(int, ss->curr);
}

void G_StorageSite_SetPriority(uint32_t uid, int priority)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    ss->priority = MAX(SS_PRIORITY_LOW, MIN(priority, SS_PRIORITY_UPKEEP));
}

int G_StorageSite_GetPriority(uint32_t uid)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    return ss->priority;
}

void G_StorageSite_SetKeep(uint32_t uid, const char *rname, int keep)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    ss_state_set_key(ss->keep, rname, MAX(keep, 0));
}

int G_StorageSite_GetKeep(uint32_t uid, const char *rname)
{
    int ret = 0;
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    ss_state_get_key(ss->keep, rname, &ret);
    return ret;
}

void G_StorageSite_SetChip(uint32_t uid, const char *rname, enum ss_chip chip)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    ss_state_set_key(ss->chip, rname, chip);
}

enum ss_chip G_StorageSite_GetChip(uint32_t uid, const char *rname)
{
    int ret = SS_CHIP_STORE;
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    ss_state_get_key(ss->chip, rname, &ret);
    return ret;
}

int G_StorageSite_Request(uint32_t uid, const char *rname)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);

    int desired = 0;
    if(ss->use_alt) {
        ss_state_get_key(ss->alt_desired, rname, &desired);
        return desired;
    }
    ss_state_get_key(ss->desired, rname, &desired);

    switch(G_StorageSite_GetChip(uid, rname)) {
    case SS_CHIP_COLLECT:
    case SS_CHIP_RELAY:
        return MAX(desired, G_StorageSite_GetKeep(uid, rname));
    case SS_CHIP_OFF:
        return 0;
    default:
        return desired;
    }
}

int G_StorageSite_Surplus(uint32_t uid, const char *rname)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    if(ss->use_alt)
        return 0;

    int curr = 0, desired = 0;
    ss_state_get_key(ss->curr, rname, &curr);
    ss_state_get_key(ss->desired, rname, &desired);

    switch(G_StorageSite_GetChip(uid, rname)) {
    case SS_CHIP_HOLD:
        return 0;
    case SS_CHIP_OFF:
        return curr;
    case SS_CHIP_RELAY:
        return MAX(curr - desired, 0);
    default:
        return MAX(curr - MAX(desired, G_StorageSite_GetKeep(uid, rname)), 0);
    }
}

int G_StorageSite_GetReserve(uint32_t uid, const char *rname)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);

    int desired = 0;
    if(ss->use_alt) {
        ss_state_get_key(ss->alt_desired, rname, &desired);
        return desired;
    }
    if(G_StorageSite_GetChip(uid, rname) == SS_CHIP_OFF)
        return 0;
    ss_state_get_key(ss->desired, rname, &desired);
    return MAX(desired, G_StorageSite_GetKeep(uid, rname));
}

/* What a building being built may take: all but the player's reserve, as it
 * comes before the stock any building asks for to work from */
int G_StorageSite_SurplusForSite(uint32_t uid, const char *rname)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    if(ss->use_alt)
        return 0;

    int curr = 0;
    ss_state_get_key(ss->curr, rname, &curr);

    switch(G_StorageSite_GetChip(uid, rname)) {
    case SS_CHIP_HOLD:
        return 0;
    case SS_CHIP_OFF:
    case SS_CHIP_RELAY:
        return curr;
    default:
        return MAX(curr - G_StorageSite_GetKeep(uid, rname), 0);
    }
}

bool G_StorageSite_Accepts(uint32_t uid, const char *rname)
{
    return G_StorageSite_GetChip(uid, rname) != SS_CHIP_OFF;
}

bool G_StorageSite_IsSaturated(uint32_t uid)
{
    struct ss_state *ss = ss_view(uid);
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
    ss_state_get_key(ss->capacity, rname, &prev);
    int delta = max - prev;

    if(!ss->use_alt) {
        update_cap_delta(rname, delta, G_GetFactionID(uid));
    }

    bool ret = ss_state_set_key(ss->capacity, rname, max);
    constrain_desired(ss, rname);

    struct ss_state *pool = ss->pool ? pool_get(ss->pool) : NULL;
    if(pool) {
        int pool_prev = 0;
        ss_state_get_key(pool->capacity, rname, &pool_prev);
        ss_state_set_key(pool->capacity, rname, pool_prev + delta);
        constrain_desired(pool, rname);
    }
    return ret;
}

int G_StorageSite_GetCapacity(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_CAPACITY;
    struct ss_state *ss = ss_view(uid);
    assert(ss);

    khash_t(int) *table = (ss->use_alt) ? ss->alt_capacity : ss->capacity;
    ss_state_get_key(table, rname, &ret);
    return ret;
}

int G_StorageSite_GetOwnCapacity(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_CAPACITY;
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);
    ss_state_get_key(ss->capacity, rname, &ret);
    return ret;
}

bool G_StorageSite_SetCurr(uint32_t uid, const char *rname, int curr)
{
    struct ss_state *ss = ss_view(uid);
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
        struct ss_delta_event *event = stalloc(&s_eventargs, sizeof(struct ss_delta_event));
        *event = (struct ss_delta_event){
            .name = si_intern(rname, &s_stringpool, s_stridx),
            .delta = delta
        };
        E_Entity_Notify(EVENT_STORAGE_SITE_AMOUNT_CHANGED, uid, event, ES_ENGINE);
    }

    return ss_state_set_key(ss->curr, rname, curr);
}

int G_StorageSite_GetCurr(uint32_t uid, const char *rname)
{
    int ret = 0;
    struct ss_state *ss = ss_view(uid);
    assert(ss);

    ss_state_get_key(ss->curr, rname, &ret);
    return ret;
}

bool G_StorageSite_SetDesired(uint32_t uid, const char *rname, int des)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    bool ret = ss_state_set_key(ss->desired, rname, des);
    constrain_desired(ss, rname);
    return ret;
}

int G_StorageSite_GetDesired(uint32_t uid, const char *rname)
{
    int ret = DEFAULT_CAPACITY;
    struct ss_state *ss = ss_view(uid);
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

int G_StorageSite_GetFactionStored(int faction, const char *rname)
{
    if(faction < 0 || faction >= MAX_FACTIONS)
        return 0;
    khiter_t k = kh_get(res, s_global_resource_tables[faction], rname);
    if(k == kh_end(s_global_resource_tables[faction]))
        return 0;
    return kh_value(s_global_resource_tables[faction], k);
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

void G_StorageSite_RecordProduced(int faction_id, const char *rname, int amount)
{
    record_flow(faction_id, rname, amount, 0);
}

void G_StorageSite_RecordConsumed(int faction_id, const char *rname, int amount)
{
    record_flow(faction_id, rname, 0, amount);
}

int G_StorageSite_GetPlayerProduced(const char *rname)
{
    return player_flow_total(rname, true);
}

int G_StorageSite_GetPlayerConsumed(const char *rname)
{
    return player_flow_total(rname, false);
}

int G_StorageSite_GetAllNames(size_t maxout, const char *out[])
{
    size_t ret = 0;
    const char *key;

    kh_foreach_key(s_flows, key, {
        ret = add_unique_name(key, ret, maxout, out);
    });
    for(int i = 0; i < MAX_FACTIONS; i++) {
        kh_foreach_key(s_global_resource_tables[i], key, {
            ret = add_unique_name(key, ret, maxout, out);
        });
    }
    return ret;
}

int G_StorageSite_GetStorableResources(uint32_t uid, size_t maxout, const char *out[])
{
    struct ss_state *ss = ss_view(uid);
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

bool G_StorageSite_GetShowUI(void)
{
    return s_show_ui;
}

void G_StorageSite_GetAll(vec_entity_t *out)
{
    uint32_t uid;
    kh_foreach_key(s_entity_state_table, uid, {
        vec_entity_push(out, uid);
    });
}

bool G_StorageSite_GetDoNotTakeLand(uint32_t uid)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    return ss->do_not_take_land;
}

void G_StorageSite_SetDoNotTakeLand(uint32_t uid, bool on)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    ss->do_not_take_land = on;
}

bool G_StorageSite_GetDoNotTakeWater(uint32_t uid)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    return ss->do_not_take_water;
}

void G_StorageSite_SetDoNotTakeWater(uint32_t uid, bool on)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    ss->do_not_take_water = on;
}

bool G_StorageSite_GetDoNotTake(uint32_t flags, uint32_t uid)
{
    if(flags & ENTITY_FLAG_WATER) {
        return G_StorageSite_GetDoNotTakeWater(uid);
    }else{
        return G_StorageSite_GetDoNotTakeLand(uid);
    }
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

    kh_foreach(ss->curr, key, amount, {
        update_res_delta(key, -amount, G_GetFactionID(uid));
        record_flow(G_GetFactionID(uid), key, 0, amount);
    });
    kh_clear(int, ss->curr);
}

bool G_StorageSite_SetAltCapacity(uint32_t uid, const char *rname, int max)
{
    struct ss_state *ss = ss_state_get(uid);
    assert(ss);

    int prev = 0;
    ss_state_get_key(ss->alt_capacity, rname, &prev);
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
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    khash_t(int) *des = ss->use_alt ? ss->alt_desired  : ss->desired;

    int rdes, rcurr = 0;
    if(!ss_state_get_key(des, rname, &rdes))
        return false;

    ss_state_get_key(ss->curr, rname, &rcurr);
    return (rdes > rcurr);
}

float G_StorageSite_GetWindowHeight(uint32_t uid)
{
    struct ss_state *own = ss_state_get(uid);
    assert(own);
    struct ss_state *ss = ss_view(uid);
    if(own->pool && ss->anchor != uid)
        return 0.0f;
    khash_t(int) *table = (ss->use_alt) ? ss->alt_capacity : ss->capacity;
    return MIN(kh_size(table), 16) * 20 + 32;
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

        struct attr do_not_take_land = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.do_not_take_land
        };
        CHK_TRUE_RET(Attr_Write(stream, &do_not_take_land, "do_not_take_land"));

        struct attr do_not_take_water = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.do_not_take_water
        };
        CHK_TRUE_RET(Attr_Write(stream, &do_not_take_water, "do_not_take_water"));

        struct attr pool = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.pool
        };
        CHK_TRUE_RET(Attr_Write(stream, &pool, "pool"));

        struct attr priority = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.priority
        };
        CHK_TRUE_RET(Attr_Write(stream, &priority, "priority"));
        CHK_TRUE_RET(save_table(curr.keep, "num_keep", stream));
        CHK_TRUE_RET(save_table(curr.chip, "num_chip", stream));
        Sched_TryYield();
    });

    struct attr num_pools = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_pools)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_pools, "num_pools"));

    int pool_id;
    kh_foreach(s_pools, pool_id, curr, {

        struct attr id = (struct attr){
            .type = TYPE_INT,
            .val.as_int = pool_id
        };
        CHK_TRUE_RET(Attr_Write(stream, &id, "pool_id"));

        struct attr nmembers = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.nmembers
        };
        CHK_TRUE_RET(Attr_Write(stream, &nmembers, "nmembers"));

        struct attr anchor = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.anchor
        };
        CHK_TRUE_RET(Attr_Write(stream, &anchor, "anchor"));

        struct attr do_not_take_land = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.do_not_take_land
        };
        CHK_TRUE_RET(Attr_Write(stream, &do_not_take_land, "do_not_take_land"));

        struct attr do_not_take_water = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.do_not_take_water
        };
        CHK_TRUE_RET(Attr_Write(stream, &do_not_take_water, "do_not_take_water"));

        CHK_TRUE_RET(save_table(curr.capacity, "num_capacity", stream));
        CHK_TRUE_RET(save_table(curr.curr, "num_curr", stream));
        CHK_TRUE_RET(save_table(curr.desired, "num_desired", stream));

        struct attr priority = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.priority
        };
        CHK_TRUE_RET(Attr_Write(stream, &priority, "priority"));
        CHK_TRUE_RET(save_table(curr.keep, "num_keep", stream));
        CHK_TRUE_RET(save_table(curr.chip, "num_chip", stream));
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
    case NK_STYLE_ITEM_NINE_SLICE_TEXPATH: {

        CHK_TRUE_RET(save_nine_patch(&s_bg_style.data.slice_texpath, stream));
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

    CHK_TRUE_RET(save_flows(stream));
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
        if(!ss) {
            /* Certain Storage Site state can be created 
             * from outside the scripting layer, such as 
             * when building builings or replenishing reources 
             */
            struct ss_state newstate;
            CHK_TRUE_RET(ss_state_init(&newstate));
            CHK_TRUE_RET(ss_state_set(uid, newstate));
            ss = ss_state_get(uid);
            CHK_TRUE_RET(ss);

            uint32_t flags = G_FlagsGet(uid);
            flags |= ENTITY_FLAG_STORAGE_SITE;
            G_FlagsSet(uid, flags);
        }

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
        ss->do_not_take_land = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        ss->do_not_take_water = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ss->pool = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ss->priority = attr.val.as_int;
        CHK_TRUE_RET(load_table(ss->keep, stream));
        CHK_TRUE_RET(load_table(ss->chip, stream));
        Sched_TryYield();
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_pools = attr.val.as_int;

    for(int i = 0; i < num_pools; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int pool_id = attr.val.as_int;

        struct ss_state pool;
        CHK_TRUE_RET(ss_state_init(&pool));
        int status;
        khiter_t k = kh_put(state, s_pools, pool_id, &status);
        CHK_TRUE_RET(status != -1);
        kh_value(s_pools, k) = pool;
        struct ss_state *ps = &kh_value(s_pools, k);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ps->nmembers = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ps->anchor = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        ps->do_not_take_land = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        ps->do_not_take_water = attr.val.as_bool;

        CHK_TRUE_RET(load_table(ps->capacity, stream));
        CHK_TRUE_RET(load_table(ps->curr, stream));
        CHK_TRUE_RET(load_table(ps->desired, stream));

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ps->priority = attr.val.as_int;
        CHK_TRUE_RET(load_table(ps->keep, stream));
        CHK_TRUE_RET(load_table(ps->chip, stream));
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
    case NK_STYLE_ITEM_NINE_SLICE_TEXPATH: {

        CHK_TRUE_RET(load_nine_patch(&s_bg_style.data.slice_texpath, stream));
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

    if(Attr_NextKeyIs(stream, "num_flows")) {
        CHK_TRUE_RET(load_flows(stream));
        Sched_TryYield();
    }

    return true;
}

