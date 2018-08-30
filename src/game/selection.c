/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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

#include "selection.h"
#include "public/game.h"
#include "../pf_math.h"
#include "../event.h"
#include "../render/public/render.h"
#include "../config.h"
#include "../camera.h"

#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <float.h>

#include <SDL.h>

#define MIN(a, b)     ((a) < (b) ? (a) : (b))
#define MAX(a, b)     ((a) > (b) ? (a) : (b))

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct selection_ctx{
    bool installed;
    enum state{
        STATE_MOUSE_SEL_UP = 0,
        STATE_MOUSE_SEL_DOWN,
        STATE_MOUSE_SEL_RELEASED,
    }state;
    vec2_t mouse_down_coord;
    vec2_t mouse_up_coord;
}s_ctx;

/*                       Mouse down                  Mouse 
 *                      over map area               released
 * [start] ---> [MOUSE_SEL_UP] ---> [MOUSE_SEL_DOWN] ---> [MOUSE_SEL_RELEASED]
 *                   ^                                            |
 *                   |      'G_Sel_GetSelection(...)' called      |
 *                   +--------------------------------------------+
 *
 * The 'MOUSE_SEL_RELEASED' state lasts one tick. This is the point where we 
 * re-calculate the current selection.
 */

pentity_kvec_t s_selected;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    if(mouse_event->button != SDL_BUTTON_LEFT)
        return;

    if(G_MouseOverMinimap())
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    /* Don't allow dragging a selection box when the mouse is at the edges of 
     * the screen (camera pan action) which is mutually exclusive to unit selection. */
    if(mouse_event->x == 0 || mouse_event->x == CONFIG_RES_X-1
    || mouse_event->y == 0 || mouse_event->y == CONFIG_RES_Y-1)
        return;

    s_ctx.state = STATE_MOUSE_SEL_DOWN;
    s_ctx.mouse_down_coord = (vec2_t){mouse_event->x, mouse_event->y};
}

static void on_mouseup(void *user, void *event)
{
    if(s_ctx.state != STATE_MOUSE_SEL_DOWN)
        return;

    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);
    s_ctx.state = STATE_MOUSE_SEL_RELEASED;
    s_ctx.mouse_up_coord = (vec2_t){mouse_event->x, mouse_event->y};
}

static void on_render(void *user, void *event)
{
    if(s_ctx.state != STATE_MOUSE_SEL_DOWN)
        return;

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    vec2_t signed_size = (vec2_t){mouse_x - s_ctx.mouse_down_coord.x, mouse_y - s_ctx.mouse_down_coord.y};
    R_GL_DrawBox2D(s_ctx.mouse_down_coord, signed_size, (vec3_t){0.0f, 1.0f, 0.0f}, 2.0f);
}

static vec3_t sel_unproject_mouse_coords(struct camera *cam, vec2_t mouse_coords, float ndc_z)
{
    vec3_t ndc = (vec3_t){-1.0f + 2.0*(mouse_coords.raw[0]/CONFIG_RES_X),
                           1.0f - 2.0*(mouse_coords.raw[1]/CONFIG_RES_Y),
                           ndc_z};
    vec4_t clip = (vec4_t){ndc.x, ndc.y, ndc.z, 1.0f};

    mat4x4_t view_proj_inverse; 
    mat4x4_t view, proj, tmp;

    Camera_MakeViewMat(cam, &view);
    Camera_MakeProjMat(cam, &proj);
    PFM_Mat4x4_Mult4x4(&proj, &view, &tmp);
    PFM_Mat4x4_Inverse(&tmp, &view_proj_inverse); 

    vec4_t ret_homo;
    PFM_Mat4x4_Mult4x1(&view_proj_inverse, &clip, &ret_homo);
    return (vec3_t){ret_homo.x/ret_homo.w, ret_homo.y/ret_homo.w, ret_homo.z/ret_homo.w};
}

static void sel_make_frustum(struct camera *cam, vec2_t mouse_down, vec2_t mouse_up, struct frustum *out)
{
    struct frustum cam_frust;
    Camera_MakeFrustum(cam, &cam_frust);

    out->near = cam_frust.near;
    out->far = cam_frust.far;

    vec2_t corners[4] = {
        (vec2_t){MIN(mouse_down.x, mouse_up.x), MIN(mouse_down.y, mouse_up.y)},
        (vec2_t){MIN(mouse_down.x, mouse_up.x), MAX(mouse_down.y, mouse_up.y)},
        (vec2_t){MAX(mouse_down.x, mouse_up.x), MIN(mouse_down.y, mouse_up.y)},
        (vec2_t){MAX(mouse_down.x, mouse_up.x), MAX(mouse_down.y, mouse_up.y)},
    };

    out->ntl = sel_unproject_mouse_coords(cam, corners[0], -1.0f);
    out->nbl = sel_unproject_mouse_coords(cam, corners[1], -1.0f);
    out->ntr = sel_unproject_mouse_coords(cam, corners[2], -1.0f);
    out->nbr = sel_unproject_mouse_coords(cam, corners[3], -1.0f);

    out->ftl = sel_unproject_mouse_coords(cam, corners[0], 1.0f);
    out->fbl = sel_unproject_mouse_coords(cam, corners[1], 1.0f);
    out->ftr = sel_unproject_mouse_coords(cam, corners[2], 1.0f);
    out->fbr = sel_unproject_mouse_coords(cam, corners[3], 1.0f);

    vec3_t tl_dir, bl_dir, tr_dir, br_dir;
    PFM_Vec3_Sub(&out->ftl, &out->ntl, &tl_dir);
    PFM_Vec3_Sub(&out->fbl, &out->nbl, &bl_dir);
    PFM_Vec3_Sub(&out->ftr, &out->ntr, &tr_dir);
    PFM_Vec3_Sub(&out->fbr, &out->nbr, &br_dir);

    PFM_Vec3_Normal(&tl_dir, &tl_dir);
    PFM_Vec3_Normal(&bl_dir, &bl_dir);
    PFM_Vec3_Normal(&tr_dir, &tr_dir);
    PFM_Vec3_Normal(&br_dir, &br_dir);

    vec3_t up, left;
    PFM_Vec3_Sub(&out->ntl, &out->nbl, &up);
    PFM_Vec3_Normal(&up, &up);
    PFM_Vec3_Sub(&out->ntl, &out->ntr, &left);
    PFM_Vec3_Normal(&left, &left);

    out->top.point = out->ntl;
    PFM_Vec3_Cross(&tl_dir, &left, &out->top.normal);
    PFM_Vec3_Normal(&out->top.normal, &out->top.normal);

    out->bot.point = out->nbr;
    PFM_Vec3_Cross(&left, &bl_dir, &out->bot.normal);
    PFM_Vec3_Normal(&out->bot.normal, &out->bot.normal);

    out->right.point = out->ntr;
    PFM_Vec3_Cross(&tr_dir, &up, &out->right.normal);
    PFM_Vec3_Normal(&out->right.normal, &out->right.normal);
    
    out->left.point = out->nbl;
    PFM_Vec3_Cross(&up, &tl_dir, &out->left.normal);
    PFM_Vec3_Normal(&out->left.normal, &out->left.normal);
}

static bool pentities_equal(struct entity *const *a, struct entity *const *b)
{
    return ((*a) == (*b));
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Sel_Init(void)
{
    kv_init(s_selected);
}

void G_Sel_Shutdown(void)
{
    G_Sel_Disable();
    kv_destroy(s_selected);
}

void G_Sel_Enable(void)
{
    if(s_ctx.installed)
        return;
    s_ctx.installed = true;

    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL);
    E_Global_Register(SDL_MOUSEBUTTONUP,   on_mouseup, NULL);
    E_Global_Register(EVENT_RENDER_UI,     on_render, NULL);
}

void G_Sel_Disable(void)
{
    if(!s_ctx.installed)
        return;
    s_ctx.installed = false;

    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    E_Global_Unregister(SDL_MOUSEBUTTONUP,   on_mouseup);
    E_Global_Unregister(EVENT_RENDER_UI,     on_render);

    G_Sel_Clear();
}

/* Note that the selection is only changed if there is at least one entity in the new selection. Otherwise
 * (ex. if the player is left-clicking on an empty part of the map), the previous selection is kept. */
bool G_Sel_Update(struct camera *cam, const pentity_kvec_t *visible, const obb_kvec_t *visible_obbs)
{
    if(s_ctx.state != STATE_MOUSE_SEL_RELEASED)
        return false;
    s_ctx.state = STATE_MOUSE_SEL_UP;

    bool sel_empty = true;
    if(s_ctx.mouse_down_coord.x == s_ctx.mouse_up_coord.x && s_ctx.mouse_down_coord.y && s_ctx.mouse_up_coord.y) {

        /* Case 1: The mouse is pressed and released in the same spot, meaning we can use a single ray
         * to test against the OBBs 
         *
         * The behaviour is that only a single entity can be selected with a 'click' action, even if multiple
         * OBBs intersect with the mouse ray. We pick the one with the closest intersection point.
         */
        vec3_t ray_origin = sel_unproject_mouse_coords(cam, s_ctx.mouse_up_coord, -1.0f);
        vec3_t ray_dir;

        vec3_t cam_pos = Camera_GetPos(cam);
        PFM_Vec3_Sub(&ray_origin, &cam_pos, &ray_dir);
        PFM_Vec3_Normal(&ray_dir, &ray_dir);

        float t_min = FLT_MAX;
        for(int i = 0; i < kv_size(*visible_obbs); i++) {

            if(!(kv_A(*visible, i)->flags & ENTITY_FLAG_SELECTABLE))
                continue;
        
            float t;
            if(C_RayIntersectsOBB(ray_origin, ray_dir, kv_A(*visible_obbs, i), &t)) {

                sel_empty = false;
                if(t < t_min) {
                    t_min = t;
                    kv_reset(s_selected);                
                    kv_push(struct entity*, s_selected, kv_A(*visible, i));
                }
            }
        }

        return false;
    
    }else{

        /* Case 2: The mouse is pressed and released in different spots, meaning the OBBs must be tested against
         * a frustum that is defined by the selection box.*/
        struct frustum frust;
        sel_make_frustum(cam, s_ctx.mouse_down_coord, s_ctx.mouse_up_coord, &frust);

        for(int i = 0; i < kv_size(*visible_obbs); i++) {

            if(!(kv_A(*visible, i)->flags & ENTITY_FLAG_SELECTABLE))
                continue;

            if(C_FrustumOBBIntersectionExact(&frust, &kv_A(*visible_obbs, i))) {

                if(sel_empty) {
                    kv_reset(s_selected);
                    sel_empty = false;
                }
                kv_push(struct entity*, s_selected, kv_A(*visible, i));
            }
        }
        
    }

    if(!sel_empty) {
        E_Global_Notify(EVENT_UNIT_SELECTION_CHANGED, NULL, ES_ENGINE);
        return true;
    }else{
        return false;
    }
}

void G_Sel_Clear(void)
{
    bool installed = s_ctx.installed;
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.installed = installed;

    kv_reset(s_selected);
}

void G_Sel_Add(struct entity *ent)
{
    assert(ent->flags & ENTITY_FLAG_SELECTABLE);

    int idx;
    kv_indexof(struct entity*, s_selected, ent, pentities_equal, idx);
    if(idx == -1) {
        kv_push(struct entity*, s_selected, ent);
    }
}

void G_Sel_Remove(struct entity *ent)
{
    assert(ent->flags & ENTITY_FLAG_SELECTABLE);

    int idx;
    kv_indexof(struct entity*, s_selected, ent, pentities_equal, idx);
    if(idx != -1) {
        kv_del(struct entity*, s_selected, idx);
    }
}

const pentity_kvec_t *G_Sel_Get(void)
{
    return &s_selected;
}

