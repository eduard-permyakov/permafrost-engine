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

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <SDL.h>

#define CONFIG_SCHED_TARGET_FPS     (60)

/* The far end of the camera's clipping frustrum, in OpenGL coordinates */
#define CONFIG_DRAWDIST             (1000)
#define CONFIG_TILE_TEX_RES         (64)
#define CONFIG_ARR_TEX_RES          (128)
#define CONFIG_LOADING_SCREEN       "assets/loading_screens/default.png"
#define CONFIG_LOADING_WINDOW       "assets/loading_screens/window.png"
#define CONFIG_LOADING_SPRITE       "assets/loading_screens/sprite"

#define CONFIG_SHADOW_MAP_RES       (2048)
/* Upper bound on the draw distance (depth) of the light's frustum. The actual
 * near/far planes are fitted per-frame to the visible area's depth range.
 */
#define CONFIG_SHADOW_DRAWDIST      (4096)
/* Upper bound on the half-extent of the light's (orthographic) frustum, in
 * OpenGL coordinates.
 */
#define CONFIG_SHADOW_MAX_EXTENT    (1536)

/* The RTS camera's yaw, which gameplay never changes */
#define CONFIG_RTS_CAMERA_YAW_DEG   (135.0f)

/* Billboard atlas baking parameters. Folded into the impostor cache tag, so
 * changing any of them invalidates the on-disk atlases.
 */
#define CONFIG_BILLBOARD_AZIMUTHS   (8)
/* Static models at least this elongated in footprint bake the finer azimuth
 * set below: coarse bins visibly rotate a long sprite about its centre, and
 * 72 bins are exactly the editor's 5-degree rotation step.
 */
#define CONFIG_BILLBOARD_ELONGATED_ASPECT   (1.75f)
#define CONFIG_BILLBOARD_AZIMUTHS_ELONGATED (72)
#define CONFIG_BILLBOARD_MAX_KF     (8)
#define CONFIG_BILLBOARD_PX_PER_WU  (4.0f)
#define CONFIG_BILLBOARD_MIN_RES    (32)
#define CONFIG_BILLBOARD_MAX_RES    (128)
/* Models too large to reach this density at CONFIG_BILLBOARD_MAX_RES get no
 * atlas and keep rendering as meshes; they are few and would only blur.
 */
#define CONFIG_BILLBOARD_MIN_PX_PER_WU (2.0f)
/* At most this many stale atlases are re-baked per frame, so a mid-session
 * variant change (shadow toggle, light change) is a spread of small hitches
 * rather than one long stall.
 */
#define CONFIG_BILLBOARD_REBAKES_PER_FRAME (1)
/* The light must move by this much before the atlases re-bake, so an animated
 * day-night light re-bakes a handful of times per cycle rather than per frame.
 */
#define CONFIG_BILLBOARD_LIGHT_REBAKE_DEG (10.0f)
/* Statics billboard only when the active camera's pitch is within this many
 * degrees of the RTS camera's, the elevation the sprites are baked for.
 */
#define CONFIG_BILLBOARD_MAX_PITCH_DEV_DEG (2.5f)
/* Depth extent of the bake's light frustum. Kept wide regardless of model size
 * so the shaders' constant depth bias stays reasonable in world units and the
 * model sits well inside the shadow lookup's valid depth band.
 */
#define CONFIG_BILLBOARD_SHADOW_DEPTH_RANGE (250.0f)
#define CONFIG_BILLBOARD_CACHE_VER  (3)

#define CONFIG_SETTINGS_FILENAME    "pf.conf"

#define CONFIG_LOS_CACHE_SZ         (2048)
#define CONFIG_FLOW_CACHE_SZ        (2048)
#define CONFIG_MAPPING_CACHE_SZ     (4096)
#define CONFIG_GRID_PATH_CACHE_SZ   (8192)

#define CONFIG_FRAME_STEP_HOTKEY    (SDL_SCANCODE_SPACE)

#endif
