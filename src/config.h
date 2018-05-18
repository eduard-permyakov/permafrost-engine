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

#ifndef CONFIG_H
#define CONFIG_H

#include <SDL.h>
#include <stdbool.h>

enum pf_window_flags{
    PF_WINDOWFLAGS_FULLSCREEN          = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_INPUT_GRABBED,
    PF_WINDOWFLAGS_BORDERLESS_WINDOWED = SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_INPUT_GRABBED
};

/* The far end of the camera's clipping frustrum, in OpenGL coordinates */
#define CONFIG_DRAWDIST             1000.0f
#define CONFIG_RES_X                1920
#define CONFIG_RES_Y                1080
#define CONFIG_BAKED_TILE_TEX_RES   128
#define CONFIG_WINDOWFLAGS          PF_WINDOWFLAGS_BORDERLESS_WINDOWED
#define CONFIG_VSYNC                false

#endif
