/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2024 Eduard Permyakov 
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

#include "garrison.h"
#include "public/game.h"
#include "game_private.h"
#include "fog_of_war.h"
#include "selection.h"
#include "../ui.h"
#include "../entity.h"
#include "../event.h"
#include "../lib/public/khash.h"
#include "../lib/public/vec.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/pf_string.h"

#include <assert.h>

struct garrison_state{
    int capacity_consumed;
};

struct garrisonable_state{
    int          capacity;
    int          current;
    vec_entity_t garrisoned;
};

KHASH_MAP_INIT_INT(garrison, struct garrison_state)
KHASH_MAP_INIT_INT(garrisonable, struct garrisonable_state)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map      *s_map;
static khash_t(garrison)     *s_garrison_state_table;
static khash_t(garrisonable) *s_garrisonable_state_table;

static char                   s_garrison_icon_path[512] = {0};
static struct nk_style_item   s_bg_style = {0};
static struct nk_color        s_font_clr = {0};
static bool                   s_show_ui = true;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/* gu - garrison unit 
 * gb - garrisonable building
 */

static struct garrison_state *gu_state_get(uint32_t uid)
{
    khiter_t k = kh_get(garrison, s_garrison_state_table, uid);
    if(k == kh_end(s_garrison_state_table))
        return NULL;

    return &kh_value(s_garrison_state_table, k);
}

static bool gu_state_set(uint32_t uid, struct garrison_state gus)
{
    int status;
    khiter_t k = kh_put(garrison, s_garrison_state_table, uid, &status);
    if(status == -1 || status == 0)
        return false;
    kh_value(s_garrison_state_table, k) = gus;
    return true;
}

static void gu_state_remove(uint32_t uid)
{
    khiter_t k = kh_get(garrison, s_garrison_state_table, uid);
    if(k != kh_end(s_garrison_state_table))
        kh_del(garrison, s_garrison_state_table, k);
}

static struct garrisonable_state *gb_state_get(uint32_t uid)
{
    khiter_t k = kh_get(garrisonable, s_garrisonable_state_table, uid);
    if(k == kh_end(s_garrisonable_state_table))
        return NULL;

    return &kh_value(s_garrisonable_state_table, k);
}

static bool gb_state_set(uint32_t uid, struct garrisonable_state gus)
{
    int status;
    khiter_t k = kh_put(garrisonable, s_garrisonable_state_table, uid, &status);
    if(status == -1 || status == 0)
        return false;
    kh_value(s_garrisonable_state_table, k) = gus;
    return true;
}

static void gb_state_remove(uint32_t uid)
{
    khiter_t k = kh_get(garrisonable, s_garrisonable_state_table, uid);
    if(k != kh_end(s_garrisonable_state_table))
        kh_del(garrisonable, s_garrisonable_state_table, k);
}

static void on_update_ui(void *user, void *event)
{
    if(!s_show_ui)
        return;

    uint32_t uid;
    struct garrisonable_state gbs;
    struct nk_context *ctx = UI_GetContext();

    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background, s_bg_style);

    kh_foreach(s_garrisonable_state_table, uid, gbs, {

        struct obb obb;
        Entity_CurrentOBB(uid, &obb, true);
        if(!G_Fog_ObjExplored(G_GetPlayerControlledFactions(), uid, &obb))
            continue;

        char name[256];
        pf_snprintf(name, sizeof(name), "__garrisonable__.%x", uid);

        const vec2_t vres = (vec2_t){1920, 1080};
        const vec2_t adj_vres = UI_ArAdjustedVRes(vres);

        vec2_t ss_pos = Entity_TopScreenPos(uid, adj_vres.x, adj_vres.y);
        const int width = 100;
        const int height = 32;
        const vec2_t pos = (vec2_t){ss_pos.x - width/2, ss_pos.y + 20};
        const int flags = NK_WINDOW_NOT_INTERACTIVE | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR;

        struct rect adj_bounds = UI_BoundsForAspectRatio(
            (struct rect){pos.x, pos.y, width, height}, 
            vres, adj_vres, ANCHOR_DEFAULT
        );

        if(nk_begin_with_vres(ctx, name, 
            (struct nk_rect){adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h}, 
            flags, (struct nk_vec2i){adj_vres.x, adj_vres.y})) {

            char text[32];
            pf_snprintf(text, sizeof(text), "%d / %d", gbs.current, gbs.capacity);

            nk_layout_row_begin(ctx, NK_STATIC, 24, 3);
            nk_layout_row_push(ctx, 24);
            nk_image_texpath(ctx, s_garrison_icon_path);

            nk_layout_row_push(ctx, 2);
            nk_spacing(ctx, 1);

            nk_layout_row_push(ctx, 72);
            nk_label_colored(ctx, text, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, s_font_clr);
        }
        nk_end(ctx);
    });
    nk_style_pop_style_item(ctx);
}

static void filter_selection_garrison(const vec_entity_t *in_sel, vec_entity_t *out_sel)
{
    vec_entity_init(out_sel);
    for(int i = 0; i < vec_size(in_sel); i++) {

        uint32_t uid = vec_AT(in_sel, i);
        uint32_t flags = G_FlagsGet(uid);
        if(!(flags & ENTITY_FLAG_GARRISON))
            continue;
        vec_entity_push(out_sel, uid);
    }
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    if(G_MouseOverMinimap())
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    bool right = (mouse_event->button == SDL_BUTTON_RIGHT);
    if(!right)
        return;

    int action = G_CurrContextualAction();
    if(action != CTX_ACTION_GARRISON)
        return;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);
    uint32_t target = G_Sel_GetHovered();

    vec_entity_t filtered;
    filter_selection_garrison(sel, &filtered);

    for(int i = 0; i < vec_size(&filtered); i++) {

        uint32_t curr = vec_AT(&filtered, i);
        G_Garrison_Enter(target, curr);
    }

    if(vec_size(&filtered) > 0) {
        Entity_Ping(target);
    }
    vec_entity_destroy(&filtered);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Garrison_Init(const struct map *map)
{
    if((s_garrison_state_table = kh_init(garrison)) == NULL)
        goto fail_garrison;
    if((s_garrisonable_state_table = kh_init(garrisonable)) == NULL)
        goto fail_garrisonable;

    struct nk_context ctx;
    nk_style_default(&ctx);
    s_bg_style = ctx.style.window.fixed_background;
    s_font_clr = ctx.style.text.color;

    E_Global_Register(EVENT_UPDATE_UI, on_update_ui, NULL, 
        G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);

    s_map = map;
    return true;

fail_garrisonable:
    kh_destroy(garrison, s_garrison_state_table);
fail_garrison:
    return false;
}

void G_Garrison_Shutdown(void)
{
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    E_Global_Unregister(EVENT_UPDATE_UI, on_update_ui);
    kh_destroy(garrisonable, s_garrisonable_state_table);
    kh_destroy(garrison, s_garrison_state_table);
}

bool G_Garrison_AddGarrison(uint32_t uid)
{
    struct garrison_state gus;
    gus.capacity_consumed = 1;
    return gu_state_set(uid, gus);
}

void G_Garrison_RemoveGarrison(uint32_t uid)
{
    gu_state_remove(uid);
}

bool G_Garrison_AddGarrisonable(uint32_t uid)
{
    struct garrisonable_state gbs;
    gbs.capacity = 0;
    gbs.current = 0;
    vec_entity_init(&gbs.garrisoned);
    return gb_state_set(uid, gbs);
}

void G_Garrison_RemoveGarrisonable(uint32_t uid)
{
    gb_state_remove(uid);
}

void G_Garrison_SetCapacityConsumed(uint32_t uid, int capacity)
{
    struct garrison_state *gus = gu_state_get(uid);
    assert(gus);
    gus->capacity_consumed = capacity;
}

int G_Garrison_GetCapacityConsumed(uint32_t uid)
{
    struct garrison_state *gus = gu_state_get(uid);
    assert(gus);
    return gus->capacity_consumed;
}

void G_Garrison_SetGarrisonableCapacity(uint32_t uid, int capacity)
{
    struct garrisonable_state *gbs = gb_state_get(uid);
    assert(gbs);
    gbs->capacity = capacity;
}

int G_Garrison_GetGarrisonableCapacity(uint32_t uid)
{
    struct garrisonable_state *gbs = gb_state_get(uid);
    assert(gbs);
    return gbs->capacity;
}

int G_Garrison_GetCurrentGarrisoned(uint32_t uid)
{
    struct garrisonable_state *gbs = gb_state_get(uid);
    assert(gbs);
    return gbs->current;
}

bool G_Garrison_Enter(uint32_t garrisonable, uint32_t unit)
{
    return true;
}

bool G_Garrison_Evict(uint32_t garrisonable, uint32_t unit)
{
    return true;
}

void G_Garrison_SetFontColor(const struct nk_color *clr)
{
    s_font_clr = *clr;
}

void G_Garrison_SetIcon(const char *path)
{
    size_t len = strlen(path) + 1;
    size_t buffsize = sizeof(s_garrison_icon_path);
    size_t copysize = len < buffsize ? len : buffsize;
    pf_strlcpy(s_garrison_icon_path, path, copysize);
}

void G_Garrison_SetBackgroundStyle(const struct nk_style_item *item)
{
    s_bg_style = *item;
}

void G_Garrison_SetShowUI(bool show)
{
    s_show_ui = show;
}

int G_Garrison_CurrContextualAction(void)
{
    uint32_t hovered = G_Sel_GetHovered();
    if(!G_EntityExists(hovered))
        return CTX_ACTION_NONE;

    if(M_MouseOverMinimap(s_map))
        return CTX_ACTION_NONE;

    if(!(G_FlagsGet(hovered) & ENTITY_FLAG_GARRISONABLE))
        return CTX_ACTION_NONE;

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    if(S_UI_MouseOverWindow(mouse_x, mouse_y))
        return CTX_ACTION_NONE;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);

    vec_entity_t filtered;
    filter_selection_garrison(sel, &filtered);

    if(vec_size(&filtered) == 0 || sel_type != SELECTION_TYPE_PLAYER) {
        vec_entity_destroy(&filtered);
        return CTX_ACTION_NONE;
    }

    uint32_t first = vec_AT(&filtered, 0);
    if(G_GetFactionID(hovered) != G_GetFactionID(first)) {
        vec_entity_destroy(&filtered);
        return CTX_ACTION_NONE;
    }

    vec_entity_destroy(&filtered);
    return CTX_ACTION_GARRISON;
}

