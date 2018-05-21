/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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

#include "selection.h"
#include "public/game.h"
#include "../pf_math.h"
#include "../event.h"
#include "../render/public/render.h"
#include "../config.h"

#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include <SDL.h>

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct selection_ctx{
    bool   installed;
    vec2_t mouse_down_coord;
    vec2_t mouse_up_coord;
    bool   mouse_down_in_map;
    bool   mouse_released;
}s_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);
    s_ctx.mouse_released = false;

    if(mouse_event->button != SDL_BUTTON_LEFT) {
        s_ctx.mouse_down_in_map = false;
        return;
    }

    if(G_MouseOverMinimap()) {
        s_ctx.mouse_down_in_map = false;
        return;
    }

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y)) {
        s_ctx.mouse_down_in_map = false;
        return;
    }

    /* Don't allow dragging a selection box when the mouse is at the edges of 
     * the screen (camera pan action) which is mutually exclusive to unit selection. */
    if(mouse_event->x == 0 || mouse_event->x == CONFIG_RES_X-1
    || mouse_event->y == 0 || mouse_event->y == CONFIG_RES_Y-1) {
        s_ctx.mouse_down_in_map = false;
        return;
    }

    s_ctx.mouse_down_in_map = true;
    s_ctx.mouse_down_coord = (vec2_t){mouse_event->x, mouse_event->y};
}

static void on_mouseup(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);
    s_ctx.mouse_released = true;
    s_ctx.mouse_up_coord = (vec2_t){mouse_event->x, mouse_event->y};
}

static void on_render(void *user, void *event)
{
    if(!(s_ctx.mouse_down_in_map && !s_ctx.mouse_released))
        return;

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    vec2_t signed_size = (vec2_t){mouse_x - s_ctx.mouse_down_coord.x, mouse_y - s_ctx.mouse_down_coord.y};
    R_GL_DrawBox2D(s_ctx.mouse_down_coord, signed_size, (vec3_t){0.0f, 1.0f, 0.0f}, 2.0f);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void G_Sel_Install(void)
{
    if(s_ctx.installed)
        return;
    s_ctx.installed = true;

    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL);
    E_Global_Register(SDL_MOUSEBUTTONUP,   on_mouseup, NULL);
    E_Global_Register(EVENT_RENDER_UI,     on_render, NULL);
}

void G_Sel_Uninstall(void)
{
    if(!s_ctx.installed)
        return;
    s_ctx.installed = false;

    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    E_Global_Unregister(SDL_MOUSEBUTTONUP,   on_mouseup);
    E_Global_Unregister(EVENT_RENDER_UI,     on_render);

    G_Sel_Clear();
}

void G_Sel_Clear(void)
{
    bool installed = s_ctx.installed;
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.installed = installed;
}

