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
#include "selection.h"
#include "../render/public/render.h"
#include "../anim/public/anim.h"
#include "../map/public/map.h"
#include "../entity.h"
#include "../camera.h"
#include "../cam_control.h"
#include "../asset_load.h"
#include "../event.h"
#include "../config.h"
#include "../collision.h"

#include <assert.h> 


#define CAM_HEIGHT          175.0f
#define CAM_TILT_UP_DEGREES 25.0f
#define CAM_SPEED           0.20f

#define ACTIVE_CAM          (s_gs.cameras[s_gs.active_cam_idx])
#define DEFAULT_SEL_COLOR   (vec3_t){0.95f, 0.95f, 0.95f}

/* By default, the minimap is in the bottom left corner with 10 px padding. */
#define DEFAULT_MINIMAP_POS (vec2_t) { \
    (MINIMAP_SIZE + 6)/cos(M_PI/4.0f)/2.0f + 10.0f, \
    CONFIG_RES_Y - (MINIMAP_SIZE + 6)/cos(M_PI/4.0f)/2.0f - 10.0f \
}

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
    G_Sel_Clear();

    uint32_t key;
    struct entity *curr;
    kh_foreach(s_gs.active, key, curr, {
        AL_EntityFree(curr);
    });

    kh_clear(entity, s_gs.active);
    kv_reset(s_gs.visible);
    kv_reset(s_gs.visible_obbs);
    kv_reset(s_gs.selected);

    if(s_gs.map) {
        M_Raycast_Uninstall();
        M_FreeMinimap(s_gs.map);
        AL_MapFree(s_gs.map);
        s_gs.map = NULL;
    }

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

        Camera_SetSpeed(s_gs.cameras[i], CAM_SPEED);
        Camera_SetSens (s_gs.cameras[i], 0.05f);
        g_reset_camera(s_gs.cameras[i]);
    }
}

static void g_init_map(void)
{
    M_CenterAtOrigin(s_gs.map);
    M_RestrictRTSCamToMap(s_gs.map, ACTIVE_CAM);
    M_Raycast_Install(s_gs.map, ACTIVE_CAM);
    M_InitMinimap(s_gs.map, DEFAULT_MINIMAP_POS);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Init(void)
{
    s_gs.active = kh_init(entity);
    kv_init(s_gs.visible);
    kv_init(s_gs.visible_obbs);
    kv_init(s_gs.selected);

    if(!s_gs.active)
        return false;

    if(g_init_cameras())
        return false; 

    g_reset();
    G_Sel_Install();

    return true;
}

bool G_NewGameWithMapString(const char *mapstr)
{
    g_reset();

    s_gs.map = AL_MapFromPFMapString(mapstr);
    if(!s_gs.map)
        return false;
    g_init_map();
    E_Global_Notify(EVENT_NEW_GAME, NULL, ES_ENGINE);

    return true;
}

bool G_NewGameWithMap(const char *dir, const char *pfmap)
{
    g_reset();

    s_gs.map = AL_MapFromPFMap(dir, pfmap);
    if(!s_gs.map)
        return false;
    g_init_map();
    E_Global_Notify(EVENT_NEW_GAME, NULL, ES_ENGINE);

    return true;
}

void G_SetMapRenderMode(enum chunk_render_mode mode)
{
    assert(s_gs.map);
    M_SetMapRenderMode(s_gs.map, mode);
}

void G_SetMinimapPos(float x, float y)
{
    assert(s_gs.map);
    M_SetMinimapPos(s_gs.map, (vec2_t){x, y});
}

bool G_MouseOverMinimap(void)
{
    assert(s_gs.map);
    return M_MouseOverMinimap(s_gs.map);
}

bool G_MapHeightAtPoint(vec2_t xz, float *out_height)
{
    assert(s_gs.map);

    if(!M_PointInsideMap(s_gs.map, xz))
        return false;

    *out_height = M_HeightAtPoint(s_gs.map, xz);
    return true;
}

bool G_UpdateMinimapChunk(int chunk_r, int chunk_c)
{
    assert(s_gs.map);
    return M_UpdateMinimapChunk(s_gs.map, chunk_r, chunk_c);
}

void G_MoveActiveCamera(vec2_t xz_ground_pos)
{
    vec3_t old_pos = Camera_GetPos(ACTIVE_CAM);
    float offset_mag = cos(DEG_TO_RAD(Camera_GetPitch(ACTIVE_CAM))) * Camera_GetHeight(ACTIVE_CAM);

    /* We position the camera such that the camera ray intersects the ground plane (Y=0)
     * at the specified xz position. */
    vec3_t new_pos = (vec3_t) {
        xz_ground_pos.raw[0] - cos(DEG_TO_RAD(Camera_GetYaw(ACTIVE_CAM))) * offset_mag,
        old_pos.y,
        xz_ground_pos.raw[1] + sin(DEG_TO_RAD(Camera_GetYaw(ACTIVE_CAM))) * offset_mag 
    };

    Camera_SetPos(ACTIVE_CAM, new_pos);
}

void G_Shutdown(void)
{
    G_Sel_Uninstall();

    for(int i = 0; i < NUM_CAMERAS; i++)
        Camera_Free(s_gs.cameras[i]);

    assert(s_gs.active);
    kh_destroy(entity, s_gs.active);
    kv_destroy(s_gs.visible);
    kv_destroy(s_gs.visible_obbs);
    kv_destroy(s_gs.selected);
}

void G_Update(void)
{
    /* Build the set of currently visible entities. Note that there may be some false positives due to 
       using the fast frustum cull. */
    kv_reset(s_gs.visible);
    kv_reset(s_gs.visible_obbs);

    struct frustum frust;
    Camera_MakeFrustum(ACTIVE_CAM, &frust);

    uint32_t key;
    struct entity *curr;
    kh_foreach(s_gs.active, key, curr, {

        struct obb obb;
        Entity_CurrentOBB(curr, &obb);

        if(C_FrustumOBBIntersectionFast(&frust, &obb) != VOLUME_INTERSEC_OUTSIDE) {
            kv_push(struct entity *, s_gs.visible, curr);
            kv_push(struct obb, s_gs.visible_obbs, obb);
        }
    });

    /* Next, update the set of currently selected entities. */
    G_Sel_GetSelection(ACTIVE_CAM, (const pentity_kvec_t*)&s_gs.visible, 
        (obb_kvec_t*)&s_gs.visible_obbs, (pentity_kvec_t*)&s_gs.selected);
}

void G_Render(void)
{
    if(s_gs.map)
        M_RenderVisibleMap(s_gs.map, ACTIVE_CAM);

    for(int i = 0; i < kv_size(s_gs.visible); i++) {
    
        struct entity *curr = kv_A(s_gs.visible, i);

        if(curr->flags & ENTITY_FLAG_ANIMATED)
            A_Update(curr);

        mat4x4_t model;
        Entity_ModelMatrix(curr, &model);
        R_GL_Draw(curr->render_private, &model);
    }

    for(int i = 0; i < kv_size(s_gs.selected); i++) {

        struct entity *curr = kv_A(s_gs.selected, i);
        R_GL_DrawSelectionCircle((vec2_t){curr->pos.x, curr->pos.z}, curr->selection_radius, 0.4f, DEFAULT_SEL_COLOR, s_gs.map);
    }

    E_Global_NotifyImmediate(EVENT_RENDER_3D, NULL, ES_ENGINE);

    /* Render the minimap/HUD last. Screenspace rendering clobbers the view/projection uniforms. */
    M_RenderMinimap(s_gs.map, ACTIVE_CAM);
    E_Global_NotifyImmediate(EVENT_RENDER_UI, NULL, ES_ENGINE);
}

bool G_AddEntity(struct entity *ent)
{
    int ret;
    khiter_t k;

    k = kh_put(entity, s_gs.active, ent->uid, &ret);
    if(ret == -1 || ret == 0)
        return false;

    kh_value(s_gs.active, k) = ent;
    return true;
}

bool G_RemoveEntity(struct entity *ent)
{
    khiter_t k = kh_get(entity, s_gs.active, ent->uid);
    if(k == kh_end(s_gs.active))
        return false;

    kh_del(entity, s_gs.active, k);
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

void G_EnableUnitSelection(void)
{
    G_Sel_Install();
}

void G_DisableUnitSelection(void)
{
    G_Sel_Uninstall();
}

