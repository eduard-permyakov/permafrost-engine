/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
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
#define MEM_FILE_SUB MEM_SUB_GAME_CLUSTER

#include "cluster.h"
#include "game_private.h"
#include "storage_site.h"
#include "position.h"
#include "fog_of_war.h"
#include "../ui.h"
#include "../event/public/event.h"
#include "../entity.h"
#include "../sched/public/sched.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/pf_nuklear.h"

#include <assert.h>
#include <math.h>

#include "../perf/public/mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_CLUSTER)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_CLUSTER)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_CLUSTER)

#define ARR_SIZE(a)      (sizeof(a)/sizeof((a)[0]))
#define MIN(a, b)        ((a) < (b) ? (a) : (b))
#define CLUSTER_TYPE_LEN (64)
#define ICON_SIZE        (24)
#define DIAMOND_SIZE     (16)
#define DIAMOND_GAP      (3)
#define DIAMOND_EDGE     (1.5f)
/* Clusters of a faction whose anchors are this close take different colours
 * where the palette allows */
#define COLOUR_REACH     (800.0f)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

struct cluster_type{
    char  name[CLUSTER_TYPE_LEN];
    float radius;
};

struct cluster{
    int          faction;
    char         type[CLUSTER_TYPE_LEN];
    uint32_t     anchor;
    /* Index into s_palette, -1 until chosen; chosen anew on load, in the 
     * order the clusters were made, rather than saved */
    int          colour;
    /* In the order they joined, so that the oldest member is first */
    vec_entity_t members;
    /* Sites marked within reach, promised a place once they stand. They
     * show the cluster's colour, but count for nothing else; one of them is
     * the anchor only while no member stands */
    vec_entity_t pending;
};

KHASH_MAP_INIT_INT(cluster, struct cluster)
KHASH_MAP_INIT_INT(member, int)
KHASH_MAP_INIT_INT(type, struct cluster_type)
KHASH_MAP_INIT_INT(footprint, struct cluster_footprint)

static void on_building_marked(void *user, void *event);
static void on_building_constructed(void *user, void *event);
static void on_update_ui(void *user, void *event);
static void draw_badge(struct nk_context *ctx, uint32_t uid, int colour, bool linked);
static void draw_diamond(struct nk_context *ctx, int colour, int scale);
static void draw_preview(struct nk_context *ctx);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(cluster) *s_clusters;
/* Maps each member building to its cluster */
static khash_t(member)  *s_member_of;
/* Maps each pending site to the cluster promised it */
static khash_t(member)  *s_pending_of;
/* The cluster type of every building that can join one */
static khash_t(type)    *s_types;
static int               s_next_id = 1;
static char              s_icon_path[512] = {0};
/* The building being placed and the cluster it would join, shown over it */
static uint32_t          s_preview_uid = NULL_UID;
static int               s_preview_cluster = 0;

/* Told apart at a glance, and kin to the skin's embroidery */
static const struct nk_color s_palette[] = {
    {198,  37,  45, 255},   /* crimson */
    { 52, 104, 178, 255},   /* azure */
    {214, 160,  40, 255},   /* ochre */
    { 46, 132,  50, 255},   /* green */
    {122,  64, 150, 255},   /* violet */
    { 30, 140, 140, 255},   /* teal */
    {226, 110,  30, 255},   /* orange */
    {128,  82,  44, 255},   /* brown */
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct cluster *cluster_get(int id)
{
    khiter_t k = kh_get(cluster, s_clusters, id);
    if(k == kh_end(s_clusters))
        return NULL;
    return &kh_value(s_clusters, k);
}

static struct cluster_type *type_get(uint32_t uid)
{
    khiter_t k = kh_get(type, s_types, uid);
    if(k == kh_end(s_types))
        return NULL;
    return &kh_value(s_types, k);
}

static bool compare_uids(uint32_t *a, uint32_t *b)
{
    return *a == *b;
}

static float distance(uint32_t a, uint32_t b)
{
    vec2_t pa = G_Pos_GetXZ(a);
    vec2_t pb = G_Pos_GetXZ(b);
    vec2_t delta;
    PFM_Vec2_Sub(&pa, &pb, &delta);
    return PFM_Vec2_Len(&delta);
}

/* The palette's colour least used by the faction's clusters near this one,
 * the cluster's id choosing among equals */
static int least_used_colour(int id, const struct cluster *cl)
{
    int uses[ARR_SIZE(s_palette)] = {0};
    int oid;
    struct cluster *other;
    kh_foreach_val_ptr(s_clusters, oid, other, {
        if(oid == id || other->colour < 0 || other->faction != cl->faction)
            continue;
        if(distance(other->anchor, cl->anchor) > COLOUR_REACH)
            continue;
        uses[other->colour]++;
    });

    int best = -1;
    for(int i = 0; i < ARR_SIZE(s_palette); i++) {
        int idx = (id + i) % ARR_SIZE(s_palette);
        if(best < 0 || uses[idx] < uses[best])
            best = idx;
    }
    return best;
}

static int compare_ids(const void *a, const void *b)
{
    return *(const int*)a - *(const int*)b;
}

static void choose_colours(void)
{
    size_t nclusters = kh_size(s_clusters);
    if(nclusters == 0)
        return;
    int *ids = PF_MALLOC(nclusters * sizeof(int));
    if(!ids)
        return;

    size_t n = 0;
    int id;
    struct cluster *cl;
    kh_foreach_val_ptr(s_clusters, id, cl, {
        cl->colour = -1;
        ids[n++] = id;
    });
    qsort(ids, n, sizeof(int), compare_ids);

    for(int i = 0; i < n; i++) {
        cl = cluster_get(ids[i]);
        cl->colour = least_used_colour(ids[i], cl);
    }
    PF_FREE(ids);
}

static int create_cluster(uint32_t anchor, const struct cluster_type *type)
{
    int id = s_next_id++;
    int status;
    khiter_t k = kh_put(cluster, s_clusters, id, &status);
    if(status == -1)
        return 0;

    struct cluster *cl = &kh_value(s_clusters, k);
    cl->faction = G_GetFactionID(anchor);
    pf_strlcpy(cl->type, type->name, sizeof(cl->type));
    cl->anchor = anchor;
    cl->colour = -1;
    vec_entity_init(&cl->members);
    vec_entity_init(&cl->pending);
    cl->colour = least_used_colour(id, cl);
    return id;
}

/* The oldest cluster of the building's type and faction whose anchor is in
 * reach, or 0. Measuring from the anchor keeps a row of buildings from
 * chaining one cluster across the map. */
static int cluster_in_reach(uint32_t uid, const struct cluster_type *type)
{
    int faction = G_GetFactionID(uid);
    int best = 0;

    int id;
    struct cluster *cl;
    kh_foreach_val_ptr(s_clusters, id, cl, {
        if(cl->faction != faction)
            continue;
        if(strcmp(cl->type, type->name))
            continue;
        if(distance(cl->anchor, uid) > type->radius)
            continue;
        if(best == 0 || id < best)
            best = id;
    });
    return best;
}

static void remove_from(vec_entity_t *vec, uint32_t uid)
{
    int idx = vec_entity_indexof(vec, uid, compare_uids);
    if(idx == -1)
        return;
    /* Keep the join order: the oldest left takes over as the anchor */
    for(int i = idx; i < vec_size(vec) - 1; i++) {
        vec_AT(vec, i) = vec_AT(vec, i + 1);
    }
    vec_entity_pop(vec);
}

/* Takes the site off the pending list of the cluster promised it, returning that cluster or 0 */
static int unpend(uint32_t uid)
{
    khiter_t k = kh_get(member, s_pending_of, uid);
    if(k == kh_end(s_pending_of))
        return 0;
    int id = kh_value(s_pending_of, k);
    kh_del(member, s_pending_of, k);

    struct cluster *cl = cluster_get(id);
    if(cl) {
        remove_from(&cl->pending, uid);
    }
    return id;
}

/* Deletes a cluster nothing is left of, or hands its anchor on from the building that left it */
static void settle_anchor(int id, uint32_t left)
{
    struct cluster *cl = cluster_get(id);
    if(!cl)
        return;

    if(vec_size(&cl->members) == 0 && vec_size(&cl->pending) == 0) {
        vec_entity_destroy(&cl->members);
        vec_entity_destroy(&cl->pending);
        kh_del(cluster, s_clusters, kh_get(cluster, s_clusters, id));
        return;
    }
    if(cl->anchor != left)
        return;

    cl->anchor = vec_size(&cl->members) ? vec_AT(&cl->members, 0) : vec_AT(&cl->pending, 0);
    cl->colour = least_used_colour(id, cl);
    G_StorageSite_PoolSetAnchor(id, cl->anchor);
}

/* A marked site is promised the cluster in reach, or one of its own, and shows its colour */
static void pend(uint32_t uid)
{
    const struct cluster_type *type = type_get(uid);
    if(!type)
        return;
    if(kh_get(member, s_member_of, uid) != kh_end(s_member_of)
    || kh_get(member, s_pending_of, uid) != kh_end(s_pending_of))
        return;

    int best = cluster_in_reach(uid, type);
    if(best == 0) {
        best = create_cluster(uid, type);
        if(best == 0)
            return;
    }

    struct cluster *cl = cluster_get(best);
    vec_entity_push(&cl->pending, uid);

    int status;
    khiter_t k = kh_put(member, s_pending_of, uid, &status);
    if(status != -1) {
        kh_value(s_pending_of, k) = best;
    }
}

/* Joins the cluster promised the building when it was marked, else the one
 * in reach, else starts a cluster of its own */
static void join(uint32_t uid)
{
    const struct cluster_type *type = type_get(uid);
    if(!type)
        return;
    if(kh_get(member, s_member_of, uid) != kh_end(s_member_of))
        return;

    int best = unpend(uid);
    if(best == 0) {
        best = cluster_in_reach(uid, type);
    }
    if(best == 0) {
        best = create_cluster(uid, type);
        if(best == 0)
            return;
    }

    struct cluster *cl = cluster_get(best);
    if(!cl)
        return;
    /* The first to stand of a cluster of sites takes over as its anchor */
    if(vec_size(&cl->members) == 0) {
        cl->anchor = uid;
    }
    vec_entity_push(&cl->members, uid);

    int status;
    khiter_t k = kh_put(member, s_member_of, uid, &status);
    if(status == -1)
        return;
    kh_value(s_member_of, k) = best;

    if(G_FlagsGet(uid) & ENTITY_FLAG_STORAGE_SITE) {
        G_StorageSite_PoolJoin(uid, best);
        G_StorageSite_PoolSetAnchor(best, cl->anchor);
    }
    E_Global_Notify(EVENT_CLUSTER_CHANGED, (void*)(uintptr_t)best, ES_ENGINE);
}

static void leave(uint32_t uid, bool died)
{
    int pending = unpend(uid);
    if(pending) {
        settle_anchor(pending, uid);
        return;
    }

    khiter_t k = kh_get(member, s_member_of, uid);
    if(k == kh_end(s_member_of))
        return;

    int id = kh_value(s_member_of, k);
    kh_del(member, s_member_of, k);

    struct cluster *cl = cluster_get(id);
    if(!cl)
        return;

    remove_from(&cl->members, uid);
    if(G_FlagsGet(uid) & ENTITY_FLAG_STORAGE_SITE) {
        G_StorageSite_PoolLeave(uid, id, died);
    }
    settle_anchor(id, uid);
    E_Global_Notify(EVENT_CLUSTER_CHANGED, (void*)(uintptr_t)id, ES_ENGINE);
}

static void on_building_marked(void *user, void *event)
{
    uint32_t uid = (uintptr_t)event;
    if(!G_EntityExists(uid) || G_EntityIsZombie(uid))
        return;
    pend(uid);
}

static void on_building_constructed(void *user, void *event)
{
    uint32_t uid = (uintptr_t)event;
    if(!G_EntityExists(uid) || G_EntityIsZombie(uid))
        return;
    join(uid);
}

/* The cluster's colour, as a diamond beside its chain */
static void draw_diamond(struct nk_context *ctx, int colour, int scale)
{
    struct nk_rect bounds;
    if(colour < 0 || !nk_widget(&bounds, ctx))
        return;

    const struct nk_color fill = s_palette[colour];
    const struct nk_color edge = (struct nk_color){fill.r / 2, fill.g / 2, fill.b / 2, 255};
    /* Whole pixels for the centre and corners, so the sides are even */
    const float cx = floorf(bounds.x + (DIAMOND_GAP + DIAMOND_SIZE / 2.0f) * scale);
    const float cy = floorf(bounds.y + bounds.h / 2.0f);
    const float r = DIAMOND_SIZE * scale / 2.0f;
    const float ri = r - DIAMOND_EDGE * scale;

    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    float outer[] = {cx, cy - r, cx + r, cy, cx, cy + r, cx - r, cy};
    float inner[] = {cx, cy - ri, cx + ri, cy, cx, cy + ri, cx - ri, cy};
    nk_fill_polygon(canvas, outer, 4, edge);
    nk_fill_polygon(canvas, inner, 4, fill);
}

static void on_update_ui(void *user, void *event)
{
    if(!s_icon_path[0])
        return;

    struct nk_context *ctx = UI_GetContext();
    struct nk_style_item bg_style = (struct nk_style_item){
        .type = NK_STYLE_ITEM_COLOR,
        .data.color = (struct nk_color){0, 0, 0, 0}
    };
    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background, bg_style);
    nk_style_push_vec2(ctx, &ctx->style.window.padding, nk_vec2(0.0f, 0.0f));
    nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(0.0f, 0.0f));

    uint16_t pfacs = G_GetPlayerControlledFactions();
    struct cluster *cl;

    kh_foreach_val_ptr(s_clusters, (int){0}, cl, {

        if(!(pfacs & (0x1 << cl->faction)))
            continue;

        /* The sites still to stand count toward the chain, as they show it too */
        const bool linked = (vec_size(&cl->members) + vec_size(&cl->pending) > 1);

        for(int i = 0; i < vec_size(&cl->members); i++) {
            draw_badge(ctx, vec_AT(&cl->members, i), cl->colour, linked);
        }
        for(int i = 0; i < vec_size(&cl->pending); i++) {
            draw_badge(ctx, vec_AT(&cl->pending, i), cl->colour, linked);
        }
    });
    draw_preview(ctx);

    nk_style_pop_vec2(ctx);
    nk_style_pop_vec2(ctx);
    nk_style_pop_style_item(ctx);
}

/* The cluster's colour over the building, beside its chain if it has company */
static void draw_badge(struct nk_context *ctx, uint32_t uid, int colour, bool linked)
{
    uint16_t pfacs = G_GetPlayerControlledFactions();
    struct obb obb;
    Entity_CurrentOBB(uid, &obb, true);
    if(!G_Fog_ObjExplored(pfacs, uid, &obb))
        return;

    char name[64];
    pf_snprintf(name, sizeof(name), "__cluster__.%x", uid);

    const vec2_t vres = (vec2_t){1920, 1080};
    const vec2_t adj_vres = UI_ArAdjustedVRes(vres);
    vec2_t ss_pos = Entity_TopScreenPos(uid, adj_vres.x, adj_vres.y);
    if(!G_OverlayShownAt(ss_pos.x))
        return;
    const int width = (linked ? ICON_SIZE : 0) + DIAMOND_GAP + DIAMOND_SIZE;
    const vec2_t pos = (vec2_t){floorf(ss_pos.x - width / 2), floorf(ss_pos.y - ICON_SIZE - 4)};
    const int flags = NK_WINDOW_NOT_INTERACTIVE | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR;

    struct rect adj_bounds = UI_BoundsForAspectRatio(
        (struct rect){pos.x, pos.y, width, ICON_SIZE},
        vres, adj_vres, ANCHOR_DEFAULT
    );

    if(nk_begin_with_vres(ctx, name,
        (struct nk_rect){adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h},
        flags, (struct nk_vec2i){adj_vres.x, adj_vres.y})) {

        nk_layout_row_begin(ctx, NK_STATIC, ICON_SIZE, linked ? 2 : 1);
        if(linked) {
            nk_layout_row_push(ctx, ICON_SIZE);
            nk_image_texpath(ctx, s_icon_path);
        }
        nk_layout_row_push(ctx, DIAMOND_GAP + DIAMOND_SIZE);
        draw_diamond(ctx, colour, 1);
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

/* The chain and colour of the cluster the building being placed would join,
 * large over its ghost */
static void draw_preview(struct nk_context *ctx)
{
    if(s_preview_uid == NULL_UID || !G_EntityExists(s_preview_uid))
        return;
    struct cluster *cl = cluster_get(s_preview_cluster);
    if(!cl)
        return;

    const int scale = 2;
    const int icon = ICON_SIZE * scale;
    const int diamond = (DIAMOND_GAP + DIAMOND_SIZE) * scale;
    const vec2_t vres = (vec2_t){1920, 1080};
    const vec2_t adj_vres = UI_ArAdjustedVRes(vres);
    vec2_t ss_pos = Entity_TopScreenPos(s_preview_uid, adj_vres.x, adj_vres.y);
    const int width = icon + diamond;
    const vec2_t pos = (vec2_t){floorf(ss_pos.x - width / 2), floorf(ss_pos.y - icon - 4)};
    const int flags = NK_WINDOW_NOT_INTERACTIVE | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR;

    struct rect adj_bounds = UI_BoundsForAspectRatio(
        (struct rect){pos.x, pos.y, width, icon},
        vres, adj_vres, ANCHOR_DEFAULT
    );

    if(nk_begin_with_vres(ctx, "__cluster_preview__",
        (struct nk_rect){adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h},
        flags, (struct nk_vec2i){adj_vres.x, adj_vres.y})) {

        nk_layout_row_begin(ctx, NK_STATIC, icon, 2);
        nk_layout_row_push(ctx, icon);
        nk_image_texpath(ctx, s_icon_path);
        nk_layout_row_push(ctx, diamond);
        draw_diamond(ctx, cl->colour, scale);
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Cluster_Init(void)
{
    if(!(s_clusters = kh_init(cluster)))
        goto fail_clusters;
    if(!(s_member_of = kh_init(member)))
        goto fail_member_of;
    if(!(s_pending_of = kh_init(member)))
        goto fail_pending_of;
    if(!(s_types = kh_init(type)))
        goto fail_types;

    s_next_id = 1;
    E_Global_Register(EVENT_BUILDING_MARKED, on_building_marked, NULL, G_ALL);
    E_Global_Register(EVENT_BUILDING_CONSTRUCTED, on_building_constructed, NULL, G_ALL);
    E_Global_Register(EVENT_UPDATE_UI, on_update_ui, NULL, G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);
    return true;

fail_types:
    kh_destroy(member, s_pending_of);
fail_pending_of:
    kh_destroy(member, s_member_of);
fail_member_of:
    kh_destroy(cluster, s_clusters);
fail_clusters:
    return false;
}

void G_Cluster_Shutdown(void)
{
    E_Global_Unregister(EVENT_UPDATE_UI, on_update_ui);
    E_Global_Unregister(EVENT_BUILDING_CONSTRUCTED, on_building_constructed);
    E_Global_Unregister(EVENT_BUILDING_MARKED, on_building_marked);

    struct cluster *cl;
    kh_foreach_val_ptr(s_clusters, (int){0}, cl, {
        vec_entity_destroy(&cl->members);
        vec_entity_destroy(&cl->pending);
    });
    kh_destroy(type, s_types);
    kh_destroy(member, s_pending_of);
    kh_destroy(member, s_member_of);
    kh_destroy(cluster, s_clusters);
    /* Movement starts before the clusters on the next load and reads none */
    s_types = NULL;
    s_pending_of = NULL;
    s_member_of = NULL;
    s_clusters = NULL;
}

void G_Cluster_RemoveEntity(uint32_t uid)
{
    leave(uid, false);
    khiter_t k = kh_get(type, s_types, uid);
    if(k != kh_end(s_types)) {
        kh_del(type, s_types, k);
    }
}

void G_Cluster_OnDeath(uint32_t uid)
{
    leave(uid, true);
}

void G_Cluster_SetType(uint32_t uid, const char *type, float radius)
{
    int status;
    khiter_t k = kh_put(type, s_types, uid, &status);
    if(status == -1)
        return;
    struct cluster_type *ct = &kh_value(s_types, k);
    pf_strlcpy(ct->name, type, sizeof(ct->name));
    ct->radius = radius;

    /* A building already standing joins at once; a site already marked is promised its place */
    if(!(G_FlagsGet(uid) & ENTITY_FLAG_BUILDING))
        return;
    if(G_Building_IsCompleted(uid)) {
        join(uid);
    }else if(G_Building_IsMarked(uid)) {
        pend(uid);
    }
}

int G_Cluster_Of(uint32_t uid)
{
    khiter_t k = kh_get(member, s_member_of, uid);
    if(k == kh_end(s_member_of))
        return 0;
    return kh_value(s_member_of, k);
}

int G_Cluster_PendingOf(uint32_t uid)
{
    khiter_t k = kh_get(member, s_pending_of, uid);
    if(k == kh_end(s_pending_of))
        return 0;
    return kh_value(s_pending_of, k);
}

uint32_t G_Cluster_Anchor(int cluster_id)
{
    struct cluster *cl = cluster_get(cluster_id);
    if(!cl)
        return NULL_UID;
    return cl->anchor;
}

size_t G_Cluster_Members(int cluster_id, size_t maxout, uint32_t out[])
{
    struct cluster *cl = cluster_get(cluster_id);
    if(!cl)
        return 0;
    size_t ret = 0;
    for(int i = 0; i < vec_size(&cl->members) && ret < maxout; i++) {
        out[ret++] = vec_AT(&cl->members, i);
    }
    return ret;
}

size_t G_Cluster_Footprint(uint32_t uid, size_t maxout, uint32_t out[])
{
    int cid = G_Cluster_Of(uid);
    size_t ret = cid ? G_Cluster_Members(cid, maxout, out) : 0;
    if(ret == 0 && maxout > 0) {
        out[0] = uid;
        ret = 1;
    }
    return ret;
}

uint32_t G_Cluster_FootprintAnchor(uint32_t uid)
{
    uint32_t anchor = G_Cluster_Anchor(G_Cluster_Of(uid));
    return (anchor == NULL_UID) ? uid : anchor;
}

khash_t(footprint) *G_Cluster_CopyFootprintsInto(khash_t(footprint) *dst)
{
    if(!dst) {
        dst = kh_init(footprint);
    }
    kh_clear(footprint, dst);
    /* Taken before the clusters exist, as the map loads */
    if(!s_clusters)
        return dst;

    int cid;
    struct cluster *cl;
    kh_foreach_val_ptr(s_clusters, cid, cl, {
        (void)cid;
        struct cluster_footprint fp = {0};
        fp.nmembers = MIN(vec_size(&cl->members), CLUSTER_FOOTPRINT_MAX);
        for(int i = 0; i < fp.nmembers; i++) {
            fp.members[i] = vec_AT(&cl->members, i);
        }
        for(int i = 0; i < vec_size(&cl->members); i++) {
            int status;
            khiter_t k = kh_put(footprint, dst, vec_AT(&cl->members, i), &status);
            if(status != -1) {
                kh_value(dst, k) = fp;
            }
        }
    });
    return dst;
}

uint32_t G_Cluster_NearestMember(int cluster_id, vec2_t pos)
{
    struct cluster *cl = cluster_get(cluster_id);
    if(!cl)
        return NULL_UID;

    uint32_t ret = NULL_UID;
    float best = 0.0f;
    for(int i = 0; i < vec_size(&cl->members); i++) {
        uint32_t uid = vec_AT(&cl->members, i);
        vec2_t mpos = G_Pos_GetXZ(uid);
        vec2_t delta;
        PFM_Vec2_Sub(&mpos, &pos, &delta);
        float len = PFM_Vec2_Len(&delta);
        if(ret == NULL_UID || len < best) {
            ret = uid;
            best = len;
        }
    }
    return ret;
}

size_t G_Cluster_GetAll(size_t maxout, int out[])
{
    size_t ret = 0;
    int id;
    struct cluster *cl;
    kh_foreach_val_ptr(s_clusters, id, cl, {
        /* One of sites alone is a promise, not yet a cluster to anyone else */
        if(vec_size(&cl->members) == 0)
            continue;
        if(ret == maxout)
            break;
        out[ret++] = id;
    });
    return ret;
}

const char *G_Cluster_Type(int cluster_id)
{
    struct cluster *cl = cluster_get(cluster_id);
    return cl ? cl->type : "";
}

int G_Cluster_Faction(int cluster_id)
{
    struct cluster *cl = cluster_get(cluster_id);
    return cl ? cl->faction : -1;
}

bool G_Cluster_Colour(int cluster_id, struct nk_color *out)
{
    struct cluster *cl = cluster_get(cluster_id);
    if(!cl || cl->colour < 0)
        return false;
    *out = s_palette[cl->colour];
    return true;
}

int G_Cluster_PreviewJoin(const char *type, int faction, vec2_t pos, float radius)
{
    int best = 0;
    int id;
    struct cluster *cl;
    kh_foreach_val_ptr(s_clusters, id, cl, {
        if(cl->faction != faction || strcmp(cl->type, type))
            continue;
        vec2_t apos = G_Pos_GetXZ(cl->anchor);
        vec2_t delta;
        PFM_Vec2_Sub(&apos, &pos, &delta);
        if(PFM_Vec2_Len(&delta) > radius)
            continue;
        if(best == 0 || id < best)
            best = id;
    });
    return best;
}

void G_Cluster_SetPreview(uint32_t uid, int cluster_id)
{
    s_preview_uid = uid;
    s_preview_cluster = cluster_id;
}

bool G_Cluster_TypeOf(uint32_t uid, char *out_type, size_t len, float *out_radius)
{
    const struct cluster_type *type = type_get(uid);
    if(!type)
        return false;
    pf_strlcpy(out_type, type->name, len);
    *out_radius = type->radius;
    return true;
}

void G_Cluster_SetIcon(const char *path)
{
    pf_strlcpy(s_icon_path, path, sizeof(s_icon_path));
}

bool G_Cluster_SaveState(struct SDL_RWops *stream)
{
    struct attr next_id = (struct attr){
        .type = TYPE_INT,
        .val.as_int = s_next_id
    };
    CHK_TRUE_RET(Attr_Write(stream, &next_id, "next_id"));

    struct attr icon = (struct attr){ .type = TYPE_STRING };
    pf_strlcpy(icon.val.as_string, s_icon_path, sizeof(icon.val.as_string));
    CHK_TRUE_RET(Attr_Write(stream, &icon, "icon"));

    struct attr num_types = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_types)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_types, "num_types"));

    uint32_t uid;
    struct cluster_type ct;
    kh_foreach(s_types, uid, ct, {

        struct attr uid_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid_attr, "uid"));

        struct attr name = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(name.val.as_string, ct.name, sizeof(name.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &name, "type"));

        struct attr radius = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = ct.radius
        };
        CHK_TRUE_RET(Attr_Write(stream, &radius, "radius"));
    });

    struct attr num_clusters = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_clusters)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_clusters, "num_clusters"));

    int id;
    struct cluster *cl;
    kh_foreach_val_ptr(s_clusters, id, cl, {

        struct attr id_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = id
        };
        CHK_TRUE_RET(Attr_Write(stream, &id_attr, "id"));

        struct attr faction = (struct attr){
            .type = TYPE_INT,
            .val.as_int = cl->faction
        };
        CHK_TRUE_RET(Attr_Write(stream, &faction, "faction"));

        struct attr type = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(type.val.as_string, cl->type, sizeof(type.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &type, "type"));

        struct attr anchor = (struct attr){
            .type = TYPE_INT,
            .val.as_int = cl->anchor
        };
        CHK_TRUE_RET(Attr_Write(stream, &anchor, "anchor"));

        struct attr num_members = (struct attr){
            .type = TYPE_INT,
            .val.as_int = vec_size(&cl->members)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_members, "num_members"));

        for(int i = 0; i < vec_size(&cl->members); i++) {
            struct attr member = (struct attr){
                .type = TYPE_INT,
                .val.as_int = vec_AT(&cl->members, i)
            };
            CHK_TRUE_RET(Attr_Write(stream, &member, "member"));
        }

        struct attr num_pending = (struct attr){
            .type = TYPE_INT,
            .val.as_int = vec_size(&cl->pending)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_pending, "num_pending"));

        for(int i = 0; i < vec_size(&cl->pending); i++) {
            struct attr pending = (struct attr){
                .type = TYPE_INT,
                .val.as_int = vec_AT(&cl->pending, i)
            };
            CHK_TRUE_RET(Attr_Write(stream, &pending, "pending"));
        }
        Sched_TryYield();
    });
    return true;
}

bool G_Cluster_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    s_next_id = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_STRING);
    pf_strlcpy(s_icon_path, attr.val.as_string, sizeof(s_icon_path));

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_types = attr.val.as_int;

    for(int i = 0; i < num_types; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t uid = attr.val.as_int;

        struct cluster_type ct;
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        pf_strlcpy(ct.name, attr.val.as_string, sizeof(ct.name));

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        ct.radius = attr.val.as_float;

        int status;
        khiter_t k = kh_put(type, s_types, uid, &status);
        CHK_TRUE_RET(status != -1);
        kh_value(s_types, k) = ct;
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_clusters = attr.val.as_int;

    for(int i = 0; i < num_clusters; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int id = attr.val.as_int;

        int status;
        khiter_t k = kh_put(cluster, s_clusters, id, &status);
        CHK_TRUE_RET(status != -1);
        struct cluster *cl = &kh_value(s_clusters, k);
        vec_entity_init(&cl->members);
        vec_entity_init(&cl->pending);
        cl->colour = -1;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        cl->faction = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        pf_strlcpy(cl->type, attr.val.as_string, sizeof(cl->type));

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        cl->anchor = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const size_t num_members = attr.val.as_int;

        for(int j = 0; j < num_members; j++) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            uint32_t member = attr.val.as_int;
            vec_entity_push(&cl->members, member);

            khiter_t km = kh_put(member, s_member_of, member, &status);
            CHK_TRUE_RET(status != -1);
            kh_value(s_member_of, km) = id;
        }

        /* Saves from before sites were promised a cluster have none listed */
        if(Attr_NextKeyIs(stream, "num_pending")) {

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            const size_t num_pending = attr.val.as_int;

            for(int j = 0; j < num_pending; j++) {
                CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
                CHK_TRUE_RET(attr.type == TYPE_INT);
                uint32_t pending = attr.val.as_int;
                vec_entity_push(&cl->pending, pending);

                khiter_t kp = kh_put(member, s_pending_of, pending, &status);
                CHK_TRUE_RET(status != -1);
                kh_value(s_pending_of, kp) = id;
            }
        }
        Sched_TryYield();
    }
    choose_colours();
    return true;
}
