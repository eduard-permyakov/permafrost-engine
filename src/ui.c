/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2023 Eduard Permyakov 
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

#define MEM_FILE_SYS MEM_SYS_UI
#define MEM_FILE_SUB 0

#include "ui.h"
#include "script/public/script.h"
#include "config.h"
#include "event/public/event.h"
#include "main.h"

#include "lib/public/pf_nuklear.h"
#include "render/public/render.h"
#include "render/public/render_ctrl.h"
#include "lib/public/vec.h"
#include "lib/public/pf_string.h"
#include "lib/public/nk_file_browser.h"
#include "lib/public/khash.h"
#include "lib/public/stb_image.h"
#include "perf/public/mem.h"
#include "game/public/game.h"

#include <stdbool.h>
#include <string.h>
#include <assert.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_UI, 0)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_UI, 0)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_UI, 0)

#define MAX_VERTEX_MEMORY   (2 * 1024* 1024)
#define MAX_ELEMENT_MEMORY      (512 * 1024)
#define ARR_SIZE(a)             (sizeof(a)/sizeof(a[0]))

struct text_desc{
    char        text[256];
    struct rect rect;
    struct rgba rgba;
    /* None if its alpha is 0 */
    struct rgba outline;
    /* Centred across its rect rather than set at its left */
    bool        centred;
};

enum ui_render_mode{
    UI_RENDER_MODE_NORMAL,
    UI_RENDER_MODE_LOADING_SCREEN
};

VEC_TYPE(td, struct text_desc)
VEC_IMPL(static inline, td, struct text_desc)

VEC_TYPE(sdlev, SDL_Event)
VEC_IMPL(static inline, sdlev, SDL_Event)

/* A frame of the scripts' windows as the script thread drew it: the draw
 * list it was converted into, and the style it was drawn in. The script
 * thread fills the one that is not shown and then shows it; the main thread
 * copies the shown one to the render thread, for as many frames as it stays.
 */
struct script_frame{
    struct nk_draw_list list;
    struct nk_buffer    cmds;
    struct nk_buffer    verts;
    struct nk_buffer    elems;
    void               *vmem;
    void               *emem;
    struct nk_style     style;
    /* The scripts' font as the frame ended, which the engine's windows use */
    const char         *font;
    bool                filled;
};

KHASH_MAP_INIT_STR(font, struct nk_font*)
KHASH_MAP_INIT_STR(imgsize, uint64_t)

static void *ui_nk_alloc(nk_handle h, void *old, nk_size n)
{
    (void)old;
    uint16_t sys = (uint16_t)((unsigned)h.id >> 16);
    uint16_t sub = (uint16_t)((unsigned)h.id & 0xFFFF);
    return Mem_MallocTagged(n, sys, sub);
}

static void ui_nk_free(nk_handle h, void *p)
{
    (void)h;
    Mem_Free(p);
}

static struct nk_allocator s_nk_alloc = {
    .userdata.id = ((int)MEM_FILE_SYS << 16) | (int)MEM_FILE_SUB,
    .alloc       = ui_nk_alloc,
    .free        = ui_nk_free,
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

/* The engine's own windows are drawn into one context and the scripts' into
 * another, so that the scripts' can be drawn off the main thread. The scripts'
 * windows are always above the engine's.
 */
static struct nk_context            s_ctx;
static struct nk_context            s_script_ctx;
static struct nk_font_atlas         s_atlas;
static struct nk_draw_null_texture  s_null;
static vec_td_t                     s_curr_frame_labels;
static khash_t(font)               *s_fontmap;
/* Image dimensions by path, width in the high half */
static khash_t(imgsize)            *s_image_sizes;
static const char                  *s_active_font = NULL;
static SDL_mutex                   *s_lock;
/* The script thread's side of the UI. The lock guards the input it is yet to
 * take and which frame is shown; the rest is its own.
 */
static SDL_mutex                   *s_script_lock;
static vec_sdlev_t                  s_script_input;
static vec_sdlev_t                  s_script_events;
static struct script_frame          s_script_frames[2];
static int                          s_script_shown = -1;
static vec_td_t                     s_script_labels;
static vec_td_t                     s_script_labels_prev;
static const char                  *s_script_active_font = NULL;
/* A font the main thread set for the scripts, as at a session's start: the
 * script thread takes it up as it next draws, unless a script set one first
 */
static const char                  *s_script_font_pending = NULL;  /* atomic */
/* The frame the scripts' windows were last asked to be drawn for */
static uint64_t                     s_script_asked_frame;
static vec2_t                       s_vres = {1920, 1080};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void ui_draw_text(struct nk_context *ctx, const struct text_desc desc)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    assert(canvas);

    struct nk_rect  rect = nk_rect(desc.rect.x, desc.rect.y, desc.rect.w, desc.rect.h);
    if(desc.centred) {
        const struct nk_user_font *font = ctx->style.font;
        float width = font->width(font->userdata, font->height, desc.text, strlen(desc.text));
        rect.x += (rect.w - width) / 2.0f;
        rect.w = width + 1.0f;
    }
    struct nk_color rgba = nk_rgba(desc.rgba.r, desc.rgba.g, desc.rgba.b, desc.rgba.a);

    if(desc.outline.a > 0) {
        struct nk_color outline = nk_rgba(desc.outline.r, desc.outline.g, desc.outline.b, desc.outline.a);
        const struct nk_color none = nk_rgba(0, 0, 0, 0);
        /* The font's outline is a pixel wide: a pass a pixel off each way thickens it */
        for(int dx = -1; dx <= 1; dx++) {
        for(int dy = -1; dy <= 1; dy++) {
            if(dx == 0 && dy == 0)
                continue;
            struct nk_rect off = nk_rect(rect.x + dx, rect.y + dy, rect.w, rect.h);
            nk_draw_text_outlined(canvas, off, desc.text, strlen(desc.text), ctx->style.font,
                none, outline, outline);
        }}
        nk_draw_text_outlined(canvas, rect, desc.text, strlen(desc.text), ctx->style.font,
            none, rgba, outline);
        return;
    }

    nk_draw_text(canvas, rect, desc.text, strlen(desc.text), ctx->style.font,
        (struct nk_color){0,0,0,255}, rgba);
}

/* The labels asked for since the last frame, drawn behind every window */
static void ui_draw_labels(struct nk_context *ctx, const vec_td_t *labels)
{
    struct nk_style *s = &ctx->style;
    nk_style_push_color(ctx, &s->window.background, nk_rgba(0,0,0,0));
    nk_style_push_style_item(ctx, &s->window.fixed_background, nk_style_item_color(nk_rgba(0,0,0,0)));

    const vec2_t adj_vres = UI_ArAdjustedVRes(s_vres);
    struct rect adj_bounds = UI_BoundsForAspectRatio(
        (struct rect){0, 0, s_vres.x, s_vres.y}, 
        s_vres, 
        adj_vres, 
        ANCHOR_DEFAULT
    );

    if(nk_begin_with_vres(ctx,"__labels__", 
        nk_rect(adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h), 
        NK_WINDOW_NO_INPUT | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR, 
        (struct nk_vec2i){adj_vres.x, adj_vres.y})) {
    
        for(int i = 0; i < vec_size(labels); i++)
             ui_draw_text(ctx, vec_AT(labels, i)); 
    }
    nk_end(ctx);

    nk_style_pop_color(ctx);
    nk_style_pop_style_item(ctx);
}

static void on_update_ui(void *user, void *event)
{
    ui_draw_labels(&s_ctx, &s_curr_frame_labels);
    vec_td_reset(&s_curr_frame_labels);
}

static int left_x_point(struct rect from_bounds, vec2_t from_res, vec2_t to_res, int resize_mask)
{
    int x_res_mask = resize_mask & ANCHOR_X_MASK;
    int from_left_margin = from_bounds.x;
    int from_right_margin = from_res.x - (from_bounds.x + from_bounds.w);
    int from_center_off = (from_bounds.x + from_bounds.w/2) - (from_res.x / 2);

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
    int from_center_off = (from_bounds.x + from_bounds.w/2) - (from_res.x / 2);

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
    int from_center_off = (from_bounds.y + from_bounds.h / 2) - (from_res.y / 2);

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
    int from_center_off = (from_bounds.y + from_bounds.h / 2) - (from_res.y / 2);

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

    /* Only what was written of the vertices and elements goes to the render
     * thread. The commands are kept whole: they are laid out from the end.
     */
    void *st_cmdbuff = R_PushArg(dl->buffer->memory.ptr, dl->buffer->memory.size);
    void *st_vbuff = R_PushArg(dl->vertices->memory.ptr, dl->vertices->allocated);
    void *st_ebuff = R_PushArg(dl->elements->memory.ptr, dl->elements->allocated);

    st_dl->buffer = R_PushArg(dl->buffer, sizeof(struct nk_buffer));
    st_dl->buffer->memory.ptr = st_cmdbuff;
    st_dl->vertices = R_PushArg(dl->vertices, sizeof(struct nk_buffer));
    st_dl->vertices->memory.ptr = st_vbuff;
    st_dl->vertices->memory.size = dl->vertices->allocated;
    st_dl->elements = R_PushArg(dl->elements, sizeof(struct nk_buffer));
    st_dl->elements->memory.ptr = st_ebuff;
    st_dl->elements->memory.size = dl->elements->allocated;

    return st_dl;
}

static void ui_init_font_stash(struct nk_context *ctx)
{
    const void *image; 
    int w, h;

    char fontdir[NK_MAX_PATH_LEN];
    pf_snprintf(fontdir, sizeof(fontdir), "%s/%s", g_basepath, "assets/fonts");

    nk_font_atlas_init(&s_atlas, &s_nk_alloc);
    nk_font_atlas_begin(&s_atlas);

    s_atlas.default_font = nk_font_atlas_add_default(&s_atlas, 16, NULL);

    int result;
    khiter_t k = kh_put(font, s_fontmap, pf_strdup("__default__"), &result);
    assert(result != -1 && result != 0);
    kh_value(s_fontmap, k) = s_atlas.default_font;
    s_active_font = kh_key(s_fontmap, k);

    struct nk_font *default_large = nk_font_atlas_add_default(&s_atlas, 32, NULL);
    k = kh_put(font, s_fontmap, pf_strdup("__default__.32"), &result);
    assert(result != -1 && result != 0);
    kh_value(s_fontmap, k) = default_large;

    size_t nfiles = 0;
    struct file *files = nk_file_list(fontdir, &nfiles);

    for(int i = 0; i < nfiles; i++) {

        if(files[i].is_dir)
            continue;
        if(!pf_endswith(files[i].name, ".ttf"))
            continue;

        char path[NK_MAX_PATH_LEN];
        pf_snprintf(path, sizeof(path), "%s/%s", fontdir, files[i].name);

        static const nk_rune glyph_ranges[] = {
            /* English */
            0x0020, 0x00FF,
            /* Chinese */
            0x3000, 0x30FF,
            0x31F0, 0x31FF,
            0xFF00, 0xFFEF,
            0x4e00, 0x9FAF,
            /* Cyrillic */
            0x0400, 0x052F,
            0x2DE0, 0x2DFF,
            0xA640, 0xA69F,
            /* Terminator */
            0
        };

        static const nk_rune outline_ranges[] = {
            /* English */
            0x0020, 0x00FF,
            /* Cyrillic */
            0x0400, 0x052F,
            0x2DE0, 0x2DFF,
            0xA640, 0xA69F,
            /* Terminator */
            0
        };

        enum font_size{
            FONT_NORMAL,
            FONT_LARGE,
            FONT_SMALL,
            FONT_TINY,
            NUM_FONT_SIZES
        };
        size_t font_sizes[] = {
            [FONT_NORMAL] = 16,
            [FONT_LARGE]  = 32,
            [FONT_SMALL]  = 13,
            [FONT_TINY]   = 11,
        };
        for(enum font_size fs = 0; fs < NUM_FONT_SIZES; fs++) {

            size_t font_size = font_sizes[fs];
            struct nk_font_config config = nk_font_config(font_size);
            config.oversample_h = 1;
            config.oversample_v = 1;
            config.range = glyph_ranges;
            config.outline_width = (font_size >= 32) ? 2 : 1;
            config.outline_range = outline_ranges;

            struct nk_font *font = nk_font_atlas_add_from_file(&s_atlas, path, font_size, &config);
            if(!font)
                continue;

            char fontname[256];
            switch(fs) {
            case FONT_NORMAL:
                pf_snprintf(fontname, sizeof(fontname), "%s", files[i].name);
                break;
            case FONT_LARGE:
                pf_snprintf(fontname, sizeof(fontname), "%s.32", files[i].name);
                break;
            case FONT_SMALL:
                pf_snprintf(fontname, sizeof(fontname), "%s.13", files[i].name);
                break;
            case FONT_TINY:
                pf_snprintf(fontname, sizeof(fontname), "%s.11", files[i].name);
                break;
            default: assert(0);
            }
            int result;
            khiter_t k = kh_put(font, s_fontmap, pf_strdup(fontname), &result);
            assert(result != -1 && result != 0);
            kh_value(s_fontmap, k) = font;
        }
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

/* The clipboard goes through the window, which only the main thread may touch */
static void clipboard_get_main(void *arg)
{
    *(const char**)arg = SDL_GetClipboardText();
}

static void clipboard_set_main(void *arg)
{
    SDL_SetClipboardText(arg);
}

static void ui_clipboard_paste(nk_handle usr, struct nk_text_edit *edit)
{
    const char *text = NULL;
    S_View_RequireWorld();
    S_Thread_CallMain(clipboard_get_main, &text);
    if(text) 
        nk_textedit_paste(edit, text, nk_strlen(text));
}

static void ui_clipboard_copy(nk_handle usr, const char *text, int len)
{
    char *str = 0;
    if(!len) 
        return;
    str = (char*)PF_MALLOC((size_t)len+1);
    if(!str) 
        return;
    memcpy(str, text, (size_t)len);
    str[len] = '\0';
    S_View_RequireWorld();
    S_Thread_CallMain(clipboard_set_main, str);
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

/* A click or a scroll over a script window does not reach what is under it */
static void ui_handle_event(struct nk_context *ctx, SDL_Event *evt, bool covered)
{
    if(evt->type == SDL_KEYUP || evt->type == SDL_KEYDOWN) {

        /* key events */
        int down = evt->type == SDL_KEYDOWN;
        const Uint8* state = SDL_GetKeyboardState(0);
        SDL_Keycode sym = evt->key.keysym.sym;

        if(sym == SDLK_RSHIFT || sym == SDLK_LSHIFT)
            nk_input_key(ctx, NK_KEY_SHIFT, down);
        else if(sym == SDLK_DELETE)
            nk_input_key(ctx, NK_KEY_DEL, down);
        else if(sym == SDLK_RETURN)
            nk_input_key(ctx, NK_KEY_ENTER, down);
        else if(sym == SDLK_TAB)
            nk_input_key(ctx, NK_KEY_TAB, down);
        else if(sym == SDLK_BACKSPACE)
            nk_input_key(ctx, NK_KEY_BACKSPACE, down);
        else if(sym == SDLK_HOME) {
            nk_input_key(ctx, NK_KEY_TEXT_START, down);
            nk_input_key(ctx, NK_KEY_SCROLL_START, down);
        } else if(sym == SDLK_END) {
            nk_input_key(ctx, NK_KEY_TEXT_END, down);
            nk_input_key(ctx, NK_KEY_SCROLL_END, down);
        } else if(sym == SDLK_PAGEDOWN) {
            nk_input_key(ctx, NK_KEY_SCROLL_DOWN, down);
        } else if(sym == SDLK_PAGEUP) {
            nk_input_key(ctx, NK_KEY_SCROLL_UP, down);
        } else if(sym == SDLK_z)
            nk_input_key(ctx, NK_KEY_TEXT_UNDO, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_r)
            nk_input_key(ctx, NK_KEY_TEXT_REDO, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_c)
            nk_input_key(ctx, NK_KEY_COPY, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_v)
            nk_input_key(ctx, NK_KEY_PASTE, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_x)
            nk_input_key(ctx, NK_KEY_CUT, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_b)
            nk_input_key(ctx, NK_KEY_TEXT_LINE_START, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_e)
            nk_input_key(ctx, NK_KEY_TEXT_LINE_END, down && state[SDL_SCANCODE_LCTRL]);
        else if(sym == SDLK_UP)
            nk_input_key(ctx, NK_KEY_UP, down);
        else if(sym == SDLK_DOWN)
            nk_input_key(ctx, NK_KEY_DOWN, down);
        else if(sym == SDLK_LEFT) {
            if(state[SDL_SCANCODE_LCTRL])
                nk_input_key(ctx, NK_KEY_TEXT_WORD_LEFT, down);
            else 
                nk_input_key(ctx, NK_KEY_LEFT, down);
        } else if (sym == SDLK_RIGHT) {
            if(state[SDL_SCANCODE_LCTRL])
                nk_input_key(ctx, NK_KEY_TEXT_WORD_RIGHT, down);
            else 
                nk_input_key(ctx, NK_KEY_RIGHT, down);
        }

    }else if(evt->type == SDL_MOUSEBUTTONDOWN || evt->type == SDL_MOUSEBUTTONUP) {

        /* mouse button */
        int down = evt->type == SDL_MOUSEBUTTONDOWN;
        if(covered && down)
            return;
        const int x = evt->button.x, y = evt->button.y;

        if(evt->button.button == SDL_BUTTON_LEFT) {
            if (evt->button.clicks > 1)
                nk_input_button(ctx, NK_BUTTON_DOUBLE, x, y, down);
            nk_input_button(ctx, NK_BUTTON_LEFT, x, y, down);
        }else if(evt->button.button == SDL_BUTTON_MIDDLE)
            nk_input_button(ctx, NK_BUTTON_MIDDLE, x, y, down);
        else if(evt->button.button == SDL_BUTTON_RIGHT)
            nk_input_button(ctx, NK_BUTTON_RIGHT, x, y, down);

    } else if(evt->type == SDL_MOUSEMOTION) {

        /* mouse motion */
        if(ctx->input.mouse.grabbed) {
            int x = (int)ctx->input.mouse.prev.x, y = (int)ctx->input.mouse.prev.y;
            nk_input_motion(ctx, x + evt->motion.xrel, y + evt->motion.yrel);
        }else 
            nk_input_motion(ctx, evt->motion.x, evt->motion.y);

    }else if(evt->type == SDL_TEXTINPUT) {

        /* text input */
        nk_glyph glyph;
        memcpy(glyph, evt->text.text, NK_UTF_SIZE);
        nk_input_glyph(ctx, glyph);

    }else if(evt->type == SDL_MOUSEWHEEL) {

        /* mouse wheel */
        if(covered)
            return;
        nk_input_scroll(ctx,nk_vec2((float)evt->wheel.x,(float)evt->wheel.y));
    }
}

static struct nk_convert_config ui_convert_config(void)
{
    const enum nk_anti_aliasing aa = NK_ANTI_ALIASING_ON;
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
    return config;
}

/* Turns what was drawn into a context this frame into a draw list for the
 * render thread, or returns NULL if nothing was
 */
static void *ui_convert(struct nk_context *ctx)
{
    struct nk_buffer cmds, vbuf, ebuf;
    void *vbuff = stalloc(&G_GetSimWS()->args, MAX_VERTEX_MEMORY);
    void *ebuff = stalloc(&G_GetSimWS()->args, MAX_ELEMENT_MEMORY);
    assert(vbuff && ebuff);
    struct nk_convert_config config = ui_convert_config();

    /* setup buffers to load vertices and elements */
    nk_buffer_init_fixed(&vbuf, vbuff, MAX_VERTEX_MEMORY);
    nk_buffer_init_fixed(&ebuf, ebuff, MAX_ELEMENT_MEMORY);
    nk_buffer_init(&cmds, &s_nk_alloc, 4096);

    nk_convert(ctx, &cmds, &vbuf, &ebuf, &config);

    void *ret = (ctx->draw_list.element_count > 0) ? push_draw_list(&ctx->draw_list) : NULL;
    nk_buffer_free(&cmds);
    nk_clear(ctx);
    return ret;
}

static bool ui_image_size(const char *texpath, int *out_w, int *out_h)
{
    khiter_t k = kh_get(imgsize, s_image_sizes, texpath);
    if(k == kh_end(s_image_sizes)) {

        char path[512];
        pf_snprintf(path, sizeof(path), "%s/%s", g_basepath, texpath);

        int w, h, components;
        if(stbi_info(path, &w, &h, &components) != 1)
            return false;

        char *key = pf_strdup(texpath);
        if(!key)
            return false;
        int status;
        k = kh_put(imgsize, s_image_sizes, key, &status);
        if(status == -1) {
            PF_FREE(key);
            return false;
        }
        kh_value(s_image_sizes, k) = ((uint64_t)(uint32_t)w << 32) | (uint32_t)h;
    }
    uint64_t size = kh_value(s_image_sizes, k);
    *out_w = (int)(size >> 32);
    *out_h = (int)(uint32_t)size;
    return true;
}

/* A label a script asks for is drawn with the scripts' windows, the engine's
 * with the engine's
 */
static void ui_label_push(const struct text_desc *desc)
{
    if(S_Thread_OnScript()) {
        vec_td_push(&s_script_labels, *desc);
        return;
    }
    SDL_LockMutex(s_lock);
    vec_td_push(&s_curr_frame_labels, *desc);
    SDL_UnlockMutex(s_lock);
}

/* The image and resolution commands of a draw list carry a block each that the
 * render thread frees once it has drawn them
 */
static void script_frame_free_userdata(struct script_frame *sf)
{
    const struct nk_draw_command *cmd;
    nk_draw_list_foreach(cmd, &sf->list, &sf->cmds) {
        free(cmd->userdata.ptr);
    }
}

/* The shown frame of the scripts' windows, copied for the render thread: it
 * may be shown for many frames, so its blocks are copied each time
 */
static void *script_frame_push(void)
{
    void *ret = NULL;
    SDL_LockMutex(s_script_lock);
    /* A frame nobody asked to renew, such as one from before a full pause, is
     * not shown again
     */
    bool stale = (g_frame_idx - s_script_asked_frame > 1);
    if(s_script_shown >= 0 && !stale && s_script_frames[s_script_shown].list.element_count > 0) {
        struct script_frame *sf = &s_script_frames[s_script_shown];
        struct nk_draw_list *dl = push_draw_list(&sf->list);
        const struct nk_draw_command *cmd;
        nk_draw_list_foreach(cmd, dl, dl->buffer) {
            if(!cmd->userdata.ptr)
                continue;
            void *copy = malloc(sizeof(struct nk_command_userdata));
            if(copy)
                memcpy(copy, cmd->userdata.ptr, sizeof(struct nk_command_userdata));
            ((struct nk_draw_command*)cmd)->userdata.ptr = copy;
        }
        ret = dl;
    }
    SDL_UnlockMutex(s_script_lock);
    return ret;
}

static void ui_render(void *user, void *event)
{
    enum ui_render_mode mode = (enum ui_render_mode)((uintptr_t)user);
    void *engine = ui_convert(&s_ctx);
    /* Nothing draws the scripts' windows while a session loads */
    void *script = (mode == UI_RENDER_MODE_NORMAL) ? script_frame_push() : NULL;

    switch(mode) {
    case UI_RENDER_MODE_NORMAL: {
        void *lists[] = {engine, script};
        for(int i = 0; i < ARR_SIZE(lists); i++) {
            if(!lists[i])
                continue;
            R_PushCmd((struct rcmd){
                .func = R_GL_UI_Render,
                .nargs = 1,
                .args = { lists[i] },
            });
        }
        break;
    }
    case UI_RENDER_MODE_LOADING_SCREEN: {
        /* Each goes ahead of what is queued: the last one pushed is drawn first */
        void *lists[] = {script, engine};
        for(int i = 0; i < ARR_SIZE(lists); i++) {
            if(!lists[i])
                continue;
            R_PushCmdImmediateFront((struct rcmd){
                .func = R_GL_UI_Render,
                .nargs = 1,
                .args = { lists[i] },
            });
        }
        break;
    }
    default: assert(0);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool UI_Init(const char *basedir, SDL_Window *win)
{
    s_fontmap = kh_init(font);
    if(!s_fontmap)
        return false;

    s_image_sizes = kh_init(imgsize);
    if(!s_image_sizes) {
        kh_destroy(font, s_fontmap);
        return false;
    }

    s_lock = SDL_CreateMutex();
    if(!s_lock) {
        kh_destroy(imgsize, s_image_sizes);
        kh_destroy(font, s_fontmap);
        return false;
    }
    s_script_lock = SDL_CreateMutex();
    vec_sdlev_init(&s_script_input);
    vec_sdlev_init(&s_script_events);
    vec_td_init(&s_script_labels);
    vec_td_init(&s_script_labels_prev);
    s_script_shown = -1;
    for(int i = 0; i < ARR_SIZE(s_script_frames); i++) {
        memset(&s_script_frames[i], 0, sizeof(s_script_frames[i]));
        s_script_frames[i].vmem = PF_MALLOC(MAX_VERTEX_MEMORY);
        s_script_frames[i].emem = PF_MALLOC(MAX_ELEMENT_MEMORY);
    }

    struct nk_context *ctxs[] = {&s_ctx, &s_script_ctx};
    for(int i = 0; i < ARR_SIZE(ctxs); i++) {
        nk_init(ctxs[i], &s_nk_alloc, 0);
        ctxs[i]->clip.copy = ui_clipboard_copy;
        ctxs[i]->clip.paste = ui_clipboard_paste;
        ctxs[i]->clip.userdata = nk_handle_ptr(0);
        ctxs[i]->screen.get_drawable_size = ui_get_drawable_size;
        ctxs[i]->screen.get_screen_size = ui_get_screen_size;
    }

    R_PushCmd((struct rcmd) { R_GL_UI_Init, 0 });

    ui_init_font_stash(&s_ctx);
    nk_style_set_font(&s_script_ctx, s_ctx.style.font);

    vec_td_init(&s_curr_frame_labels);
    E_Global_Register(EVENT_UPDATE_UI, on_update_ui, NULL, G_RUNNING | G_PAUSED_UI_RUNNING);
    E_Global_Register(EVENT_RENDER_UI, ui_render, (void*)(uintptr_t)UI_RENDER_MODE_NORMAL, 
        G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);

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
    nk_free(&s_script_ctx);
    memset(&s_script_ctx, 0, sizeof(s_script_ctx));
    for(int i = 0; i < ARR_SIZE(s_script_frames); i++) {
        struct script_frame *sf = &s_script_frames[i];
        if(sf->filled) {
            script_frame_free_userdata(sf);
            nk_buffer_free(&sf->cmds);
        }
        PF_FREE(sf->vmem);
        PF_FREE(sf->emem);
    }
    vec_sdlev_destroy(&s_script_input);
    vec_sdlev_destroy(&s_script_events);
    vec_td_destroy(&s_script_labels);
    vec_td_destroy(&s_script_labels_prev);
    SDL_DestroyMutex(s_script_lock);
    SDL_DestroyMutex(s_lock);

    const char *key;
    struct nk_font *curr;
    (void)curr;

    kh_foreach(s_fontmap, key, curr, { PF_FREE(key); });
    kh_destroy(font, s_fontmap);

    uint64_t size;
    (void)size;
    kh_foreach(s_image_sizes, key, size, { PF_FREE(key); });
    kh_destroy(imgsize, s_image_sizes);

    R_PushCmd((struct rcmd){ R_GL_UI_Shutdown, 0});
}

void UI_InputBegin(void)
{
    nk_input_begin(&s_ctx);
}

void UI_InputEnd(void)
{
    nk_input_end(&s_ctx);
    /* The engine's windows are drawn in the style the scripts set, as it
     * stood when their last frame ended
     */
    SDL_LockMutex(s_script_lock);
    if(s_script_shown >= 0) {
        const struct script_frame *sf = &s_script_frames[s_script_shown];
        s_ctx.style = sf->style;
        if(sf->font)
            s_active_font = sf->font;
    }
    SDL_UnlockMutex(s_script_lock);
}

void UI_ScriptFrameAsk(void)
{
    s_script_asked_frame = g_frame_idx;
}

bool UI_ScriptFrameAsked(void)
{
    return (g_frame_idx - s_script_asked_frame <= 1);
}

void UI_ScriptFrameBegin(void)
{
    const char *pending = __atomic_exchange_n(&s_script_font_pending, NULL, __ATOMIC_ACQ_REL);
    if(pending)
        UI_SetActiveFont(pending);

    SDL_LockMutex(s_script_lock);
    vec_sdlev_t taken = s_script_input;
    s_script_input = s_script_events;
    s_script_events = taken;
    SDL_UnlockMutex(s_script_lock);

    nk_input_begin(&s_script_ctx);
    for(int i = 0; i < vec_size(&s_script_events); i++) {
        ui_handle_event(&s_script_ctx, &vec_AT(&s_script_events, i), false);
    }
    nk_input_end(&s_script_ctx);
    vec_sdlev_reset(&s_script_events);

    /* The scripts that draw labels and the frames of their windows keep their
     * own time: a frame that no label came in for draws the last one's again,
     * once, so that the labels do not blink when the two fall out of step
     */
    if(vec_size(&s_script_labels) > 0) {
        ui_draw_labels(&s_script_ctx, &s_script_labels);
        vec_td_t drawn = s_script_labels;
        s_script_labels = s_script_labels_prev;
        s_script_labels_prev = drawn;
        vec_td_reset(&s_script_labels);
    }else{
        ui_draw_labels(&s_script_ctx, &s_script_labels_prev);
        vec_td_reset(&s_script_labels_prev);
    }
}

void UI_ScriptFrameEnd(void)
{
    SDL_LockMutex(s_script_lock);
    int back = (s_script_shown < 0) ? 0 : 1 - s_script_shown;
    SDL_UnlockMutex(s_script_lock);

    struct script_frame *sf = &s_script_frames[back];
    if(sf->filled) {
        script_frame_free_userdata(sf);
        nk_buffer_free(&sf->cmds);
        sf->filled = false;
    }

    struct nk_buffer cmds, vbuf, ebuf;
    struct nk_convert_config config = ui_convert_config();
    nk_buffer_init_fixed(&vbuf, sf->vmem, MAX_VERTEX_MEMORY);
    nk_buffer_init_fixed(&ebuf, sf->emem, MAX_ELEMENT_MEMORY);
    nk_buffer_init(&cmds, &s_nk_alloc, 4096);
    nk_convert(&s_script_ctx, &cmds, &vbuf, &ebuf, &config);

    /* The context forgets the list when it is cleared: the frame keeps its
     * own copy of it, and the memory it was laid out in
     */
    sf->list = s_script_ctx.draw_list;
    sf->cmds = cmds;
    sf->verts = vbuf;
    sf->elems = ebuf;
    sf->list.buffer = &sf->cmds;
    sf->list.vertices = &sf->verts;
    sf->list.elements = &sf->elems;
    sf->style = s_script_ctx.style;
    sf->font = s_script_active_font;
    sf->filled = true;
    nk_clear(&s_script_ctx);

    SDL_LockMutex(s_script_lock);
    s_script_shown = back;
    SDL_UnlockMutex(s_script_lock);
}

void UI_HandleEvent(SDL_Event *evt)
{
    bool covered = false;
    if(evt->type == SDL_MOUSEBUTTONDOWN) {
        covered = S_UI_MouseOverObscuringWindow(evt->button.x, evt->button.y);
    }else if(evt->type == SDL_MOUSEWHEEL) {
        int x, y;
        SDL_GetMouseState(&x, &y);
        covered = S_UI_MouseOverObscuringWindow(x, y);
    }
    ui_handle_event(&s_ctx, evt, covered);

    SDL_LockMutex(s_script_lock);
    vec_sdlev_push(&s_script_input, *evt);
    SDL_UnlockMutex(s_script_lock);
}

void UI_DrawText(const char *text, struct rect rect, struct rgba rgba)
{
    struct text_desc d = (struct text_desc){.rect = rect, .rgba = rgba};
    pf_strlcpy(d.text, text, sizeof(d.text));
    ui_label_push(&d);
}

void UI_DrawTextOutlined(const char *text, struct rect rect, struct rgba rgba, struct rgba outline,
                         bool centred)
{
    struct text_desc d = (struct text_desc){.rect = rect, .rgba = rgba, .outline = outline, .centred = centred};
    pf_strlcpy(d.text, text, sizeof(d.text));
    ui_label_push(&d);
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

struct nk_context *UI_GetScriptContext(void)
{
    return &s_script_ctx;
}

const char *UI_GetActiveFont(void)
{
    if(S_Thread_OnScript() && s_script_active_font)
        return s_script_active_font;
    assert(s_active_font);
    return s_active_font;
}

bool UI_SetActiveFont(const char *name)
{
    khiter_t k = kh_get(font, s_fontmap, name);
    if(k == kh_end(s_fontmap))
        return false;

    struct nk_user_font *uf = &kh_value(s_fontmap, k)->handle;
    /* The scripts' font reaches the engine's windows with the style of their
     * next frame
     */
    if(S_Thread_OnScript()) {
        nk_style_set_font(&s_script_ctx, uf);
        s_script_active_font = kh_key(s_fontmap, k);
        __atomic_store_n(&s_script_font_pending, NULL, __ATOMIC_RELEASE);
        return true;
    }
    nk_style_set_font(&s_ctx, uf);
    s_active_font = kh_key(s_fontmap, k);
    return true;
}

void UI_SetScriptFont(const char *name)
{
    khiter_t k = kh_get(font, s_fontmap, name);
    if(k == kh_end(s_fontmap))
        return;
    if(!S_Thread_Running()) {
        nk_style_set_font(&s_script_ctx, &kh_value(s_fontmap, k)->handle);
        s_script_active_font = kh_key(s_fontmap, k);
        return;
    }
    __atomic_store_n(&s_script_font_pending, kh_key(s_fontmap, k), __ATOMIC_RELEASE);
}

void UI_ClearState(void)
{
    UI_SetActiveFont("__default__");
    UI_SetScriptFont("__default__");
}

bool UI_ImageSize(const char *texpath, int *out_w, int *out_h)
{
    /* Both the main thread and the script thread ask */
    SDL_LockMutex(s_lock);
    bool ret = ui_image_size(texpath, out_w, out_h);
    SDL_UnlockMutex(s_lock);
    return ret;
}

void UI_LoadingScreenTick(void)
{
    on_update_ui(NULL, NULL);
    ui_render((void*)(uintptr_t)UI_RENDER_MODE_LOADING_SCREEN, NULL);
}

