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

#ifndef UI_H
#define UI_H

#include "pf_math.h"
#include <SDL.h>

struct nk_context;

struct rect{
    int x, y, w, h;
};

struct rgba{
    unsigned char r, g, b, a;
};

/* When the window is anchored to more than one point in a single dimension, the aspect ratio of the window 
 * will change as the screen aspect ratio changes. For example, anchoring a full-screen window to the top,
 * bottom, left and right edges will ensure that it always takes up the entire screen regardless of resolution.
 * When a window is anchored to only one point in each dimension, the aspect ratio will stay constant. */
enum resize_opts{
    /* Distance between window's left edge and screen's left edge is constant */
    ANCHOR_X_LEFT       = (1 << 0), 
    /* Distance between window's right edge and screen's right edge is constant */
    ANCHOR_X_RIGHT      = (1 << 1),
    /* Distance between window's horizontal center and the screen's horizontal center is constant */
    ANCHOR_X_CENTER     = (1 << 2),
    ANCHOR_Y_TOP        = (1 << 3),
    ANCHOR_Y_BOT        = (1 << 4),
    ANCHOR_Y_CENTER     = (1 << 5),
    ANCHOR_DEFAULT      = ANCHOR_X_LEFT | ANCHOR_Y_TOP,
    ANCHOR_X_MASK       = ANCHOR_X_LEFT | ANCHOR_X_CENTER | ANCHOR_X_RIGHT,
    ANCHOR_Y_MASK       = ANCHOR_Y_TOP | ANCHOR_Y_CENTER | ANCHOR_Y_BOT
};

struct nk_context *UI_Init(const char *basedir, SDL_Window *win);
void               UI_Shutdown(void);
void               UI_InputBegin(struct nk_context *ctx);
void               UI_InputEnd(struct nk_context *ctx);
void               UI_Render(void);
void               UI_HandleEvent(SDL_Event *event);
void               UI_DrawText(const char *text, struct rect rect, struct rgba rgba);

/* Returns a trimmed version of the virtual resolution when the aspect ratio of the window is 
 * different than the virtual resolution aspect ratio. The adjusted resolution has the same
 * aspect ratio as the window and has the largest possible dimensions such that it still 'fits'
 * into the original virtual resolution. */
vec2_t             UI_ArAdjustedVRes(vec2_t vres);
struct rect        UI_BoundsForAspectRatio(struct rect from_bounds, vec2_t from_res, 
                                           vec2_t to_res, int resize_mask);

#endif

