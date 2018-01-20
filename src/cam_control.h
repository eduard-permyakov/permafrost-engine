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

#ifndef CAM_CONTROL_H
#define CAM_CONTROL_H

#include "camera.h"

#include <SDL2/SDL.h>

struct cam_fps_ctx;
struct cam_rts_ctx;


struct cam_fps_ctx *CamControl_FPS_CtxNew     (void);
void                CamControl_FPS_CtxFree    (struct cam_fps_ctx *ctx);
void                CamControl_FPS_HandleEvent(struct cam_fps_ctx *ctx, struct camera *cam, SDL_Event *e);
/* This will call Camera_TickFinish on the cam */
void                CamControl_FPS_TickFinish (struct cam_fps_ctx *ctx, struct camera *cam);
/* Locks mouse to the current window for use with FPS camera, removes cursor image */
void                CamControl_FPS_SetMouseMode(void);


struct cam_rts_ctx *CamControl_RTS_CtxNew     (void);
void                CamControl_RTS_CtxFree    (struct cam_rts_ctx *ctx);
void                CamControl_RTS_HandleEvent(struct cam_rts_ctx *ctx, struct camera *cam, SDL_Event *e);
/* This will call Camera_TickFinish on the cam */
void                CamControl_RTS_TickFinish (struct cam_rts_ctx *ctx, struct camera *cam);
/* Allows free movement of cursor around the window */
void                CamControl_RTS_SetMouseMode(void);


#endif
