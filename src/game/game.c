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

#include "public/game.h"
#include "gamestate.h"
#include "selection.h"
#include "timer_events.h"
#include "movement.h"
#include "game_private.h"
#include "combat.h" 
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
#include "../settings.h"

#include <assert.h> 


#define CAM_HEIGHT          175.0f
#define CAM_TILT_UP_DEGREES 25.0f
#define CAM_SPEED           0.20f

#define ACTIVE_CAM          (s_gs.cameras[s_gs.active_cam_idx])

__KHASH_IMPL(entity, extern, khint32_t, struct entity*, 1, kh_int_hash_func, kh_int_hash_equal)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct gamestate s_gs;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static vec2_t g_default_minimap_pos(void)
{
    struct sval res = (struct sval){ 
        .type = ST_TYPE_VEC2, 
        .as_vec2 = (vec2_t){1920, 1080}
    };
    const float PAD = 10.0f;
    int size = 256;
    
    return (vec2_t) {
        (size + 2*MINIMAP_BORDER_WIDTH) / cos(M_PI/4.0f)/2 + PAD,
        res.as_vec2.y - (size + 2*MINIMAP_BORDER_WIDTH) / cos(M_PI/4.0f)/2 - PAD,
    };
}

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
    kh_clear(entity, s_gs.dynamic);
    kv_reset(s_gs.visible);
    kv_reset(s_gs.visible_obbs);

    if(s_gs.map) {
        M_Raycast_Uninstall();
        M_FreeMinimap(s_gs.map);
        AL_MapFree(s_gs.map);
        G_Move_Shutdown();
        G_Combat_Shutdown();
        s_gs.map = NULL;
    }

    for(int i = 0; i < NUM_CAMERAS; i++)
        g_reset_camera(s_gs.cameras[i]);

    G_ActivateCamera(0, CAM_MODE_RTS);

    s_gs.num_factions = 0;
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
    return true;
}

static void g_init_map(void)
{
    M_CenterAtOrigin(s_gs.map);
    M_RestrictRTSCamToMap(s_gs.map, ACTIVE_CAM);
    M_Raycast_Install(s_gs.map, ACTIVE_CAM);
    M_InitMinimap(s_gs.map, g_default_minimap_pos());
    G_Move_Init(s_gs.map);
    G_Combat_Init();
    N_FC_ClearStats();
}

static void g_shadow_pass(void)
{
    R_GL_DepthPassBegin();

    if(s_gs.map) {
        M_RenderVisibleMap(s_gs.map, ACTIVE_CAM, RENDER_PASS_DEPTH);
    }

    struct frustum frust;
    R_GL_GetLightFrustum(&frust);

    uint32_t key;
    struct entity *curr;
    kh_foreach(s_gs.active, key, curr, {

        if(!(curr->flags & ENTITY_FLAG_COLLISION))
            continue;

        if(curr->flags & ENTITY_FLAG_INVISIBLE)
            continue;
    
        struct obb obb;
        Entity_CurrentOBB(curr, &obb);

        if(!(C_FrustumOBBIntersectionFast(&frust, &obb) != VOLUME_INTERSEC_OUTSIDE))
            continue;

        if(curr->flags & ENTITY_FLAG_ANIMATED)
            A_SetRenderState(curr);

        mat4x4_t model;
        Entity_ModelMatrix(curr, &model);

        R_GL_RenderDepthMap(curr->render_private, &model);
    });

    R_GL_DepthPassEnd();
}

static void g_draw_pass(void)
{
    if(s_gs.map) {
        M_RenderVisibleMap(s_gs.map, ACTIVE_CAM, RENDER_PASS_REGULAR);
    }

    for(int i = 0; i < kv_size(s_gs.visible); i++) {
    
        struct entity *curr = kv_A(s_gs.visible, i);

        if(curr->flags & ENTITY_FLAG_INVISIBLE)
            continue;

        if(curr->flags & ENTITY_FLAG_ANIMATED)
            A_SetRenderState(curr);

        mat4x4_t model;
        Entity_ModelMatrix(curr, &model);

        R_GL_Draw(curr->render_private, &model);
    }
}

static void g_render_healthbars(void)
{
    size_t max_ents = kv_size(s_gs.visible);
    size_t num_combat_visible = 0;

    GLfloat ent_health_pc[max_ents];
    vec3_t ent_top_pos_ws[max_ents];

    for(int i = 0; i < max_ents; i++) {
    
        struct entity *curr = kv_A(s_gs.visible, i);

        if(!(curr->flags & ENTITY_FLAG_COMBATABLE))
            continue;

        int max_health = curr->ca.max_hp;
        int curr_health = G_Combat_GetCurrentHP(curr);

        ent_top_pos_ws[num_combat_visible] = Entity_TopCenterPointWS(curr);
        ent_health_pc[num_combat_visible] = ((GLfloat)curr_health)/max_health;

        num_combat_visible++;
    }

    R_GL_DrawHealthbars(num_combat_visible, ent_health_pc, ent_top_pos_ws, ACTIVE_CAM);
}

static bool bool_val_validate(const struct sval *new_val)
{
    return (new_val->type == ST_TYPE_BOOL);
}

static void shadows_en_commit(const struct sval *new_val)
{
    bool on = new_val->as_bool;
    if(s_gs.map) {
        M_SetShadowsEnabled(s_gs.map, on);
    }

    if(!s_gs.active)
        return;

    uint32_t key;
    struct entity *curr;
    kh_foreach(s_gs.active, key, curr, {
        R_GL_SetShadowsEnabled(curr->render_private, on);
    });
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Init(void)
{
    kv_init(s_gs.visible);
    kv_init(s_gs.visible_obbs);

    s_gs.active = kh_init(entity);
    if(!s_gs.active)
        goto fail_active;

    s_gs.dynamic = kh_init(entity);
    if(!s_gs.dynamic)
        goto fail_dynamic;

    if(!g_init_cameras())
        goto fail_cams; 

    g_reset();
    G_Sel_Init();
    G_Sel_Enable();
    G_Timer_Init();

    ss_e status = Settings_Create((struct setting){
        .name = "pf.game.healthbar_mode",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = true
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.video.shadows_enabled",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = true 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = shadows_en_commit,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_navigation_grid",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_last_cmd_flow_field",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    return true;

fail_cams:
    kh_destroy(entity, s_gs.dynamic);
fail_dynamic:
    kh_destroy(entity, s_gs.active);
fail_active:
    return false;
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

void G_GetMinimapPos(float *out_x, float *out_y)
{
    assert(s_gs.map);
    vec2_t center_pos;
    M_GetMinimapPos(s_gs.map, &center_pos);
    *out_x = center_pos.x;
    *out_y = center_pos.y;
}

void G_SetMinimapPos(float x, float y)
{
    assert(s_gs.map);
    M_SetMinimapPos(s_gs.map, (vec2_t){x, y});
}

int G_GetMinimapSize(void)
{
    assert(s_gs.map);
    return M_GetMinimapSize(s_gs.map);
}

void G_SetMinimapSize(int size)
{
    assert(s_gs.map);
    M_SetMinimapSize(s_gs.map, size);
}

bool G_MouseOverMinimap(void)
{
    if(!s_gs.map)
        return false;
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

void G_MakeStaticObjsImpassable(void)
{
    uint32_t key;
    struct entity *curr;
    kh_foreach(s_gs.active, key, curr, {

        if(((ENTITY_FLAG_COLLISION | ENTITY_FLAG_STATIC) & curr->flags) 
         != (ENTITY_FLAG_COLLISION | ENTITY_FLAG_STATIC))
            continue;

        struct obb obb;
        Entity_CurrentOBB(curr, &obb);
        M_NavCutoutStaticObject(s_gs.map, &obb);
    });
    M_NavUpdatePortals(s_gs.map);
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
    g_reset();

    G_Timer_Shutdown();
    G_Sel_Shutdown();

    for(int i = 0; i < NUM_CAMERAS; i++)
        Camera_Free(s_gs.cameras[i]);

    kh_destroy(entity, s_gs.active);
    kh_destroy(entity, s_gs.dynamic);
    kv_destroy(s_gs.visible);
    kv_destroy(s_gs.visible_obbs);
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

        if(curr->flags & ENTITY_FLAG_ANIMATED)
            A_Update(curr);
    });

    /* Next, update the set of currently selected entities. */
    G_Sel_Update(ACTIVE_CAM, (const pentity_kvec_t*)&s_gs.visible, (obb_kvec_t*)&s_gs.visible_obbs);
}

void G_Render(void)
{
    struct sval setting;
    ss_e status = Settings_Get("pf.video.shadows_enabled", &setting);
    assert(status == SS_OKAY);

    if(setting.as_bool) {
        g_shadow_pass();
    }
    g_draw_pass();

    status = Settings_Get("pf.debug.show_navigation_grid", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool && s_gs.map) {
        M_RenderVisiblePathableLayer(s_gs.map, ACTIVE_CAM);
    }

    enum selection_type sel_type;
    const pentity_kvec_t *selected = G_Sel_Get(&sel_type);
    for(int i = 0; i < kv_size(*selected); i++) {

        struct entity *curr = kv_A(*selected, i);
        R_GL_DrawSelectionCircle((vec2_t){curr->pos.x, curr->pos.z}, curr->selection_radius, 0.4f, 
            g_seltype_color_map[sel_type], s_gs.map);
    }

    E_Global_NotifyImmediate(EVENT_RENDER_3D, NULL, ES_ENGINE);

    R_GL_SetScreenspaceDrawMode();
    E_Global_NotifyImmediate(EVENT_RENDER_UI, NULL, ES_ENGINE);

    struct sval hb_setting;
    status = Settings_Get("pf.game.healthbar_mode", &hb_setting);
    assert(status == SS_OKAY);

    if(hb_setting.as_bool) {
        g_render_healthbars();
    }

    if(s_gs.map) {
        M_RenderMinimap(s_gs.map, ACTIVE_CAM);
    }
}

bool G_AddEntity(struct entity *ent)
{
    int ret;
    khiter_t k;

    k = kh_put(entity, s_gs.active, ent->uid, &ret);
    if(ret == -1 || ret == 0)
        return false;
    kh_value(s_gs.active, k) = ent;

    if(ent->flags & ENTITY_FLAG_COMBATABLE)
        G_Combat_AddEntity(ent, COMBAT_STANCE_AGGRESSIVE);

    if(ent->flags & ENTITY_FLAG_STATIC)
        return true;

    k = kh_put(entity, s_gs.dynamic, ent->uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_gs.dynamic, k) = ent;
    return true;
}

bool G_RemoveEntity(struct entity *ent)
{
    khiter_t k = kh_get(entity, s_gs.active, ent->uid);
    if(k == kh_end(s_gs.active))
        return false;
    kh_del(entity, s_gs.active, k);

    if(ent->flags & ENTITY_FLAG_SELECTABLE)
        G_Sel_Remove(ent);

    if(!(ent->flags & ENTITY_FLAG_STATIC)) {
        k = kh_get(entity, s_gs.dynamic, ent->uid);
        assert(k != kh_end(s_gs.dynamic));
        kh_del(entity, s_gs.dynamic, k);
    }

    G_Combat_RemoveEntity(ent);
    G_Move_RemoveEntity(ent);
    return true;
}

void G_StopEntity(const struct entity *ent)
{
    G_Combat_StopAttack(ent);
    G_Combat_SetStance(ent, COMBAT_STANCE_AGGRESSIVE);
    G_Move_RemoveEntity(ent);
}

bool G_AddFaction(const char *name, vec3_t color)
{
    if(s_gs.num_factions == MAX_FACTIONS)
        return false;
    if(strlen(name) >= sizeof(s_gs.factions[0].name))
        return false;

    int new_fac_id = s_gs.num_factions;
    strcpy(s_gs.factions[new_fac_id].name, name);
    s_gs.factions[new_fac_id].color = color;
    s_gs.factions[new_fac_id].controllable = true;
    s_gs.num_factions++;

    /* By default, a new faction is mutually at peace with 
     * every other faction. */
    for(int i = 0; i < new_fac_id; i++) {
        s_gs.diplomacy_table[i][new_fac_id] = DIPLOMACY_STATE_PEACE;
        s_gs.diplomacy_table[new_fac_id][i] = DIPLOMACY_STATE_PEACE;
    }

    return true;
}

bool G_RemoveFaction(int faction_id)
{
    if(s_gs.num_factions < 2) 
        return false;
    if(faction_id < 0 || faction_id >= s_gs.num_factions)
        return false;

    /* Remove all entities belonging to the faction. There is no problem
     * with deleting an entry while iterating - the table bin simply gets 
     * marked as 'deleted'. 
     * Also, patch the faction_ids (which are used to index 's_gs.factions' 
     * to account for the shift in entries in this array. */
    for(khiter_t k = kh_begin(s_gs.active); k != kh_end(s_gs.active); k++) {

        if(!kh_exist(s_gs.active, k))
            continue;

        struct entity *curr = kh_value(s_gs.active, k);
        if(curr->faction_id == faction_id)
            G_RemoveEntity(curr);
        else if(curr->faction_id > faction_id) 
            --curr->faction_id;
    }

    /* Reflect the faction_id changes in the diplomacy table */
    memmove(&s_gs.diplomacy_table[faction_id], &s_gs.diplomacy_table[faction_id + 1],
        sizeof(s_gs.diplomacy_table[0]) * (s_gs.num_factions - faction_id - 1));
    for(int i = 0; i < s_gs.num_factions-1; i++) {

        memmove(&s_gs.diplomacy_table[i][faction_id], &s_gs.diplomacy_table[i][faction_id + 1],
            sizeof(enum diplomacy_state) * (s_gs.num_factions - faction_id - 1));
    }

    memmove(s_gs.factions + faction_id, s_gs.factions + faction_id + 1, 
        sizeof(struct faction) * (s_gs.num_factions - faction_id - 1));
    --s_gs.num_factions;

    return true;
}

bool G_UpdateFaction(int faction_id, const char *name, vec3_t color, bool control)
{
    if(faction_id >= s_gs.num_factions)
        return false;
    if(strlen(name) >= sizeof(s_gs.factions[0].name))
        return false;

    strcpy(s_gs.factions[faction_id].name, name);
    s_gs.factions[faction_id].color = color;
    s_gs.factions[faction_id].controllable = control;
    return true;
}

int G_GetFactions(char out_names[][MAX_FAC_NAME_LEN], vec3_t *out_colors, bool *out_ctrl)
{
    for(int i = 0; i < s_gs.num_factions; i++) {
    
        if(out_names) {
            strncpy(out_names[i], s_gs.factions[i].name, MAX_FAC_NAME_LEN);
            out_names[i][MAX_FAC_NAME_LEN-1] = '\0';
        }
        if(out_colors) {
            out_colors[i] = s_gs.factions[i].color;
        }
        if(out_ctrl) {
            out_ctrl[i] = s_gs.factions[i].controllable;
        }
    }
    return s_gs.num_factions;
}

bool G_SetDiplomacyState(int fac_id_a, int fac_id_b, enum diplomacy_state ds)
{
    if(fac_id_a < 0 || fac_id_a >= s_gs.num_factions)
        return false;
    if(fac_id_b < 0 || fac_id_b >= s_gs.num_factions)
        return false;
    if(fac_id_a == fac_id_b)
        return false;

    s_gs.diplomacy_table[fac_id_a][fac_id_b] = ds;
    s_gs.diplomacy_table[fac_id_b][fac_id_a] = ds;
    return true;
}

bool G_GetDiplomacyState(int fac_id_a, int fac_id_b, enum diplomacy_state *out)
{
    if(fac_id_a < 0 || fac_id_a >= s_gs.num_factions)
        return false;
    if(fac_id_b < 0 || fac_id_b >= s_gs.num_factions)
        return false;
    if(fac_id_a == fac_id_b)
        return false;

    *out = s_gs.diplomacy_table[fac_id_a][fac_id_b];
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

vec3_t G_ActiveCamPos(void)
{
    return Camera_GetPos(ACTIVE_CAM);
}

const struct camera *G_GetActiveCamera(void)
{
    return ACTIVE_CAM;
}

vec3_t G_ActiveCamDir(void)
{
    mat4x4_t lookat;
    Camera_MakeViewMat(ACTIVE_CAM, &lookat);
    vec3_t ret = (vec3_t){-lookat.cols[0][2], -lookat.cols[1][2], -lookat.cols[2][2]};
    PFM_Vec3_Normal(&ret, &ret);
    return ret;
}

bool G_UpdateTile(const struct tile_desc *desc, const struct tile *tile)
{
    return M_AL_UpdateTile(s_gs.map, desc, tile);
}

const khash_t(entity) *G_GetDynamicEntsSet(void)
{
    return s_gs.dynamic;
}

const khash_t(entity) *G_GetAllEntsSet(void)
{
    return s_gs.active;
}

