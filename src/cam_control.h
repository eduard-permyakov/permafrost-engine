#ifndef CAM_CONTROL_H
#define CAM_CONTROL_H

#include "camera.h"

#include <SDL2/SDL.h>

struct cam_fps_ctx;
struct cam_rts_ctx;


struct cam_fps_ctx *CamControl_FPS_CtxNew     (void);
void                CamControl_FPS_CtxFree    (struct cam_fps_ctx *ctx);
void                CamControl_FPS_HandleEvent(struct cam_fps_ctx *ctx, struct camera *cam, SDL_Event e);
/* This will call Camera_TickFinish on the cam */
void                CamControl_FPS_TickFinish (struct cam_fps_ctx *ctx, struct camera *cam);
/* Locks mouse to the current window for use with FPS camera, removes cursor image */
void                CamControl_FPS_SetMouseMode(void);


struct cam_rts_ctx *CamControl_RTS_CtxNew     (void);
void                CamControl_RTS_CtxFree    (struct cam_rts_ctx *ctx);
void                CamControl_RTS_HandleEvent(struct cam_rts_ctx *ctx, struct camera *cam, SDL_Event e);
/* This will call Camera_TickFinish on the cam */
void                CamControl_RTS_TickFinish (struct cam_rts_ctx *ctx, struct camera *cam);
/* Allows free movement of cursor around the window */
void                CamControl_RTS_SetMouseMode(void);


#endif
