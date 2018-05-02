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

#include "public/game.h"
#include "gamestate.h"
#include "../render/public/render.h"
#include "../anim/public/anim.h"
#include "../map/public/map.h"
#include "../entity.h"
#include "../camera.h"
#include "../cam_control.h"
#include "../asset_load.h"
#include "../event.h"

#include <assert.h> 


#define CAM_HEIGHT          175.0f
#define CAM_TILT_UP_DEGREES 25.0f

#define ACTIVE_CAM          (s_gs.cameras[s_gs.active_cam_idx])


/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct gamestate s_gs;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void g_reset_camera(struct camera *cam)
{
    Camera_SetPitchAndYaw(cam, -(90.0f - CAM_TILT_UP_DEGREES), 90.0f + 45.0f);
    Camera_SetPos(cam, (vec3_t){ 0.0f, CAM_HEIGHT, 0.0f }); 
}

static void g_reset(void)
{
    khiter_t k;
    for (k = kh_begin(s_gs.active); k != kh_end(s_gs.active); ++k) {

        struct entity *ent = kh_value(s_gs.active, k);
        AL_EntityFree(ent);
    }
    kh_clear(entity, s_gs.active);

    M_Raycast_Uninstall();
    if(s_gs.map) AL_MapFree(s_gs.map);
    s_gs.map = NULL;

    for(int i = 0; i < NUM_CAMERAS; i++)
        g_reset_camera(s_gs.cameras[i]);

    G_ActivateCamera(0, CAM_MODE_RTS);
}

static bool g_init_cameras(void) 
{
    for(int i = 0; i < NUM_CAMERAS; i++) {
    
        s_gs.cameras[i] = Camera_New();
        if(!s_gs.cameras[i]) {
            return false;
        }

        Camera_SetSpeed(s_gs.cameras[i], 0.15f);
        Camera_SetSens (s_gs.cameras[i], 0.05f);
        g_reset_camera(s_gs.cameras[i]);
    }
}

static void g_init_map(void)
{
    M_CenterAtOrigin(s_gs.map);
    M_RestrictRTSCamToMap(s_gs.map, ACTIVE_CAM);
    M_Raycast_Install(s_gs.map, ACTIVE_CAM);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Init(void)
{
    s_gs.active = kh_init(entity);

    if(!s_gs.active)
        return false;

    if(g_init_cameras())
        return false; 

    g_reset();

    return true;
}

bool G_NewGameWithMapString(const char *mapstr)
{
    g_reset();

    s_gs.map = AL_MapFromPFMapString(mapstr);
    if(!s_gs.map)
        return false;
    g_init_map();

    return true;
}

bool G_NewGameWithMap(const char *dir, const char *pfmap)
{
    g_reset();

    s_gs.map = AL_MapFromPFMap(dir, pfmap);
    if(!s_gs.map)
        return false;
    g_init_map();

    return true;
}

void G_SetMapRenderMode(enum chunk_render_mode mode)
{
    if(!s_gs.map)
        return;

    M_SetMapRenderMode(s_gs.map, mode);
}

void G_Shutdown(void)
{
    for(int i = 0; i < NUM_CAMERAS; i++)
        Camera_Free(s_gs.cameras[i]);

    assert(s_gs.active);
    kh_destroy(entity, s_gs.active);
}

void G_Render(void)
{
    assert(s_gs.active);

    if(s_gs.map) M_RenderVisibleMap(s_gs.map, ACTIVE_CAM);

    khiter_t k;
    for(k = kh_begin(s_gs.active); k != kh_end(s_gs.active); ++k) {
    
        if(!kh_exist(s_gs.active, k))
            continue;
        struct entity *curr = kh_value(s_gs.active, k);

        /* TODO: Currently, we perform animation right before rendering due to 'A_Update' setting
         * some uniforms for the shader. Investigate if it's better to perform the animation for all
         * entities at once and just set the uniform right before rendering.  */
        if(curr->animated) A_Update(curr);

        mat4x4_t model;
        Entity_ModelMatrix(curr, &model);
        R_GL_Draw(curr->render_private, &model);
    }

    E_Global_NotifyImmediate(EVENT_RENDER, NULL, ES_ENGINE);
}

bool G_AddEntity(struct entity *ent)
{
    int ret;
    khiter_t k;

    k = kh_put(entity, s_gs.active, ent->uid, &ret);
    if(ret != 1)
        return false;

    kh_value(s_gs.active, k) = ent;
    return true;
}

bool G_RemoveEntity(struct entity *ent)
{
    kh_del(entity, s_gs.active, ent->uid);
    return true;
}

bool G_ActivateCamera(int idx, enum cam_mode mode)
{
    if( !(idx >= 0 && idx < NUM_CAMERAS) )
        return false;

    s_gs.active_cam_idx = idx;

    switch(mode) {
    case CAM_MODE_RTS:  CamControl_RTS_Install(s_gs.cameras[idx]); break;
    case CAM_MODE_FPS:  CamControl_FPS_Install(s_gs.cameras[idx]); break;
    default: assert(0);
    }

    return true;
}

bool G_UpdateChunkMats(int chunk_c, int chunk_r, const char *mats_string)
{
    return M_AL_UpdateChunkMats(s_gs.map, chunk_c, chunk_r, mats_string);
}

bool G_UpdateTile(const struct tile_desc *desc, const struct tile *tile)
{
    return M_AL_UpdateTile(s_gs.map, desc, tile);
}

