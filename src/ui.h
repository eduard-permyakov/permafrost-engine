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

#ifndef UI_H
#define UI_H


#include "lib/public/nuklear.h"
#include "lib/public/nuklear_sdl_gl3.h"

#include <stdbool.h>
#include <string.h>

#define MAX_VERTEX_MEMORY  (512 * 1024)
#define MAX_ELEMENT_MEMORY (128 * 1024)

/*****************************************************************************/
/* INLINE FUNCTIONS                                                          */
/*****************************************************************************/

inline struct nk_context *UI_Init(const char *basedir, SDL_Window *win)
{
    struct nk_context *ctx = nk_sdl_init(win);
    if(!ctx)
        return NULL;

    struct nk_font_atlas *atlas;
    char font_path[256];

    strcpy(font_path, basedir);
    strcat(font_path, "assets/fonts/OptimusPrinceps.ttf");

    nk_sdl_font_stash_begin(&atlas);
    struct nk_font *optimus_princeps = nk_font_atlas_add_from_file(atlas, font_path, 14, 0);

    atlas->default_font = optimus_princeps;
    nk_sdl_font_stash_end();

    return ctx;
}

inline void UI_Shutdown(void)
{
    nk_sdl_shutdown();
}

inline void UI_InputBegin(struct nk_context *ctx)
{
    nk_input_begin(ctx);
}

inline void UI_InputEnd(struct nk_context *ctx)
{
    nk_input_end(ctx);
}

inline void UI_Render(void)
{
    nk_sdl_render(NK_ANTI_ALIASING_ON, MAX_VERTEX_MEMORY, MAX_ELEMENT_MEMORY);
}

#endif

