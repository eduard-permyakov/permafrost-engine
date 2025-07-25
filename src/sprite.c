/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2025 Eduard Permyakov 
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

#include "sprite.h"
#include "event.h"
#include "game/public/game.h"
#include "lib/public/vec.h"
#include "lib/public/khash.h"

#include <assert.h>
#include <SDL.h>

enum sprite_type{
    SPRITE_STATIC,
    SPRITE_ANIM,
};

struct sprite_ctx{
    enum sprite_type         type;
    struct sprite_sheet_desc desc;
    vec2_t                   size;
    int                      fps;
    vec3_t                   ws_pos;
    uint32_t                 duration_ms;
    size_t                   count_left;
    int                      curr_frame;
    uint32_t                 begin_tick_ms;
};

KHASH_MAP_INIT_INT(sprite, struct sprite_ctx)

VEC_TYPE(id, uint32_t);
VEC_IMPL(static, id, uint32_t);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(sprite) *s_active;
static uint32_t         s_next_id = 0;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void on_update(void *user, void *event)
{
    vec_id_t to_delete;
    vec_id_init(&to_delete);
    uint32_t curr_time = SDL_GetTicks();

    uint32_t id;
    struct sprite_ctx *curr;
    kh_foreach_val_ptr(s_active, id, curr, {

        switch(curr->type) {
        case SPRITE_ANIM: {
            const uint32_t period = (1.0f / curr->fps) * 1000;
            if(!SDL_TICKS_PASSED(curr_time, curr->begin_tick_ms + period))
                break;
            size_t next_frame = curr->curr_frame + 1;
            if(next_frame == curr->desc.nframes) {
                next_frame = 0;
                curr->count_left--;
                curr->begin_tick_ms = curr_time;
            }
            if(curr->count_left == 0) {
                vec_id_push(&to_delete, id);
                break;
            }
            curr->curr_frame = next_frame;
            break;
        }
        case SPRITE_STATIC: {
            if(SDL_TICKS_PASSED(curr_time, curr->begin_tick_ms + curr->duration_ms)) {
                vec_id_push(&to_delete, id);
                break;
            }
            break;
        }
        default: assert(0);
        }
    });

    for(int i = 0; i < vec_size(&to_delete); i++) {
        khiter_t k = kh_get(sprite, s_active, vec_AT(&to_delete, i));
        assert(k != kh_end(s_active));
        kh_del(sprite, s_active, k);
    }
    vec_id_destroy(&to_delete);
}

static void on_render_ui(void *user, void *event)
{
    /* Push commands to render all active sprites as a batch */
}

static void add_ctx(struct sprite_ctx *ctx)
{
    uint32_t id = s_next_id++;
    int status;
    khiter_t k = kh_put(sprite, s_active, id, &status);
    assert(status != 0 && status != -1);
    kh_value(s_active, k) = *ctx;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Sprite_Init(void)
{
    s_next_id = 0;
    if(!(s_active = kh_init(sprite)))
        return false;
    E_Global_Register(EVENT_UPDATE_START, on_update, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_UI, on_render_ui, NULL, G_ALL);
    return true;
}

void Sprite_Shutdown(void)
{
    E_Global_Unregister(EVENT_RENDER_UI, on_render_ui);
    E_Global_Unregister(EVENT_UPDATE_START, on_update);
    kh_destroy(sprite, s_active);
}

void Sprite_Clear(void)
{
    kh_clear(sprite, s_active);
}

void Sprite_AddTimeDelta(uint32_t dt)
{
    uint32_t id;
    struct sprite_ctx *curr;
    (void)id;

    kh_foreach_val_ptr(s_active, id, curr, {
        curr->begin_tick_ms += dt;
    });
}

void Sprite_PlayAnim(size_t count, int fps, vec2_t size, 
                     struct sprite_sheet_desc desc, vec3_t ws_pos)
{
    add_ctx(&(struct sprite_ctx){
        .type = SPRITE_ANIM,
        .desc = desc,
        .size = size,
        .fps = fps,
        .ws_pos = ws_pos,
        .count_left = count,
        .curr_frame = 0,
        .begin_tick_ms = SDL_GetTicks()
    });
}

void Sprite_ShowStatic(struct sprite_sheet_desc desc, vec2_t size, float duration, vec3_t ws_pos)
{
    add_ctx(&(struct sprite_ctx){
        .type = SPRITE_STATIC,
        .desc = desc,
        .size = size,
        .ws_pos = ws_pos,
        .duration_ms = duration,
        .begin_tick_ms = SDL_GetTicks(),
    });
}

