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

#ifndef CONFIG_H
#define CONFIG_H

#include <SDL.h>
#include <stdbool.h>

enum pf_window_flags{
    PF_WINDOWFLAGS_FULLSCREEN          = SDL_WINDOW_FULLSCREEN 
                                       | SDL_WINDOW_INPUT_GRABBED,
    PF_WINDOWFLAGS_BORDERLESS_WINDOWED = SDL_WINDOW_BORDERLESS 
                                       | SDL_WINDOW_ALWAYS_ON_TOP 
                                       | SDL_WINDOW_INPUT_GRABBED
};

/* The far end of the camera's clipping frustrum, in OpenGL coordinates */
#define CONFIG_DRAWDIST             1000
#define CONFIG_RES_X                1920
#define CONFIG_RES_Y                1080
#define CONFIG_BAKED_TILE_TEX_RES   128
#define CONFIG_WINDOWFLAGS          PF_WINDOWFLAGS_BORDERLESS_WINDOWED
#define CONFIG_VSYNC                false
#define CONFIG_SHADOW_MAP_RES       2048
/* Determines the draw distance from the light source when creating the
 * shadow map. A higher drawdistance leads to more peterpanning. */
#define CONFIG_SHADOW_DRAWDIST      512
/* This is the half-width of the light source's frustum, in OpenGL coordinates.
 * A value is 128 is enough for all visible shadows to be rendered when 
 * viewing the scene from the standard RTS camera. When looking around with
 * an FPS camera, objects at the edges of the camera may not be properly
 * shadowed. However, increasing the FOV results in lower-quality shadows
 * for the same shadow map resolution. */
#define CONFIG_SHADOW_FOV           128

#endif
