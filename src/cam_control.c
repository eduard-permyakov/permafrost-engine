#include "cam_control.h"

#include <stdbool.h>


struct camcontrol_ctx{
    bool w_down;
    bool a_down;
    bool s_down;
    bool d_down;
};

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

struct camcontrol_ctx *CamControl_CtxNew(void)
{
    struct camcontrol_ctx *ret = calloc(1, sizeof(struct camcontrol_ctx)); 
    if(!ret)
        return NULL;

    return ret;
}

void CamControl_CtxFree(struct camcontrol_ctx *ctx)
{
    free(ctx);
}

void CamControl_FPS_HandleEvent(struct camcontrol_ctx *ctx, struct camera *cam, SDL_Event e)
{
    switch(e.type) {

    case SDL_KEYDOWN:
        switch(e.key.keysym.scancode) {
        case SDL_SCANCODE_W: ctx->w_down = true; break;
        case SDL_SCANCODE_A: ctx->a_down = true; break;
        case SDL_SCANCODE_S: ctx->s_down = true; break;
        case SDL_SCANCODE_D: ctx->d_down = true; break;
        
        }
    
        break;
    
    case SDL_KEYUP:
        switch(e.key.keysym.scancode) {
        case SDL_SCANCODE_W: ctx->w_down = false; break;
        case SDL_SCANCODE_A: ctx->a_down = false; break;
        case SDL_SCANCODE_S: ctx->s_down = false; break;
        case SDL_SCANCODE_D: ctx->d_down = false; break;
        }

        break;

    case SDL_MOUSEMOTION: 
        Camera_ChangeDirection(cam, e.motion.xrel, e.motion.yrel);
        break;
    };

}

void CamControl_RTS_HandleEvent(struct camcontrol_ctx *ctx, struct camera *cam, SDL_Event e)
{

}

void CamControl_TickFinish(struct camcontrol_ctx *ctx, struct camera *cam)
{
    if(ctx->w_down) Camera_MoveFrontTick(cam);
    if(ctx->a_down) Camera_MoveLeftTick (cam);
    if(ctx->s_down) Camera_MoveBackTick (cam);
    if(ctx->d_down) Camera_MoveRightTick(cam);

    Camera_TickFinish(cam);
}

