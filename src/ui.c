/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2020 Eduard Permyakov 
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
#include "render/public/render.h"
#include "render/public/render_ctrl.h"
#include "lib/public/vec.h"
#include "lib/public/pf_string.h"
#include "lib/public/nk_file_browser.h"
#include "lib/public/khash.h"
#include "lib/public/mem.h"
#include "game/public/game.h"

#include <stdbool.h>
#include <string.h>
#include <assert.h>

#define MAX_VERTEX_MEMORY   (2 * 1024* 1024)
#define MAX_ELEMENT_MEMORY      (512 * 1024)

struct text_desc{
    char        text[256];
    struct rect rect;
    struct rgba rgba;
};

VEC_TYPE(td, struct text_desc)
VEC_IMPL(static inline, td, struct text_desc)

KHASH_MAP_INIT_STR(font, struct nk_font*)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct nk_context            s_ctx;
static struct nk_font_atlas         s_atlas;
static struct nk_draw_null_texture  s_null;
static vec_td_t                     s_curr_frame_labels;
static khash_t(font)               *s_fontmap;
static const char                  *s_active_font = NULL;
static SDL_mutex                   *s_lock;
static vec2_t                       s_vres = {1920, 1080};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void ui_draw_text(const struct text_desc desc)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(&s_ctx);
    assert(canvas);

    struct nk_rect  rect = nk_rect(desc.rect.x, desc.rect.y, desc.rect.w, desc.rect.h);
    struct nk_color rgba = nk_rgba(desc.rgba.r, desc.rgba.g, desc.rgba.b, desc.rgba.a);

    nk_draw_text(canvas, rect, desc.text, strlen(desc.text), s_ctx.style.font,
        (struct nk_color){0,0,0,255}, rgba);
}

static void on_update_ui(void *user, void *event)
{
    struct nk_style *s = &s_ctx.style;
    nk_style_push_color(&s_ctx, &s->window.background, nk_rgba(0,0,0,0));
    nk_style_push_style_item(&s_ctx, &s->window.fixed_background, nk_style_item_color(nk_rgba(0,0,0,0)));

    const vec2_t adj_vres = UI_ArAdjustedVRes(s_vres);
    struct rect adj_bounds = UI_BoundsForAspectRatio(
        (struct rect){0, 0, s_vres.x, s_vres.y}, 
        s_vres, 
        adj_vres, 
        ANCHOR_DEFAULT
    );

    if(nk_begin_with_vres(&s_ctx,"__labels__", 
        nk_rect(adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h), 
        NK_WINDOW_NO_INPUT | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR, 
        (struct nk_vec2i){adj_vres.x, adj_vres.y})) {
    
       for(int i = 0; i < vec_size(&s_curr_frame_labels); i++)
            ui_draw_text(vec_AT(&s_curr_frame_labels, i)); 
    }
    nk_end(&s_ctx);

    nk_style_pop_color(&s_ctx);
    nk_style_pop_style_item(&s_ctx);

    vec_td_reset(&s_curr_frame_labels);
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

static void *push_draw_list(const struct nk_draw_list *dl)
{
    struct nk_draw_list *st_dl = R_PushArg(dl, sizeof(struct nk_draw_list));

    void *st_cmdbuff = R_PushArg(dl->buffer->memory.ptr, dl->buffer->memory.size);
    void *st_vbuff = R_PushArg(dl->vertices->memory.ptr, dl->vertices->memory.size);
    void *st_ebuff = R_PushArg(dl->elements->memory.ptr, dl->elements->memory.size);

    st_dl->buffer = R_PushArg(dl->buffer, sizeof(struct nk_buffer));
    st_dl->buffer->memory.ptr = st_cmdbuff;
    st_dl->vertices = R_PushArg(dl->vertices, sizeof(struct nk_buffer));
    st_dl->vertices->memory.ptr = st_vbuff;
    st_dl->elements = R_PushArg(dl->elements, sizeof(struct nk_buffer));
    st_dl->elements->memory.ptr = st_ebuff;

    return st_dl;
}

static void ui_init_font_stash(struct nk_context *ctx)
{
    const void *image; 
    int w, h;

    char fontdir[NK_MAX_PATH_LEN];
    pf_snprintf(fontdir, sizeof(fontdir), "%s/%s", g_basepath, "assets/fonts");

    nk_font_atlas_init_default(&s_atlas);
    nk_font_atlas_begin(&s_atlas);

    s_atlas.default_font = nk_font_atlas_add_default(&s_atlas, 16, NULL);

    int result;
    khiter_t k = kh_put(font, s_fontmap, pf_strdup("__default__"), &result);
    assert(result != -1 && result != 0);
    kh_value(s_fontmap, k) = s_atlas.default_font;
    s_active_font = kh_key(s_fontmap, k);

    size_t nfiles = 0;
    struct file *files = nk_file_list(fontdir, &nfiles);

    for(int i = 0; i < nfiles; i++) {

        if(files[i].is_dir)
            continue;
        if(!pf_endswith(files[i].name, ".ttf"))
            continue;

        char path[NK_MAX_PATH_LEN];
        pf_snprintf(path, sizeof(path), "%s/%s", fontdir, files[i].name);

        struct nk_font *font = nk_font_atlas_add_from_file(&s_atlas, path, 16, NULL);
        if(!font)
            continue;

        int result;
        khiter_t k = kh_put(font, s_fontmap, pf_strdup(files[i].name), &result);
        assert(result != -1 && result != 0);
        kh_value(s_fontmap, k) = font;
    }

    PF_FREE(files);
    image = nk_font_atlas_bake(&s_atlas, &w, &h, NK_FONT_ATLAS_RGBA32);

    R_PushCmd((struct rcmd){
        .func = R_GL_UI_UploadFontAtlas,
        .nargs = 3,
        .args = {
            R_PushArg(image, w * h * sizeof(uint32_t)),
            R_PushArg(&w, sizeof(w)),
            R_PushArg(&h, sizeof(h)),
        },
    });

    Engine_FlushRenderWorkQueue();

    nk_font_atlas_end(&s_atlas, nk_handle_id(R_UI_GetFontTexID()), &s_null);
    nk_style_set_font(ctx, &s_atlas.default_font->handle);
}

static void ui_clipboard_paste(nk_handle usr, struct nk_text_edit *edit)
{
    const char *text = SDL_GetClipboardText();
    if(text) 
        nk_textedit_paste(edit, text, nk_strlen(text));
}

static void ui_clipboard_copy(nk_handle usr, const char *text, int len)
{
    char *str = 0;
    if(!len) 
        return;
    str = (char*)malloc((size_t)len+1);
    if(!str) 
        return;
    memcpy(str, text, (size_t)len);
    str[len] = '\0';
    SDL_SetClipboardText(str);
    PF_FREE(str);
}

static struct nk_vec2i ui_get_drawable_size(void)
{
    int w, h;
    Engine_WinDrawableSize(&w, &h);
    return (struct nk_vec2i){w, h};
}

static struct nk_vec2i ui_get_screen_size(void)
{
    SDL_DisplayMode dm;
    SDL_GetDesktopDisplayMode(0, &dm);
    return (struct nk_vec2i){dm.w, dm.h};
}

static void ui_render(void *user, void *event)
{
    struct nk_buffer cmds, vbuf, ebuf;
    const enum nk_anti_aliasing aa = NK_ANTI_ALIASING_ON;

    void *vbuff = stalloc(&G_GetSimWS()->args, MAX_VERTEX_MEMORY);
    void *ebuff = stalloc(&G_GetSimWS()->args, MAX_ELEMENT_MEMORY);
    assert(vbuff && ebuff);

    /* fill convert configuration */
    struct nk_convert_config config;
    static const struct nk_draw_vertex_layout_element vertex_layout[] = {
        {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, NK_OFFSETOF(struct ui_vert, screen_pos)},
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(struct ui_vert, uv)},
        {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, NK_OFFSETOF(struct ui_vert, color)},
        {NK_VERTEX_LAYOUT_END}
    };
    memset(&config, 0, sizeof(config));
    config.vertex_layout = vertex_layout;
    config.vertex_size = sizeof(struct ui_vert);
    config.vertex_alignment = NK_ALIGNOF(struct ui_vert);
    config.null = s_null;
    config.circle_segment_count = 22;
    config.curve_segment_count = 22;
    config.arc_segment_count = 22;
    config.global_alpha = 1.0f;
    config.shape_AA = aa;
    config.line_AA = aa;

    /* setup buffers to load vertices and elements */
    nk_buffer_init_fixed(&vbuf, vbuff, MAX_VERTEX_MEMORY);
    nk_buffer_init_fixed(&ebuf, ebuff, MAX_ELEMENT_MEMORY);
    nk_buffer_init_default(&cmds);

    nk_convert(&s_ctx, &cmds, &vbuf, &ebuf, &config);

    R_PushCmd((struct rcmd){
        .func = R_GL_UI_Render,
        .nargs = 1,
        .args = {
            push_draw_list(&s_ctx.draw_list),
        },
    });

    nk_buffer_free(&cmds);
    nk_clear(&s_ctx);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool UI_Init(const char *basedir, SDL_Window *win)
{
    s_fontmap = kh_init(font);
    if(!s_fontmap)
        return false;

    s_lock = SDL_CreateMutex();
    if(!s_lock) {
        kh_destroy(font, s_fontmap);
        return false;
    }

    nk_init_default(&s_ctx, 0);
    s_ctx.clip.copy = ui_clipboard_copy;
    s_ctx.clip.paste = ui_clipboard_paste;
    s_ctx.clip.userdata = nk_handle_ptr(0);
    s_ctx.screen.get_drawable_size = ui_get_drawable_size;
    s_ctx.screen.get_screen_size = ui_get_screen_size;

    R_PushCmd((struct rcmd) { R_GL_UI_Init, 0 });

    ui_init_font_stash(&s_ctx);

    vec_td_init(&s_curr_frame_labels);
    E_Global_Register(EVENT_UPDATE_UI, on_update_ui, NULL, G_RUNNING | G_PAUSED_UI_RUNNING);
    E_Global_Register(EVENT_RENDER_UI, ui_render, NULL, G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);

    return true;
}

void UI_Shutdown(void)
{
    E_Global_Unregister(EVENT_UPDATE_UI, on_update_ui);
    E_Global_Unregister(EVENT_RENDER_UI, ui_render);
    vec_td_destroy(&s_curr_frame_labels);

    nk_font_atlas_clear(&s_atlas);
    nk_free(&s_ctx);
    memset(&s_ctx, 0, sizeof(s_ctx));
    SDL_DestroyMutex(s_lock);

    const char *key;
    struct nk_font *curr;
    (void)curr;

    kh_foreach(s_fontmap, key, curr, { free((void*)key); });
    kh_destroy(font, s_fontmap);

    R_PushCmd((struct rcmd){ R_GL_UI_Shutdown, 0});
}

void UI_InputBegin(void)
{
    nk_input_begin(&s_ctx);
}

void UI_InputEnd(void)
{
    nk_input_end(&s_ctx);
}

void UI_HandleEvent(SDL_Event *evt)
{
    if(evt->type == SDL_KEYUP || evt->type == SDL_KEYDOWN) {

        /* key events */
        int down = evt->type == SDL_KEYDOWN;
        const Uint8* state = SDL_GetKeyboardState(0);
        SDL_Keycode sym = evt->key.keysym.sym;

        if(sym == SDLK_RSHIFT || sym == SDLK_LSHIFT)
            nk_input_key(&s_ctx, NK_KEY_SHIFT, down);
        else if(sym == SDLK_DELETE)
            nk_input_key(&s_ctx, NK_KEY_DEL, down);
        else if(sym == SDLK_RETURN)
            nk_input_key(&s_ctx, NK_KEY_ENTER, down);
        else if(sym == SDLK_TAB)
            nk_input_key(&s_ctx, NK_KEY_TAB, down);
        else if(sym == SDLK_BACKSPACE)
            nk_input_key(&s_ctx, NK_KEY_BACKSPACE, down);
        else if(sym == SDLK_HOME) {
            nk_input_key(&s_ctx, NK_KEY_TEXT_START, down);
            nk_input_key(&s_ctx, NK_KEY_SCROLL_START, down);
        } else if(sym == SDLK_END) {
            nk_input_key(&s_ctx, NK_KEY_TEXT_END, down);
            nk_input_key(&s_ctx, NK_KEY_SCROLL_END, down);
        } else if(sym == SDLK_PAGEDOWN) {
            nk_input_key(&s_ctx, NK_KEY_SCROLL_DOWN, down);
        } else if(sym == SDLK_PAGEUP) {
            nk_input_key(&s_ctx, NK_KEY_SCROLL_UP, down);
        } else if(sym == SDLK_z)
            nk_input_key(&s_ctx, NK_KEY_TEXT_UNDO, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_r)
            nk_input_key(&s_ctx, NK_KEY_TEXT_REDO, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_c)
            nk_input_key(&s_ctx, NK_KEY_COPY, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_v)
            nk_input_key(&s_ctx, NK_KEY_PASTE, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_x)
            nk_input_key(&s_ctx, NK_KEY_CUT, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_b)
            nk_input_key(&s_ctx, NK_KEY_TEXT_LINE_START, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_e)
            nk_input_key(&s_ctx, NK_KEY_TEXT_LINE_END, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_UP)
            nk_input_key(&s_ctx, NK_KEY_UP, down);
        else if(sym == SDLK_DOWN)
            nk_input_key(&s_ctx, NK_KEY_DOWN, down);
        else if(sym == SDLK_LEFT) {
            if(state[SDL_SCANCODE_LCTRL])
                nk_input_key(&s_ctx, NK_KEY_TEXT_WORD_LEFT, down);
            else 
                nk_input_key(&s_ctx, NK_KEY_LEFT, down);
        } else if (sym == SDLK_RIGHT) {
            if(state[SDL_SCANCODE_LCTRL])
                nk_input_key(&s_ctx, NK_KEY_TEXT_WORD_RIGHT, down);
            else 
                nk_input_key(&s_ctx, NK_KEY_RIGHT, down);
        }

    }else if(evt->type == SDL_MOUSEBUTTONDOWN || evt->type == SDL_MOUSEBUTTONUP) {

        /* mouse button */
        int down = evt->type == SDL_MOUSEBUTTONDOWN;
        const int x = evt->button.x, y = evt->button.y;

        if(evt->button.button == SDL_BUTTON_LEFT) {
            if (evt->button.clicks > 1)
                nk_input_button(&s_ctx, NK_BUTTON_DOUBLE, x, y, down);
            nk_input_button(&s_ctx, NK_BUTTON_LEFT, x, y, down);
        }else if(evt->button.button == SDL_BUTTON_MIDDLE)
            nk_input_button(&s_ctx, NK_BUTTON_MIDDLE, x, y, down);
        else if(evt->button.button == SDL_BUTTON_RIGHT)
            nk_input_button(&s_ctx, NK_BUTTON_RIGHT, x, y, down);

    } else if(evt->type == SDL_MOUSEMOTION) {

        /* mouse motion */
        if(s_ctx.input.mouse.grabbed) {
            int x = (int)s_ctx.input.mouse.prev.x, y = (int)s_ctx.input.mouse.prev.y;
            nk_input_motion(&s_ctx, x + evt->motion.xrel, y + evt->motion.yrel);
        }else 
            nk_input_motion(&s_ctx, evt->motion.x, evt->motion.y);

    }else if(evt->type == SDL_TEXTINPUT) {

        /* text input */
        nk_glyph glyph;
        memcpy(glyph, evt->text.text, NK_UTF_SIZE);
        nk_input_glyph(&s_ctx, glyph);

    }else if(evt->type == SDL_MOUSEWHEEL) {

        /* mouse wheel */
        nk_input_scroll(&s_ctx,nk_vec2((float)evt->wheel.x,(float)evt->wheel.y));
    }
}

void UI_DrawText(const char *text, struct rect rect, struct rgba rgba)
{
    struct text_desc d = (struct text_desc){.rect = rect, .rgba = rgba};
    pf_strlcpy(d.text, text, sizeof(d.text));

    SDL_LockMutex(s_lock);
    vec_td_push(&s_curr_frame_labels, d);
    SDL_UnlockMutex(s_lock);
}

vec2_t UI_ArAdjustedVRes(vec2_t vres)
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

vec2_t UI_GetTextVres(void)
{
    return s_vres;
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

struct nk_context *UI_GetContext(void)
{
    return &s_ctx;
}

const char *UI_GetActiveFont(void)
{
    assert(s_active_font);
    return s_active_font;
}

bool UI_SetActiveFont(const char *name)
{
    khiter_t k = kh_get(font, s_fontmap, name);
    if(k == kh_end(s_fontmap))
        return false;

    struct nk_user_font *uf = &kh_value(s_fontmap, k)->handle;
    nk_style_set_font(&s_ctx, uf);

    s_active_font = kh_key(s_fontmap, k);
    return true;
}

void UI_ClearState(void)
{
    UI_SetActiveFont("__default__");
    nk_clear(&s_ctx);
}

