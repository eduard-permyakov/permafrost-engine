/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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

#include "ui.h"
#include "config.h"
#include "event.h"
#include "main.h"

#include "lib/public/pf_nuklear.h"
#include "lib/public/nuklear_sdl_gl3.h"
#include "lib/public/kvec.h"
#include "game/public/game.h"

#include <stdbool.h>
#include <string.h>
#include <assert.h>

#define MAX_VERTEX_MEMORY  (512 * 1024)
#define MAX_ELEMENT_MEMORY (128 * 1024)

struct text_desc{
    char        text[256];
    struct rect rect;
    struct rgba rgba;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct nk_context        *s_nk_ctx;
static kvec_t(struct text_desc)  s_curr_frame_labels;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void ui_draw_text(const struct text_desc desc)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(s_nk_ctx);
    assert(canvas);

    struct nk_rect  rect = nk_rect(desc.rect.x, desc.rect.y, desc.rect.w, desc.rect.h);
    struct nk_color rgba = nk_rgba(desc.rgba.r, desc.rgba.g, desc.rgba.b, desc.rgba.a);

    nk_draw_text(canvas, rect, desc.text, strlen(desc.text), s_nk_ctx->style.font,
        (struct nk_color){0,0,0,255}, rgba);
}

static void on_update_ui(void *user, void *event)
{
    struct nk_style *s = &s_nk_ctx->style;
    nk_style_push_color(s_nk_ctx, &s->window.background, nk_rgba(0,0,0,0));
    nk_style_push_style_item(s_nk_ctx, &s->window.fixed_background, nk_style_item_color(nk_rgba(0,0,0,0)));

    int width, height;
    Engine_WinDrawableSize(&width, &height);

    //TODO: use vres here as well
    if(nk_begin(s_nk_ctx, "__labels__", nk_rect(0, 0, width, height), 
       NK_WINDOW_NO_INPUT | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR)) {
    
       for(int i = 0; i < kv_size(s_curr_frame_labels); i++)
            ui_draw_text(kv_A(s_curr_frame_labels, i)); 
    }
    nk_end(s_nk_ctx);

    nk_style_pop_color(s_nk_ctx);
    nk_style_pop_style_item(s_nk_ctx);

    kv_reset(s_curr_frame_labels);
}

static int left_x_point(struct rect from_bounds, vec2_t from_res, vec2_t to_res, int resize_mask)
{
    int x_res_mask = resize_mask & ANCHOR_X_MASK;
    int from_left_margin = from_bounds.x;
    int from_right_margin = from_res.x - (from_bounds.x + from_bounds.w);
    int from_center_off = (from_res.x / 2) - (from_bounds.x + from_bounds.w/2);

    if(x_res_mask & ANCHOR_X_LEFT)
        return from_left_margin;

    switch(x_res_mask) {
    case ANCHOR_X_CENTER: {
        int to_center_x = to_res.x / 2 + from_center_off;
        return to_center_x - from_bounds.w / 2;
    }
    case ANCHOR_X_RIGHT:
        return to_res.x - from_right_margin - from_bounds.w;
    case ANCHOR_X_CENTER | ANCHOR_X_RIGHT: {
        int to_half_width = (from_bounds.x - from_right_margin) - (to_res.x / 2 + from_center_off);
        return (to_res.x / 2 + from_center_off - to_half_width);
    }
    default:
        return (assert(0),0);
    }
}

static int right_x_point(struct rect from_bounds, vec2_t from_res, vec2_t to_res, int resize_mask)
{
    int x_res_mask = resize_mask & ANCHOR_X_MASK;
    int from_left_margin = from_bounds.x;
    int from_right_margin = from_res.x - (from_bounds.x + from_bounds.w);
    int from_center_off = (from_res.x / 2) - (from_bounds.x + from_bounds.w/2);

    if(x_res_mask & ANCHOR_X_RIGHT)
        return to_res.x - from_right_margin;

    switch(x_res_mask) {
    case ANCHOR_X_LEFT:  
        return from_left_margin + from_bounds.w;
    case ANCHOR_X_CENTER: {
        int to_center_x = to_res.x / 2 + from_center_off;
        return to_center_x + from_bounds.w / 2;
    }
    case ANCHOR_X_LEFT | ANCHOR_X_CENTER: {
        int to_half_width = (to_res.x / 2 + from_center_off) - from_left_margin;
        return (to_res.x / 2 + from_center_off + to_half_width);
    }
    default:
        return (assert(0),0);
    }
}

static int top_y_point(struct rect from_bounds, vec2_t from_res, vec2_t to_res, int resize_mask)
{
    int y_res_mask = resize_mask & ANCHOR_Y_MASK;
    int from_top_margin = from_bounds.y;
    int from_bot_margin = from_res.y - (from_bounds.y + from_bounds.h);
    int from_center_off = (from_res.y / 2) - (from_bounds.y + from_bounds.h / 2);

    if(y_res_mask & ANCHOR_Y_TOP)
        return from_top_margin;

    switch(y_res_mask) {
    case ANCHOR_Y_BOT:  
        return to_res.y - from_bot_margin - from_bounds.h;
    case ANCHOR_Y_CENTER: {
        int to_center_y = to_res.y / 2 + from_center_off;
        return to_center_y - from_bounds.h / 2;
    }
    case ANCHOR_Y_CENTER | ANCHOR_Y_BOT: {
        int to_half_height = (to_res.y - from_bot_margin) - (to_res.y / 2 + from_center_off);
        return (to_res.y / 2 + from_center_off - to_half_height);
    }
    default:
        return (assert(0),0);
    }
}

static int bot_y_point(struct rect from_bounds, vec2_t from_res, vec2_t to_res, int resize_mask)
{
    int y_res_mask = resize_mask & ANCHOR_Y_MASK;
    int from_top_margin = from_bounds.y;
    int from_bot_margin = from_res.y - (from_bounds.y + from_bounds.h);
    int from_center_off = (from_res.y / 2) - (from_bounds.y + from_bounds.h / 2);

    if(y_res_mask & ANCHOR_Y_BOT)
        return to_res.y - from_bot_margin;

    switch(y_res_mask) {
    case ANCHOR_Y_TOP:  
        return from_top_margin + from_bounds.h;
    case ANCHOR_Y_CENTER: {
        int to_center_y = to_res.y / 2 + from_center_off;
        return to_center_y + from_bounds.h / 2;
    }
    case ANCHOR_Y_TOP | ANCHOR_Y_CENTER: {
        int to_half_height = (to_res.y / 2 + from_center_off) - from_top_margin;
        return (to_res.y / 2 + from_center_off + to_half_height);
    }
    default:
        return (assert(0),0);
    }

}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

struct nk_context *UI_Init(const char *basedir, SDL_Window *win)
{
    struct nk_context *ctx = nk_sdl_init(win);
    if(!ctx)
        return NULL;

    struct nk_font_atlas *atlas;
    char font_path[256];

    strcpy(font_path, basedir);
    strcat(font_path, "assets/fonts/OptimusPrinceps.ttf");

    nk_sdl_font_stash_begin(&atlas);
    struct nk_font *optimus_princeps = nk_font_atlas_add_from_file(atlas, font_path, 16, 0);

    atlas->default_font = optimus_princeps;
    nk_sdl_font_stash_end();

    kv_init(s_curr_frame_labels);
    E_Global_Register(EVENT_UPDATE_UI, on_update_ui, NULL, G_RUNNING | G_PAUSED_UI_RUNNING);

    s_nk_ctx = ctx;
    return ctx;
}

void UI_Shutdown(void)
{
    E_Global_Unregister(EVENT_UPDATE_UI, on_update_ui);
    kv_destroy(s_curr_frame_labels);
    nk_sdl_shutdown();
}

void UI_InputBegin(struct nk_context *ctx)
{
    nk_input_begin(ctx);
}

void UI_InputEnd(struct nk_context *ctx)
{
    nk_input_end(ctx);
}

void UI_Render(void)
{
    nk_sdl_render(NK_ANTI_ALIASING_ON, MAX_VERTEX_MEMORY, MAX_ELEMENT_MEMORY);
}

void UI_HandleEvent(SDL_Event *event)
{
    nk_sdl_handle_event(event);
}

void UI_DrawText(const char *text, struct rect rect, struct rgba rgba)
{
    struct text_desc d = (struct text_desc){.rect = rect, .rgba = rgba};
    strncpy(d.text, text, sizeof(d.text));
    d.text[sizeof(d.text)-1] = '\0';
    kv_push(struct text_desc, s_curr_frame_labels, d);
}

vec2_t UI_AdjustedVRes(vec2_t vres)
{
    int winw, winh;
    Engine_WinDrawableSize(&winw, &winh);

    float curr_ar = ((float)winw) / winh;
    float old_ar = vres.x / vres.y;

    if(curr_ar < old_ar) { /* vertical compression */

        return (vec2_t) {
            round(vres.x * (curr_ar / old_ar)),
            round(vres.y)
        };
    
    }else { /* horizontal compression */

        return (vec2_t) {
            round(vres.x),
            round(vres.y * (old_ar / curr_ar))
        };
    }
}

struct rect UI_BoundsForAspectRatio(struct rect from_bounds, vec2_t from_res, 
                                    vec2_t to_res, int resize_mask)
{
    int left_x = left_x_point(from_bounds, from_res, to_res, resize_mask);
    int right_x = right_x_point(from_bounds, from_res, to_res, resize_mask);
    int top_y = top_y_point(from_bounds, from_res, to_res, resize_mask);
    int bot_y = bot_y_point(from_bounds, from_res, to_res, resize_mask);

    return (struct rect){
        left_x,        
        top_y,
        right_x - left_x, 
        bot_y - top_y
    };
}

