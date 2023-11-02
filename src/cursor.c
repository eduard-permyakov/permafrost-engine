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

#include "cursor.h"
#include "config.h"
#include "event.h"
#include "main.h"
#include "sched.h"
#include "game/public/game.h"
#include "lib/public/pf_string.h"
#include "lib/public/khash.h"

#include <SDL.h>

#include <string.h>
#include <assert.h>

#define CHK_TRUE_RET(_pred)   \
    do{                       \
        if(!(_pred))          \
            return false;     \
    }while(0)

#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))

struct cursor_resource{
    SDL_Cursor  *cursor;     
    SDL_Surface *surface;
    char         path[512];
    size_t       hot_x, hot_y;
};

KHASH_MAP_INIT_STR(cursor, struct cursor_resource)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct cursor_resource s_default[_CURSOR_MAX] = {
    [CURSOR_POINTER] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/pointer.bmp",
        .hot_x   = 0,
        .hot_y   = 0
    },
    [CURSOR_SCROLL_TOP] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_top.bmp",
        .hot_x   = 16,
        .hot_y   = 0
    },
    [CURSOR_SCROLL_TOP_RIGHT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_top_right.bmp",
        .hot_x   = 31,
        .hot_y   = 0
    },
    [CURSOR_SCROLL_RIGHT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_right.bmp",
        .hot_x   = 31,
        .hot_y   = 16 
    },
    [CURSOR_SCROLL_BOT_RIGHT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_bot_right.bmp",
        .hot_x   = 31,
        .hot_y   = 31
    },
    [CURSOR_SCROLL_BOT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_bot.bmp",
        .hot_x   = 16,
        .hot_y   = 31 
    },
    [CURSOR_SCROLL_BOT_LEFT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_bot_left.bmp",
        .hot_x   = 0,
        .hot_y   = 31
    },
    [CURSOR_SCROLL_LEFT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_left.bmp",
        .hot_x   = 0,
        .hot_y   = 16
    },
    [CURSOR_SCROLL_TOP_LEFT] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/scroll_top_left.bmp",
        .hot_x   = 0,
        .hot_y   = 0
    },
    [CURSOR_TARGET] = {
        .cursor  = NULL,
        .surface = NULL,
        .path    = "assets/cursors/target.bmp",
        .hot_x   = 24,
        .hot_y   = 24 
    },
};

static bool                    s_moved = false;
static bool                    s_rts_mode = false;
static SDL_Cursor             *s_rts_pointer;
static khash_t(cursor)        *s_named_cursors;
static struct cursor_resource  s_cursors[_CURSOR_MAX];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void cursor_rts_set_active(int mouse_x, int mouse_y)
{
    if(!s_rts_mode) {
        return;
    }

    if(!s_moved) {
        SDL_SetCursor(s_rts_pointer);
        return;
    }

    int width, height;
    Engine_WinDrawableSize(&width, &height);

    bool top = (mouse_y == 0);
    bool bot = (mouse_y == height - 1);
    bool left  = (mouse_x == 0);
    bool right = (mouse_x == width - 1);

    /* Check the corners first, then edges */
    if(top && left) {
        Cursor_SetActive(CURSOR_SCROLL_TOP_LEFT); 
    }else if(top && right) {
        Cursor_SetActive(CURSOR_SCROLL_TOP_RIGHT); 
    }else if(bot && left) {
        Cursor_SetActive(CURSOR_SCROLL_BOT_LEFT); 
    }else if(bot && right) {
        Cursor_SetActive(CURSOR_SCROLL_BOT_RIGHT); 
    }else if(top) {
        Cursor_SetActive(CURSOR_SCROLL_TOP); 
    }else if(bot) {
        Cursor_SetActive(CURSOR_SCROLL_BOT); 
    }else if(left) {
        Cursor_SetActive(CURSOR_SCROLL_LEFT); 
    }else if(right) {
        Cursor_SetActive(CURSOR_SCROLL_RIGHT); 
    }else {
        SDL_SetCursor(s_rts_pointer);
    }
}

static void cursor_on_mousemove(void *unused1, void *unused2)
{
    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    
    s_moved = true;
    cursor_rts_set_active(mouse_x, mouse_y); 
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void Cursor_SetRTSMode(bool on)
{
    if(s_rts_mode == on)
        return;
    if(on) {
        E_Global_Register(SDL_MOUSEMOTION, cursor_on_mousemove, NULL, G_RUNNING);
    }else {
        E_Global_Unregister(SDL_MOUSEMOTION, cursor_on_mousemove);
    }
    s_rts_mode = on;
}

bool Cursor_GetRTSMode(void)
{
    return s_rts_mode;
}

bool Cursor_InitDefault(const char *basedir)
{
    s_named_cursors = kh_init(cursor);
    if(!s_named_cursors)
        return false;

    memcpy(s_cursors, s_default, sizeof(s_cursors));

    for(int i = 0; i < ARR_SIZE(s_cursors); i++) {
    
        struct cursor_resource *curr = &s_cursors[i];

        if(!curr->path 
        || !Cursor_LoadBMP(i, curr->path, curr->hot_x, curr->hot_y)) {

            curr->cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
            if(!curr->cursor)
                goto fail;
        }
    }

    s_rts_pointer = s_cursors[CURSOR_POINTER].cursor;
    return true;

fail:
    Cursor_FreeAll();
    return false;
}

void Cursor_FreeAll(void)
{
    const char *key;
    struct cursor_resource curr;

    kh_foreach(s_named_cursors, key, curr, {
        free((void*)key);
        SDL_FreeSurface(curr.surface);
        SDL_FreeCursor(curr.cursor);
    });
    kh_destroy(cursor, s_named_cursors);

    for(int i = 0; i < ARR_SIZE(s_cursors); i++) {
    
        struct cursor_resource *curr = &s_cursors[i];
        SDL_FreeSurface(curr->surface);
        SDL_FreeCursor(curr->cursor);
        curr->surface = NULL;
        curr->cursor = NULL;
    }
}

void Cursor_SetActive(enum cursortype type)
{
    assert(type >= 0 && type < ARR_SIZE(s_cursors));
    SDL_SetCursor(s_cursors[type].cursor);
}

bool Cursor_LoadBMP(enum cursortype type, const char *path, int hotx, int hoty)
{
    if(type < 0 || type >= _CURSOR_MAX)
        return false;

    char fullpath[512];
    pf_snprintf(fullpath, sizeof(fullpath), "%s/%s", g_basepath, path);

    SDL_Cursor *cursor = NULL;
    SDL_Surface *surface = SDL_LoadBMP(fullpath);
    if(!surface)
        goto fail;

    cursor = SDL_CreateColorCursor(surface, hotx, hoty);
    if(!cursor)
        goto fail;

    struct cursor_resource *curr = &s_cursors[type];
    SDL_FreeSurface(curr->surface);
    SDL_FreeCursor(curr->cursor);

    curr->cursor = cursor;
    curr->surface = surface;
    curr->hot_x = hotx;
    curr->hot_y = hoty;
    pf_snprintf(curr->path, sizeof(curr->path), path);

    return true;

fail:
    SDL_FreeSurface(surface);
    SDL_FreeCursor(cursor);
    return false;
}

bool Cursor_NamedLoadBMP(const char *name, const char *path, int hotx, int hoty)
{
    char fullpath[512];
    pf_snprintf(fullpath, sizeof(fullpath), "%s/%s", g_basepath, path);

    SDL_Cursor *cursor = NULL;
    SDL_Surface *surface = SDL_LoadBMP(fullpath);
    if(!surface)
        goto fail;

    cursor = SDL_CreateColorCursor(surface, hotx, hoty);
    if(!cursor)
        goto fail;

    struct cursor_resource entry = (struct cursor_resource) {
        .cursor  = cursor,
        .surface = surface,
        .hot_x   = hotx,
        .hot_y   = hoty
    };
    pf_snprintf(entry.path, sizeof(entry.path), path);

    int status;
    khiter_t k = kh_put(cursor, s_named_cursors, pf_strdup(name), &status);
    if(status == -1)
        goto fail;
    kh_value(s_named_cursors, k) = entry;

    return true;

fail:
    SDL_FreeSurface(surface);
    SDL_FreeCursor(cursor);
    return false;
}

bool Cursor_NamedSetActive(const char *name)
{
    khiter_t k = kh_get(cursor, s_named_cursors, name);
    if(k == kh_end(s_named_cursors))
        return false;

    struct cursor_resource *entry = &kh_value(s_named_cursors, k);
    SDL_SetCursor(entry->cursor);
    return true;
}

void Cursor_SetRTSPointer(enum cursortype type)
{
    assert(type >= 0 && type < ARR_SIZE(s_cursors));

    struct cursor_resource *curr = &s_cursors[type];
    s_rts_pointer = curr->cursor;

    int x, y;
    SDL_GetMouseState(&x, &y);
    cursor_rts_set_active(x, y);
}

bool Cursor_NamedSetRTSPointer(const char *name)
{
    khiter_t k = kh_get(cursor, s_named_cursors, name);
    if(k == kh_end(s_named_cursors))
        return false;

    struct cursor_resource *entry = &kh_value(s_named_cursors, k);
    s_rts_pointer = entry->cursor;

    int x, y;
    SDL_GetMouseState(&x, &y);
    cursor_rts_set_active(x, y);
    return true;
}

void Cursor_ClearState(void)
{
    Cursor_SetRTSMode(false);
    Cursor_FreeAll();
    Cursor_InitDefault(g_basepath);
    Cursor_SetActive(CURSOR_POINTER);
}

bool Cursor_SaveState(struct SDL_RWops *stream)
{
    struct attr rts_mode = (struct attr){
        .type = TYPE_BOOL, 
        .val.as_bool = s_rts_mode
    };
    CHK_TRUE_RET(Attr_Write(stream, &rts_mode, "rts_mode"));

    struct attr ncursors = (struct attr){
        .type = TYPE_INT, 
        .val.as_int = kh_size(s_named_cursors)
    };
    CHK_TRUE_RET(Attr_Write(stream, &ncursors, "ncursors"));

    Sched_TryYield();

    const char *key;
    struct cursor_resource curr;

    kh_foreach(s_named_cursors, key, curr, {
    
        struct attr cursor_name = (struct attr){
            .type = TYPE_STRING, 
        };
        pf_strlcpy(cursor_name.val.as_string, key, sizeof(cursor_name.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &cursor_name, "cursor_name"));

        struct attr cursor_path = (struct attr){
            .type = TYPE_STRING, 
        };
        pf_strlcpy(cursor_path.val.as_string, curr.path, sizeof(cursor_path.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &cursor_path, "cursor_path"));

        struct attr hotx = (struct attr){
            .type = TYPE_INT, 
            .val.as_int = curr.hot_x
        };
        CHK_TRUE_RET(Attr_Write(stream, &hotx, "hotx"));

        struct attr hoty = (struct attr){
            .type = TYPE_INT, 
            .val.as_int = curr.hot_y
        };
        CHK_TRUE_RET(Attr_Write(stream, &hoty, "hoty"));

        Sched_TryYield();
    });

    enum cursortype type = CURSOR_POINTER;
    for(int i = 0; i < ARR_SIZE(s_cursors); i++) {
        if(s_cursors[i].cursor == s_rts_pointer) {
            type = i;
            break;
        }
    }

    struct attr cursortype = (struct attr){
        .type = TYPE_INT, 
        .val.as_int = type
    };
    CHK_TRUE_RET(Attr_Write(stream, &cursortype, "cursortype"));

    size_t nsystem = 0;
    for(int i = 0; i < ARR_SIZE(s_cursors); i++) {
        if(!strlen(s_cursors[i].path))
            continue;
        nsystem++;
    }
    struct attr nsystem_attr = (struct attr){
        .type = TYPE_INT, 
        .val.as_int = nsystem
    };
    CHK_TRUE_RET(Attr_Write(stream, &nsystem_attr, "nsystem"));

    for(int i = 0; i < ARR_SIZE(s_cursors); i++) {

        struct cursor_resource *curr = &s_cursors[i];
        if(!strlen(curr->path))
            continue;

        struct attr type = (struct attr){
            .type = TYPE_INT, 
            .val.as_int = i
        };
        CHK_TRUE_RET(Attr_Write(stream, &type, "type"));

        struct attr path = (struct attr){
            .type = TYPE_STRING, 
        };
        pf_snprintf(path.val.as_string, sizeof(path.val.as_string), curr->path);
        CHK_TRUE_RET(Attr_Write(stream, &path, "path"));

        struct attr hotx = (struct attr){
            .type = TYPE_INT, 
            .val.as_int = curr->hot_x
        };
        CHK_TRUE_RET(Attr_Write(stream, &hotx, "hotx"));

        struct attr hoty = (struct attr){
            .type = TYPE_INT, 
            .val.as_int = curr->hot_y
        };
        CHK_TRUE_RET(Attr_Write(stream, &hoty, "hoty"));

        Sched_TryYield();
    }

    return true;
}

bool Cursor_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_BOOL);
    Cursor_SetRTSMode(attr.val.as_bool);

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    size_t ncursors = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < ncursors; i++) {

        struct attr name;
        CHK_TRUE_RET(Attr_Parse(stream, &name, true));
        CHK_TRUE_RET(name.type == TYPE_STRING);

        struct attr path;
        CHK_TRUE_RET(Attr_Parse(stream, &path, true));
        CHK_TRUE_RET(path.type == TYPE_STRING);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int hotx = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int hoty = attr.val.as_int;

        Cursor_NamedLoadBMP(name.val.as_string, path.val.as_string, hotx, hoty);
        Sched_TryYield();
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    enum cursortype type = attr.val.as_int;
    CHK_TRUE_RET(type >= 0 && type < _CURSOR_MAX);

    Cursor_SetRTSPointer(type);

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    size_t nsystem = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < nsystem; i++) {

        struct attr type;
        CHK_TRUE_RET(Attr_Parse(stream, &type, true));
        CHK_TRUE_RET(type.type == TYPE_INT);

        struct attr path;
        CHK_TRUE_RET(Attr_Parse(stream, &path, true));
        CHK_TRUE_RET(path.type == TYPE_STRING);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int hotx = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int hoty = attr.val.as_int;

        Cursor_LoadBMP(type.val.as_int, path.val.as_string, hotx, hoty);
        Sched_TryYield();
    }

    return true;
}

