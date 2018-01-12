#ifndef CAM_CONTROL_H
#define CAM_CONTROL_H

#include "camera.h"

#include <SDL2/SDL.h>

struct camcontrol_ctx;

struct camcontrol_ctx *CamControl_CtxNew(void);
void                   CamControl_CtxFree(struct camcontrol_ctx *ctx);

void                   CamControl_FPS_HandleEvent(struct camcontrol_ctx *ctx, struct camera *cam, SDL_Event e);
void                   CamControl_RTS_HandleEvent(struct camcontrol_ctx *ctx, struct camera *cam, SDL_Event e);

/* This will call Camera_TickFinish on the cam */
void                   CamControl_TickFinish     (struct camcontrol_ctx *ctx, struct camera *cam);

#endif
