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

#include "public/render.h"
#include "shader.h"
#include "texture.h"
#include "render_gl.h"
#include "../settings.h"
#include "../main.h"

#include <SDL.h>
#include <assert.h>
#include <math.h>

#define EPSILON (1.0f/1024)

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool ar_validate(const struct sval *new_val)
{
    if(new_val->type != ST_TYPE_VEC2)
        return false;

    float AR_MIN = 0.5f, AR_MAX = 2.5f;
    return (new_val->as_vec2.x / new_val->as_vec2.y >= AR_MIN)
        && (new_val->as_vec2.x / new_val->as_vec2.y <= AR_MAX);
}

static void ar_commit(const struct sval *new_val)
{
    struct sval res;
    ss_e status = Settings_Get("pf.video.resolution", &res);
    if(status == SS_NO_SETTING)
        return;

    assert(status == SS_OKAY);
    float curr_ratio = res.as_vec2.x/res.as_vec2.y;
    float new_ratio = new_val->as_vec2.x/new_val->as_vec2.y;
    if(fabs(new_ratio - curr_ratio) < EPSILON)
        return;

    /* Here, we choose to always decrease a dimension rather than 
     * increase one so the window continues to fit on the screen */
    struct sval new_res = {.type = ST_TYPE_VEC2};
    if(new_ratio > curr_ratio) {

        new_res.as_vec2 = (vec2_t){
            .x = res.as_vec2.x,
            .y = res.as_vec2.y / (new_ratio/curr_ratio)
        };
    }else{
    
        new_res.as_vec2 = (vec2_t){
            .x = res.as_vec2.x / (curr_ratio/new_ratio),
            .y = res.as_vec2.y
        };
    }

    status = Settings_SetNoValidate("pf.video.resolution", &new_res);
    assert(status == SS_OKAY);
}

static bool res_validate(const struct sval *new_val)
{
    if(new_val->type != ST_TYPE_VEC2)
        return false;

    struct sval ar;
    ss_e status = Settings_Get("pf.video.aspect_ratio", &ar);
    if(status != SS_NO_SETTING) {

        assert(status == SS_OKAY);
        float set_ar = ar.as_vec2.x / ar.as_vec2.y;
        if(fabs(new_val->as_vec2.x / new_val->as_vec2.y - set_ar) > EPSILON)
            return false;
    }

    const int DIM_MIN = 360, DIM_MAX = 5120;

    return (new_val->as_vec2.x >= DIM_MIN && new_val->as_vec2.x <= DIM_MAX)
        && (new_val->as_vec2.y >= DIM_MIN && new_val->as_vec2.y <= DIM_MAX);
}

static void res_commit(const struct sval *new_val)
{
    int rval = Engine_SetRes(new_val->as_vec2.x, new_val->as_vec2.y);
    assert(0 == rval || fprintf(stderr, "Failed to set window resolution:%s\n", SDL_GetError()));

    int width, height;
    Engine_WinDrawableSize(&width, &height);
    glViewport(0, 0, width, height);
}

static bool dm_validate(const struct sval *new_val)
{
    assert(new_val->type == ST_TYPE_INT);
    if(new_val->type != ST_TYPE_INT)
        return false;

    return new_val->as_int == PF_WF_FULLSCREEN
        || new_val->as_int == PF_WF_BORDERLESS_WIN
        || new_val->as_int == PF_WF_WINDOW;
}

static void dm_commit(const struct sval *new_val)
{
    Engine_SetDispMode(new_val->as_int);
}

static bool win_on_top_validate(const struct sval *new_val)
{
    return (new_val->type == ST_TYPE_BOOL);
}

static bool vsync_validate(const struct sval *new_val)
{
    return (new_val->type == ST_TYPE_BOOL);
}

static void vsync_commit(const struct sval *new_val)
{
    if(new_val->as_bool) {
        SDL_GL_SetSwapInterval(1);
    }else {
        SDL_GL_SetSwapInterval(0);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_Init(const char *base_path)
{
    ss_e status;
    SDL_DisplayMode dm;
    SDL_GetDesktopDisplayMode(0, &dm);

    status = Settings_Create((struct setting){
        .name = "pf.video.aspect_ratio",
        .val = (struct sval) {
            .type = ST_TYPE_VEC2,
            .as_vec2 = (vec2_t){dm.w, dm.h}
        },
        .prio = 0,
        .validate = ar_validate,
        .commit = ar_commit,
    });
    assert(status == SS_OKAY);

    struct sval ar_pair;
    Settings_Get("pf.video.aspect_ratio", &ar_pair);
    float ar = ar_pair.as_vec2.x / ar_pair.as_vec2.y;
    float native_ar = (float)dm.w / dm.h;

    vec2_t res_default;
    if(ar < native_ar) {
        res_default = (vec2_t){dm.h * ar, dm.h};
    }else{
        res_default = (vec2_t){dm.w, dm.w / ar}; 
    }

    status = Settings_Create((struct setting){
        .name = "pf.video.resolution",
        .val = (struct sval) {
            .type = ST_TYPE_VEC2,
            .as_vec2 = res_default
        },
        .prio = 1, /* Depends on aspect_ratio */
        .validate = res_validate,
        .commit = res_commit,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.video.display_mode",
        .val = (struct sval) {
            .type = ST_TYPE_INT,
            .as_int = PF_WF_BORDERLESS_WIN
        },
        .prio = 0,
        .validate = dm_validate,
        .commit = dm_commit,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.video.window_always_on_top",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = win_on_top_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.video.vsync",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = vsync_validate,
        .commit = vsync_commit,
    });
    assert(status == SS_OKAY);

    if(!R_Shader_InitAll(base_path))
        return false;

    R_Texture_Init();
    R_GL_InitShadows();

    return true; 
}

