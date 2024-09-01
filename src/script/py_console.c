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

#include <Python.h> /* Must be included first */

#include "../ui.h"
#include "../event.h"
#include "../lib/public/lru_cache.h"
#include "../lib/public/mem.h"
#include "../lib/public/pf_string.h"
#include "../game/public/game.h"
#include "../lib/public/pf_nuklear.h"

#include <stdlib.h>

#define CONSOLE_HIST_SIZE (1024)

struct strbuff{
    char line[256];
};

LRU_CACHE_TYPE(hist, struct strbuff)
LRU_CACHE_PROTOTYPES(static, hist, struct strbuff)
LRU_CACHE_IMPL(static, hist, struct strbuff)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static bool      s_shown = false;
static lru(hist) s_history;
static uint64_t  s_next_lineid;
static char      s_inputbuff[256];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void add_history(const char *str)
{
    struct strbuff next;
    pf_strlcpy(next.line, str, sizeof(next.line));
    lru_hist_put(&s_history, s_next_lineid++, &next);
}

static void on_update(void *user, void *event)
{
    if(!s_shown)
        return;

    const char *font = UI_GetActiveFont();
    UI_SetActiveFont("__default__");

    struct nk_context *ctx = UI_GetContext();
    const vec2_t vres = (vec2_t){1920, 1080};
    const vec2_t adj_vres = UI_ArAdjustedVRes(vres);
    const struct rect bounds = (struct rect){
        vres.x / 2.0f - 400,
        vres.y / 2.0f - 300,
        800,
        600,
    };
    const struct rect adj_bounds = UI_BoundsForAspectRatio(bounds, 
        vres, adj_vres, ANCHOR_X_CENTER | ANCHOR_Y_CENTER);

    if(nk_begin_with_vres(ctx, "Console", 
        nk_rect(adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h), 
        NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE 
      | NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR, 
        (struct nk_vec2i){adj_vres.x, adj_vres.y})) {

        nk_layout_row_dynamic(ctx, 500, 1);
        if(nk_group_begin(ctx, "__history__", NK_WINDOW_BORDER)) {

            uint64_t key;
            (void)key;
            struct strbuff line;
            LRU_FOREACH_REVERSE_SAFE_REMOVE(hist, &s_history, key, line, {
                nk_layout_row_dynamic(ctx, 12, 1);
                nk_label_colored(ctx, line.line, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE,
                    nk_rgb(255, 255, 255));
            });
            nk_group_end(ctx);
        }
        nk_layout_row_begin(ctx, NK_DYNAMIC, 40, 3);
        nk_layout_row_push(ctx, 0.05);
        nk_label_colored(ctx, ">>>", NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE, 
            nk_rgb(0, 255, 0));

        int len = strlen(s_inputbuff);
        nk_layout_row_push(ctx, 0.8);
        nk_edit_string(ctx, NK_EDIT_SIMPLE | NK_EDIT_ALWAYS_INSERT_MODE | NK_EDIT_ALLOW_TAB, 
            s_inputbuff, &len, sizeof(s_inputbuff), nk_filter_default);
        s_inputbuff[len] = '\0';

        bool enter_pressed = ctx->current->edit.active 
                          && nk_input_is_key_pressed(&ctx->input, NK_KEY_ENTER);

        nk_layout_row_push(ctx, 0.15);
        if((nk_button_label(ctx, "ENTER") || enter_pressed)) {
            add_history(s_inputbuff);
            s_inputbuff[0] = '\0';
        }
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
    UI_SetActiveFont(font);

    struct nk_window *win = nk_window_find(ctx, "Console");
    if(win->flags & (NK_WINDOW_CLOSED | NK_WINDOW_HIDDEN)) {
        s_shown = false;
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool S_Console_Init(void)
{
    s_next_lineid = 0;
    memset(s_inputbuff, 0, sizeof(s_inputbuff));
    E_Global_Register(EVENT_UPDATE_START, on_update, NULL, G_ALL);
    if(!lru_hist_init(&s_history, CONSOLE_HIST_SIZE, NULL))
        return false;
    return true;
}

void S_Console_Shutdown(void)
{
    lru_hist_destroy(&s_history);
    E_Global_Unregister(EVENT_UPDATE_START, on_update);
}

void S_Console_Show(void)
{
    s_shown = true;
}

