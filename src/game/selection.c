/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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
#include "game_private.h"
#include "public/game.h"
#include "../pf_math.h"
#include "../event.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../config.h"
#include "../camera.h"
#include "../main.h"
#include "../perf.h"
#include "../sched.h"

#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <float.h>

#include <SDL.h>

#define MIN(a, b)     ((a) < (b) ? (a) : (b))
#define MAX(a, b)     ((a) > (b) ? (a) : (b))

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

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

static struct selection_ctx{
    bool installed;
    enum state{
        STATE_MOUSE_SEL_UP = 0,
        STATE_MOUSE_SEL_DOWN,
        STATE_MOUSE_SEL_RELEASED,
    }state;
    vec2_t mouse_down_coord;
    vec2_t mouse_up_coord;
    int num_clicks;
    enum selection_type type;
}s_ctx;

static vec_entity_t  s_selected;

static bool     s_hovered_dirty = true;
static uint32_t s_hovered_uid = NULL_UID;

/*****************************************************************************/
/* GLOBAL VARIABLES                                                          */
/*****************************************************************************/

const vec3_t g_seltype_color_map[] = {
    [SELECTION_TYPE_PLAYER] = {0.95f, 0.95f, 0.95f},
    [SELECTION_TYPE_ALLIED] = { 0.0f,  1.0f,  0.0f},
    [SELECTION_TYPE_ENEMY]  = { 1.0f,  0.0f,  0.0f}
};

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

    if(G_MouseInTargetMode())
        return;

    int w, h;
    Engine_WinDrawableSize(&w, &h);

    /* Don't allow dragging a selection box when the mouse is at the edges of 
     * the screen (camera pan action) which is mutually exclusive to unit selection. */
    if(mouse_event->x == 0 || mouse_event->x == w-1
    || mouse_event->y == 0 || mouse_event->y == h-1)
        return;

    s_ctx.state = STATE_MOUSE_SEL_DOWN;
    s_ctx.mouse_down_coord = (vec2_t){mouse_event->x, mouse_event->y};
}

static void on_mousemove(void *user, void *event)
{
    s_hovered_dirty = true;
}

static void on_mouseup(void *user, void *event)
{
    if(s_ctx.state != STATE_MOUSE_SEL_DOWN)
        return;

    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);
    s_ctx.state = STATE_MOUSE_SEL_RELEASED;
    s_ctx.mouse_up_coord = (vec2_t){mouse_event->x, mouse_event->y};
    s_ctx.num_clicks = mouse_event->clicks;
}

static void on_render_ui(void *user, void *event)
{
    if(s_ctx.state != STATE_MOUSE_SEL_DOWN)
        return;

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    vec2_t signed_size = (vec2_t){mouse_x - s_ctx.mouse_down_coord.x, mouse_y - s_ctx.mouse_down_coord.y};
    const float width = 2.0f;
    const vec3_t color = (vec3_t){0.0f, 1.0f, 0.0f};

    R_PushCmd((struct rcmd){
        .func = R_GL_DrawBox2D,
        .nargs = 4,
        .args = {
            R_PushArg(&s_ctx.mouse_down_coord, sizeof(s_ctx.mouse_down_coord)),
            R_PushArg(&signed_size, sizeof(signed_size)),
            R_PushArg(&color, sizeof(color)),
            R_PushArg(&width, sizeof(width)),
        }
    });
}

static vec3_t sel_unproject_mouse_coords(struct camera *cam, vec2_t mouse_coords, float ndc_z)
{
    int w, h;
    Engine_WinDrawableSize(&w, &h);

    vec3_t ndc = (vec3_t){-1.0f + 2.0*(mouse_coords.raw[0]/(float)w),
                           1.0f - 2.0*(mouse_coords.raw[1]/(float)h),
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

    out->nearp = cam_frust.nearp;
    out->farp = cam_frust.farp;

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

static bool sel_shift_pressed(void)
{
    const Uint8 *state = SDL_GetKeyboardState(NULL);
    return (state[SDL_SCANCODE_LSHIFT] || state[SDL_SCANCODE_RSHIFT]);
}

static bool sel_ctrl_pressed(void)
{
    const Uint8 *state = SDL_GetKeyboardState(NULL);
    return (state[SDL_SCANCODE_LCTRL] || state[SDL_SCANCODE_RCTRL]);
}

static void sel_compute_hovered(struct camera *cam, const vec_entity_t *visible, const vec_obb_t *visible_obbs)
{
    if(!s_hovered_dirty)
        return;

    int mouse_x, mouse_y;
    SDL_GetMouseState(&mouse_x, &mouse_y);

    vec3_t ray_origin = sel_unproject_mouse_coords(cam, (vec2_t){mouse_x, mouse_y}, -1.0f);
    vec3_t ray_dir;

    vec3_t cam_pos = Camera_GetPos(cam);
    PFM_Vec3_Sub(&ray_origin, &cam_pos, &ray_dir);
    PFM_Vec3_Normal(&ray_dir, &ray_dir);

    float t_min = FLT_MAX;
    s_hovered_uid = NULL_UID;

    for(int i = 0; i < vec_size(visible_obbs); i++) {

        if(G_EntityIsZombie(vec_AT(visible, i)))
            continue;

        float t;
        if(C_RayIntersectsOBB(ray_origin, ray_dir, vec_AT(visible_obbs, i), &t)) {

            if(t < t_min) {
                t_min = t;
                s_hovered_uid = vec_AT(visible, i);
            }
        }
    }
    s_hovered_dirty = false;
}

static bool entities_equal(uint32_t *a, uint32_t *b)
{
    return ((*a) == (*b));
}

static bool allied_to_player_controllabe(const bool *controllable,
                                         uint16_t fac_mask, int faction_id)
{
    assert(!controllable[faction_id]);

    for(int i = 0; fac_mask; fac_mask >>= 1, i++) {
    
        if(!(fac_mask & 0x1))
            continue;
        if(i == faction_id)
            continue;

        enum diplomacy_state ds;
        G_GetDiplomacyState(faction_id, i, &ds);

        if(controllable[i] && ds != DIPLOMACY_STATE_WAR)
            return true;
    }
    return false;
}

/* An additional rule of selection is that units are prioritized 
 * over buildings. If there are units in the selection, remove 
 * any buildings. 
 */
static void sel_filter_buildings(void)
{
    bool has_units = false;
    for(int i = 0; i < vec_size(&s_selected); i++) {

        uint32_t curr = vec_AT(&s_selected, i);
        uint32_t flags = G_FlagsGet(curr);

        if(!(flags & ENTITY_FLAG_BUILDING)) {
            has_units = true;
            break;
        }
    }

    if(!has_units)
        return;

    /* Iterate the vector backwards so we can delete while iterating. */
    for(int i = vec_size(&s_selected)-1; i >= 0; i--) {

        uint32_t curr = vec_AT(&s_selected, i);
        uint32_t flags = G_FlagsGet(curr);

        if(flags & ENTITY_FLAG_BUILDING) {
            vec_entity_del(&s_selected, i);
        }
    }
}

/* Apply the following rules to the selection set:
 * 
 * 1) If there is at least one player-controllable entity in the selection set,
 *    leave only player-controllable entities.
 * 2) Else if there is at least one player ally in the selection set, leave only
 *    allied units in the selection set.
 * 3) Else we know there are only player enemy units in the selection set.
 *
 * The filtering should be performed after any addition to the 'selected' set
 * to keep the state consistent.
 */
static void sel_filter_and_set_type(void)
{
    bool controllable[MAX_FACTIONS];
    uint16_t fac_mask = G_GetFactions(NULL, NULL, controllable);

    bool has_player = false, has_allied = false;
    for(int i = 0; i < vec_size(&s_selected); i++) {
        
        uint32_t curr = vec_AT(&s_selected, i);
        assert(fac_mask & (0x1 << G_GetFactionID(curr)));

        if(controllable[G_GetFactionID(curr)]) {
            has_player = true; 
            break;
        }

        if(allied_to_player_controllabe(controllable, fac_mask, G_GetFactionID(curr))) {
            has_allied = true;
        }
    }

    if(has_player) {
        s_ctx.type = SELECTION_TYPE_PLAYER;
    }else if(has_allied) {
        s_ctx.type = SELECTION_TYPE_ALLIED; 
    }else {
        s_ctx.type = SELECTION_TYPE_ENEMY; 
    }

    /* Iterate the vector backwards so we can delete while iterating. */
    for(int i = vec_size(&s_selected)-1; i >= 0; i--) {

        uint32_t curr = vec_AT(&s_selected, i);
        if(has_player && !controllable[G_GetFactionID(curr)]) {

            vec_entity_del(&s_selected, i);

        }else if(!has_player 
              && has_allied
              && !allied_to_player_controllabe(controllable, fac_mask, G_GetFactionID(curr))) {

            vec_entity_del(&s_selected, i);
        }
    }

    sel_filter_buildings();
}

static void sel_process_unit(uint32_t uid)
{
    if(sel_shift_pressed()) {
        int idx = vec_entity_indexof(&s_selected, uid, entities_equal);
        if(idx == -1) {
            vec_entity_push(&s_selected, uid);
        }
    }else if(sel_ctrl_pressed()) {
        int idx = vec_entity_indexof(&s_selected, uid, entities_equal);
        if(idx != -1) {
            vec_entity_del(&s_selected, idx);
        }
    }else{
        vec_entity_push(&s_selected, uid);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Sel_Init(void)
{
    vec_entity_init(&s_selected);
    E_Global_Register(SDL_MOUSEMOTION, on_mousemove, NULL, G_RUNNING);
    return true;
}

void G_Sel_Shutdown(void)
{
    G_Sel_Disable();
    E_Global_Unregister(SDL_MOUSEMOTION, on_mousemove);
    vec_entity_destroy(&s_selected);
}

void G_Sel_Enable(void)
{
    if(s_ctx.installed)
        return;
    s_ctx.installed = true;

    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONUP,   on_mouseup, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_UI,     on_render_ui, NULL, G_RUNNING);
}

void G_Sel_Disable(void)
{
    if(!s_ctx.installed)
        return;
    s_ctx.installed = false;

    E_Global_Unregister(EVENT_RENDER_UI,     on_render_ui);
    E_Global_Unregister(SDL_MOUSEBUTTONUP,   on_mouseup);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
}

/* Note that the selection is only changed if there is at least one entity in the new selection. Otherwise
 * (ex. if the player is left-clicking on an empty part of the map), the previous selection is kept. */
void G_Sel_Update(struct camera *cam, const vec_entity_t *visible, const vec_obb_t *visible_obbs)
{
    PERF_ENTER();

    sel_compute_hovered(cam, visible, visible_obbs);

    if(G_MouseInTargetMode())
        PERF_RETURN_VOID();

    if(s_ctx.state != STATE_MOUSE_SEL_RELEASED)
        PERF_RETURN_VOID();
    s_ctx.state = STATE_MOUSE_SEL_UP;

    bool sel_empty = true;
    if(s_ctx.mouse_down_coord.x == s_ctx.mouse_up_coord.x 
    && s_ctx.mouse_down_coord.y == s_ctx.mouse_up_coord.y) {

        /* Case 1: The mouse is pressed and released in the same spot, meaning we can use a single ray
         * to test against the OBBs 
         *
         * The behaviour is that only a single entity can be selected with a 'click' action, even if multiple
         * OBBs intersect with the mouse ray. We pick the one with the closest intersection point.
         */
        if(G_EntityExists(s_hovered_uid) && (G_FlagsGet(s_hovered_uid) & ENTITY_FLAG_SELECTABLE)) {

            sel_empty = false;
            if(!sel_ctrl_pressed() && !sel_shift_pressed()) {
                vec_entity_reset(&s_selected);
            }
            /* A double-click selects all units of the same 'type' as the hovered unit */
            if(s_ctx.num_clicks > 1) {

                uint64_t hovered_id = S_ScriptTypeID(s_hovered_uid);
                for(int i = 0; i < vec_size(visible_obbs); i++) {

                    uint32_t curr = vec_AT(visible, i);
                    uint32_t flags = G_FlagsGet(curr);

                    if(!(flags & ENTITY_FLAG_SELECTABLE))
                        continue;
                    uint64_t curr_id = S_ScriptTypeID(curr);
                    if(curr_id != 0 && (hovered_id == curr_id)) {
                        sel_process_unit(curr);
                    }
                }
            }else{
                sel_process_unit(s_hovered_uid);
            }
        }

    }else{

        /* Case 2: The mouse is pressed and released in different spots, meaning the OBBs must be tested against
         * a frustum that is defined by the selection box.*/
        struct frustum frust;
        sel_make_frustum(cam, s_ctx.mouse_down_coord, s_ctx.mouse_up_coord, &frust);

        for(int i = 0; i < vec_size(visible_obbs); i++) {

            uint32_t flags = G_FlagsGet(vec_AT(visible, i));
            if(!(flags & ENTITY_FLAG_SELECTABLE))
                continue;

            if(C_FrustumOBBIntersectionExact(&frust, &vec_AT(visible_obbs, i))) {

                if(sel_empty) {
                    sel_empty = false;
                    if(!sel_shift_pressed() && !sel_ctrl_pressed()) {
                        vec_entity_reset(&s_selected);
                    }
                }
                uint32_t curr = vec_AT(visible, i);
                sel_process_unit(curr);
            }
        }
    }

    if(!sel_empty) {
        sel_filter_and_set_type();
        E_Global_Notify(EVENT_UNIT_SELECTION_CHANGED, NULL, ES_ENGINE);
    }
    PERF_RETURN_VOID();
}

void G_Sel_Clear(void)
{
    bool installed = s_ctx.installed;
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.installed = installed;

    if(vec_size(&s_selected) > 0) {
        E_Global_Notify(EVENT_UNIT_SELECTION_CHANGED, NULL, ES_ENGINE);
    }
    vec_entity_reset(&s_selected);
}

void G_Sel_Add(uint32_t uid)
{
    int idx = vec_entity_indexof(&s_selected, uid, entities_equal);
    if(idx == -1) {
        vec_entity_push(&s_selected, uid);
        sel_filter_and_set_type();
        E_Global_Notify(EVENT_UNIT_SELECTION_CHANGED, NULL, ES_ENGINE);
    }
}

void G_Sel_Remove(uint32_t uid)
{
    uint32_t flags = G_FlagsGet(uid);
    if(!(flags & ENTITY_FLAG_SELECTABLE))
        return;

    int idx = vec_entity_indexof(&s_selected, uid, entities_equal);
    if(idx != -1) {
        vec_entity_del(&s_selected, idx);
        E_Global_Notify(EVENT_UNIT_SELECTION_CHANGED, NULL, ES_ENGINE);
    }
}

const vec_entity_t *G_Sel_Get(enum selection_type *out_type)
{
    *out_type = s_ctx.type;
    return &s_selected;
}

void G_Sel_Set(uint32_t *ents, size_t nents)
{
    G_Sel_Clear();
    for(int i = 0; i < nents; i++) {
        uint32_t flags = G_FlagsGet(ents[i]);
        if(!(flags & ENTITY_FLAG_SELECTABLE))
            continue;
        vec_entity_push(&s_selected, ents[i]);
    }
    sel_filter_and_set_type();
    E_Global_Notify(EVENT_UNIT_SELECTION_CHANGED, NULL, ES_ENGINE);
}

bool G_Sel_SaveState(struct SDL_RWops *stream)
{
    struct attr installed = (struct attr){
        .type = TYPE_BOOL,
        .val.as_bool = s_ctx.installed
    };
    CHK_TRUE_RET(Attr_Write(stream, &installed, "installed"));

    struct attr sel_type = (struct attr){
        .type = TYPE_INT,
        .val.as_int = s_ctx.type
    };
    CHK_TRUE_RET(Attr_Write(stream, &sel_type, "sel_type"));

    struct attr num_selected = (struct attr){
        .type = TYPE_INT,
        .val.as_int = vec_size(&s_selected)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_selected, "num_selected"));

    Sched_TryYield();

    for(int i = 0; i < vec_size(&s_selected); i++) {
    
        struct attr selected_ent = (struct attr){
            .type = TYPE_INT,
            .val.as_int = vec_AT(&s_selected, i)
        };
        CHK_TRUE_RET(Attr_Write(stream, &selected_ent, "selected_ent"));
        Sched_TryYield();
    }

    return true;
}

bool G_Sel_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_BOOL);
    if(attr.val.as_bool) {
        G_Sel_Enable();
    }else{
        G_Sel_Disable();
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    s_ctx.type = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_selected = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_selected; i++) {
    
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        CHK_TRUE_RET(G_EntityExists(attr.val.as_int));
        vec_entity_push(&s_selected, attr.val.as_int);
        Sched_TryYield();
    }

    s_hovered_dirty = true;
    return true;
}

uint32_t G_Sel_GetHovered(void)
{
    return s_hovered_uid;
}

void G_Sel_MarkHoveredDirty(void)
{
    s_hovered_dirty = true;
}

bool G_Sel_IsSelected(uint32_t uid)
{
    int idx = vec_entity_indexof(&s_selected, uid, entities_equal);
    return (idx != -1);
}

