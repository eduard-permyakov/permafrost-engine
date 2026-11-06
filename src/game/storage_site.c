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
#include "duty.h"
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
#include <math.h>

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

static bool ss_state_get_key(kh_int_t *table, const char *name, int *out);

/* The goods a site eats in one another's stead, as one draw */
#define MAX_DRAW_MEMBERS    (8)
#define MAX_DRAWS           (8)
/* A draw set lower than it was comes down over this long, one set higher takes at once */
#define DRAW_SMOOTH_S       (60.0f)
/* The stock worked from covers a load's trip and this much more */
#define DRAW_LEAD_S         (60.0f)
#define ROUTE_DEFAULT_S     (120.0f)
#define ROUTE_SMOOTH        (0.3f)
#define LOW_MARK_S          (60.0f)

/* The rate a site eats a class of goods at, set by the game from what it makes */
struct ss_draw{
    uint32_t               site;
    int                    nmembers;
    const char            *members[MAX_DRAW_MEMBERS];
    float                  rate;
    /* What the draw stood at when the rate was last set */
    float                  eff;
    /* One cycle's inputs: the least stock worth working from */
    int                    batch;
    /* The most worth working from, for a run with an end; 0 for no end */
    int                    cap;
    uint32_t               set_s;
};

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
    /* Per resource: what is held for a tribute on top of the keep, which
     * carriers bring in and other stores leave, but a building site may take */
    kh_int_t              *held;
    /* What the site eats, and how long a load takes to reach it, which
     * together size the stock it works from */
    struct ss_draw         draws[MAX_DRAWS];
    int                    ndraws;
    float                  route_s;
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
    /* The strip stamp at which the good last qualified for a row, 0 if never */
    uint32_t    listed_stamp;
};

/* The goods trends of one pool, or of a site in none */
struct strip{
    uint32_t     stamp;
    size_t       ntrends;
    struct trend trends[16];
};

KHASH_MAP_INIT_INT64(strip, struct strip)

/* What a pool, or a site in none, made in each of the last seconds: its
 * products and the units it trained, never what was only brought to it. Only
 * the strip reads it, and it is not saved, as it fills again in a minute. */
#define MADE_WINDOW_S       (60)

struct made{
    const char *name;
    const char *icon;
    uint16_t    per_s[MADE_WINDOW_S];
    /* The sum of per_s: how many were made in the last minute */
    int         total;
};

struct made_record{
    size_t      nmade;
    struct made made[8];
};

KHASH_MAP_INIT_INT64(made, struct made_record)

/* How much of each good was gathered in a district since the session began;
 * not saved, as the scripts only read how it grows */
struct gathered{
    const char *name;
    int         total;
};

struct gathered_record{
    size_t          ngoods;
    struct gathered goods[16];
};

KHASH_MAP_INIT_INT(gathered, struct gathered_record)

/* A line a script shows under a site's strip, such as an izba's growth; not
 * saved, as the scripts set it again every second
 */
struct strip_note{
    const char *icon;
    char        text[48];
    bool        warn;
    /* A second icon and text after the first, such as the rations an izba holds */
    const char *after_icon;
    char        after_text[16];
};

KHASH_MAP_INIT_INT(note, struct strip_note)

/* A good as the overlay strip shows it */
struct strip_good{
    const char *icon;
    char        count[8];
    float       count_w;
    int         arrow;
    bool        blocked;
};

#define GREEN_CLR (struct nk_color){22, 84, 5, 255}
#define RED_CLR   (struct nk_color){150, 30, 20, 255}

#define STRIP_TEXT_CLR      (struct nk_color){235, 228, 210, 255}
#define STRIP_OUTLINE_CLR   (struct nk_color){20, 16, 12, 230}
#define STRIP_WARN_CLR      (struct nk_color){255, 150, 40, 255}
#define TREND_UP_CLR        (struct nk_color){110, 205, 80, 255}
#define TREND_DOWN_CLR      (struct nk_color){225, 70, 55, 255}
#define TREND_FLAT_CLR      (struct nk_color){185, 180, 170, 255}

#define STRIP_MAX_SITES     (64)
#define STRIP_MAX_GOODS     (32)
#define STRIP_ROW_GOODS     (6)
#define STRIP_MAX_MADE      (4)
#define STRIP_ICON_SZ       (22.0f)
/* How far below the anchor the first row sits, and the gap kept under a
 * garrison count */
#define STRIP_DROP          (20.0f)
#define STRIP_GAP_Y         (2.0f)
#define STRIP_GAP           (8.0f)
#define TREND_W             (7.0f)
#define PENNANT_W           (11.0f)
/* Weight of each one-second sample in the moving average of a stock's change */
#define TREND_ALPHA         (0.1f)
/* Goods per second that turn the arrow on, and that it must fall back under to turn off */
#define TREND_ON            (0.05f)
#define TREND_OFF           (0.02f)
#define RECENT_CHANGE_S     (30)
/* How long a row stays on the strip after it stops qualifying, so that one
 * flickering in and out does not reshape it */
#define STRIP_GRACE_S       (2)
#define CARRIERS_ICON       "assets/icons/transport-icon.png"
#define GATHERERS_ICON      "assets/icons/gather-command-normal.png"
#define BUILDERS_ICON       "assets/icons/build-command-normal.png"
#define TREND_UP_ICON       "assets/icons/trend-up-icon.png"
#define TREND_DOWN_ICON     "assets/icons/trend-down-icon.png"
#define POSTS_ICON          "assets/icons/population-icon.png"
/* The window over a picked out site: its padding, row height, icon size and
 * the cells of a good's row, which are as wide as the widest text each may hold */
#define WIN_PAD_X           (8.0f)
#define WIN_PAD_Y           (16.0f)
#define WIN_ROW_H           (16.0f)
#define WIN_ICON_SZ         (16.0f)
#define WIN_ARROW_W         (14.0f)
#define WIN_COLS            (12)
#define WIN_MADE_PER_ROW    (2)

/* The rows of a site's window: its goods, then the marks, crew and output the strip shows */
struct window_plan{
    const char        *names[16];
    size_t             nnames;
    int                priority;
    const char        *reason;
    int                ncarriers;
    int                ngatherers;
    int                nbuilders;
    /* Whether any good has carriers, or gatherers, of its own beside it */
    bool               good_carriers;
    bool               good_gatherers;
    /* The pool's workshop posts filled, out of those not kept empty; -1 for none */
    int                posts_inside;
    int                posts_open;
    int                posts_eating;
    const struct made *made[STRIP_MAX_MADE];
    size_t             nmade;
};

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
/* Seconds of play, for the draws' smoothing */
static uint32_t         s_now_s;
static khash_t(res)    *s_global_capacity_tables[MAX_FACTIONS];
static khash_t(flow)   *s_flows;
static khash_t(made)   *s_made;
static khash_t(note)   *s_notes;
static khash_t(gathered) *s_gathered;
/* The goods carried into and out of each district from and to the others, totalled the same way */
static khash_t(gathered) *s_imported;
static khash_t(gathered) *s_exported;
/* The second of the window that the records are filling now */
static int              s_made_slot;
/* Keyed by strip_key */
static khash_t(strip)  *s_strips;
static uint32_t         s_strip_stamp;
/* Per site: who is bringing it goods, tallied once per UI pass, and the
 * gatherers bringing it their next load, tallied once a second */

static struct nk_style_item s_bg_style = {0};
static struct nk_color      s_border_clr = {0};
static struct nk_color      s_font_clr = {0};
static bool                 s_show_ui = true;
/* Where the outline passes of the strip's text and icons are drawn */
static const float          s_outline_offsets[][2] = {
    {-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f},
    {-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f},
};

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
    kh_destroy(int, hs->held);
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
    hs->held = kh_init(int);

    if(!hs->capacity || !hs->curr || !hs->desired
    || !hs->alt_capacity || !hs->alt_desired
    || !hs->keep || !hs->chip || !hs->held) {

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
    hs->ndraws = 0;
    hs->route_s = ROUTE_DEFAULT_S;
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

static bool draw_has(const struct ss_draw *d, const char *rname)
{
    for(int j = 0; j < d->nmembers; j++) {
        if(!strcmp(d->members[j], rname))
            return true;
    }
    return false;
}

/* The draw the site set that the good is a member of */
static struct ss_draw *draw_of(struct ss_state *ss, uint32_t site, const char *rname)
{
    for(int i = 0; i < ss->ndraws; i++) {
        struct ss_draw *d = &ss->draws[i];
        if(d->site == site && draw_has(d, rname))
            return d;
    }
    return NULL;
}

static void draw_remove(struct ss_state *ss, int idx)
{
    ss->draws[idx] = ss->draws[--ss->ndraws];
}

/* The draw as it stands: one set lower than it was comes down to the rate
 * over a while, so that a lull does not let the stock worked from go */
static float draw_eff(const struct ss_draw *d)
{
    if(d->eff <= d->rate)
        return d->rate;
    float dt = (s_now_s > d->set_s) ? (float)(s_now_s - d->set_s) : 0.0f;
    return d->rate + (d->eff - d->rate) * expf(-dt / DRAW_SMOOTH_S);
}

static float draw_rate(struct ss_state *ss, const char *rname)
{
    float ret = 0.0f;
    for(int i = 0; i < ss->ndraws; i++) {
        if(draw_has(&ss->draws[i], rname)) {
            ret += draw_eff(&ss->draws[i]);
        }
    }
    return ret;
}

/* The draws the good is in, taken as one, so that a pool's sites ask for
 * theirs between them: the stock they are worked from, what they eat over a
 * load's trip and a minute more, and what all their goods come to now */
static int draw_working_of(struct ss_state *ss, const char *rname, int *out_stock)
{
    const char *members[MAX_DRAWS * MAX_DRAW_MEMBERS];
    int nmembers = 0;
    float eff = 0.0f;
    int batch = 0;
    int cap = 0;
    bool capped = true;

    for(int i = 0; i < ss->ndraws; i++) {
        const struct ss_draw *d = &ss->draws[i];
        if(!draw_has(d, rname))
            continue;
        eff += draw_eff(d);
        batch += d->batch;
        cap += d->cap;
        capped = capped && (d->cap > 0);
        for(int j = 0; j < d->nmembers; j++) {
            bool seen = false;
            for(int k = 0; k < nmembers; k++) {
                if(!strcmp(members[k], d->members[j])) {
                    seen = true;
                    break;
                }
            }
            if(!seen) {
                members[nmembers++] = d->members[j];
            }
        }
    }

    int stock = 0, room = 0;
    for(int k = 0; k < nmembers; k++) {
        int curr = 0, max = 0;
        ss_state_get_key(ss->curr, members[k], &curr);
        ss_state_get_key(ss->capacity, members[k], &max);
        stock += curr;
        room += max;
    }
    *out_stock = stock;
    if(nmembers == 0)
        return 0;
    int ret = (int)ceilf(eff * (ss->route_s + DRAW_LEAD_S) / 60.0f);
    ret = MAX(ret, batch);
    if(capped) {
        ret = MIN(ret, cap);
    }
    return MIN(ret, room);
}

/* How much of the good would fill the stock worked from, given the rest of it */
static int draw_reserve(struct ss_state *ss, const char *rname)
{
    int stock;
    int working = draw_working_of(ss, rname, &stock);
    if(working == 0)
        return 0;
    int curr = 0;
    ss_state_get_key(ss->curr, rname, &curr);
    return MAX(working - (stock - curr), 0);
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

/* How many hold a duty of the kind on the site, or on any site of its pool */
static int count_pool(uint32_t uid, enum duty_kind kind, const char *rname)
{
    int cid = G_Cluster_Of(uid);
    if(cid == 0)
        return G_Duty_CountAt(uid, kind, rname);

    uint32_t members[64];
    size_t nmembers = G_Cluster_Members(cid, ARR_SIZE(members), members);
    int ret = 0;
    for(int i = 0; i < nmembers; i++) {
        ret += G_Duty_CountAt(members[i], kind, rname);
    }
    return ret;
}

/* The suppliers of the site, or its pool, and the builders of one whose
 * materials have not reached it */
static int bound_for(uint32_t uid)
{
    int ret = count_pool(uid, DUTY_SUPPLY, NULL);
    if((G_FlagsGet(uid) & ENTITY_FLAG_BUILDING) && !G_Building_IsSupplied(uid)) {
        ret += count_pool(uid, DUTY_BUILD, NULL);
    }
    return ret;
}

/* Carriers on a trip to the site, or its pool, with the good in hand or on their way to fetch it */
static int bound_for_good(uint32_t uid, const char *rname)
{
    int cid = G_Cluster_Of(uid);
    if(cid == 0)
        return G_Harvester_CountBound(uid, rname);

    uint32_t members[64];
    size_t nmembers = G_Cluster_Members(cid, ARR_SIZE(members), members);
    int ret = 0;
    for(int i = 0; i < nmembers; i++) {
        ret += G_Harvester_CountBound(members[i], rname);
    }
    return ret;
}

/* Gatherers whose loads go to the site, or its pool; of every good for a NULL name */
static int gatherers_for(uint32_t uid, const char *rname)
{
    return count_pool(uid, DUTY_GATHER, rname);
}

/* A request the pool's own gatherers are meeting is not stuck */
static enum request_status good_status(uint32_t uid, const char *rname)
{
    int waited_ms;
    enum request_status ret = G_Automation_RequestStatus(uid, rname, &waited_ms);
    if(ret != REQUEST_OK && gatherers_for(uid, rname) > 0)
        return REQUEST_OK;
    return ret;
}

static float good_rate(uint32_t uid, const char *rname)
{
    khiter_t k = kh_get(strip, s_strips, strip_key(uid));
    if(k == kh_end(s_strips))
        return 0.0f;
    const struct strip *strip = &kh_value(s_strips, k);
    for(int i = 0; i < strip->ntrends; i++) {
        if(!strcmp(strip->trends[i].rname, rname))
            return strip->trends[i].rate;
    }
    return 0.0f;
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

static bool strip_shows(uint32_t uid, const char *rname, const struct trend *trend)
{
    if(G_StorageSite_GetCurr(uid, rname) > 0)
        return true;
    if(G_StorageSite_Request(uid, rname) > 0)
        return true;
    if(G_StorageSite_GetKeep(uid, rname) > 0)
        return true;
    return trend && (trend->arrow != 0 || trend->unchanged_s < RECENT_CHANGE_S);
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
                .unchanged_s = RECENT_CHANGE_S,
                .listed_stamp = 0
            };
            trends[i].listed_stamp = strip_shows(uid, names[i], &trends[i]) ? s_strip_stamp : 0;
            continue;
        }

        int delta = amount - old->last;
        float rate = old->rate + (delta - old->rate) * TREND_ALPHA;
        trends[i] = (struct trend){
            .rname = names[i],
            .last = amount,
            .rate = rate,
            .arrow = trend_arrow(old->arrow, rate),
            .unchanged_s = delta ? 0 : MIN(old->unchanged_s + 1, RECENT_CHANGE_S),
            .listed_stamp = old->listed_stamp
        };
        if(strip_shows(uid, names[i], &trends[i])) {
            trends[i].listed_stamp = s_strip_stamp;
        }
    }
    memcpy(strip->trends, trends, nnames * sizeof(struct trend));
    strip->ntrends = nnames;
}

static bool strip_lists(uint32_t uid, const char *rname, const struct trend *trend)
{
    if(strip_shows(uid, rname, trend))
        return true;
    return trend && trend->listed_stamp && (s_strip_stamp - trend->listed_stamp) <= STRIP_GRACE_S;
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

/* How far a reason is from the carriers' own business, so that a pool shows
 * the one the player has to see to first */
static int reason_rank(enum request_status status)
{
    switch(status) {
    case REQUEST_NOT_CONNECTED: return 6;
    case REQUEST_NO_SOURCE:     return 5;
    case REQUEST_UNREACHABLE:   return 4;
    case REQUEST_KEPT:          return 3;
    case REQUEST_NO_CARRIER:    return 2;
    case REQUEST_PROMISED:      return 1;
    default:                    return 0;
    }
}

static const char *pool_reason(uint32_t key, size_t nnames, const char *names[])
{
    enum request_status ret = REQUEST_OK;
    for(int i = 0; i < nnames; i++) {
        enum request_status status = good_status(key, names[i]);
        if(reason_rank(status) > reason_rank(ret)) {
            ret = status;
        }
    }
    return reason_code(ret);
}

/* The posts of the pool's workshops: those filled, those not kept empty, and
 * those whose holders are out at a meal */
static bool pool_posts(uint32_t key, int *out_inside, int *out_open, int *out_eating)
{
    int cid = G_Cluster_Of(key);
    uint32_t members[64];
    size_t nmembers = cid ? G_Cluster_Members(cid, ARR_SIZE(members), members) : 0;
    if(nmembers == 0) {
        members[nmembers++] = key;
    }
    bool ret = false;
    *out_inside = 0;
    *out_open = 0;
    *out_eating = 0;
    for(int i = 0; i < nmembers; i++) {
        int posts, locked, inside, coming;
        if(!G_Posts_Get(members[i], &posts, &locked, &inside, &coming))
            continue;
        *out_inside += inside;
        *out_open += posts - locked;
        *out_eating += G_Posts_Eating(members[i]);
        ret = true;
    }
    return ret;
}

/* What shows a holder out at a meal; a world without rations has none of its own */
static const char *meal_icon(void)
{
    const char *icon = G_Resource_GetIcon("Rations");
    return icon ? icon : POSTS_ICON;
}

static float text_width(const struct nk_user_font *font, const char *text)
{
    return font->width(font->userdata, font->height, text, strlen(text));
}

/* The font's outline is a pixel wide; the text is first drawn all in the
 * outline's colour a pixel off each way, which thickens it to two */
static void draw_strip_text(struct nk_command_buffer *canvas, const struct nk_user_font *font,
                            float x, float y, float w, const char *text, struct nk_color clr)
{
    for(int i = 0; i < ARR_SIZE(s_outline_offsets); i++) {
        const float *off = s_outline_offsets[i];
        nk_draw_text_outlined(canvas, nk_rect(x + off[0], y + off[1], w + 1, font->height), text,
            strlen(text), font, nk_rgba(0, 0, 0, 0), STRIP_OUTLINE_CLR, STRIP_OUTLINE_CLR);
    }
    nk_draw_text_outlined(canvas, nk_rect(x, y, w + 1, font->height), text, strlen(text),
        font, nk_rgba(0, 0, 0, 0), clr, STRIP_OUTLINE_CLR);
}

/* The icon over its own silhouette in the outline's colour, a pixel off each
 * way, so that it stands out from the ground */
static void draw_strip_icon(struct nk_command_buffer *canvas, float x, float y, const char *icon)
{
    for(int i = 0; i < ARR_SIZE(s_outline_offsets); i++) {
        const float *off = s_outline_offsets[i];
        nk_draw_texpath(canvas, nk_rect(x + off[0], y + off[1], STRIP_ICON_SZ, STRIP_ICON_SZ), icon,
            STRIP_OUTLINE_CLR);
    }
    nk_draw_texpath(canvas, nk_rect(x, y, STRIP_ICON_SZ, STRIP_ICON_SZ), icon, nk_rgba(255, 255, 255, 255));
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

static int compare_made(const void *a, const void *b)
{
    const struct made *ma = *(const struct made**)a;
    const struct made *mb = *(const struct made**)b;
    if(ma->total != mb->total)
        return mb->total - ma->total;
    return strcmp(ma->name, mb->name);
}

/* What the pool made in the last minute, the most made first */
static size_t made_sorted(uint32_t key, const struct made *out[], size_t maxout)
{
    khiter_t km = kh_get(made, s_made, strip_key(key));
    if(km == kh_end(s_made))
        return 0;

    const struct made *made[ARR_SIZE(((struct made_record*)NULL)->made)];
    size_t nmade = 0;
    const struct made_record *rec = &kh_value(s_made, km);
    for(int i = 0; i < rec->nmade; i++) {
        made[nmade++] = &rec->made[i];
    }
    qsort(made, nmade, sizeof(made[0]), compare_made);
    nmade = MIN(nmade, maxout);
    for(int i = 0; i < nmade; i++) {
        out[i] = made[i];
    }
    return nmade;
}

static float strip_good_width(const struct strip_good *good)
{
    return STRIP_ICON_SZ + 2 + good->count_w + 2 + TREND_W + STRIP_GAP;
}

/* Over a pool's anchor, rows of the goods it holds or wants, the last one
 * followed by the pool's priority, the carriers bound for it, and why a
 * request of it is stuck; under them, a row of what the pool made in the
 * last minute */
static void draw_strip(struct nk_command_buffer *canvas, const struct nk_user_font *font,
                       uint32_t key, vec2_t anchor)
{
    struct ss_state *ss = shown_state(key);
    if(!ss)
        return;

    khiter_t k = kh_get(strip, s_strips, strip_key(key));
    const struct strip *strip = (k != kh_end(s_strips)) ? &kh_value(s_strips, k) : NULL;

    const char *names[STRIP_MAX_GOODS];
    size_t nnames = ss_get_keys(ss, names, ARR_SIZE(names));

    struct strip_good goods[STRIP_MAX_GOODS];
    size_t ngoods = 0;
    const char *reason = pool_reason(key, nnames, names);

    for(int i = 0; i < nnames; i++) {

        enum request_status status = good_status(key, names[i]);

        const struct trend *trend = strip ? trend_find(strip, names[i]) : NULL;
        if(ngoods == STRIP_MAX_GOODS || !strip_lists(key, names[i], trend))
            continue;

        struct strip_good *good = &goods[ngoods++];
        good->icon = G_Resource_GetIcon(names[i]);
        pf_snprintf(good->count, sizeof(good->count), "%d", G_StorageSite_GetCurr(key, names[i]));
        good->count_w = text_width(font, good->count);
        good->arrow = trend ? trend->arrow : 0;
        good->blocked = (status != REQUEST_OK);
    }

    /* The goods wrap into rows; the last row also carries the pool's marks */
    float rows_w = 0.0f, width = 0.0f;
    for(int i = 0; i < ngoods; i++) {
        width += strip_good_width(&goods[i]);
        if((i + 1) % STRIP_ROW_GOODS == 0 && i + 1 < ngoods) {
            rows_w = MAX(rows_w, width - STRIP_GAP);
            width = 0.0f;
        }
    }

    int priority = ss->priority;
    if(priority != SS_PRIORITY_NORMAL) {
        width += PENNANT_W + STRIP_GAP;
    }

    /* The carriers and the gatherers share a row */
    char carriers[8] = {0};
    float carriers_w = 0.0f, carriers_row_w = 0.0f;
    int ncarriers = bound_for(key);
    if(ncarriers > 0) {
        pf_snprintf(carriers, sizeof(carriers), "%d", ncarriers);
        carriers_w = text_width(font, carriers);
        carriers_row_w = STRIP_ICON_SZ + 2 + carriers_w;
    }

    char gatherers[8] = {0};
    float gatherers_w = 0.0f;
    int ngatherers = gatherers_for(key, NULL);
    if(ngatherers > 0) {
        pf_snprintf(gatherers, sizeof(gatherers), "%d", ngatherers);
        gatherers_w = text_width(font, gatherers);
        carriers_row_w += (ncarriers > 0 ? STRIP_GAP : 0.0f) + STRIP_ICON_SZ + 2 + gatherers_w;
    }

    float reason_w = reason ? text_width(font, reason) : 0.0f;
    width += reason_w;
    if(width > 0.0f && !reason)
        width -= STRIP_GAP;
    width = MAX(width, rows_w);
    const bool has_marks = (width > 0.0f);
    width = MAX(width, carriers_row_w);

    char posts[16] = {0};
    float posts_w = 0.0f;
    char eating[16] = {0};
    float eating_w = 0.0f;
    int posts_inside, posts_open, posts_eating;
    if(pool_posts(key, &posts_inside, &posts_open, &posts_eating)) {
        pf_snprintf(posts, sizeof(posts), posts_eating ? "%d/%d (" : "%d/%d", posts_inside, posts_open);
        posts_w = text_width(font, posts);
        if(posts_eating) {
            pf_snprintf(eating, sizeof(eating), "%d)", posts_eating);
            eating_w = STRIP_ICON_SZ + 2 + text_width(font, eating);
        }
        width = MAX(width, STRIP_ICON_SZ + 2 + posts_w + eating_w);
    }

    khiter_t kn = kh_get(note, s_notes, key);
    const struct strip_note *note = (kn != kh_end(s_notes)) ? &kh_value(s_notes, kn) : NULL;
    float note_w = 0.0f;
    float note_after_w = 0.0f;
    if(note) {
        note_w = text_width(font, note->text);
        if(note->after_icon) {
            note_after_w = text_width(font, note->after_text);
        }
        width = MAX(width, (note->icon ? STRIP_ICON_SZ + 2 : 0.0f) + note_w
            + (note->after_icon ? STRIP_GAP + STRIP_ICON_SZ + 2 + note_after_w : 0.0f));
    }

    /* The second row: what the pool made in the last minute, most first */
    const struct made *made[STRIP_MAX_MADE];
    size_t nmade = made_sorted(key, made, ARR_SIZE(made));

    char rates[STRIP_MAX_MADE][16];
    float rates_w[STRIP_MAX_MADE];
    float made_width = 0.0f;
    for(int i = 0; i < nmade; i++) {
        pf_snprintf(rates[i], sizeof(rates[i]), "%d/min", made[i]->total);
        rates_w[i] = text_width(font, rates[i]);
        made_width += STRIP_ICON_SZ + 2 + rates_w[i] + STRIP_GAP;
    }
    if(nmade > 0)
        made_width -= STRIP_GAP;

    if(width == 0.0f && made_width == 0.0f)
        return;

    /* The rows start together, centred as a block over the anchor */
    const float left = anchor.x - MAX(width, made_width) / 2.0f;
    float x = left;
    const float y = anchor.y + STRIP_DROP;
    const float text_y = y + (STRIP_ICON_SZ - font->height) / 2.0f;
    const float cy = y + STRIP_ICON_SZ / 2.0f;

    const float row_h = STRIP_ICON_SZ + 2;
    float row_y = y;
    for(int i = 0; i < ngoods; i++) {

        if(i > 0 && i % STRIP_ROW_GOODS == 0) {
            x = left;
            row_y += row_h;
        }
        const struct strip_good *good = &goods[i];
        if(good->icon) {
            draw_strip_icon(canvas, x, row_y, good->icon);
        }
        x += STRIP_ICON_SZ + 2;
        draw_strip_text(canvas, font, x, text_y + (row_y - y), good->count_w, good->count,
            good->blocked ? STRIP_WARN_CLR : STRIP_TEXT_CLR);
        x += good->count_w + 2;
        draw_trend(canvas, x, cy + (row_y - y), good->arrow);
        x += TREND_W + STRIP_GAP;
    }
    const float marks_y = row_y;

    if(priority != SS_PRIORITY_NORMAL) {
        draw_pennant(canvas, x, marks_y, priority);
        x += PENNANT_W + STRIP_GAP;
    }

    if(reason) {
        draw_strip_text(canvas, font, x, text_y + (marks_y - y), reason_w, reason, STRIP_WARN_CLR);
    }

    float carriers_y = has_marks ? marks_y + row_h : y;
    x = left;
    if(ncarriers > 0) {
        draw_strip_icon(canvas, x, carriers_y, CARRIERS_ICON);
        draw_strip_text(canvas, font, x + STRIP_ICON_SZ + 2, carriers_y + (STRIP_ICON_SZ - font->height) / 2.0f,
            carriers_w, carriers, STRIP_TEXT_CLR);
        x += STRIP_ICON_SZ + 2 + carriers_w + STRIP_GAP;
    }
    if(ngatherers > 0) {
        draw_strip_icon(canvas, x, carriers_y, GATHERERS_ICON);
        draw_strip_text(canvas, font, x + STRIP_ICON_SZ + 2, carriers_y + (STRIP_ICON_SZ - font->height) / 2.0f,
            gatherers_w, gatherers, STRIP_TEXT_CLR);
    }
    if(ncarriers > 0 || ngatherers > 0) {
        carriers_y += row_h;
    }
    if(posts[0]) {
        draw_strip_icon(canvas, left, carriers_y, POSTS_ICON);
        draw_strip_text(canvas, font, left + STRIP_ICON_SZ + 2, carriers_y + (STRIP_ICON_SZ - font->height) / 2.0f,
            posts_w, posts, STRIP_TEXT_CLR);
        if(eating[0]) {
            x = left + STRIP_ICON_SZ + 2 + posts_w;
            draw_strip_icon(canvas, x, carriers_y, meal_icon());
            draw_strip_text(canvas, font, x + STRIP_ICON_SZ + 2, carriers_y + (STRIP_ICON_SZ - font->height) / 2.0f,
                eating_w - STRIP_ICON_SZ - 2, eating, STRIP_TEXT_CLR);
        }
        carriers_y += row_h;
    }

    if(note) {
        x = left;
        if(note->icon) {
            draw_strip_icon(canvas, x, carriers_y, note->icon);
            x += STRIP_ICON_SZ + 2;
        }
        draw_strip_text(canvas, font, x, carriers_y + (STRIP_ICON_SZ - font->height) / 2.0f,
            note_w, note->text, note->warn ? STRIP_WARN_CLR : STRIP_TEXT_CLR);
        if(note->after_icon) {
            x += note_w + STRIP_GAP;
            draw_strip_icon(canvas, x, carriers_y, note->after_icon);
            draw_strip_text(canvas, font, x + STRIP_ICON_SZ + 2, carriers_y + (STRIP_ICON_SZ - font->height) / 2.0f,
                note_after_w, note->after_text, note->warn ? STRIP_WARN_CLR : STRIP_TEXT_CLR);
        }
        carriers_y += row_h;
    }

    x = left;
    const float made_y = carriers_y;
    for(int i = 0; i < nmade; i++) {
        if(made[i]->icon) {
            draw_strip_icon(canvas, x, made_y, made[i]->icon);
        }
        x += STRIP_ICON_SZ + 2;
        draw_strip_text(canvas, font, x, made_y + (STRIP_ICON_SZ - font->height) / 2.0f,
            rates_w[i], rates[i], STRIP_TEXT_CLR);
        x += rates_w[i] + STRIP_GAP;
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
            if(!G_OverlayShownAt(anchor.x))
                continue;
            /* Under the building's garrison count, if it shows one */
            float garrison = G_Garrison_UIBottom(keys[i]);
            if(garrison > 0.0f) {
                anchor.y += garrison - STRIP_DROP + STRIP_GAP_Y;
            }
            draw_strip(canvas, ctx->style.font, keys[i], anchor);
        }
        UI_SetActiveFont(prev);
    }
    nk_end(ctx);

    nk_style_pop_color(ctx);
    nk_style_pop_style_item(ctx);
}

/* The cells of a good's row, a carriers' or gatherers' pair left out at
 * width 0 when no good has any; the content is at least as wide as the row
 * of totals */
static float window_columns(const struct nk_user_font *font, const struct window_plan *plan,
                            float out[WIN_COLS])
{
    const float spacing = UI_GetContext()->style.window.spacing.x;
    const float num_w = text_width(font, "8888");
    const float brace_w = text_width(font, "(") + 2;
    const float count_w = text_width(font, "88");
    const float carriers_w = plan->good_carriers ? count_w : 0.0f;
    const float gatherers_w = plan->good_gatherers ? count_w : 0.0f;
    const float cols[WIN_COLS] = {
        WIN_ICON_SZ, num_w, text_width(font, "/") + 2, num_w, brace_w, text_width(font, "888"),
        brace_w, WIN_ARROW_W, carriers_w ? WIN_ICON_SZ : 0.0f, carriers_w,
        gatherers_w ? WIN_ICON_SZ : 0.0f, gatherers_w
    };
    float ret = -spacing;
    for(int i = 0; i < WIN_COLS; i++) {
        out[i] = cols[i];
        if(cols[i] > 0.0f) {
            ret += cols[i] + spacing;
        }
    }

    float totals_w = -spacing;
    if(plan->ncarriers > 0)
        totals_w += WIN_ICON_SZ + num_w + 2 * spacing;
    if(plan->ngatherers > 0)
        totals_w += WIN_ICON_SZ + num_w + 2 * spacing;
    if(plan->posts_inside >= 0)
        totals_w += 2 * WIN_ICON_SZ + 2 * cols[1] + cols[2] + cols[3] + 4 * spacing;
    return MAX(ret, totals_w);
}

static int good_arrow(uint32_t uid, const char *rname)
{
    khiter_t k = kh_get(strip, s_strips, strip_key(uid));
    if(k == kh_end(s_strips))
        return 0;
    const struct trend *trend = trend_find(&kh_value(s_strips, k), rname);
    return trend ? trend->arrow : 0;
}

static bool window_lists(uint32_t key, const char *rname)
{
    khiter_t k = kh_get(strip, s_strips, strip_key(key));
    const struct trend *trend = (k != kh_end(s_strips)) ? trend_find(&kh_value(s_strips, k), rname) : NULL;
    if(strip_lists(key, rname, trend))
        return true;
    return bound_for_good(key, rname) > 0 || gatherers_for(key, rname) > 0;
}

/* The goods the site holds, wants or is brought; all of its goods when it has none of those */
static void plan_window(uint32_t key, struct ss_state *ss, struct window_plan *out)
{
    const char *names[ARR_SIZE(out->names)];
    size_t nnames = ss_get_keys(ss, names, ARR_SIZE(names));
    out->nnames = 0;
    for(int i = 0; i < nnames; i++) {
        if(window_lists(key, names[i])) {
            out->names[out->nnames++] = names[i];
        }
    }
    if(out->nnames == 0) {
        memcpy(out->names, names, nnames * sizeof(names[0]));
        out->nnames = nnames;
    }
    out->priority = ss->priority;
    out->reason = pool_reason(key, out->nnames, out->names);
    out->ncarriers = bound_for(key);
    out->nbuilders = count_pool(key, DUTY_BUILD, NULL);
    out->ngatherers = gatherers_for(key, NULL);
    out->good_carriers = false;
    out->good_gatherers = false;
    for(int i = 0; i < out->nnames; i++) {
        out->good_carriers |= (bound_for_good(key, out->names[i]) > 0);
        out->good_gatherers |= (gatherers_for(key, out->names[i]) > 0);
    }
    if(!pool_posts(key, &out->posts_inside, &out->posts_open, &out->posts_eating)) {
        out->posts_inside = -1;
    }
    out->nmade = made_sorted(key, out->made, ARR_SIZE(out->made));
}

static float window_height(const struct window_plan *plan)
{
    const float spacing = UI_GetContext()->style.window.spacing.y;
    float ret = 2 * WIN_PAD_Y + plan->nnames * (WIN_ROW_H + spacing);
    if(plan->priority != SS_PRIORITY_NORMAL || plan->reason) {
        ret += STRIP_ICON_SZ + spacing;
    }
    if(plan->ncarriers > 0 || plan->ngatherers > 0 || plan->posts_inside >= 0) {
        ret += WIN_ROW_H + spacing;
    }
    ret += ((plan->nmade + WIN_MADE_PER_ROW - 1) / WIN_MADE_PER_ROW) * (WIN_ROW_H + spacing);
    return ret;
}

static void window_icon(struct nk_context *ctx, const char *icon)
{
    if(icon) {
        nk_image_texpath(ctx, icon);
    }else{
        nk_label_colored(ctx, "?", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
    }
}

/* A row of the window for each good: its stock out of its room, the keep in
 * braces, which way it is going and the carriers and gatherers bringing it;
 * then the rows the strip has: the priority and a stuck request, the carriers
 * and gatherers bound for it, and what it made in the last minute */
static void draw_window(struct nk_context *ctx, uint32_t key, const struct ss_state *ss,
                        const struct window_plan *plan, const float cols[WIN_COLS], float content_w)
{
    const float spacing = ctx->style.window.spacing.x;
    int ncols = 0;
    for(int i = 0; i < WIN_COLS; i++) {
        ncols += (cols[i] > 0.0f);
    }

    for(int i = 0; i < plan->nnames; i++) {

        const char *rname = plan->names[i];
        int capacity = ss->use_alt ? G_StorageSite_GetAltCapacity(key, rname)
                                   : G_StorageSite_GetCapacity(key, rname);
        int reserve = G_StorageSite_GetReserve(key, rname);
        int curr_amount = G_StorageSite_GetCurr(key, rname);
        int ncarts = bound_for_good(key, rname);
        int ngather = gatherers_for(key, rname);
        int arrow = good_arrow(key, rname);
        bool blocked = (good_status(key, rname) != REQUEST_OK);

        char amount[8], cap[8], keep[8], carts[8] = {0}, gather[8] = {0};
        pf_snprintf(amount, sizeof(amount), "%d", curr_amount);
        pf_snprintf(cap, sizeof(cap), "%d", capacity);
        pf_snprintf(keep, sizeof(keep), "%d", reserve);
        if(ncarts > 0) {
            pf_snprintf(carts, sizeof(carts), "%d", ncarts);
        }
        if(ngather > 0) {
            pf_snprintf(gather, sizeof(gather), "%d", ngather);
        }

        nk_layout_row_begin(ctx, NK_STATIC, WIN_ROW_H, ncols);
        nk_layout_row_push(ctx, cols[0]);
        window_icon(ctx, G_Resource_GetIcon(rname));

        nk_layout_row_push(ctx, cols[1]);
        nk_label_colored(ctx, amount, NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE,
            blocked ? RED_CLR : (curr_amount > 0) ? GREEN_CLR : s_font_clr);

        nk_layout_row_push(ctx, cols[2]);
        nk_label_colored(ctx, "/", NK_TEXT_CENTERED, s_font_clr);

        nk_layout_row_push(ctx, cols[3]);
        nk_label_colored(ctx, cap, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

        /* The braces keep their columns whatever the width of the number between */
        nk_layout_row_push(ctx, cols[4]);
        nk_label_colored(ctx, "(", NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

        nk_layout_row_push(ctx, cols[5]);
        nk_label_colored(ctx, keep, NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

        nk_layout_row_push(ctx, cols[6]);
        nk_label_colored(ctx, ")", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);

        nk_layout_row_push(ctx, cols[7]);
        if(arrow != 0) {
            nk_image_texpath(ctx, (arrow > 0) ? TREND_UP_ICON : TREND_DOWN_ICON);
        }else{
            nk_label_colored(ctx, "", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
        }

        if(plan->good_carriers) {
            nk_layout_row_push(ctx, cols[8]);
            if(ncarts > 0) {
                nk_image_texpath(ctx, CARRIERS_ICON);
            }else{
                nk_label_colored(ctx, "", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
            }
            nk_layout_row_push(ctx, cols[9]);
            nk_label_colored(ctx, carts, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
        }

        if(plan->good_gatherers) {
            nk_layout_row_push(ctx, cols[10]);
            if(ngather > 0) {
                nk_image_texpath(ctx, GATHERERS_ICON);
            }else{
                nk_label_colored(ctx, "", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
            }
            nk_layout_row_push(ctx, cols[11]);
            nk_label_colored(ctx, gather, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
        }
        nk_layout_row_end(ctx);
    }

    if(plan->priority != SS_PRIORITY_NORMAL || plan->reason) {

        nk_layout_row_begin(ctx, NK_STATIC, STRIP_ICON_SZ, 2);
        nk_layout_row_push(ctx, PENNANT_W + 4);
        struct nk_rect bounds;
        if(nk_widget(&bounds, ctx) && plan->priority != SS_PRIORITY_NORMAL) {
            draw_pennant(nk_window_get_canvas(ctx), bounds.x + 2, bounds.y, plan->priority);
        }
        nk_layout_row_push(ctx, content_w - PENNANT_W - 4 - spacing);
        nk_label_colored(ctx, plan->reason ? plan->reason : "", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, RED_CLR);
        nk_layout_row_end(ctx);
    }

    if(plan->ncarriers > 0 || plan->ngatherers > 0 || plan->nbuilders > 0 || plan->posts_inside >= 0) {

        char carriers[8], gatherers[8], builders[8], posts[16], eating[16];
        pf_snprintf(carriers, sizeof(carriers), "%d", plan->ncarriers);
        pf_snprintf(gatherers, sizeof(gatherers), "%d", plan->ngatherers);
        pf_snprintf(builders, sizeof(builders), "%d", plan->nbuilders);
        pf_snprintf(posts, sizeof(posts), plan->posts_eating ? "%d/%d (" : "%d/%d", plan->posts_inside, plan->posts_open);
        pf_snprintf(eating, sizeof(eating), "%d)", plan->posts_eating);
        const bool show_eating = plan->posts_inside >= 0 && plan->posts_eating > 0;

        nk_layout_row_begin(ctx, NK_STATIC, WIN_ROW_H, (plan->ncarriers > 0 ? 2 : 0) + (plan->ngatherers > 0 ? 2 : 0)
            + (plan->nbuilders > 0 ? 2 : 0) + (plan->posts_inside >= 0 ? 2 : 0) + (show_eating ? 2 : 0));
        if(plan->ncarriers > 0) {
            nk_layout_row_push(ctx, WIN_ICON_SZ);
            nk_image_texpath(ctx, CARRIERS_ICON);
            nk_layout_row_push(ctx, cols[1]);
            nk_label_colored(ctx, carriers, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
        }
        if(plan->nbuilders > 0) {
            nk_layout_row_push(ctx, WIN_ICON_SZ);
            nk_image_texpath(ctx, BUILDERS_ICON);
            nk_layout_row_push(ctx, cols[1]);
            nk_label_colored(ctx, builders, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
        }
        if(plan->ngatherers > 0) {
            nk_layout_row_push(ctx, WIN_ICON_SZ);
            nk_image_texpath(ctx, GATHERERS_ICON);
            nk_layout_row_push(ctx, cols[1]);
            nk_label_colored(ctx, gatherers, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
        }
        if(plan->posts_inside >= 0) {
            nk_layout_row_push(ctx, WIN_ICON_SZ);
            nk_image_texpath(ctx, POSTS_ICON);
            nk_layout_row_push(ctx, cols[1] + cols[2] + cols[3]);
            nk_label_colored(ctx, posts, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
            if(show_eating) {
                nk_layout_row_push(ctx, WIN_ICON_SZ);
                nk_image_texpath(ctx, meal_icon());
                nk_layout_row_push(ctx, cols[1]);
                nk_label_colored(ctx, eating, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
            }
        }
        nk_layout_row_end(ctx);
    }

    for(int i = 0; i < plan->nmade; i += WIN_MADE_PER_ROW) {

        size_t n = MIN(plan->nmade - i, WIN_MADE_PER_ROW);
        nk_layout_row_begin(ctx, NK_STATIC, WIN_ROW_H, 2 * n);
        for(int j = i; j < i + n; j++) {
            char made[16];
            pf_snprintf(made, sizeof(made), "%d/min", plan->made[j]->total);
            nk_layout_row_push(ctx, WIN_ICON_SZ);
            window_icon(ctx, plan->made[j]->icon);
            nk_layout_row_push(ctx, (content_w - WIN_MADE_PER_ROW * (WIN_ICON_SZ + spacing)) / WIN_MADE_PER_ROW - spacing);
            nk_label_colored(ctx, made, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
        }
        nk_layout_row_end(ctx);
    }
}

static void on_update_ui(void *user, void *event)
{
    if(!s_show_ui)
        return;

    struct sval ui_setting;
    ss_e status = Settings_Get("pf.game.storage_site_ui_mode", &ui_setting);
    assert(status == SS_OKAY);

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
    nk_style_push_vec2(ctx, &ctx->style.window.padding, nk_vec2(WIN_PAD_X, WIN_PAD_Y));

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

        bool full = (ui_setting.as_int == SS_UI_SHOW_SELECTED)
                 && (pool_selected(key, pool_id)
                     || (hover_setting.as_bool && pool_hovered(key, pool_id)));
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
        if(!G_OverlayShownAt(ss_pos.x))
            continue;
        float garrison = G_Garrison_UIBottom(key);
        if(garrison > 0.0f) {
            ss_pos.y += garrison - STRIP_DROP + STRIP_GAP_Y;
        }
        struct window_plan plan;
        plan_window(key, &curr, &plan);
        if(plan.nnames == 0)
            continue;

        float cols[WIN_COLS];
        const float content_w = window_columns(ctx->style.font, &plan, cols);
        /* The border is drawn inside the window and takes its width off the rows */
        const int width = ceilf(content_w + 2 * (WIN_PAD_X + ctx->style.window.border) + 2);
        const int height = ceilf(window_height(&plan));
        const vec2_t pos = (vec2_t){ss_pos.x - width/2, ss_pos.y + STRIP_DROP};
        const int flags = NK_WINDOW_NOT_INTERACTIVE | NK_WINDOW_BORDER | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR;

        struct rect adj_bounds = UI_BoundsForAspectRatio(
            (struct rect){pos.x, pos.y, width, height},
            vres, adj_vres, ANCHOR_DEFAULT
        );

        if(nk_begin_with_vres(ctx, name,
            (struct nk_rect){adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h},
            flags, (struct nk_vec2i){adj_vres.x, adj_vres.y})) {

            draw_window(ctx, key, &curr, &plan, cols, content_w);
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

/* Moves every record on by a second, forgetting the second a minute ago */
static void advance_made(void)
{
    s_made_slot = (s_made_slot + 1) % MADE_WINDOW_S;

    for(khiter_t k = kh_begin(s_made); k != kh_end(s_made); k++) {
        if(!kh_exist(s_made, k))
            continue;
        struct made_record *rec = &kh_value(s_made, k);
        size_t left = 0;
        for(int i = 0; i < rec->nmade; i++) {
            struct made *m = &rec->made[i];
            m->total -= m->per_s[s_made_slot];
            m->per_s[s_made_slot] = 0;
            if(m->total > 0) {
                rec->made[left++] = *m;
            }
        }
        rec->nmade = left;
        if(left == 0) {
            kh_del(made, s_made, k);
        }
    }
}

static void on_1hz_tick(void *user, void *event)
{
    uint32_t key;
    struct ss_state curr;
    (void)curr;

    s_now_s++;
    advance_made();
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
    if(val->as_int < SS_UI_SHOW_SELECTED || val->as_int > SS_UI_SHOW_NEVER)
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
    if(!G_StorageSite_GetUseAlt(uid)) {
        update_res_delta(rname, -amount, faction);
    }

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

static bool save_draws(const struct ss_state *ss, SDL_RWops *stream)
{
    struct attr num = (struct attr){
        .type = TYPE_INT,
        .val.as_int = ss->ndraws
    };
    CHK_TRUE_RET(Attr_Write(stream, &num, "num_draws"));

    for(int i = 0; i < ss->ndraws; i++) {

        const struct ss_draw *d = &ss->draws[i];
        struct attr site = (struct attr){
            .type = TYPE_INT,
            .val.as_int = d->site
        };
        CHK_TRUE_RET(Attr_Write(stream, &site, "draw_site"));

        struct attr nmembers = (struct attr){
            .type = TYPE_INT,
            .val.as_int = d->nmembers
        };
        CHK_TRUE_RET(Attr_Write(stream, &nmembers, "draw_members"));

        for(int j = 0; j < d->nmembers; j++) {
            struct attr member = (struct attr){ .type = TYPE_STRING, };
            pf_strlcpy(member.val.as_string, d->members[j], sizeof(member.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &member, "draw_member"));
        }

        struct attr rate = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = d->rate
        };
        CHK_TRUE_RET(Attr_Write(stream, &rate, "draw_rate"));

        struct attr eff = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = d->eff
        };
        CHK_TRUE_RET(Attr_Write(stream, &eff, "draw_eff"));

        struct attr batch = (struct attr){
            .type = TYPE_INT,
            .val.as_int = d->batch
        };
        CHK_TRUE_RET(Attr_Write(stream, &batch, "draw_batch"));

        struct attr cap = (struct attr){
            .type = TYPE_INT,
            .val.as_int = d->cap
        };
        CHK_TRUE_RET(Attr_Write(stream, &cap, "draw_cap"));
    }

    struct attr route = (struct attr){
        .type = TYPE_FLOAT,
        .val.as_float = ss->route_s
    };
    CHK_TRUE_RET(Attr_Write(stream, &route, "route_s"));
    return true;
}

static bool load_draws(struct ss_state *ss, SDL_RWops *stream)
{
    struct attr attr;
    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num = attr.val.as_int;
    ss->ndraws = 0;

    for(int i = 0; i < num; i++) {

        struct ss_draw d = {0};
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        d.site = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const int nmembers = attr.val.as_int;

        for(int j = 0; j < nmembers; j++) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_STRING);
            const char *key = si_intern(attr.val.as_string, &s_stringpool, s_stridx);
            CHK_TRUE_RET(key);
            if(d.nmembers < MAX_DRAW_MEMBERS) {
                d.members[d.nmembers++] = key;
            }
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        d.rate = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        d.eff = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        d.batch = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        d.cap = attr.val.as_int;

        if(ss->ndraws < MAX_DRAWS) {
            ss->draws[ss->ndraws++] = d;
        }
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_FLOAT);
    ss->route_s = attr.val.as_float;
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

    if(!(s_made = kh_init(made)))
        goto fail_made;
    s_made_slot = 0;

    if(!(s_gathered = kh_init(gathered)))
        goto fail_gathered;
    if(!(s_imported = kh_init(gathered)))
        goto fail_imported;
    if(!(s_exported = kh_init(gathered)))
        goto fail_exported;

    if(!(s_notes = kh_init(note)))
        goto fail_notes;

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
            .as_int = SS_UI_SHOW_SELECTED
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

fail_notes:
    kh_destroy(gathered, s_exported);
fail_exported:
    kh_destroy(gathered, s_imported);
fail_imported:
    kh_destroy(gathered, s_gathered);
fail_gathered:
    kh_destroy(made, s_made);
fail_made:
    kh_destroy(strip, s_strips);
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

    kh_destroy(note, s_notes);
    kh_destroy(gathered, s_exported);
    kh_destroy(gathered, s_imported);
    kh_destroy(gathered, s_gathered);
    kh_destroy(made, s_made);
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
    G_StorageSite_SetNote(uid, NULL, NULL, false, NULL, NULL);

    struct ss_state *ss = ss_state_get(uid);
    if(!ss)
        return;

    if(ss->pool) {
        G_StorageSite_PoolLeave(uid, ss->pool, false);
    }

    const char *key;
    int amount;

    kh_foreach(ss->curr, key, amount, {
        if(!ss->use_alt) {
            update_res_delta(key, -amount, G_GetFactionID(uid));
        }
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

    /* Its draws go with it, to be asked for between the pool's sites */
    for(int i = 0; i < ss->ndraws && pool->ndraws < MAX_DRAWS; i++) {
        pool->draws[pool->ndraws++] = ss->draws[i];
    }
    ss->ndraws = 0;

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
    kh_foreach(ss->held, key, amount, {
        int prev = 0;
        ss_state_get_key(pool->held, key, &prev);
        ss_state_set_key(pool->held, key, MAX(prev, amount));
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

    for(int i = 0; i < pool->ndraws;) {
        if(pool->draws[i].site == uid) {
            draw_remove(pool, i);
        }else{
            i++;
        }
    }

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

void G_StorageSite_SetHeld(uint32_t uid, const char *rname, int held)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    ss_state_set_key(ss->held, rname, MAX(held, 0));
}

int G_StorageSite_GetHeld(uint32_t uid, const char *rname)
{
    int ret = 0;
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    ss_state_get_key(ss->held, rname, &ret);
    return ret;
}

void G_StorageSite_SetDraw(uint32_t uid, size_t nmembers, const char **members, float per_min,
                           int batch, int cap)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    nmembers = MIN(nmembers, MAX_DRAW_MEMBERS);

    struct ss_draw *d = NULL;
    for(size_t j = 0; j < nmembers && !d; j++) {
        d = draw_of(ss, uid, members[j]);
    }
    if(per_min <= 0.0f || nmembers == 0) {
        if(d) {
            draw_remove(ss, d - ss->draws);
        }
        return;
    }
    if(!d) {
        if(ss->ndraws == MAX_DRAWS)
            return;
        d = &ss->draws[ss->ndraws++];
        *d = (struct ss_draw){ .site = uid, .eff = per_min };
    }else{
        /* One set higher takes at once; the rest of the way down goes on from here */
        d->eff = MAX(draw_eff(d), per_min);
    }
    d->rate = per_min;
    d->batch = MAX(batch, 0);
    d->cap = MAX(cap, 0);
    d->set_s = s_now_s;
    d->nmembers = 0;
    for(size_t j = 0; j < nmembers; j++) {
        const char *key = si_intern(members[j], &s_stringpool, s_stridx);
        if(key) {
            d->members[d->nmembers++] = key;
        }
    }
}

void G_StorageSite_ClearDraw(uint32_t uid, const char *rname)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    struct ss_draw *d = draw_of(ss, uid, rname);
    if(d) {
        draw_remove(ss, d - ss->draws);
    }
}

float G_StorageSite_GetDraw(uint32_t uid, const char *rname)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    return draw_rate(ss, rname);
}

int G_StorageSite_WorkingStock(uint32_t uid, const char *rname)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    int stock;
    return draw_working_of(ss, rname, &stock);
}

int G_StorageSite_LowMark(uint32_t uid, const char *rname)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    return (int)ceilf(draw_rate(ss, rname) * LOW_MARK_S / 60.0f);
}

float G_StorageSite_MinutesOfStock(uint32_t uid, const char *rname)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    float rate = draw_rate(ss, rname);
    if(rate <= 0.0f)
        return INFINITY;
    int stock;
    draw_working_of(ss, rname, &stock);
    return stock / rate;
}

size_t G_StorageSite_DrawMembers(uint32_t uid, const char *rname, size_t maxout, const char **out)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    size_t ret = 0;
    for(int i = 0; i < ss->ndraws; i++) {
        const struct ss_draw *d = &ss->draws[i];
        if(!draw_has(d, rname))
            continue;
        for(int j = 0; j < d->nmembers; j++) {
            bool seen = false;
            for(size_t k = 0; k < ret; k++) {
                if(!strcmp(out[k], d->members[j])) {
                    seen = true;
                    break;
                }
            }
            if(!seen && ret < maxout) {
                out[ret++] = d->members[j];
            }
        }
    }
    return ret;
}

uint32_t G_StorageSite_NowS(void)
{
    return s_now_s;
}

void G_StorageSite_NoteRoute(uint32_t uid, float trip_s)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    ss->route_s += (trip_s - ss->route_s) * ROUTE_SMOOTH;
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
    desired = MAX(desired, draw_reserve(ss, rname));

    switch(G_StorageSite_GetChip(uid, rname)) {
    case SS_CHIP_COLLECT:
    case SS_CHIP_RELAY:
        return MAX(desired, G_StorageSite_GetKeep(uid, rname) + G_StorageSite_GetHeld(uid, rname));
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
    /* What the site works from stays, like what it was told to keep */
    int working = draw_reserve(ss, rname);

    int held = G_StorageSite_GetHeld(uid, rname);
    switch(G_StorageSite_GetChip(uid, rname)) {
    case SS_CHIP_HOLD:
        return 0;
    case SS_CHIP_OFF:
        return curr;
    case SS_CHIP_RELAY:
        return MAX(curr - MAX(desired + held, working), 0);
    default:
        return MAX(curr - MAX(MAX(desired, working), G_StorageSite_GetKeep(uid, rname) + held), 0);
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
    desired = MAX(desired, draw_reserve(ss, rname));
    return MAX(desired, G_StorageSite_GetKeep(uid, rname) + G_StorageSite_GetHeld(uid, rname));
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
        return MAX(curr - MAX(G_StorageSite_GetKeep(uid, rname), draw_reserve(ss, rname)), 0);
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
    /* What is brought to a building being raised is spent: the realm's count leaves it out */
    if(!ss->use_alt) {
        update_res_delta(rname, delta, G_GetFactionID(uid));
    }

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

int G_StorageSite_GetFactionCapacity(int faction, const char *rname)
{
    if(faction < 0 || faction >= MAX_FACTIONS)
        return 0;
    khiter_t k = kh_get(res, s_global_capacity_tables[faction], rname);
    if(k == kh_end(s_global_capacity_tables[faction]))
        return 0;
    return kh_value(s_global_capacity_tables[faction], k);
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

static void add_total(khash_t(gathered) *table, int district, const char *rname, int amount)
{
    if(amount <= 0 || !district)
        return;

    int status;
    khiter_t k = kh_put(gathered, table, district, &status);
    if(status == -1)
        return;
    struct gathered_record *rec = &kh_value(table, k);
    if(status != 0) {
        rec->ngoods = 0;
    }

    const char *key = si_intern(rname, &s_stringpool, s_stridx);
    if(!key)
        return;
    for(int i = 0; i < rec->ngoods; i++) {
        if(rec->goods[i].name == key) {
            rec->goods[i].total += amount;
            return;
        }
    }
    if(rec->ngoods == ARR_SIZE(rec->goods))
        return;
    rec->goods[rec->ngoods++] = (struct gathered){key, amount};
}

static size_t totals_of(khash_t(gathered) *table, int district, const char **out_names, int *out_totals,
                        size_t maxout)
{
    khiter_t k = kh_get(gathered, table, district);
    if(k == kh_end(table))
        return 0;
    const struct gathered_record *rec = &kh_value(table, k);
    size_t ret = MIN(rec->ngoods, maxout);
    for(int i = 0; i < ret; i++) {
        out_names[i] = rec->goods[i].name;
        out_totals[i] = rec->goods[i].total;
    }
    return ret;
}

void G_StorageSite_RecordGathered(uint32_t uid, const char *rname, int amount)
{
    add_total(s_gathered, G_District_Of(uid), rname, amount);
}

void G_StorageSite_RecordCarried(uint32_t src, uint32_t dest, const char *rname, int amount)
{
    if(!G_EntityExists(src) || !G_EntityExists(dest))
        return;
    int from = G_District_Of(src);
    int to = G_District_Of(dest);
    if(from == to)
        return;
    add_total(s_exported, from, rname, amount);
    add_total(s_imported, to, rname, amount);
}

size_t G_StorageSite_DistrictGathered(int district, const char **out_names, int *out_totals, size_t maxout)
{
    return totals_of(s_gathered, district, out_names, out_totals, maxout);
}

size_t G_StorageSite_DistrictImported(int district, const char **out_names, int *out_totals, size_t maxout)
{
    return totals_of(s_imported, district, out_names, out_totals, maxout);
}

size_t G_StorageSite_DistrictExported(int district, const char **out_names, int *out_totals, size_t maxout)
{
    return totals_of(s_exported, district, out_names, out_totals, maxout);
}

void G_StorageSite_SetNote(uint32_t uid, const char *icon, const char *text, bool warn,
                           const char *after_icon, const char *after_text)
{
    if(!text) {
        khiter_t k = kh_get(note, s_notes, uid);
        if(k != kh_end(s_notes)) {
            kh_del(note, s_notes, k);
        }
        return;
    }

    int status;
    khiter_t k = kh_put(note, s_notes, uid, &status);
    if(status == -1)
        return;
    struct strip_note *note = &kh_value(s_notes, k);
    note->icon = icon ? si_intern(icon, &s_stringpool, s_stridx) : NULL;
    pf_strlcpy(note->text, text, sizeof(note->text));
    note->warn = warn;
    note->after_icon = (after_icon && after_text) ? si_intern(after_icon, &s_stringpool, s_stridx) : NULL;
    pf_strlcpy(note->after_text, after_text ? after_text : "", sizeof(note->after_text));
}

void G_StorageSite_RecordMade(uint32_t uid, const char *name, const char *icon, int amount)
{
    if(amount <= 0)
        return;

    int status;
    khiter_t k = kh_put(made, s_made, strip_key(uid), &status);
    if(status == -1)
        return;
    struct made_record *rec = &kh_value(s_made, k);
    if(status != 0) {
        rec->nmade = 0;
    }

    const char *key = si_intern(name, &s_stringpool, s_stridx);
    if(!key)
        return;
    struct made *m = NULL;
    for(int i = 0; i < rec->nmade; i++) {
        if(rec->made[i].name == key) {
            m = &rec->made[i];
            break;
        }
    }
    if(!m) {
        if(rec->nmade == ARR_SIZE(rec->made))
            return;
        m = &rec->made[rec->nmade++];
        *m = (struct made){ .name = key };
    }
    if(icon) {
        m->icon = si_intern(icon, &s_stringpool, s_stridx);
    }else if(!m->icon) {
        const char *ricon = G_Resource_GetIcon(name);
        m->icon = ricon ? si_intern(ricon, &s_stringpool, s_stridx) : NULL;
    }

    int prev = m->per_s[s_made_slot];
    m->per_s[s_made_slot] = MIN(UINT16_MAX, prev + amount);
    m->total += m->per_s[s_made_slot] - prev;
}

int G_StorageSite_MadePerMinute(uint32_t uid, const char *name)
{
    khiter_t k = kh_get(made, s_made, strip_key(uid));
    if(k == kh_end(s_made))
        return 0;
    const char *key = si_intern(name, &s_stringpool, s_stridx);
    const struct made_record *rec = &kh_value(s_made, k);
    for(int i = 0; i < rec->nmade; i++) {
        if(rec->made[i].name == key)
            return rec->made[i].total;
    }
    return 0;
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
        if(!ss->use_alt) {
            update_res_delta(key, -amount, G_GetFactionID(uid));
        }
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

    if(ss->use_alt)
        return;
    kh_foreach(ss->curr, key, amount, {
        update_res_delta(key, -amount, oldfac);
        update_res_delta(key,  amount, newfac);
    });
}

bool G_StorageSite_Desires(uint32_t uid, const char *rname)
{
    struct ss_state *ss = ss_view(uid);
    assert(ss);
    int rcurr = 0;
    ss_state_get_key(ss->curr, rname, &rcurr);
    return G_StorageSite_Request(uid, rname) > rcurr;
}

int G_StorageSite_BoundFor(uint32_t uid)
{
    return bound_for(uid);
}

int G_StorageSite_BoundForGood(uint32_t uid, const char *rname)
{
    return bound_for_good(uid, rname);
}

int G_StorageSite_Gatherers(uint32_t uid, const char *rname)
{
    return gatherers_for(uid, rname);
}

int G_StorageSite_Builders(uint32_t uid)
{
    return count_pool(uid, DUTY_BUILD, NULL);
}

float G_StorageSite_Rate(uint32_t uid, const char *rname)
{
    return good_rate(uid, rname);
}

float G_StorageSite_GetWindowHeight(uint32_t uid)
{
    struct ss_state *own = ss_state_get(uid);
    assert(own);
    struct ss_state *ss = ss_view(uid);
    if(own->pool && ss->anchor != uid)
        return 0.0f;
    struct window_plan plan;
    plan_window(uid, ss, &plan);
    return window_height(&plan);
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
        CHK_TRUE_RET(save_table(curr.held, "num_held", stream));
        CHK_TRUE_RET(save_draws(&curr, stream));
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
        CHK_TRUE_RET(save_table(curr.held, "num_held", stream));
        CHK_TRUE_RET(save_draws(&curr, stream));
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
        if(Attr_NextKeyIs(stream, "num_held")) {
            CHK_TRUE_RET(load_table(ss->held, stream));
        }
        if(Attr_NextKeyIs(stream, "num_draws")) {
            CHK_TRUE_RET(load_draws(ss, stream));
        }
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
        if(Attr_NextKeyIs(stream, "num_held")) {
            CHK_TRUE_RET(load_table(ps->held, stream));
        }
        if(Attr_NextKeyIs(stream, "num_draws")) {
            CHK_TRUE_RET(load_draws(ps, stream));
        }
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

