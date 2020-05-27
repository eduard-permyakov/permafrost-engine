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

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <SDL.h>

/* The maximum nubmer of entities we are ever going to render in a single shot. 
 * In a typical shot, the nubmer will be much less... */
#define CONFIG_MAX_ENTS_RENDER      (4096)

/* The far end of the camera's clipping frustrum, in OpenGL coordinates */
#define CONFIG_DRAWDIST             (1000)
#define CONFIG_TILE_TEX_RES         (128)
#define CONFIG_ARR_TEX_RES          (512)
#define CONFIG_LOADING_SCREEN       "assets/loading_screens/battle_of_kulikovo.png"

#define CONFIG_SHADOW_MAP_RES       (2048)
/* Determines the draw distance from the light source when creating the
 * shadow map. Note that a higher drawdistance leads to more peterpanning.
 */
#define CONFIG_SHADOW_DRAWDIST      (1536)
/* This is the half-width of the light source's frustum, in OpenGL coordinates.
 * Increasing the FOV results in lower-quality shadows for the same shadow map 
 * resolution. However, the light frustum needs to be sufficiently large to 
 * contain all shadow casters visible by the RTS camera.
 */
#define CONFIG_SHADOW_FOV           (160)

#define CONFIG_SETTINGS_FILENAME    "pf.conf"

#define CONFIG_LOS_CACHE_SZ         (512)
#define CONFIG_FLOW_CAHCE_SZ        (512)
#define CONFIG_MAPPING_CACHE_SZ     (512)
#define CONFIG_GRID_PATH_CACHE_SZ   (8192)

#define CONFIG_FRAME_STEP_HOTKEY    (SDL_SCANCODE_SPACE)

#endif
