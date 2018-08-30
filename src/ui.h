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

#include <SDL.h>

struct nk_context;

struct rect{
    float x, y, w, h;
};

struct rgba{
    unsigned char r, g, b, a;
};

struct nk_context *UI_Init(const char *basedir, SDL_Window *win);
void               UI_Shutdown(void);
void               UI_InputBegin(struct nk_context *ctx);
void               UI_InputEnd(struct nk_context *ctx);
void               UI_Render(void);
void               UI_HandleEvent(SDL_Event *event);
void               UI_DrawText(const char *text, struct rect rect, struct rgba rgba);

#endif

