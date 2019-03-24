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

    int AR_MIN = 0.5f, AR_MAX = 2.5f;
    return (new_val->as_vec2.x / new_val->as_vec2.y >= AR_MIN)
        && (new_val->as_vec2.x / new_val->as_vec2.y <= AR_MAX);
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
        .validate = ar_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.video.resolution",
        .val = (struct sval) {
            .type = ST_TYPE_VEC2,
            .as_vec2 = (vec2_t){dm.w, dm.h}
        },
        .validate = res_validate,
        .commit = res_commit,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.video.display_mode",
        .val = (struct sval) {
            .type = ST_TYPE_INT,
            .as_int = PF_WF_FULLSCREEN
        },
        .validate = dm_validate,
        .commit = dm_commit,
    });
    assert(status == SS_OKAY);

    if(!R_Shader_InitAll(base_path))
        return false;

    R_Texture_Init();
    R_GL_InitShadows();

    return true; 
}

