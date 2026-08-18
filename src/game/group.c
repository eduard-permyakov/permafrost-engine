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
#define MEM_FILE_SUB MEM_SUB_GAME_GROUP

#include "group.h"
#include "formation.h"
#include "public/game.h"
#include "../script/public/script.h"
#include "../main.h"
#include "../sched.h"
#include "../ui.h"
#include "../event.h"
#include "../camera.h"
#include "../lib/public/khash.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/stb_image.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"

#include <assert.h>
#include <string.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_GROUP)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_GROUP)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_GROUP)

#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define ARR_SIZE(a)         (sizeof(a)/sizeof(a[0]))

#define UI_WIDTH            (76)
#define UI_HEIGHT           (96)
/* Contributions per group and lines of them on the banner. */
#define MAX_GROUP_BONUSES   (8)
#define MAX_BANNER_BONUSES  (3)
/* The icon is drawn into its whole cell, so the cell has to be square or the
 * image comes out stretched.
 */
#define BONUS_ROW_HEIGHT    (13)
#define BONUS_ICON_WIDTH    BONUS_ROW_HEIGHT
#define BONUS_TEXT_WIDTH    (23)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

KHASH_MAP_INIT_INT(gid, int)
KHASH_MAP_INIT_INT(members, vec_entity_t)
KHASH_MAP_INIT_INT(bounds, struct rect)

/* The bonuses a group carries, folded from its members' declarations by tag so
 * that eight bannermen contribute one banner.
 */
struct group_bonuses{
    int                     count;
    struct group_bonus_desc descs[MAX_GROUP_BONUSES];
};

KHASH_MAP_INIT_INT(gbonus, struct group_bonuses)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(gid)     *s_ent_group_map;
static khash_t(members) *s_groups;
static khash_t(bounds)  *s_ui_bounds;
static khash_t(gbonus)  *s_group_bonuses;
static int               s_next_group_id;
/* At most one group is ever asked about at a time, so the highlight is a single
 * slot rather than per-group state to be kept tidy.
 */
static int               s_highlight_gid;
static struct bonus_highlight s_highlight;

static struct nk_style_item s_bg_style;
static struct nk_color      s_font_clr;
static struct nk_color      s_bonus_clr;
static bool                 s_ui_style_set = false;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool entities_equal(uint32_t *a, uint32_t *b)
{
    return ((*a) == (*b));
}

/* Two contributions of the same tag are the same bonus, so the stronger one
 * wins rather than the two of them adding up.
 */
static void group_bonuses_fold(struct group_bonuses *inout,
                               const struct group_bonus_desc *desc)
{
    for(int i = 0; i < inout->count; i++) {
        if(strcmp(inout->descs[i].tag, desc->tag))
            continue;
        if(desc->amount > inout->descs[i].amount) {
            inout->descs[i] = *desc;
        }
        return;
    }

    if(inout->count == MAX_GROUP_BONUSES)
        return;
    inout->descs[inout->count++] = *desc;
}

/* A percentage reads as one, a flat amount as a plain signed number, and a
 * latch has no magnitude worth showing beside its icon.
 */
static void bonus_amount_str(const struct group_bonus_desc *desc, char *out, size_t maxout)
{
    if(desc->kind == COMBAT_MOD_INVULNERABLE) {
        pf_strlcpy(out, "", maxout);
    }else if(desc->percent) {
        pf_snprintf(out, maxout, "%+d%%", (int)roundf(desc->amount * 100.0f));
    }else{
        pf_snprintf(out, maxout, "%+d", (int)roundf(desc->amount));
    }
}

static void group_bonuses_recompute(int group_id)
{
    const vec_entity_t *members = G_Group_Members(group_id);
    khiter_t k = kh_get(gbonus, s_group_bonuses, group_id);

    if(!members) {
        if(k != kh_end(s_group_bonuses)) {
            kh_del(gbonus, s_group_bonuses, k);
        }
        return;
    }

    struct group_bonuses bonuses = {0};
    for(int i = 0; i < vec_size(members); i++) {

        struct group_bonus_desc descs[MAX_GROUP_BONUSES];
        int ndescs = G_Combat_GetGroupBonuses(vec_AT(members, i), descs, ARR_SIZE(descs));

        for(int j = 0; j < ndescs; j++) {
            group_bonuses_fold(&bonuses, &descs[j]);
        }
    }

    if(bonuses.count == 0) {
        if(k != kh_end(s_group_bonuses)) {
            kh_del(gbonus, s_group_bonuses, k);
        }
        return;
    }

    int ret;
    k = kh_put(gbonus, s_group_bonuses, group_id, &ret);
    assert(ret != -1);
    kh_value(s_group_bonuses, k) = bonuses;
}

static vec3_t group_centre_of_mass(const vec_entity_t *members)
{
    vec2_t centre = (vec2_t){0.0f, 0.0f};
    for(int i = 0; i < vec_size(members); i++) {
        vec2_t xz = G_Pos_GetXZ(vec_AT(members, i));
        PFM_Vec2_Add(&centre, &xz, &centre);
    }
    PFM_Vec2_Scale(&centre, 1.0f / vec_size(members), &centre);

    float height = 0.0f;
    G_MapHeightAtPoint(centre, &height);
    return (vec3_t){centre.x, MAX(height, 0.0f), centre.z};
}

static vec2_t group_screen_pos(vec3_t ws_pos, int screenw, int screenh)
{
    vec4_t pos_homo = (vec4_t){ws_pos.x, ws_pos.y, ws_pos.z, 1.0f};

    const struct camera *cam = G_GetActiveCamera();
    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view);
    Camera_MakeProjMat(cam, &proj);

    vec4_t clip, tmp;
    PFM_Mat4x4_Mult4x1(&view, &pos_homo, &tmp);
    PFM_Mat4x4_Mult4x1(&proj, &tmp, &clip);
    vec3_t ndc = (vec3_t){clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};

    float screen_x = (ndc.x + 1.0f) * screenw/2.0f;
    float screen_y = screenh - ((ndc.y + 1.0f) * screenh/2.0f);
    return (vec2_t){screen_x, screen_y};
}

static int group_at_position(int mouse_x, int mouse_y)
{
    int w, h;
    Engine_WinDrawableSize(&w, &h);
    const vec2_t adj_vres = UI_ArAdjustedVRes((vec2_t){1920, 1080});

    float vmouse_x = (float)mouse_x / w * adj_vres.x;
    float vmouse_y = (float)mouse_y / h * adj_vres.y;

    int gid;
    struct rect bounds;
    kh_foreach(s_ui_bounds, gid, bounds, {
        if(vmouse_x >= bounds.x && vmouse_x <= bounds.x + bounds.w
        && vmouse_y >= bounds.y && vmouse_y <= bounds.y + bounds.h)
            return gid;
    });
    return 0;
}

static void on_render_3d(void *user, void *event)
{
    if(!s_highlight_gid || !s_highlight.active)
        return;

    const vec_entity_t *members = G_Group_Members(s_highlight_gid);
    if(!members)
        return;

    for(int i = 0; i < vec_size(members); i++) {

        uint32_t uid = vec_AT(members, i);
        if(!G_EntityExists(uid))
            continue;

        vec2_t pos = G_Pos_GetXZ(uid);
        float radius = G_GetSelectionRadius(uid);
        R_PushCmd((struct rcmd){
            .func = R_GL_DrawSelectionCircle,
            .nargs = 5,
            .args = {
                R_PushArg(&pos, sizeof(pos)),
                R_PushArg(&radius, sizeof(radius)),
                R_PushArg(&s_highlight.width, sizeof(s_highlight.width)),
                R_PushArg(&s_highlight.color, sizeof(s_highlight.color)),
                (void*)G_GetPrevTickMap(),
            },
        });
    }
}

static void on_update_ui(void *user, void *event)
{
    struct nk_context *ctx = UI_GetContext();
    kh_clear(bounds, s_ui_bounds);

    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background, s_bg_style);
    nk_style_push_vec2(ctx, &ctx->style.window.padding, nk_vec2(0.0f, 28.0f));

    uint16_t controllable = G_GetPlayerControlledFactions();

    int gid;
    vec_entity_t members;
    kh_foreach(s_groups, gid, members, {

        uint32_t first = vec_AT(&members, 0);
        if(!((1 << G_GetFactionID(first)) & controllable))
            continue;

        char name[256];
        pf_snprintf(name, sizeof(name), "__group__.%x", gid);

        const vec2_t vres = (vec2_t){1920, 1080};
        const vec2_t adj_vres = UI_ArAdjustedVRes(vres);

        vec3_t ws_centre = group_centre_of_mass(&members);
        vec2_t ss_pos = group_screen_pos(ws_centre, adj_vres.x, adj_vres.y);

        struct group_bonus_desc bonuses[MAX_BANNER_BONUSES];
        int nbonuses = G_Group_GetBonuses(gid, bonuses, ARR_SIZE(bonuses));

        /* The cloth only has room for the count, so it lengthens to carry the
         * bonuses rather than the labels spilling off it.
         */
        const int height = UI_HEIGHT + nbonuses * BONUS_ROW_HEIGHT;
        const vec2_t pos = (vec2_t){ss_pos.x - UI_WIDTH/2, ss_pos.y - height/2};
        const int flags = NK_WINDOW_NOT_INTERACTIVE | NK_WINDOW_BACKGROUND
                        | NK_WINDOW_NO_SCROLLBAR;

        struct rect adj_bounds = UI_BoundsForAspectRatio(
            (struct rect){pos.x, pos.y, UI_WIDTH, height},
            vres, adj_vres, ANCHOR_DEFAULT
        );

        int ret;
        khiter_t k = kh_put(bounds, s_ui_bounds, gid, &ret);
        assert(ret != -1);
        kh_value(s_ui_bounds, k) = adj_bounds;

        if(nk_begin_with_vres(ctx, name,
            (struct nk_rect){adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h},
            flags, (struct nk_vec2i){adj_vres.x, adj_vres.y})) {

            char count[8];
            pf_snprintf(count, sizeof(count), "%d", (int)vec_size(&members));
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label_colored(ctx, count, NK_TEXT_CENTERED, s_font_clr);

            if(nbonuses > 0) {
                /* Zero spacing so the indent below actually centres the pair. */
                nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(0.0f, 2.0f));
                const char *font = UI_GetActiveFont();
                char small[256];
                pf_snprintf(small, sizeof(small), "%s.11", font);
                bool shrunk = UI_SetActiveFont(small);

                for(int i = 0; i < nbonuses; i++) {

                    char amount[16];
                    bonus_amount_str(&bonuses[i], amount, sizeof(amount));

                    /* A latch has no amount to sit beside, so its icon centres
                     * on its own rather than hanging off to one side.
                     */
                    const int text_width = strlen(amount) ? BONUS_TEXT_WIDTH : 0;
                    const int indent = (UI_WIDTH - BONUS_ICON_WIDTH - text_width) / 2;

                    nk_layout_row_begin(ctx, NK_STATIC, BONUS_ROW_HEIGHT, text_width ? 3 : 2);
                    nk_layout_row_push(ctx, indent);
                    nk_label_colored(ctx, "", NK_TEXT_ALIGN_LEFT, s_bonus_clr);

                    nk_layout_row_push(ctx, BONUS_ICON_WIDTH);
                    nk_image_texpath(ctx, bonuses[i].icon);

                    if(text_width) {
                        nk_layout_row_push(ctx, text_width);
                        nk_label_colored(ctx, amount, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE,
                            s_bonus_clr);
                    }
                    nk_layout_row_end(ctx);
                }

                if(shrunk) {
                    UI_SetActiveFont(font);
                }
                nk_style_pop_vec2(ctx);
            }
        }
        nk_end(ctx);
    });

    nk_style_pop_vec2(ctx);
    nk_style_pop_style_item(ctx);
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);
    if(mouse_event->button != SDL_BUTTON_LEFT)
        return;

    if(S_UI_MouseOverObscuringWindow(mouse_event->x, mouse_event->y))
        return;

    int gid = group_at_position(mouse_event->x, mouse_event->y);
    if(gid == 0)
        return;

    const vec_entity_t *members = G_Group_Members(gid);
    if(!members || vec_size(members) == 0)
        return;

    uint32_t uids[MAX_FORMATION_UNITS];
    size_t nuids = MIN(vec_size(members), MAX_FORMATION_UNITS);
    for(int i = 0; i < nuids; i++) {
        uids[i] = vec_AT(members, i);
    }
    G_Sel_Set(uids, nuids);
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

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Group_Init(void)
{
    if((s_ent_group_map = kh_init(gid)) == NULL)
        goto fail_ent_group_map;
    if((s_groups = kh_init(members)) == NULL)
        goto fail_groups;
    if((s_ui_bounds = kh_init(bounds)) == NULL)
        goto fail_ui_bounds;
    if((s_group_bonuses = kh_init(gbonus)) == NULL)
        goto fail_group_bonuses;
    s_next_group_id = 1;

    if(!s_ui_style_set) {
        s_bg_style = nk_style_item_color(nk_rgba(45, 45, 45, 220));
        s_font_clr = nk_rgba(255, 255, 255, 255);
        s_bonus_clr = nk_rgba(38, 122, 40, 255);
    }

    E_Global_Register(EVENT_UPDATE_UI, on_update_ui, NULL,
        G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL,
        G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);
    return true;

fail_group_bonuses:
    kh_destroy(bounds, s_ui_bounds);
    s_ui_bounds = NULL;
fail_ui_bounds:
    kh_destroy(members, s_groups);
    s_groups = NULL;
fail_groups:
    kh_destroy(gid, s_ent_group_map);
    s_ent_group_map = NULL;
fail_ent_group_map:
    return false;
}

void G_Group_Shutdown(void)
{
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    E_Global_Unregister(EVENT_UPDATE_UI, on_update_ui);

    if(s_ui_bounds) {
        kh_destroy(bounds, s_ui_bounds);
        s_ui_bounds = NULL;
    }
    if(s_group_bonuses) {
        kh_destroy(gbonus, s_group_bonuses);
        s_group_bonuses = NULL;
    }
    if(s_groups) {
        vec_entity_t curr;
        kh_foreach_value(s_groups, curr, {
            vec_entity_destroy(&curr);
        });
        kh_destroy(members, s_groups);
        s_groups = NULL;
    }
    if(s_ent_group_map) {
        kh_destroy(gid, s_ent_group_map);
        s_ent_group_map = NULL;
    }
}

int G_Group_Lock(const uint32_t *uids, size_t nuids)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_groups)
        return 0;
    nuids = MIN(nuids, MAX_FORMATION_UNITS);

    vec_entity_t members;
    vec_entity_init(&members);

    for(size_t i = 0; i < nuids; i++) {
        uint32_t uid = uids[i];
        if(!G_EntityExists(uid))
            continue;
        if(vec_entity_indexof(&members, uid, entities_equal) != -1)
            continue;
        vec_entity_push(&members, uid);
    }

    if(vec_size(&members) == 0) {
        vec_entity_destroy(&members);
        return 0;
    }

    int gid = s_next_group_id++;
    for(int i = 0; i < vec_size(&members); i++) {

        uint32_t uid = vec_AT(&members, i);
        G_Group_RemoveEntity(uid);

        int ret;
        khiter_t k = kh_put(gid, s_ent_group_map, uid, &ret);
        assert(ret != -1);
        kh_value(s_ent_group_map, k) = gid;
    }

    int ret;
    khiter_t k = kh_put(members, s_groups, gid, &ret);
    assert(ret != -1);
    kh_value(s_groups, k) = members;

    G_Group_RefreshBonus(gid);
    return gid;
}

void G_Group_Unlock(int group_id)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_groups)
        return;
    khiter_t k = kh_get(members, s_groups, group_id);
    if(k == kh_end(s_groups))
        return;

    vec_entity_t *members = &kh_value(s_groups, k);
    for(int i = 0; i < vec_size(members); i++) {
        khiter_t m = kh_get(gid, s_ent_group_map, vec_AT(members, i));
        assert(m != kh_end(s_ent_group_map));
        kh_del(gid, s_ent_group_map, m);
        G_Combat_RefreshBonuses(vec_AT(members, i));
    }
    vec_entity_destroy(members);
    kh_del(members, s_groups, k);

    khiter_t b = kh_get(gbonus, s_group_bonuses, group_id);
    if(b != kh_end(s_group_bonuses)) {
        kh_del(gbonus, s_group_bonuses, b);
    }
}

int G_Group_ForEnt(uint32_t uid)
{
    if(!s_ent_group_map)
        return 0;
    khiter_t k = kh_get(gid, s_ent_group_map, uid);
    if(k == kh_end(s_ent_group_map))
        return 0;
    return kh_value(s_ent_group_map, k);
}

int G_Group_ForSet(const uint32_t *uids, size_t nuids)
{
    if(nuids == 0)
        return 0;

    int gid = G_Group_ForEnt(uids[0]);
    if(gid == 0)
        return 0;

    for(size_t i = 1; i < nuids; i++) {
        if(G_Group_ForEnt(uids[i]) != gid)
            return 0;
    }
    return gid;
}

void G_Group_SetBackgroundStyle(const struct nk_style_item *style)
{
    s_bg_style = *style;
    s_ui_style_set = true;
}

void G_Group_SetFontColor(const struct nk_color *clr)
{
    s_font_clr = *clr;
}

void G_Group_SetBonusColor(const struct nk_color *clr)
{
    s_bonus_clr = *clr;
}

bool G_Group_MouseOverUI(int mouse_x, int mouse_y)
{
    if(!s_ui_bounds)
        return false;
    return (group_at_position(mouse_x, mouse_y) != 0);
}

void G_Group_GetBonus(int group_id, enum combat_mod_kind kind, float *out_flat,
                      float *out_percent)
{
    *out_flat = 0.0f;
    *out_percent = 0.0f;

    if(!s_group_bonuses)
        return;
    khiter_t k = kh_get(gbonus, s_group_bonuses, group_id);
    if(k == kh_end(s_group_bonuses))
        return;

    const struct group_bonuses *bonuses = &kh_value(s_group_bonuses, k);
    for(int i = 0; i < bonuses->count; i++) {
        if(bonuses->descs[i].kind != kind)
            continue;
        if(bonuses->descs[i].percent) {
            *out_percent += bonuses->descs[i].amount;
        }else{
            *out_flat += bonuses->descs[i].amount;
        }
    }
}

void G_Group_SetHighlight(int group_id, const struct bonus_highlight *hl)
{
    s_highlight_gid = hl->active ? group_id : 0;
    s_highlight = *hl;
}

void G_Group_RefreshBonus(int group_id)
{
    if(!s_groups || group_id == 0)
        return;

    group_bonuses_recompute(group_id);

    const vec_entity_t *members = G_Group_Members(group_id);
    if(!members)
        return;

    for(int i = 0; i < vec_size(members); i++) {
        G_Combat_RefreshBonuses(vec_AT(members, i));
    }
}

int G_Group_GetBonuses(int group_id, struct group_bonus_desc *out, size_t maxout)
{
    if(!s_group_bonuses)
        return 0;
    khiter_t k = kh_get(gbonus, s_group_bonuses, group_id);
    if(k == kh_end(s_group_bonuses))
        return 0;

    const struct group_bonuses *bonuses = &kh_value(s_group_bonuses, k);
    size_t ret = MIN((size_t)bonuses->count, maxout);
    memcpy(out, bonuses->descs, ret * sizeof(struct group_bonus_desc));
    return ret;
}

const vec_entity_t *G_Group_Members(int group_id)
{
    if(!s_groups)
        return NULL;
    khiter_t k = kh_get(members, s_groups, group_id);
    if(k == kh_end(s_groups))
        return NULL;
    return &kh_value(s_groups, k);
}

void G_Group_RemoveEntity(uint32_t uid)
{
    if(!s_ent_group_map)
        return;
    khiter_t k = kh_get(gid, s_ent_group_map, uid);
    if(k == kh_end(s_ent_group_map))
        return;

    int gid = kh_value(s_ent_group_map, k);
    kh_del(gid, s_ent_group_map, k);

    khiter_t m = kh_get(members, s_groups, gid);
    assert(m != kh_end(s_groups));

    vec_entity_t *members = &kh_value(s_groups, m);
    int idx = vec_entity_indexof(members, uid, entities_equal);
    assert(idx != -1);
    vec_entity_del(members, idx);

    if(vec_size(members) == 0) {
        vec_entity_destroy(members);
        kh_del(members, s_groups, m);

        khiter_t b = kh_get(gbonus, s_group_bonuses, gid);
        if(b != kh_end(s_group_bonuses)) {
            kh_del(gbonus, s_group_bonuses, b);
        }
    }else{
        /* The leaver may have been the one granting the bonus. */
        G_Group_RefreshBonus(gid);
    }
    G_Combat_RefreshBonuses(uid);
}

bool G_Group_SaveState(struct SDL_RWops *stream)
{
    struct attr next_group_id = (struct attr){
        .type = TYPE_INT,
        .val.as_int = s_next_group_id
    };
    CHK_TRUE_RET(Attr_Write(stream, &next_group_id, "next_group_id"));

    struct attr num_groups = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_groups)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_groups, "num_groups"));
    Sched_TryYield();

    int gid;
    vec_entity_t curr;
    kh_foreach(s_groups, gid, curr, {

        struct attr group_id = (struct attr){
            .type = TYPE_INT,
            .val.as_int = gid
        };
        CHK_TRUE_RET(Attr_Write(stream, &group_id, "group_id"));

        struct attr num_members = (struct attr){
            .type = TYPE_INT,
            .val.as_int = vec_size(&curr)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_members, "num_members"));

        for(int i = 0; i < vec_size(&curr); i++) {
            struct attr member = (struct attr){
                .type = TYPE_INT,
                .val.as_int = vec_AT(&curr, i)
            };
            CHK_TRUE_RET(Attr_Write(stream, &member, "member"));
            Sched_TryYield();
        }
    });

    struct attr bg_style_type = (struct attr){
        .type = TYPE_INT,
        .val.as_int = s_bg_style.type
    };
    CHK_TRUE_RET(Attr_Write(stream, &bg_style_type, "bg_style_type"));

    switch(s_bg_style.type) {
    case NK_STYLE_ITEM_COLOR: {

        CHK_TRUE_RET(save_color(s_bg_style.data.color, stream));
        break;
    }
    case NK_STYLE_ITEM_TEXPATH: {

        struct attr bg_texpath = (struct attr){ .type = TYPE_STRING };
        pf_strlcpy(bg_texpath.val.as_string, s_bg_style.data.texpath,
            sizeof(bg_texpath.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &bg_texpath, "bg_texpath"));
        break;
    }
    case NK_STYLE_ITEM_NINE_SLICE_TEXPATH: {

        CHK_TRUE_RET(save_nine_patch(&s_bg_style.data.slice_texpath, stream));
        break;
    }
    default: assert(0);
    }

    CHK_TRUE_RET(save_color(s_font_clr, stream));
    CHK_TRUE_RET(save_color(s_bonus_clr, stream));
    return true;
}

bool G_Group_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    s_next_group_id = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_groups = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_groups; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const int gid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const size_t num_members = attr.val.as_int;

        vec_entity_t members;
        vec_entity_init(&members);

        for(int j = 0; j < num_members; j++) {

            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_INT);
            CHK_TRUE_RET(G_EntityExists(attr.val.as_int));

            uint32_t uid = attr.val.as_int;
            vec_entity_push(&members, uid);

            int ret;
            khiter_t k = kh_put(gid, s_ent_group_map, uid, &ret);
            CHK_TRUE_RET(ret != -1);
            kh_value(s_ent_group_map, k) = gid;
            Sched_TryYield();
        }

        int ret;
        khiter_t k = kh_put(members, s_groups, gid, &ret);
        CHK_TRUE_RET(ret != -1);
        kh_value(s_groups, k) = members;
    }

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
        pf_strlcpy(s_bg_style.data.texpath, attr.val.as_string,
            sizeof(s_bg_style.data.texpath));
        break;
    }
    case NK_STYLE_ITEM_NINE_SLICE_TEXPATH: {

        CHK_TRUE_RET(load_nine_patch(&s_bg_style.data.slice_texpath, stream));
        break;
    }
    default:
        return false;
    }

    CHK_TRUE_RET(load_color(&s_font_clr, stream));
    CHK_TRUE_RET(load_color(&s_bonus_clr, stream));
    s_ui_style_set = true;

    /* The aggregates are derived from the members' declarations, which the
     * combat state has already restored by this point.
     */
    int gid;
    kh_foreach_key(s_groups, gid, {
        G_Group_RefreshBonus(gid);
    });
    return true;
}

