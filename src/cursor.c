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
 */

#include "cursor.h"
#include "config.h"

#include <SDL.h>

#include <string.h>
#include <assert.h>

#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))

struct cursor_resource{
    SDL_Cursor  *cursor;     
    SDL_Surface *surface;
    const char  *path;
    size_t       hot_x, hot_y;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct cursor_resource s_cursors[] = {
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
};

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Cursor_InitAll(const char *basedir)
{
    for(int i = 0; i < ARR_SIZE(s_cursors); i++) {
    
        struct cursor_resource *curr = &s_cursors[i];

        char path[512];
        strcpy(path, basedir);
        strcat(path, curr->path);

        curr->surface = SDL_LoadBMP(path);
        if(!curr->surface)
            goto fail;

        curr->cursor = SDL_CreateColorCursor(curr->surface, curr->hot_x, curr->hot_y);
        if(!curr->cursor)
            goto fail;
    }

    return true;

fail:
    Cursor_FreeAll();
    return false;
}

void Cursor_FreeAll(void)
{
    for(int i = 0; i < ARR_SIZE(s_cursors); i++) {
    
        struct cursor_resource *curr = &s_cursors[i];
        
        if(curr->surface) SDL_FreeSurface(curr->surface);
        if(curr->cursor)  SDL_FreeCursor(curr->cursor);
    }
}

void Cursor_SetActive(enum cursortype type)
{
    assert(type >= 0 && type < ARR_SIZE(s_cursors));
    SDL_SetCursor(s_cursors[type].cursor);
}

void Cursor_RTS_SetActive(int mouse_x, int mouse_y)
{
    bool top = (mouse_y == 0);
    bool bot = (mouse_y == CONFIG_RES_Y - 1);
    bool left  = (mouse_x == 0);
    bool right = (mouse_x == CONFIG_RES_X - 1);

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
        Cursor_SetActive(CURSOR_POINTER); 
    }
}

