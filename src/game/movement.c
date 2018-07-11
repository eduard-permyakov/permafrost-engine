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
 */

#include "movement.h"
#include "public/game.h"
#include "../config.h"
#include "../camera.h"
#include "../asset_load.h"
#include "../event.h"
#include "../entity.h"
#include "../script/public/script.h"
#include "../render/public/render.h"
#include "../map/public/map.h"
#include "../lib/public/kvec.h"
#include "../anim/public/anim.h"

#include <assert.h>
#include <SDL.h>

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

kvec_t(struct entity*) s_move_markers;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool entities_equal(struct entity **a, struct entity **b)
{
    return (0 == memcmp(*a, *b, sizeof(struct entity)));
}

static void on_marker_anim_finish(void *user, void *event)
{
    int idx;
    struct entity *ent = user;
    assert(ent);

    kv_indexof(struct entity*, s_move_markers, ent, entities_equal, idx);
    assert(idx != -1);
    kv_del(struct entity*, s_move_markers, idx);

    E_Entity_Unregister(EVENT_ANIM_FINISHED, ent->uid, on_marker_anim_finish);
    AL_EntityFree(ent);
}

static void move_marker_add(vec3_t pos)
{
    extern const char *g_basepath;
    char path[256];
    strcpy(path, g_basepath);
    strcat(path, "assets/models/arrow");

    struct entity *ent = AL_EntityFromPFObj(path, "arrow-green.pfobj", "__move_marker__");
    assert(ent);

    ent->pos = pos;
    ent->scale = (vec3_t){2.0f, 2.0f, 2.0f};
    E_Entity_Register(EVENT_ANIM_FINISHED, ent->uid, on_marker_anim_finish, ent);

    A_InitCtx(ent, "Converge", 48);
    A_SetActiveClip(ent, "Converge", ANIM_MODE_ONCE, 48);

    kv_push(struct entity*, s_move_markers, ent);
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    if(mouse_event->button != SDL_BUTTON_RIGHT)
        return;

    if(G_MouseOverMinimap())
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    vec3_t mouse_coord;
    if(!M_Raycast_IntersecCoordinate(&mouse_coord))
        return;

    move_marker_add(mouse_coord);
}

static void on_render_3d(void *user, void *event)
{
    for(int i = 0; i < kv_size(s_move_markers); i++) {

        const struct entity *curr = kv_A(s_move_markers, i);
        if(curr->flags & ENTITY_FLAG_ANIMATED)
            A_Update(curr);

        mat4x4_t model;
        Entity_ModelMatrix(curr, &model);
        R_GL_Draw(curr->render_private, &model);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Move_Init(void)
{
    kv_init(s_move_markers);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL);
    E_Global_Register(EVENT_RENDER_3D, on_render_3d, NULL);
}

void G_Move_Shutdown(void)
{
    E_Global_Unregister(EVENT_RENDER_3D, on_render_3d);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);

    for(int i = 0; i < kv_size(s_move_markers); i++) {
        E_Entity_Unregister(EVENT_ANIM_FINISHED, kv_A(s_move_markers, i)->uid, on_marker_anim_finish);
        AL_EntityFree(kv_A(s_move_markers, i));
    }
    kv_destroy(s_move_markers);
}

