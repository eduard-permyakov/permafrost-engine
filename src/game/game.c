/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2023 Eduard Permyakov 
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
#include "clearpath.h"
#include "position.h"
#include "fog_of_war.h"
#include "building.h"
#include "builder.h"
#include "harvester.h"
#include "storage_site.h"
#include "resource.h"
#include "region.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../anim/public/anim.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../audio/public/audio.h"
#include "../phys/public/phys.h"
#include "../phys/public/collision.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/mem.h"
#include "../entity.h"
#include "../camera.h"
#include "../cam_control.h"
#include "../asset_load.h"
#include "../event.h"
#include "../config.h"
#include "../settings.h"
#include "../main.h"
#include "../ui.h"
#include "../perf.h"
#include "../cursor.h"
#include "../sched.h"

#include <assert.h> 


#define CAM_HEIGHT          175.0f
#define CAM_TILT_UP_DEGREES 25.0f
#define CAM_SPEED           0.20f
#define CAM_SENS            0.05f
#define MAX_VIS_RANGE       150.0f
#define WATER_ADJ_DISTANCE  25.0f

#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define ARR_SIZE(a)         (sizeof(a)/sizeof(a[0]))

#define CHK_TRUE_RET(_pred)   \
    do{                       \
        if(!(_pred))          \
            return false;     \
    }while(0)

VEC_IMPL(extern, obb, struct obb)
__KHASH_IMPL(entity,  extern, khint32_t, uint32_t, 0, kh_int_hash_func, kh_int_hash_equal)
__KHASH_IMPL(id,      extern, khint32_t, int,      1, kh_int_hash_func, kh_int_hash_equal)
__KHASH_IMPL(range,   extern, khint32_t, float,    1, kh_int_hash_func, kh_int_hash_equal)

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

static bool g_init_camera(void) 
{
    s_gs.active_cam = Camera_New();
    if(!s_gs.active_cam) {
        return false;
    }

    Camera_SetSpeed(s_gs.active_cam, CAM_SPEED);
    Camera_SetSens (s_gs.active_cam, CAM_SENS);
    g_reset_camera (s_gs.active_cam);
    return true;
}

static void g_init_map(void)
{
    M_CenterAtOrigin(s_gs.map);
    M_RestrictRTSCamToMap(s_gs.map, s_gs.active_cam);
    M_Raycast_Install(s_gs.map, s_gs.active_cam);
    M_InitMinimap(s_gs.map, g_default_minimap_pos());
    G_Pos_Init(s_gs.map);
    G_Building_Init(s_gs.map);
    G_Fog_Init(s_gs.map);
    G_Combat_Init(s_gs.map);
    G_Move_Init(s_gs.map);
    G_Builder_Init(s_gs.map);
    G_Resource_Init(s_gs.map);
    G_Region_Init(s_gs.map);
    G_Harvester_Init(s_gs.map);
    G_ClearPath_Init(s_gs.map);
    N_ClearState();
}

static void g_shadow_pass(struct render_input *in)
{
    vec3_t pos = Camera_GetPos(in->cam);
    vec3_t dir = Camera_GetDir(in->cam);

    R_PushCmd((struct rcmd){ 
        .func = R_GL_DepthPassBegin, 
        .nargs = 3,
        .args = { 
            R_PushArg(&in->light_pos, sizeof(in->light_pos)),
            R_PushArg(&pos, sizeof(pos)),
            R_PushArg(&dir, sizeof(dir)),
        },
    });

    if(in->map) {
        M_RenderVisibleMap(in->map, in->cam, true, RENDER_PASS_DEPTH);
    }

#if CONFIG_USE_BATCH_RENDERING

    R_PushCmd((struct rcmd){
        .func = R_GL_Batch_RenderDepthMap,
        .nargs = 1,
        .args = { in }
    });

#else // !CONFIG_USE_BATCH_RENDERING
    for(int i = 0; i < vec_size(&in->light_vis_anim); i++) {
    
        struct ent_anim_rstate *curr = &vec_AT(&in->light_vis_anim, i);

        mat4x4_t model, normal;
        PFM_Mat4x4_Inverse(&curr->model, &model);
        PFM_Mat4x4_Transpose(&model, &normal);

        R_PushCmd((struct rcmd){
            .func = R_GL_SetAnimUniforms,
            .nargs = 4,
            .args = {
                (void*)curr->inv_bind_pose, 
                R_PushArg(curr->curr_pose, sizeof(curr->curr_pose)),
                R_PushArg(&normal, sizeof(normal)),
                R_PushArg(&curr->njoints, sizeof(curr->njoints)),
            },
        });

        R_PushCmd((struct rcmd){
            .func = R_GL_RenderDepthMap,
            .nargs = 2,
            .args = {
                curr->render_private,
                R_PushArg(&curr->model, sizeof(curr->model)),
            },
        });
    }

    for(int i = 0; i < vec_size(&in->light_vis_stat); i++) {
    
        struct ent_stat_rstate *curr = &vec_AT(&in->light_vis_stat, i);
        R_PushCmd((struct rcmd){
            .func = R_GL_RenderDepthMap,
            .nargs = 2,
            .args = {
                curr->render_private,
                R_PushArg(&curr->model, sizeof(curr->model)),
            },
        });
    }
#endif

    R_PushCmd((struct rcmd){ R_GL_DepthPassEnd, 0 });
}

static void g_draw_pass(struct render_input *in)
{
    if(in->map) {
        M_RenderVisibleMap(in->map, in->cam, in->shadows, RENDER_PASS_REGULAR);
    }

#if CONFIG_USE_BATCH_RENDERING

    R_PushCmd((struct rcmd){
        .func = R_GL_Batch_Draw,
        .nargs = 1,
        .args = { in }
    });

#else // !CONFIG_USE_BATCH_RENDERING
    for(int i = 0; i < vec_size(&in->cam_vis_anim); i++) {
    
        struct ent_anim_rstate *curr = &vec_AT(&in->cam_vis_anim, i);

        mat4x4_t model, normal;
        PFM_Mat4x4_Inverse(&curr->model, &model);
        PFM_Mat4x4_Transpose(&model, &normal);

        R_PushCmd((struct rcmd){
            .func = R_GL_SetAnimUniforms,
            .nargs = 4,
            .args = {
                (void*)curr->inv_bind_pose, 
                R_PushArg(curr->curr_pose, sizeof(curr->curr_pose)),
                R_PushArg(&normal, sizeof(normal)),
                R_PushArg(&curr->njoints, sizeof(curr->njoints)),
            },
        });

        R_PushCmd((struct rcmd){
            .func = R_GL_Draw,
            .nargs = 3,
            .args = {
                curr->render_private,
                R_PushArg(&curr->model, sizeof(curr->model)),
                R_PushArg(&curr->translucent, sizeof(curr->translucent)),
            },
        });
    }

    for(int i = 0; i < vec_size(&in->cam_vis_stat); i++) {
    
        struct ent_stat_rstate *curr = &vec_AT(&in->cam_vis_stat, i);
        R_PushCmd((struct rcmd){
            .func = R_GL_Draw,
            .nargs = 3,
            .args = {
                curr->render_private,
                R_PushArg(&curr->model, sizeof(curr->model)),
                R_PushArg(&curr->translucent, sizeof(curr->translucent)),
            },
        });
    }
#endif
}

static void g_render_healthbars(void)
{
    PERF_ENTER();

    struct sval hb_setting;
    ss_e status = Settings_Get("pf.game.healthbar_mode", &hb_setting);
    assert(status == SS_OKAY);
    (void)status;

    if(hb_setting.as_int == HB_MODE_NEVER)
        PERF_RETURN_VOID();

    size_t max_ents = vec_size(&s_gs.visible);
    size_t num_combat_visible = 0;

    STALLOC(GLfloat, ent_health_pc, max_ents);
    STALLOC(vec3_t, ent_top_pos_ws, max_ents);

    for(int i = 0; i < max_ents; i++) {
    
        uint32_t curr = vec_AT(&s_gs.visible, i);
        uint32_t flags = G_FlagsGet(curr);

        if(!(flags & ENTITY_FLAG_COMBATABLE))
            continue;
        if((flags & ENTITY_FLAG_BUILDING) && !G_Building_IsFounded(curr))
            continue;

        int max_health = G_Combat_GetMaxHP(curr);
        int curr_health = G_Combat_GetCurrentHP(curr);

        if(curr_health == 0 || max_health == 0)
            continue;
        if(hb_setting.as_int == HB_MODE_DAMAGED && curr_health == max_health)
            continue;

        ent_top_pos_ws[num_combat_visible] = Entity_TopCenterPointWS(curr);
        ent_health_pc[num_combat_visible] = ((GLfloat)curr_health)/max_health;

        num_combat_visible++;
    }

    R_PushCmd((struct rcmd){
        .func = R_GL_DrawHealthbars,
        .nargs = 4,
        .args = {
            R_PushArg(&num_combat_visible, sizeof(num_combat_visible)),
            R_PushArg(ent_health_pc, max_ents * sizeof(GLfloat)),
            R_PushArg(ent_top_pos_ws, max_ents * sizeof(vec3_t)),
            R_PushArg(s_gs.active_cam, g_sizeof_camera),
        },
    });

    STFREE(ent_health_pc);
    STFREE(ent_top_pos_ws);

    PERF_RETURN_VOID();
}

static void g_sort_stat_list(vec_rstat_t *inout)
{
    PERF_ENTER();
    int i = 1;
    while(i < vec_size(inout)) {
        int j = i;
        while(j > 0 && vec_AT(inout, j - 1).translucent && !vec_AT(inout, j).translucent) {

            struct ent_stat_rstate tmp = vec_AT(inout, j - 1);
            vec_AT(inout, j - 1) = vec_AT(inout, j);
            vec_AT(inout, j) = tmp;
            j--;
        }
        i++;
    }
    PERF_RETURN_VOID();
}

static void g_sort_anim_list(vec_ranim_t *inout)
{
    PERF_ENTER();
    int i = 1;
    while(i < vec_size(inout)) {
        int j = i;
        while(j > 0 && vec_AT(inout, j - 1).translucent && !vec_AT(inout, j).translucent) {

            struct ent_anim_rstate tmp = vec_AT(inout, j - 1);
            vec_AT(inout, j - 1) = vec_AT(inout, j);
            vec_AT(inout, j) = tmp;
            j--;
        }
        i++;
    }
    PERF_RETURN_VOID();
}

static void g_make_draw_list(vec_entity_t ents, vec_rstat_t *out_stat, vec_ranim_t *out_anim)
{
    PERF_ENTER();
    struct map_resolution res;
    if(s_gs.map) {
        M_GetResolution(s_gs.map, &res);
    }

    for(int i = 0; i < vec_size(&ents); i++) {

        uint32_t curr = vec_AT(&ents, i);
        uint32_t flags = G_FlagsGet(curr);
        const struct entity *ent = AL_EntityGet(curr);

        if(flags & ENTITY_FLAG_INVISIBLE)
            continue;

        PERF_PUSH("process entity");

        mat4x4_t model;
        Entity_ModelMatrix(curr, &model);

        if(flags & ENTITY_FLAG_ANIMATED) {
        
            struct ent_anim_rstate rstate = (struct ent_anim_rstate){
                .uid = curr,
                .render_private = ent->render_private, 
                .model = model,
                .translucent = !!(flags & ENTITY_FLAG_TRANSLUCENT)
            };
            A_GetRenderState(curr, &rstate.njoints, rstate.curr_pose, &rstate.inv_bind_pose);
            vec_ranim_push(out_anim, rstate);

        }else{
        
            struct tile_desc td = {0};
            if(s_gs.map) {
                M_Tile_DescForPoint2D(res, M_GetPos(s_gs.map), G_Pos_GetXZ(curr), &td);
            }

            struct ent_stat_rstate rstate = (struct ent_stat_rstate){
                .uid = curr,
                .render_private = ent->render_private, 
                .model = model,
                .translucent = !!(flags & ENTITY_FLAG_TRANSLUCENT),
                .td = td
            };
            vec_rstat_push(out_stat, rstate);
        }
        PERF_POP();
    }

    g_sort_stat_list(out_stat);
    g_sort_anim_list(out_anim);
    PERF_RETURN_VOID();
}

static void g_create_render_input(struct render_input *out)
{
    PERF_ENTER();

    struct sval shadows_setting;
    ss_e status = Settings_Get("pf.video.shadows_enabled", &shadows_setting);
    assert(status == SS_OKAY);

    out->cam = s_gs.active_cam;
    out->map = s_gs.prev_tick_map;
    out->shadows = shadows_setting.as_bool;
    out->light_pos = s_gs.light_pos;

    vec_rstat_init(&out->cam_vis_stat);
    vec_ranim_init(&out->cam_vis_anim);

    vec_rstat_init(&out->light_vis_stat);
    vec_ranim_init(&out->light_vis_anim);

    vec_rstat_resize(&out->cam_vis_stat, 2048);
    vec_ranim_resize(&out->cam_vis_anim, 2048);

    vec_rstat_resize(&out->light_vis_stat, 2048);
    vec_ranim_resize(&out->light_vis_anim, 2048);

    g_make_draw_list(s_gs.visible, &out->cam_vis_stat, &out->cam_vis_anim);
    g_make_draw_list(s_gs.light_visible, &out->light_vis_stat, &out->light_vis_anim);

    PERF_RETURN_VOID();
}

static void g_destroy_render_input(struct render_input *rinput)
{
    vec_rstat_destroy(&rinput->cam_vis_stat);
    vec_ranim_destroy(&rinput->cam_vis_anim);

    vec_rstat_destroy(&rinput->light_vis_stat);
    vec_ranim_destroy(&rinput->light_vis_anim);
}

static void *g_push_render_input(struct render_input in)
{
    struct render_input *ret = R_PushArg(&in, sizeof(in));

    ret->cam = R_PushArg(in.cam, g_sizeof_camera);

    if(in.cam_vis_stat.size) {
        ret->cam_vis_stat.array = R_PushArg(in.cam_vis_stat.array, in.cam_vis_stat.size * sizeof(struct ent_stat_rstate));
    }
    if(in.cam_vis_anim.size) {
        ret->cam_vis_anim.array = R_PushArg(in.cam_vis_anim.array, in.cam_vis_anim.size * sizeof(struct ent_anim_rstate));
    }

    if(in.light_vis_stat.size) {
        ret->light_vis_stat.array = R_PushArg(in.light_vis_stat.array, in.light_vis_stat.size * sizeof(struct ent_stat_rstate));
    }
    if(in.light_vis_anim.size) {
        ret->light_vis_anim.array = R_PushArg(in.light_vis_anim.array, in.light_vis_anim.size * sizeof(struct ent_anim_rstate));
    }

    return ret;
}

static bool bool_val_validate(const struct sval *new_val)
{
    return (new_val->type == ST_TYPE_BOOL);
}

static bool nav_layer_validate(const struct sval *new_val)
{
    if(new_val->type != ST_TYPE_INT)
        return false;
    if(new_val->as_int < 0 || new_val->as_int >= NAV_LAYER_MAX)
        return false;
    return true;
}

static bool hb_mode_validate(const struct sval *new_val)
{
    if(new_val->type != ST_TYPE_INT)
        return false;
    if(new_val->as_int < 0 || new_val->as_int > HB_MODE_NEVER)
        return false;
    return true;
}

static bool fac_vision_validate(const struct sval *new_val)
{
    if(new_val->type != ST_TYPE_INT)
        return false;
    return (new_val->as_int >= -1 && new_val->as_int <= MAX_FACTIONS);
}

static bool faction_id_validate(const struct sval *new_val)
{
    if(new_val->type != ST_TYPE_INT)
        return false;
    if(new_val->as_int < 0)
        return false;
    return true;
}

static void shadows_en_commit(const struct sval *new_val)
{
    bool on = new_val->as_bool;
    if(s_gs.map) {
        M_SetShadowsEnabled(s_gs.map, on);
    }

    if(!s_gs.active)
        return;

    uint32_t curr;
    kh_foreach_key(s_gs.active, curr, {

        R_PushCmd((struct rcmd){
            .func = R_GL_SetShadowsEnabled,
            .nargs = 2,
            .args = {
                AL_EntityGet(curr)->render_private,
                R_PushArg(&on, sizeof(on)),
            },
        });
    });
}

static bool g_save_anim_state(SDL_RWops *stream)
{
    size_t nanim = 0;

    uint32_t curr;
    kh_foreach_key(s_gs.active, curr, {

        uint32_t flags = G_FlagsGet(curr);
        if(!(flags & ENTITY_FLAG_ANIMATED))
            continue;
        if(flags & ENTITY_FLAG_MARKER)
            continue;
        nanim++;
    });

    struct attr num_anim = (struct attr){
        .type = TYPE_INT, 
        .val.as_int = nanim
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_anim, "num_anim"));

    kh_foreach_key(s_gs.active, curr, {

        uint32_t flags = G_FlagsGet(curr);
        if(!(flags & ENTITY_FLAG_ANIMATED))
            continue;
        if(flags & ENTITY_FLAG_MARKER)
            continue;

        struct attr uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid, "uid"));
        CHK_TRUE_RET(A_SaveState(stream, curr));
        Sched_TryYield();
    });

    return true;
}

static bool g_load_anim_state(SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t nanim = attr.val.as_int;

    for(int i = 0; i < nanim; i++) {

        uint32_t uid;
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uid = attr.val.as_int;

        CHK_TRUE_RET(G_EntityExists(uid));
        CHK_TRUE_RET(A_LoadState(stream, uid));
        Sched_TryYield();
    }

    return true;
}

static size_t g_num_factions(void)
{
    size_t ret = 0;
    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(s_gs.factions_allocd & (0x1 << i))
            ret++;
    }
    return ret;
}

static int g_alloc_faction(void)
{
    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(0 == (s_gs.factions_allocd & (0x1 << i))) {
            s_gs.factions_allocd |= (0x1 << i);
            return i;
        }
    }
    return -1;
}

static uint16_t g_player_mask(void)
{
    bool controllable[MAX_FACTIONS];
    uint16_t facs = G_GetFactions(NULL, NULL, controllable);

    uint16_t ret = 0;
    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(facs & (0x1 << i) && controllable[i])
            ret |= (0x1 << i);
    }
    return ret;
}

static bool g_ent_visible(uint16_t playermask, uint32_t uid, const struct obb *obb)
{
    if(!s_gs.map)
        return true;

    uint32_t flags = G_FlagsGet(uid);
    if(flags & ENTITY_FLAG_MARKER)
        return true;

    if(!(flags & ENTITY_FLAG_MOVABLE)
    ||  (flags & ENTITY_FLAG_RESOURCE)
    ||  (flags & ENTITY_FLAG_BUILDING)) {
        return G_Fog_ObjExplored(playermask, uid, obb);
    }

    return G_Fog_ObjVisible(playermask, obb);
}

static bool g_entities_equal(uint32_t *a, uint32_t *b)
{
    return ((*a) == (*b));
}

static void g_clear_map_state(void)
{
    if(s_gs.map) {

        M_Raycast_Uninstall();
        M_FreeMinimap(s_gs.map);
        G_Building_Shutdown();
        G_Fog_Shutdown();
        G_Combat_Shutdown();
        G_Move_Shutdown();
        G_Builder_Shutdown();
        G_Resource_Shutdown();
        G_Region_Shutdown();
        G_Harvester_Shutdown();
        G_ClearPath_Shutdown();
        G_Pos_Shutdown();

        AL_MapFree(s_gs.map);
        s_gs.map = NULL;
    }

    if(s_gs.prev_tick_map) {
        /* The render thread still owns the previous tick map. Wait 
         * for it to complete before we free the buffer. */
        Engine_WaitRenderWorkDone();
        PF_FREE(s_gs.prev_tick_map);
        s_gs.prev_tick_map = NULL;
    }
}

static void g_set_contextual_cursor(void)
{
    if(G_MouseInTargetMode()) {
        Cursor_SetRTSPointer(CURSOR_TARGET);
        return;
    }

    int action = G_CurrContextualAction();
    switch(action) {
    case CTX_ACTION_ATTACK:
        Cursor_SetRTSPointer(CURSOR_ATTACK);
        break;
    case CTX_ACTION_NO_ATTACK:
        Cursor_SetRTSPointer(CURSOR_NO_ATTACK);
        break;
    case CTX_ACTION_BUILD:
        Cursor_SetRTSPointer(CURSOR_BUILD);
        break;
    case CTX_ACTION_GATHER: {
        char name[256] = { [0] = '\0' };
        G_Harvester_GetContextualCursor(name, sizeof(name));
        Cursor_NamedSetRTSPointer(name);
        break;
    }
    case CTX_ACTION_DROP_OFF:
        Cursor_SetRTSPointer(CURSOR_DROP_OFF);
        break;
    case CTX_ACTION_TRANSPORT:
        Cursor_SetRTSPointer(CURSOR_TRANSPORT);
        break;
    default:
        Cursor_SetRTSPointer(CURSOR_POINTER);
        break;
    }
}

static void g_change_simstate(void)
{
    if(s_gs.ss == s_gs.requested_ss)
        return;

    uint32_t curr_tick = SDL_GetTicks();
    switch(s_gs.requested_ss) {
    case G_RUNNING: {

        uint32_t delta = curr_tick - s_gs.ss_change_tick;
        uint32_t curr;
        kh_foreach_key(s_gs.active, curr, {
           
            uint32_t flags = G_FlagsGet(curr);
            if(!(flags & ENTITY_FLAG_ANIMATED))
                continue;
            A_AddTimeDelta(curr, delta);
        });
        Audio_Resume(delta);
        if(s_gs.map) {
            G_Combat_AddTimeDelta(delta);
        }
        break;
    }
    case G_PAUSED_FULL:
    case G_PAUSED_UI_RUNNING:
        if(s_gs.ss == G_RUNNING) {
            Audio_Pause();
        }
        break;
    default: assert(0);
    }

    E_FlushEventQueue();

    E_Global_Notify(EVENT_GAME_SIMSTATE_CHANGED, (void*)s_gs.requested_ss, ES_ENGINE);
    s_gs.ss_change_tick = curr_tick;
    s_gs.ss = s_gs.requested_ss;
}

static void g_create_settings(void)
{
    ss_e status;
    (void)status;

    status = Settings_Create((struct setting){
        .name = "pf.game.healthbar_mode",
        .val = (struct sval) {
            .type = ST_TYPE_INT,
            .as_int = HB_MODE_ALWAYS
        },
        .prio = 0,
        .validate = hb_mode_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.game.fog_of_war_enabled",
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
        .name = "pf.debug.navigation_layer",
        .val = (struct sval) {
            .type = ST_TYPE_INT,
            .as_int = 0
        },
        .prio = 0,
        .validate = nav_layer_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_navigation_cost_base",
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

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_first_sel_movestate",
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
        .name = "pf.debug.show_first_sel_combined_hrvo",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_enemy_seek_fields",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.enemy_seek_fields_faction_id",
        .val = (struct sval) {
            .type = ST_TYPE_INT,
            .as_int = 0
        },
        .prio = 0,
        .validate = faction_id_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_navigation_blockers",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_navigation_portals",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_navigation_island_ids",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_navigation_local_island_ids",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_chunk_boundaries",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_faction_vision",
        .val = (struct sval) {
            .type = ST_TYPE_INT,
            .as_int = -1 /* -1 for none, else the faction ID to show vision for */
        },
        .prio = 0,
        .validate = fac_vision_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_combat_targets",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });

    status = Settings_Create((struct setting){
        .name = "pf.debug.show_combat_ranges",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });
}

static void g_render_minimap_units(void)
{
    ASSERT_IN_MAIN_THREAD();
    assert(Sched_UsingBigStack());
    PERF_ENTER();

    STALLOC(vec2_t, positions, kh_size(s_gs.active));
    STALLOC(vec3_t, colors, kh_size(s_gs.active));
    size_t nunits = 0;

    vec3_t color_map[MAX_FACTIONS];
    G_GetFactions(NULL, color_map, NULL);

    uint32_t curr;
    kh_foreach_key(s_gs.active, curr, {

        uint32_t flags = G_FlagsGet(curr);
        if(!s_gs.minimap_render_all 
        && !(flags & (ENTITY_FLAG_MOVABLE | ENTITY_FLAG_BUILDING)))
            continue;
        vec2_t xz_pos = G_Pos_GetXZ(curr);
        if(!G_Fog_PlayerVisible(xz_pos))
            continue;
        vec3_t norm_color = color_map[G_GetFactionID(curr)];
        PFM_Vec3_Scale(&norm_color, 1.0f / 255, &norm_color);

        positions[nunits] = M_WorldCoordsToNormMapCoords(s_gs.map, xz_pos);
        colors[nunits] = norm_color;
        nunits++;
    });

    M_RenderMinimapUnits(s_gs.map, nunits, positions, colors);

    STFREE(positions);
    STFREE(colors);

    PERF_RETURN_VOID();
}

static void g_remove_queued(void)
{
    for(int i = 0; i < vec_size(&s_gs.removed); i++) {
        uint32_t curr = vec_AT(&s_gs.removed, i);
        G_RemoveEntity(curr);
        G_FreeEntity(curr);
    }
    vec_entity_reset(&s_gs.removed);
}

static void g_prune_water_input(struct render_input *in)
{
    PERF_ENTER();

    assert(s_gs.map);
    uint16_t pm = g_player_mask();

    for(int i = vec_size(&in->cam_vis_stat) - 1; i >= 0; i--) {

        const struct ent_stat_rstate *rstate = &vec_AT(&in->cam_vis_stat, i);
        vec2_t xz_pos = G_Pos_GetXZ(rstate->uid);

        if(!G_Fog_NearVisibleWater(pm, xz_pos, WATER_ADJ_DISTANCE)) {
            vec_rstat_del(&in->cam_vis_stat, i);
        }
    }

    for(int i = vec_size(&in->light_vis_stat) - 1; i >= 0; i--) {

        const struct ent_stat_rstate *rstate = &vec_AT(&in->light_vis_stat, i);
        vec2_t xz_pos = G_Pos_GetXZ(rstate->uid);

        if(!G_Fog_NearVisibleWater(pm, xz_pos, WATER_ADJ_DISTANCE)) {
            vec_rstat_del(&in->light_vis_stat, i);
        }
    }

    for(int i = vec_size(&in->cam_vis_anim) - 1; i >= 0; i--) {

        const struct ent_anim_rstate *rstate = &vec_AT(&in->cam_vis_anim, i);
        vec2_t xz_pos = G_Pos_GetXZ(rstate->uid);

        if(!G_Fog_NearVisibleWater(pm, xz_pos, WATER_ADJ_DISTANCE)) {
            vec_ranim_del(&in->cam_vis_anim, i);
        }
    }

    for(int i = vec_size(&in->light_vis_anim) - 1; i >= 0; i--) {

        const struct ent_anim_rstate *rstate = &vec_AT(&in->light_vis_anim, i);
        vec2_t xz_pos = G_Pos_GetXZ(rstate->uid);

        if(!G_Fog_NearVisibleWater(pm, xz_pos, WATER_ADJ_DISTANCE)) {
            vec_ranim_del(&in->light_vis_anim, i);
        }
    }

    PERF_RETURN_VOID();
}

void g_delete_gpuid(uint32_t uid)
{
    khiter_t k = kh_get(entity, s_gs.dynamic, uid);
    assert(k != kh_end(s_gs.dynamic));
    kh_del(entity, s_gs.dynamic, k);

    k = kh_get(id, s_gs.ent_gpu_id_map, uid);
    assert(k != kh_end(s_gs.ent_gpu_id_map));
    uint32_t old_id = kh_value(s_gs.ent_gpu_id_map, k);
    uint32_t old_size = kh_size(s_gs.ent_gpu_id_map);
    kh_del(id, s_gs.ent_gpu_id_map, k);

    k = kh_get(id, s_gs.gpu_id_ent_map, old_id);
    assert(k != kh_end(s_gs.gpu_id_ent_map));
    kh_del(id, s_gs.gpu_id_ent_map, k);

    /* Make sure all existing values in the GPU ID table 
     * are in the range of [1:table_size] */
    k = kh_get(id, s_gs.gpu_id_ent_map, old_size);
    if(k != kh_end(s_gs.gpu_id_ent_map)) {

        uint32_t uid = kh_value(s_gs.gpu_id_ent_map, k);
        kh_del(id, s_gs.gpu_id_ent_map, k);

        k = kh_get(id, s_gs.ent_gpu_id_map, uid);
        assert(k != kh_end(s_gs.ent_gpu_id_map));
        kh_value(s_gs.ent_gpu_id_map, k) = old_id;

        int ret;
        k = kh_put(id, s_gs.gpu_id_ent_map, old_id, &ret);
        assert(ret != -1);
        kh_value(s_gs.gpu_id_ent_map, k) = uid;
    }

    assert(kh_size(s_gs.dynamic) == kh_size(s_gs.ent_gpu_id_map));
    assert(kh_size(s_gs.ent_gpu_id_map) == kh_size(s_gs.gpu_id_ent_map));
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Init(void)
{
    ASSERT_IN_MAIN_THREAD();

    vec_entity_init(&s_gs.visible);
    vec_entity_init(&s_gs.light_visible);
    vec_obb_init(&s_gs.visible_obbs);
    vec_entity_init(&s_gs.removed);

    s_gs.active = kh_init(entity);
    if(!s_gs.active)
        goto fail_active;

    s_gs.ent_faction_map = kh_init(id);
    if(!s_gs.ent_faction_map)
        goto fail_faction;

    s_gs.ent_visrange_map = kh_init(range);
    if(!s_gs.ent_visrange_map)
        goto fail_visrange;

    s_gs.selection_radiuses = kh_init(range);
    if(!s_gs.selection_radiuses)
        goto fail_sel_rad;

    s_gs.dynamic = kh_init(entity);
    if(!s_gs.dynamic)
        goto fail_dynamic;

    s_gs.ent_gpu_id_map = kh_init(id);
    if(!s_gs.ent_gpu_id_map)
        goto fail_ent_gpu_id_map;

    s_gs.gpu_id_ent_map = kh_init(id);
    if(!s_gs.ent_gpu_id_map)
        goto fail_gpu_id_ent_map;

    s_gs.ent_flag_map = kh_init(id);
    if(!s_gs.ent_flag_map)
        goto fail_ent_flag_map;

    if(!g_init_camera())
        goto fail_cam; 

    if(!R_InitWS(&s_gs.ws[0]))
        goto fail_ws;

    if(!R_InitWS(&s_gs.ws[1])) {
        R_DestroyWS(&s_gs.ws[0]);
        goto fail_ws;
    }

    G_ClearState();

    G_Sel_Init();
    G_Sel_Enable();
    G_Timer_Init();
    G_StorageSite_Init();
    g_create_settings();

    R_PushCmd((struct rcmd){ R_GL_WaterInit, 0 });

    s_gs.prev_tick_map = NULL;
    s_gs.curr_ws_idx = 0;
    s_gs.light_pos = (vec3_t){120.0f, 150.0f, 120.0f};
    s_gs.ss = G_RUNNING;
    s_gs.requested_ss = G_RUNNING;

    return true;

fail_ws:
    Camera_Free(s_gs.active_cam);
fail_cam:
    kh_destroy(id, s_gs.ent_flag_map);
fail_ent_flag_map:
    kh_destroy(id, s_gs.gpu_id_ent_map);
fail_gpu_id_ent_map:
    kh_destroy(id, s_gs.ent_gpu_id_map);
fail_ent_gpu_id_map:
    kh_destroy(entity, s_gs.dynamic);
fail_dynamic:
    kh_destroy(range, s_gs.selection_radiuses);
fail_sel_rad:
    kh_destroy(range, s_gs.ent_visrange_map);
fail_visrange:
    kh_destroy(id, s_gs.ent_faction_map);
fail_faction:
    kh_destroy(entity, s_gs.active);
fail_active:
    return false;
}

bool G_LoadMap(SDL_RWops *stream, bool update_navgrid)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();

    g_clear_map_state();

    size_t copysize = AL_MapShallowCopySize(stream);
    s_gs.prev_tick_map = malloc(copysize);
    if(!s_gs.prev_tick_map)
        PERF_RETURN(false);

    s_gs.map = AL_MapFromPFMapStream(stream, update_navgrid);
    if(!s_gs.map)
        PERF_RETURN(false);

    g_init_map();
    M_AL_ShallowCopy((struct map*)s_gs.prev_tick_map, s_gs.map);

    E_Global_Notify(EVENT_NEW_GAME, s_gs.map, ES_ENGINE);

#if CONFIG_USE_BATCH_RENDERING
    struct map_resolution res;
    M_GetResolution(s_gs.map, &res);
    R_PushCmd((struct rcmd){
        .func = R_GL_Batch_AllocChunks,
        .nargs = 1,
        .args = {
            R_PushArg(&res, sizeof(res)),
        }
    });
#endif

    PERF_RETURN(true);
}

void G_ClearState(void)
{
    PERF_ENTER();
    G_Sel_Clear();
    g_remove_queued();

    uint32_t curr;
    kh_foreach_key(s_gs.active, curr, {
        /* The move markers are removed in G_Move_Shutdown */
        uint32_t flags = G_FlagsGet(curr);
        if(flags & ENTITY_FLAG_MARKER)
            continue;
        G_RemoveEntity(curr);
        G_FreeEntity(curr);
    });

    kh_clear(entity, s_gs.active);
    kh_clear(entity, s_gs.dynamic);
    kh_clear(id, s_gs.ent_gpu_id_map);
    kh_clear(id, s_gs.gpu_id_ent_map);
    kh_clear(id, s_gs.ent_faction_map);
    kh_clear(id, s_gs.ent_flag_map);
    kh_clear(range, s_gs.ent_visrange_map);
    kh_clear(range, s_gs.selection_radiuses);
    vec_entity_reset(&s_gs.visible);
    vec_entity_reset(&s_gs.light_visible);
    vec_obb_reset(&s_gs.visible_obbs);

    g_clear_map_state();
    M_MinimapClearBorderClr();

    g_reset_camera(s_gs.active_cam);
    G_SetActiveCamera(s_gs.active_cam, CAM_MODE_RTS);

    G_Sel_Enable();
    G_Fog_Enable();
    G_StorageSite_ClearState();

    s_gs.factions_allocd = 0;
    s_gs.hide_healthbars = false;
    s_gs.minimap_render_all = false;

    vec3_t white = (vec3_t){1.0f, 1.0f, 1.0f};
    R_PushCmd((struct rcmd){
        .func = R_GL_SetAmbientLightColor,
        .nargs = 1,
        .args = { R_PushArg(&white, sizeof(white)) },
    });
    R_PushCmd((struct rcmd){
        .func = R_GL_SetLightEmitColor,
        .nargs = 1,
        .args = { R_PushArg(&white, sizeof(white)) },
    });
    G_SetLightPos((vec3_t){1.0f, 1.0f, 1.0f});
    R_PushCmd((struct rcmd) { R_GL_Batch_Reset, 0 });

    PERF_RETURN_VOID();
}

void G_FlushWork(void)
{
    G_Combat_FlushWork();
    G_Move_FlushWork();
}

bool G_HasWork(void)
{
    return G_Combat_HasWork() || G_Move_HasWork();
}

void G_ClearRenderWork(void)
{
    Engine_WaitRenderWorkDone();
    R_ClearWS(&s_gs.ws[0]);
    R_ClearWS(&s_gs.ws[1]);
}

bool G_GetMinimapPos(float *out_x, float *out_y)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        return false;

    vec2_t center_pos;
    M_GetMinimapPos(s_gs.map, &center_pos);
    *out_x = center_pos.x;
    *out_y = center_pos.y;
    return true;
}

bool G_SetMinimapPos(float x, float y)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        return false;

    M_SetMinimapPos(s_gs.map, (vec2_t){x, y});
    return true;
}

bool G_GetMinimapSize(int *out_size)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        return false;

    *out_size = M_GetMinimapSize(s_gs.map);
    return true;
}

bool G_SetMinimapSize(int size)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        return false;

    M_SetMinimapSize(s_gs.map, size);
    return true;
}

bool G_SetMinimapResizeMask(int mask)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        return false;

    M_SetMinimapResizeMask(s_gs.map, mask);
    return true;
}

void G_SetMinimapRenderAllEntities(bool on)
{
    ASSERT_IN_MAIN_THREAD();
    s_gs.minimap_render_all = on;
}

bool G_MouseOverMinimap(void)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        return false;
    return M_MouseOverMinimap(s_gs.map);
}

bool G_MapHeightAtPoint(vec2_t xz, float *out_height)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        return false;

    if(!M_PointInsideMap(s_gs.map, xz))
        return false;

    *out_height = M_HeightAtPoint(s_gs.map, xz);
    return true;
}

bool G_MapClosestPathable(vec2_t xz, vec2_t *out)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map) {
        *out = xz;
        return true;
    }
    return M_NavClosestPathable(s_gs.map, NAV_LAYER_GROUND_1X1, xz, out);
}

bool G_PointInsideMap(vec2_t xz)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        return false;
    return M_PointInsideMap(s_gs.map, xz);
}

void G_BakeNavDataForScene(void)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();

    uint32_t curr;
    kh_foreach_key(s_gs.active, curr, {

        uint32_t flags = G_FlagsGet(curr);
        if(!(flags & ENTITY_FLAG_COLLISION))
            continue;
        if(flags & ENTITY_FLAG_MOVABLE)
            continue;
        if(flags & ENTITY_FLAG_BUILDING)
            continue;
        if(flags & ENTITY_FLAG_RESOURCE)
            continue;

        struct obb obb;
        Entity_CurrentOBB(curr, &obb, false);
        M_NavCutoutStaticObject(s_gs.map, &obb);
        Sched_TryYield();
    });

    M_NavUpdatePortals(s_gs.map);
    Sched_TryYield();

    M_NavUpdateIslandsField(s_gs.map);
    Sched_TryYield();

    PERF_RETURN_VOID();
}

bool G_UpdateMinimapChunk(int chunk_r, int chunk_c)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        PERF_RETURN(false);

    bool ret = M_UpdateMinimapChunk(s_gs.map, chunk_r, chunk_c);
    PERF_RETURN(ret);
}

void G_Shutdown(void)
{
    ASSERT_IN_MAIN_THREAD();

    G_ClearState();

    R_DestroyWS(&s_gs.ws[0]);
    R_DestroyWS(&s_gs.ws[1]);

    R_PushCmd((struct rcmd){ R_GL_WaterShutdown, 0 });

    G_StorageSite_Shutdown();
    G_Timer_Shutdown();
    G_Sel_Shutdown();

    Camera_Free(s_gs.active_cam);

    kh_destroy(entity, s_gs.active);
    kh_destroy(entity, s_gs.dynamic);
    kh_destroy(id, s_gs.ent_gpu_id_map);
    kh_destroy(id, s_gs.gpu_id_ent_map);
    kh_destroy(id, s_gs.ent_faction_map);
    kh_destroy(id, s_gs.ent_flag_map);
    kh_destroy(range, s_gs.ent_visrange_map);
    kh_destroy(range, s_gs.selection_radiuses);
    vec_entity_destroy(&s_gs.light_visible);
    vec_entity_destroy(&s_gs.visible);
    vec_obb_destroy(&s_gs.visible_obbs);
    vec_entity_destroy(&s_gs.removed);
}

void G_Update(void)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();

    if(s_gs.map) {
        M_Update(s_gs.map);
        G_Fog_UpdateVisionState();
    }

    vec_entity_reset(&s_gs.visible);
    vec_entity_reset(&s_gs.light_visible);
    vec_obb_reset(&s_gs.visible_obbs);

    vec3_t pos = Camera_GetPos(s_gs.active_cam);
    vec3_t dir = Camera_GetDir(s_gs.active_cam);

    struct frustum cam_frust;
    Camera_MakeFrustum(s_gs.active_cam, &cam_frust);

    struct frustum light_frust;
    R_LightFrustum(s_gs.light_pos, pos, dir, &light_frust);

    uint16_t pm = g_player_mask();
    uint32_t curr;

    if(s_gs.ss == G_RUNNING) {
        A_Update();
    }

    kh_foreach_key(s_gs.active, curr, {

        struct obb obb;
        Entity_CurrentOBB(curr, &obb, false);
        bool vis_checked = false;
        bool vis = false;

        /* Note that there may be some false positives due to using the fast frustum cull. */
        if(C_FrustumOBBIntersectionFast(&cam_frust, &obb) != VOLUME_INTERSEC_OUTSIDE) {
            vis = g_ent_visible(pm, curr, &obb);
            vis_checked = true;
            if(vis) {
                vec_entity_push(&s_gs.visible, curr);
                vec_obb_push(&s_gs.visible_obbs, obb);
            }
        }

        if(C_FrustumOBBIntersectionFast(&light_frust, &obb) != VOLUME_INTERSEC_OUTSIDE) {
            if(!vis_checked) {
                vis = g_ent_visible(pm, curr, &obb);
            }
            uint32_t flags = G_FlagsGet(curr);
            if(vis || !(flags & ENTITY_FLAG_MOVABLE)) {
                vec_entity_push(&s_gs.light_visible, curr);
            }
        }
    });

    if(s_gs.map) {
        G_Region_Update();
    }

    G_Sel_Update(s_gs.active_cam, &s_gs.visible, &s_gs.visible_obbs);
    P_Projectile_Update();
    g_set_contextual_cursor();

    E_Global_NotifyImmediate(EVENT_UPDATE_UI, NULL, ES_ENGINE);

    PERF_RETURN_VOID();
}

void G_Render(void)
{
    PERF_ENTER();
    ASSERT_IN_MAIN_THREAD();
    ss_e status;
    (void)status;

    R_PushCmd((struct rcmd){ R_GL_BeginFrame, 0 });
    E_Global_NotifyImmediate(EVENT_RENDER_3D_PRE, NULL, ES_ENGINE);

    struct render_input in;
    g_create_render_input(&in);

    struct render_input *rcopy = g_push_render_input(in);
    G_RenderMapAndEntities(rcopy);

    struct sval refract_setting;
    status = Settings_Get("pf.video.water_refraction", &refract_setting);
    assert(status == SS_OKAY);

    struct sval reflect_setting;
    status = Settings_Get("pf.video.water_reflection", &reflect_setting);
    assert(status == SS_OKAY);

    if(s_gs.map && M_WaterMaybeVisible(s_gs.map, s_gs.active_cam)) {

        g_prune_water_input(&in);
        struct render_input *water_rcopy = g_push_render_input(in);

        R_PushCmd((struct rcmd){
            .func = R_GL_DrawWater,
            .nargs = 3,
            .args = { 
                water_rcopy,
                R_PushArg(&refract_setting.as_bool, sizeof(bool)),
                R_PushArg(&reflect_setting.as_bool, sizeof(bool)),
            },
        });
    }
    g_destroy_render_input(&in);

    enum selection_type sel_type;
    const vec_entity_t *selected = G_Sel_Get(&sel_type);
    for(int i = 0; i < vec_size(selected); i++) {

        uint32_t curr = vec_AT(selected, i);
        vec2_t curr_pos = G_Pos_GetXZ(curr);
        const float width = 0.4f;
        uint32_t flags = G_FlagsGet(curr);

        if(flags & ENTITY_FLAG_BUILDING) {

            struct obb obb;
            Entity_CurrentOBB(curr, &obb, false);
            R_PushCmd((struct rcmd){
                .func = R_GL_DrawSelectionRectangle,
                .nargs = 4,
                .args = {
                    R_PushArg(&obb, sizeof(obb)),
                    R_PushArg(&width, sizeof(width)),
                    R_PushArg(&g_seltype_color_map[sel_type], sizeof(g_seltype_color_map[0])),
                    (void*)s_gs.prev_tick_map,
                },
            });
        }else{

            float sel_radius = G_GetSelectionRadius(curr);
            R_PushCmd((struct rcmd){
                .func = R_GL_DrawSelectionCircle,
                .nargs = 5,
                .args = {
                    R_PushArg(&curr_pos, sizeof(curr_pos)),
                    R_PushArg(&sel_radius, sizeof(sel_radius)),
                    R_PushArg(&width, sizeof(width)),
                    R_PushArg(&g_seltype_color_map[sel_type], sizeof(g_seltype_color_map[0])),
                    (void*)s_gs.prev_tick_map,
                },
            });
        }
    }

    E_Global_NotifyImmediate(EVENT_RENDER_3D_POST, NULL, ES_ENGINE);
    R_PushCmd((struct rcmd) { R_GL_SetScreenspaceDrawMode, 0 });

    if(!s_gs.hide_healthbars) {
        g_render_healthbars();
    }

    E_Global_NotifyImmediate(EVENT_RENDER_UI, NULL, ES_ENGINE);

    if(s_gs.map) {
        M_RenderMinimap(s_gs.map, s_gs.active_cam);
        g_render_minimap_units();
        R_PushCmd((struct rcmd){ R_GL_MapInvalidate, 0 });
    }

    E_Global_NotifyImmediate(EVENT_RENDER_FINISH, NULL, ES_ENGINE);
    PERF_RETURN_VOID();
}

void G_RenderMapAndEntities(struct render_input *in)
{
    PERF_ENTER();
    if(in->shadows) {
        g_shadow_pass(in);
    }
    g_draw_pass(in);
    PERF_RETURN_VOID();
}

void G_FlagsSet(uint32_t uid, uint32_t flags)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(id, s_gs.ent_flag_map, uid);
    if(k == kh_end(s_gs.ent_flag_map)) {
        int status;
        k = kh_put(id, s_gs.ent_flag_map, uid, &status);
        assert(status != -1);
    }
    kh_value(s_gs.ent_flag_map, k) = flags;
}

uint32_t G_FlagsGet(uint32_t uid)
{
    khiter_t k = kh_get(id, s_gs.ent_flag_map, uid);
    assert(k != kh_end(s_gs.ent_flag_map));
    return kh_value(s_gs.ent_flag_map, k);
}

uint32_t G_FlagsGetFrom(khash_t(id) *table, uint32_t uid)
{
    khiter_t k = kh_get(id, table, uid);
    assert(k != kh_end(table));
    return kh_value(table, k);
}

khash_t(id) *G_FlagsCopyTable(void)
{
	return kh_copy_id(s_gs.ent_flag_map);
}

bool G_AddEntity(uint32_t uid, uint32_t flags, vec3_t pos)
{
    ASSERT_IN_MAIN_THREAD();
    assert(!(flags & ENTITY_FLAG_BUILDING) || !(flags & ENTITY_FLAG_BUILDER));

    int ret;
    khiter_t k;

    k = kh_put(entity, s_gs.active, uid, &ret);
    if(ret == -1 || ret == 0)
        return false;

    k = kh_put(id, s_gs.ent_faction_map, uid, &ret);
    if(ret == -1 || ret == 0)
        return false;
    kh_value(s_gs.ent_faction_map, k) = 0;

    k = kh_put(range, s_gs.ent_visrange_map, uid, &ret);
    if(ret == -1 || ret == 0)
        return false;
    kh_value(s_gs.ent_visrange_map, k) = 0.0f;

    k = kh_put(range, s_gs.selection_radiuses, uid, &ret);
    if(ret == -1 || ret == 0)
        return false;
    kh_value(s_gs.selection_radiuses, k) = 0.0f;

    G_FlagsSet(uid, flags);
    G_Pos_Set(uid, pos);

    if(flags & ENTITY_FLAG_ANIMATED)
        A_AddEntity(uid);

    if(flags & ENTITY_FLAG_STORAGE_SITE)
        G_StorageSite_AddEntity(uid);

    if(flags & ENTITY_FLAG_BUILDING)
        G_Building_AddEntity(uid);

    if(flags & ENTITY_FLAG_BUILDER)
        G_Builder_AddEntity(uid);

    if(flags & ENTITY_FLAG_COMBATABLE)
        G_Combat_AddEntity(uid, COMBAT_STANCE_AGGRESSIVE);

    if(flags & ENTITY_FLAG_RESOURCE)
        G_Resource_AddEntity(uid);

    if(flags & ENTITY_FLAG_HARVESTER)
        G_Harvester_AddEntity(uid);

    if(flags & ENTITY_FLAG_MOVABLE) {
    
        k = kh_put(entity, s_gs.dynamic, uid, &ret);
        assert(ret != -1 && ret != 0);

        G_Move_AddEntity(uid, pos, 0.0f, 0);

        uint32_t gpu_id = kh_size(s_gs.ent_gpu_id_map) + 1;
        k = kh_put(id, s_gs.ent_gpu_id_map, uid, &ret);
        assert(ret != -1 && ret != 0);
        kh_value(s_gs.ent_gpu_id_map, k) = gpu_id;

        k = kh_put(id, s_gs.gpu_id_ent_map, gpu_id, &ret);
        assert(ret != -1 && ret != 0);
        kh_value(s_gs.gpu_id_ent_map, k) = uid;

        assert(kh_size(s_gs.dynamic) == kh_size(s_gs.ent_gpu_id_map));
        assert(kh_size(s_gs.ent_gpu_id_map) == kh_size(s_gs.gpu_id_ent_map));
    }

    return true;
}

bool G_RemoveEntity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(entity, s_gs.active, uid);
    if(k == kh_end(s_gs.active))
        return false;
    kh_del(entity, s_gs.active, k);

    uint32_t flags = G_FlagsGet(uid);
    if(flags & ENTITY_FLAG_MOVABLE)
        g_delete_gpuid(uid);

    int idx = vec_entity_indexof(&s_gs.visible, uid, g_entities_equal);
    if(idx != -1) {
        vec_entity_del(&s_gs.visible, idx);
        vec_obb_del(&s_gs.visible_obbs, idx);
    }

    idx = vec_entity_indexof(&s_gs.light_visible, uid, g_entities_equal);
    if(idx != -1) {
        vec_entity_del(&s_gs.light_visible, idx);
    }

    A_RemoveEntity(uid);
    G_Sel_Remove(uid);
    G_Move_RemoveEntity(uid);
    G_Combat_RemoveEntity(uid);
    G_Building_RemoveEntity(uid);
    G_Builder_RemoveEntity(uid);
    G_Harvester_RemoveEntity(uid);
    G_Resource_RemoveEntity(uid);
    G_StorageSite_RemoveEntity(uid);
    G_Region_RemoveEnt(uid);
    G_Pos_Delete(uid);
    Entity_Remove(uid);

    k = kh_get(id, s_gs.ent_faction_map, uid);
    assert(k != kh_end(s_gs.ent_faction_map));
    kh_del(id, s_gs.ent_faction_map, k);

    k = kh_get(range, s_gs.ent_visrange_map, uid);
    assert(k != kh_end(s_gs.ent_visrange_map));
    kh_del(range, s_gs.ent_visrange_map, k);

    k = kh_get(range, s_gs.selection_radiuses, uid);
    assert(k != kh_end(s_gs.selection_radiuses));
    kh_del(range, s_gs.selection_radiuses, k);

    G_Sel_MarkHoveredDirty();
    return true;
}

void G_StopEntity(uint32_t uid, bool stop_move)
{
    ASSERT_IN_MAIN_THREAD();
    uint32_t flags = G_FlagsGet(uid);

    if(flags & ENTITY_FLAG_COMBATABLE) {
        G_Combat_StopAttack(uid);
        G_Combat_SetStance(uid, COMBAT_STANCE_AGGRESSIVE);
    }
    if(flags & ENTITY_FLAG_HARVESTER) {
        G_Harvester_Stop(uid);
        G_Harvester_ClearQueuedCmd(uid);
    }
    if(stop_move && (flags & ENTITY_FLAG_MOVABLE)) {
        G_Move_Stop(uid);
    }
    if(flags & ENTITY_FLAG_BUILDER) {
        G_Builder_Stop(uid);
    }

    E_Entity_Notify(EVENT_ENTITY_STOP, uid, NULL, ES_ENGINE);
}

void G_DeferredRemove(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    vec_entity_push(&s_gs.removed, uid);
}

void G_FreeEntity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    AL_EntityFree(uid);
}

uint32_t G_GPUIDForEnt(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    khiter_t k = kh_get(id, s_gs.ent_gpu_id_map, uid);
    if(k == kh_end(s_gs.ent_gpu_id_map))
        return 0;
    return kh_value(s_gs.ent_gpu_id_map, k);
}

uint32_t G_EntForGPUID(uint32_t gpuid)
{
    ASSERT_IN_MAIN_THREAD();
    assert(gpuid >= 1 && gpuid <= kh_size(G_GetDynamicEntsSet()));
    khiter_t k = kh_get(id, s_gs.gpu_id_ent_map, gpuid);
    assert(k != kh_end(s_gs.gpu_id_ent_map));
    return kh_value(s_gs.gpu_id_ent_map, k);
}

bool G_AddFaction(const char *name, vec3_t color)
{
    ASSERT_IN_MAIN_THREAD();

    if(g_num_factions() == MAX_FACTIONS)
        return false;
    if(strlen(name) >= sizeof(s_gs.factions[0].name))
        return false;

    int new_fac_id = g_alloc_faction();
    strcpy(s_gs.factions[new_fac_id].name, name);
    s_gs.factions[new_fac_id].color = color;
    s_gs.factions[new_fac_id].controllable = true;

    /* By default, a new faction is mutually at peace with 
     * every other faction. */
    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(!(s_gs.factions_allocd & (0x1 << i)))
            continue;
        s_gs.diplomacy_table[i][new_fac_id] = DIPLOMACY_STATE_PEACE;
        s_gs.diplomacy_table[new_fac_id][i] = DIPLOMACY_STATE_PEACE;
    }

    E_Global_Notify(EVENT_UPDATE_FACTION, (void*)((uintptr_t)new_fac_id), ES_ENGINE);
    return true;
}

bool G_RemoveFaction(int faction_id)
{
    ASSERT_IN_MAIN_THREAD();

    if(!(s_gs.factions_allocd & (0x1 << faction_id)))
        return false;

    uint32_t key, curr;

    kh_foreach(s_gs.active, key, curr, {
        if(G_GetFactionID(key) == faction_id)
            G_Zombiefy(curr, true);
    });

    s_gs.factions_allocd &= ~(0x1 << faction_id);
    E_Global_Notify(EVENT_UPDATE_FACTION, (void*)((uintptr_t)faction_id), ES_ENGINE);
    return true;
}

bool G_UpdateFaction(int faction_id, const char *name, vec3_t color, bool control)
{
    ASSERT_IN_MAIN_THREAD();

    if(!(s_gs.factions_allocd & (0x1 << faction_id)))
        return false;
    if(strlen(name) >= sizeof(s_gs.factions[0].name))
        return false;

    if(s_gs.factions[faction_id].controllable != control) {
        G_Fog_ClearExploredCache();
    }

    pf_strlcpy(s_gs.factions[faction_id].name, name, sizeof(s_gs.factions[0].name));
    s_gs.factions[faction_id].color = color;
    s_gs.factions[faction_id].controllable = control;
    E_Global_Notify(EVENT_UPDATE_FACTION, (void*)((uintptr_t)faction_id), ES_ENGINE);
    return true;
}

uint16_t G_GetFactions(char out_names[][MAX_FAC_NAME_LEN], vec3_t *out_colors, bool *out_ctrl)
{
    ASSERT_IN_MAIN_THREAD();

    for(int i = 0; i < MAX_FACTIONS; i++) {

        if(!(s_gs.factions_allocd & (0x1 << i)))
            continue;
    
        if(out_names) {
            pf_strlcpy(out_names[i], s_gs.factions[i].name, MAX_FAC_NAME_LEN);
        }
        if(out_colors) {
            out_colors[i] = s_gs.factions[i].color;
        }
        if(out_ctrl) {
            out_ctrl[i] = s_gs.factions[i].controllable;
        }
    }
    return s_gs.factions_allocd;
}

uint16_t G_GetPlayerControlledFactions(void)
{
    uint16_t ret = 0;

    for(int i = 0; i < MAX_FACTIONS; i++) {
        if(!(s_gs.factions_allocd & (0x1 << i)))
            continue;
        if(!s_gs.factions[i].controllable)
            continue;
        ret |= (0x1 << i);
    }
    return ret;
}

uint16_t G_GetEnemyFactions(int faction_id)
{
    uint16_t ret = 0;

    for(int i = 0; i < MAX_FACTIONS; i++) {

        enum diplomacy_state ds;
        if(!G_GetDiplomacyState(i, faction_id, &ds))
            continue;
        if(ds != DIPLOMACY_STATE_WAR)
            continue;
        ret |= (0x1 << i);
    }
    return ret;
}

void G_SetFactionID(uint32_t uid, int faction_id)
{
    ASSERT_IN_MAIN_THREAD();

    int old = G_GetFactionID(uid);
    if(old == faction_id)
        return;

    khiter_t k = kh_get(id, s_gs.ent_faction_map, uid);
    assert(k != kh_end(s_gs.ent_faction_map));
    kh_value(s_gs.ent_faction_map, k) = faction_id;

    vec2_t xz_pos = G_Pos_GetXZ(uid);
    float vrange = G_GetVisionRange(uid);

    G_Fog_RemoveVision(xz_pos, old, vrange);
    G_Fog_AddVision(xz_pos, faction_id, vrange);

    G_Combat_UpdateRef(old, faction_id, xz_pos);
    G_Move_UpdateFactionID(uid, old, faction_id);
    G_StorageSite_UpdateFaction(uid, old, faction_id);
    G_Resource_UpdateFactionID(uid, old, faction_id);
    G_Building_UpdateFactionID(uid, old, faction_id);
}

int G_GetFactionID(uint32_t uid)
{
    khiter_t k = kh_get(id, s_gs.ent_faction_map, uid);
    assert(k != kh_end(s_gs.ent_faction_map));
    return kh_value(s_gs.ent_faction_map, k);
}

khash_t(id) *G_FactionIDCopyTable(void)
{
	return kh_copy_id(s_gs.ent_faction_map);
}

int G_GetFactionIDFrom(khash_t(id) *table, uint32_t uid)
{
    khiter_t k = kh_get(id, table, uid);
    assert(k != kh_end(table));
    return kh_value(table, k);
}

void G_SetVisionRange(uint32_t uid, float range)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(range, s_gs.ent_visrange_map, uid);
    assert(k != kh_end(s_gs.ent_visrange_map));

    float oldrange = kh_value(s_gs.ent_visrange_map, k);
    vec2_t xz_pos = G_Pos_GetXZ(uid);

    G_Fog_UpdateVisionRange(xz_pos, G_GetFactionID(uid), oldrange, range);
    kh_value(s_gs.ent_visrange_map, k) = range;
}

float G_GetVisionRange(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(range, s_gs.ent_visrange_map, uid);
    assert(k != kh_end(s_gs.ent_visrange_map));
    return kh_value(s_gs.ent_visrange_map, k);
}

void G_SetSelectionRadius(uint32_t uid, float range)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(range, s_gs.selection_radiuses, uid);
    assert(k != kh_end(s_gs.selection_radiuses));

    G_Move_UpdateSelectionRadius(uid, range);
    G_Resource_UpdateSelectionRadius(uid, range);
    kh_value(s_gs.selection_radiuses, k) = range;
}

float G_GetSelectionRadius(uint32_t uid)
{
    khiter_t k = kh_get(range, s_gs.selection_radiuses, uid);
    assert(k != kh_end(s_gs.selection_radiuses));
    return kh_value(s_gs.selection_radiuses, k);
}

khash_t(range) *G_SelectionRadiusCopyTable(void)
{
	return kh_copy_range(s_gs.selection_radiuses);
}

float G_GetSelectionRadiusFrom(khash_t(range) *table, uint32_t uid)
{
    khiter_t k = kh_get(range, table, uid);
    assert(k != kh_end(table));
    return kh_value(table, k);
}

bool G_SetDiplomacyState(int fac_id_a, int fac_id_b, enum diplomacy_state ds)
{
    ASSERT_IN_MAIN_THREAD();

    if(!(s_gs.factions_allocd & (0x1 << fac_id_a)))
        return false;
    if(!(s_gs.factions_allocd & (0x1 << fac_id_b)))
        return false;
    if(fac_id_a == fac_id_b)
        return false;

    s_gs.diplomacy_table[fac_id_a][fac_id_b] = ds;
    s_gs.diplomacy_table[fac_id_b][fac_id_a] = ds;
    return true;
}

bool G_GetDiplomacyState(int fac_id_a, int fac_id_b, enum diplomacy_state *out)
{
    if(!(s_gs.factions_allocd & (0x1 << fac_id_a)))
        return false;
    if(!(s_gs.factions_allocd & (0x1 << fac_id_b)))
        return false;
    if(fac_id_a == fac_id_b)
        return false;

    *out = s_gs.diplomacy_table[fac_id_a][fac_id_b];
    return true;
}

enum diplomacy_state (*G_CopyDiplomacyTable(void))[MAX_FACTIONS]
{
    void *ret = malloc(sizeof(s_gs.diplomacy_table));
    if(!ret)
        return NULL;
    memcpy(ret, s_gs.diplomacy_table, sizeof(s_gs.diplomacy_table));
    return ret;
}

bool G_GetDiplomacyStateFrom(enum diplomacy_state (*table)[MAX_FACTIONS],
                             int fac_id_a, int fac_id_b, enum diplomacy_state *out)
{
    if(fac_id_a == fac_id_b)
        return false;

    *out = table[fac_id_a][fac_id_b];
    return true;
}

void G_SetActiveCamera(struct camera *cam, enum cam_mode mode)
{
    ASSERT_IN_MAIN_THREAD();

    M_Raycast_Uninstall();

    switch(mode) {
    case CAM_MODE_RTS:  

        CamControl_RTS_Install(cam);
        if(s_gs.map) {
            M_RestrictRTSCamToMap(s_gs.map, cam);
        }
        break;

    case CAM_MODE_FPS:  

        CamControl_FPS_Install(cam);
        break;

    case CAM_MODE_FREE: 

        CamControl_Free_Install(cam);
        break;

    default: assert(0);
    }

    s_gs.active_cam = cam;
    s_gs.active_cam_mode = mode;

    if(s_gs.map) {
        M_Raycast_Install(s_gs.map, cam);
    }
}

struct camera *G_GetActiveCamera(void)
{
    ASSERT_IN_MAIN_THREAD();
    return s_gs.active_cam;
}

enum cam_mode G_GetCameraMode(void)
{
    ASSERT_IN_MAIN_THREAD();
    return s_gs.active_cam_mode;
}

void G_MoveActiveCamera(vec2_t xz_ground_pos)
{
    ASSERT_IN_MAIN_THREAD();

    vec3_t old_pos = Camera_GetPos(s_gs.active_cam);
    float offset_mag = cos(DEG_TO_RAD(Camera_GetPitch(s_gs.active_cam))) * Camera_GetHeight(s_gs.active_cam);

    /* We position the camera such that the camera ray intersects the ground plane (Y=0)
     * at the specified xz position. */
    vec3_t new_pos = (vec3_t) {
        xz_ground_pos.x - cos(DEG_TO_RAD(Camera_GetYaw(s_gs.active_cam))) * offset_mag,
        old_pos.y,
        xz_ground_pos.z + sin(DEG_TO_RAD(Camera_GetYaw(s_gs.active_cam))) * offset_mag 
    };

    Camera_SetPos(s_gs.active_cam, new_pos);
}

bool G_UpdateTile(const struct tile_desc *desc, const struct tile *tile)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        return false;
    return M_AL_UpdateTile(s_gs.map, desc, tile);
}

bool G_GetTile(const struct tile_desc *desc, struct tile *out)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_gs.map)
        return false;
    if(!M_TileForDesc(s_gs.map, *desc, &out))
        return false;
    return true;
}

const khash_t(entity) *G_GetDynamicEntsSet(void)
{
    ASSERT_IN_MAIN_THREAD();

    return s_gs.dynamic;
}

const khash_t(entity) *G_GetAllEntsSet(void)
{
    return s_gs.active;
}

void G_SetSimState(enum simstate ss)
{
    ASSERT_IN_MAIN_THREAD();

    /* Only change the simulation states at frame boundaries. This has some nice 
     * guarantees, such as that all handlers for a particular event will run, even
     * if some handler requests a change of the simulation state state midway. 
     */
    s_gs.requested_ss = ss;
}

void G_UpdateSimStateChangeTick(void)
{
    ASSERT_IN_MAIN_THREAD();
    s_gs.ss_change_tick = SDL_GetTicks();
}

void G_SetLightPos(vec3_t pos)
{
    ASSERT_IN_MAIN_THREAD();

    s_gs.light_pos = pos;
    R_PushCmd((struct rcmd){
        .func = R_GL_SetLightPos,
        .nargs = 1,
        .args = { R_PushArg(&pos, sizeof(pos)) },
    });
}

vec3_t G_GetLightPos(void)
{
    ASSERT_IN_MAIN_THREAD();
    return s_gs.light_pos;
}

enum simstate G_GetSimState(void)
{
    ASSERT_IN_MAIN_THREAD();

    return s_gs.ss;
}

void G_Zombiefy(uint32_t uid, bool invis)
{
    ASSERT_IN_MAIN_THREAD();
    uint32_t flags = G_FlagsGet(uid);

    if(flags & ENTITY_FLAG_SELECTABLE)
        G_Sel_Remove(uid);

    if(flags & ENTITY_FLAG_MOVABLE)
        g_delete_gpuid(uid);

    G_Move_RemoveEntity(uid);
    G_Combat_RemoveEntity(uid);
    G_Building_RemoveEntity(uid);
    G_Builder_RemoveEntity(uid);
    G_Harvester_RemoveEntity(uid);
    G_Resource_RemoveEntity(uid);
    G_StorageSite_RemoveEntity(uid);

    G_SetVisionRange(uid, 0.0f);
    G_Region_RemoveEnt(uid);
    Entity_ClearTags(uid);

    flags &= ~ENTITY_FLAG_SELECTABLE;
    flags &= ~ENTITY_FLAG_COLLISION;
    flags &= ~ENTITY_FLAG_COMBATABLE;
    flags &= ~ENTITY_FLAG_BUILDING;
    flags &= ~ENTITY_FLAG_MOVABLE;
    flags &= ~ENTITY_FLAG_BUILDER;
    flags &= ~ENTITY_FLAG_HARVESTER;
    flags &= ~ENTITY_FLAG_RESOURCE;
    flags &= ~ENTITY_FLAG_STORAGE_SITE;

    flags |= ENTITY_FLAG_ZOMBIE;

    if(invis) {
        flags |= ENTITY_FLAG_INVISIBLE;
    }
    G_FlagsSet(uid, flags);
}

bool G_EntityExists(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    khiter_t k = kh_get(entity, s_gs.active, uid);
    return (k != kh_end(s_gs.active));
}

bool G_EntityIsZombie(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    if(!G_EntityExists(uid))
        return false;
    return (G_FlagsGet(uid) & ENTITY_FLAG_ZOMBIE);
}

struct render_workspace *G_GetSimWS(void)
{
    ASSERT_IN_MAIN_THREAD();

    return &s_gs.ws[s_gs.curr_ws_idx];
}

struct render_workspace *G_GetRenderWS(void)
{
    return &s_gs.ws[(s_gs.curr_ws_idx + 1) % 2];
}

void G_SwapBuffers(void)
{
    ASSERT_IN_MAIN_THREAD();

    int sim_idx = s_gs.curr_ws_idx;
    int render_idx = (sim_idx + 1) % 2;

    if(s_gs.map) {
        M_AL_ShallowCopy((struct map*)s_gs.prev_tick_map, s_gs.map);
    }

    g_remove_queued();
    assert(queue_size(s_gs.ws[render_idx].commands) == 0);
    R_ClearWS(&s_gs.ws[render_idx]);
    s_gs.curr_ws_idx = render_idx;

    g_change_simstate();
}

bool G_MapLoaded(void)
{
    ASSERT_IN_MAIN_THREAD();
    return (s_gs.map != NULL);
}

const struct map *G_GetPrevTickMap(void)
{
    ASSERT_IN_MAIN_THREAD();
    return s_gs.prev_tick_map;
}

bool G_MouseInTargetMode(void)
{
    ASSERT_IN_MAIN_THREAD();

    if(G_Move_InTargetMode())
        return true;
    if(G_Builder_InTargetMode())
        return true;
    if(G_Harvester_InTargetMode())
        return true;
    return false;
}

enum ctx_action G_CurrContextualAction(void)
{
    ASSERT_IN_MAIN_THREAD();

    int action;
    if((action = G_Builder_CurrContextualAction()) != CTX_ACTION_NONE)
        return action;
    if((action = G_Harvester_CurrContextualAction()) != CTX_ACTION_NONE)
        return action;
    if((action = G_Combat_CurrContextualAction()) != CTX_ACTION_NONE)
        return action;
    return CTX_ACTION_NONE;
}

void G_NotifyOrderIssued(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    uint32_t flags = G_FlagsGet(uid);
    if(flags & ENTITY_FLAG_HARVESTER) {
        G_Harvester_ClearQueuedCmd(uid);
    }
    if(flags & ENTITY_FLAG_COMBATABLE) {
        G_Combat_ClearSavedMoveCmd(uid);
    }
    E_Global_Notify(EVENT_ORDER_ISSUED, (void*)(uintptr_t)uid, ES_ENGINE);
}

void G_SetHideHealthbars(bool on)
{
    ASSERT_IN_MAIN_THREAD();
    s_gs.hide_healthbars = on;
}

void G_UpdateBounds(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    if(!G_EntityExists(uid))
        return;

    G_Building_UpdateBounds(uid);
    G_Resource_UpdateBounds(uid);
}

bool G_SaveGlobalState(SDL_RWops *stream)
{
    ASSERT_IN_MAIN_THREAD();

    struct attr hasmap = (struct attr){
        .type = TYPE_BOOL, 
        .val.as_bool = (s_gs.map != NULL)
    };
    CHK_TRUE_RET(Attr_Write(stream, &hasmap, "has_map"));

    if(hasmap.val.as_bool && !M_AL_WritePFMap(s_gs.map, stream))
        return false;

    if(hasmap.val.as_bool) {
    
        vec2_t mm_pos;
        G_GetMinimapPos(&mm_pos.x, &mm_pos.y);

        struct attr minimap_pos = (struct attr){
            .type = TYPE_VEC2, 
            .val.as_vec2 = mm_pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &minimap_pos, "minimap_pos"));

        struct attr minimap_border_clr = (struct attr){
            .type = TYPE_QUAT, 
            .val.as_quat = M_MinimapGetBorderClr()
        };
        CHK_TRUE_RET(Attr_Write(stream, &minimap_border_clr, "minimap_border_clr"));

        int mm_size = 0;
        G_GetMinimapSize(&mm_size);
        struct attr minimap_size = (struct attr){
            .type = TYPE_INT, 
            .val.as_int = mm_size
        };
        CHK_TRUE_RET(Attr_Write(stream, &minimap_size, "minimap_size"));

        struct attr highlight_size = (struct attr){
            .type = TYPE_INT, 
            .val.as_int = M_Raycast_GetHighlightSize()
        };
        CHK_TRUE_RET(Attr_Write(stream, &highlight_size, "highlight_size"));

        CHK_TRUE_RET(G_Fog_SaveState(stream));
    }

    Sched_TryYield();

    struct attr ss = (struct attr){
        .type = TYPE_INT, 
        .val.as_int = s_gs.ss
    };
    CHK_TRUE_RET(Attr_Write(stream, &ss, "simstate"));

    struct attr light_pos = (struct attr){
        .type = TYPE_VEC3, 
        .val.as_vec3 = s_gs.light_pos
    };
    CHK_TRUE_RET(Attr_Write(stream, &light_pos, "light_pos"));

    struct attr num_factions = (struct attr){
        .type = TYPE_INT, 
        .val.as_int = g_num_factions()
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_factions, "num_factions"));

    Sched_TryYield();

    for(int i = 0; i < MAX_FACTIONS; i++) {

        if(!(s_gs.factions_allocd & (0x1 << i)))
            continue;
        struct faction fac = s_gs.factions[i];

        struct attr fac_id = (struct attr){
            .type = TYPE_INT, 
            .val.as_int = i
        };
        CHK_TRUE_RET(Attr_Write(stream, &fac_id, "fac_id"));

        struct attr fac_color = (struct attr){
            .type = TYPE_VEC3, 
            .val.as_vec3 = fac.color
        };
        CHK_TRUE_RET(Attr_Write(stream, &fac_color, "fac_color"));

        struct attr fac_name = (struct attr){ .type = TYPE_STRING };
        pf_snprintf(fac_name.val.as_string, sizeof(fac_name.val.as_string), "%s", fac.name);
        CHK_TRUE_RET(Attr_Write(stream, &fac_name, "fac_name"));

        struct attr fac_controllable = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = fac.controllable
        };
        CHK_TRUE_RET(Attr_Write(stream, &fac_controllable, "fac_controllable"));
        Sched_TryYield();
    }

    for(int i = 0; i < MAX_FACTIONS; i++) {
    for(int j = 0; j < MAX_FACTIONS; j++) {

        struct attr dstate = (struct attr){
            .type = TYPE_INT,
            .val.as_int = s_gs.diplomacy_table[i][j]
        };
        CHK_TRUE_RET(Attr_Write(stream, &dstate, "diplomacy_state"));
        Sched_TryYield();
    }}

    struct attr cam_speed = (struct attr){
        .type = TYPE_FLOAT,
        .val.as_float = Camera_GetSpeed(s_gs.active_cam)
    };
    CHK_TRUE_RET(Attr_Write(stream, &cam_speed, "cam_speed"));

    struct attr cam_sens = (struct attr){
        .type = TYPE_FLOAT,
        .val.as_float = Camera_GetSens(s_gs.active_cam)
    };
    CHK_TRUE_RET(Attr_Write(stream, &cam_sens, "cam_sensitivity"));

    struct attr cam_pitch = (struct attr){
        .type = TYPE_FLOAT,
        .val.as_float = Camera_GetPitch(s_gs.active_cam)
    };
    CHK_TRUE_RET(Attr_Write(stream, &cam_pitch, "cam_pitch"));

    Sched_TryYield();

    struct attr cam_yaw = (struct attr){
        .type = TYPE_FLOAT,
        .val.as_float = Camera_GetYaw(s_gs.active_cam)
    };
    CHK_TRUE_RET(Attr_Write(stream, &cam_yaw, "cam_yaw"));

    struct attr cam_pos = (struct attr){
        .type = TYPE_VEC3,
        .val.as_vec3 = Camera_GetPos(s_gs.active_cam)
    };
    CHK_TRUE_RET(Attr_Write(stream, &cam_pos, "cam_position"));

    struct attr active_cam_mode = (struct attr){
        .type = TYPE_INT, 
        .val.as_int = s_gs.active_cam_mode
    };
    CHK_TRUE_RET(Attr_Write(stream, &active_cam_mode, "active_cam_mode"));

    Sched_TryYield();

    struct attr active_font = (struct attr){
        .type = TYPE_STRING, 
    };
    pf_strlcpy(active_font.val.as_string, UI_GetActiveFont(), sizeof(active_font.val.as_string));
    CHK_TRUE_RET(Attr_Write(stream, &active_font, "active_font"));

    struct attr hide_healthbars = (struct attr){
        .type = TYPE_BOOL, 
        .val.as_bool = s_gs.hide_healthbars
    };
    CHK_TRUE_RET(Attr_Write(stream, &hide_healthbars, "hide_healthbars"));

    struct attr minimap_render_all = (struct attr){
        .type = TYPE_BOOL, 
        .val.as_bool = s_gs.minimap_render_all
    };
    CHK_TRUE_RET(Attr_Write(stream, &minimap_render_all, "minimap_render_all"));

    Sched_TryYield();

    if(!G_Region_SaveState(stream))
        return false;

    return true;
}

bool G_LoadGlobalState(SDL_RWops *stream)
{
    ASSERT_IN_MAIN_THREAD();
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_BOOL);
    Sched_TryYield();

    if(attr.val.as_bool) {
        CHK_TRUE_RET(G_LoadMap(stream, true));
        Sched_TryYield();

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        G_SetMinimapPos(attr.val.as_vec2.x, attr.val.as_vec2.y);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_QUAT);
        M_MinimapSetBorderClr(attr.val.as_quat);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        G_SetMinimapSize(attr.val.as_int);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        M_Raycast_SetHighlightSize(attr.val.as_int);

        CHK_TRUE_RET(G_Fog_LoadState(stream));
        Sched_TryYield();
    }else{
        G_ClearState();
        E_Global_Notify(EVENT_NEW_GAME, NULL, ES_ENGINE);
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    G_SetSimState(attr.val.as_int);

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    G_SetLightPos(attr.val.as_vec3);

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    int num_factions = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_factions; i++) {

        struct faction fac;
        uint16_t fac_id;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        fac_id = attr.val.as_int; 

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC3);
        fac.color = attr.val.as_vec3;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        pf_snprintf(fac.name, sizeof(fac.name), "%s", attr.val.as_string);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        fac.controllable = attr.val.as_bool;

        s_gs.factions_allocd |= (0x1 << fac_id);
        s_gs.factions[fac_id] = fac;
        Sched_TryYield();
    }

    for(int i = 0; i < MAX_FACTIONS; i++) {
    for(int j = 0; j < MAX_FACTIONS; j++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        s_gs.diplomacy_table[i][j] = attr.val.as_int;
        Sched_TryYield();
    }}

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_FLOAT);
    Camera_SetSpeed(s_gs.active_cam, attr.val.as_float);

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_FLOAT);
    Camera_SetSens(s_gs.active_cam, attr.val.as_float);

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_FLOAT);
    float pitch = attr.val.as_float; 

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_FLOAT);
    float yaw = attr.val.as_float; 
    Camera_SetPitchAndYaw(s_gs.active_cam, pitch, yaw);

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    Camera_SetPos(s_gs.active_cam, attr.val.as_vec3);

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    int active_cam_mode = attr.val.as_int;

    G_SetActiveCamera(s_gs.active_cam, active_cam_mode);
    Sched_TryYield();

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_STRING);
    UI_SetActiveFont(attr.val.as_string);

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_BOOL);
    s_gs.hide_healthbars = attr.val.as_bool;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_BOOL);
    s_gs.minimap_render_all = attr.val.as_bool;
    Sched_TryYield();

    if(!G_Region_LoadState(stream))
        return false;

    return true;
}

bool G_SaveEntityState(SDL_RWops *stream)
{
    ASSERT_IN_MAIN_THREAD();
    g_remove_queued();

    if(!g_save_anim_state(stream))
        return false;

    if(!G_Sel_SaveState(stream))
        return false;

    /* Movement, combat, etc. state is only saved for sessions with a loaded map */
    if(!s_gs.map)
        return true;
    
    if(!G_Move_SaveState(stream))
        return false;

    if(!G_Combat_SaveState(stream))
        return false;

    if(!G_Building_SaveState(stream))
        return false;

    if(!G_Builder_SaveState(stream))
        return false;

    if(!G_StorageSite_SaveState(stream))
        return false;

    if(!G_Resource_SaveState(stream))
        return false;

    if(!G_Harvester_SaveState(stream))
        return false;

    return true;
}

bool G_LoadEntityState(SDL_RWops *stream)
{
    ASSERT_IN_MAIN_THREAD();

    if(s_gs.map) {
        G_BakeNavDataForScene();
    }

    if(!g_load_anim_state(stream))
        return false;

    if(!G_Sel_LoadState(stream))
        return false;

    if(!s_gs.map)
        return true;

    if(!G_Move_LoadState(stream))
        return false;

    if(!G_Combat_LoadState(stream))
        return false;

    if(!G_Building_LoadState(stream))
        return false;

    if(!G_Builder_LoadState(stream))
        return false;

    if(!G_StorageSite_LoadState(stream))
        return false;

    if(!G_Resource_LoadState(stream))
        return false;

    if(!G_Harvester_LoadState(stream))
        return false;

    return true;
}

