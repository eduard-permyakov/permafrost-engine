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
 */

#include "public/render_ctrl.h"
#include "public/render.h"
#include "backend_local.h"
#include "../settings.h"
#include "../main.h"
#include "../game/public/game.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>


#define EPSILON     (1.0f / 1024)

struct render_private;
struct vertex;

/*****************************************************************************/
/* GLOBAL VARIABLES                                                          */
/*****************************************************************************/

bool g_trace_gpu;

void R_Cmd_Init(struct render_private *priv, const char *shader,
               const struct vertex *vbuff)
{
    (void)priv;
    (void)shader;
    (void)vbuff;
    assert(!"R_Cmd_Init is a render command identity");
}

void R_Cmd_Draw(const void *render_private, mat4x4_t *model,
               const bool *translucent)
{
    (void)render_private;
    (void)model;
    (void)translucent;
    assert(!"R_Cmd_Draw is a render command identity");
}

void R_Cmd_BeginFrame(void)
{
    assert(!"R_Cmd_BeginFrame is a render command identity");
}

void R_Cmd_EndFrame(void)
{
    assert(!"R_Cmd_EndFrame is a render command identity");
}

void R_Cmd_SetViewMatAndPos(const mat4x4_t *view, const vec3_t *pos)
{
    (void)view;
    (void)pos;
    assert(!"R_Cmd_SetViewMatAndPos is a render command identity");
}

void R_Cmd_SetProj(const mat4x4_t *proj)
{
    (void)proj;
    assert(!"R_Cmd_SetProj is a render command identity");
}

void R_Cmd_SetAmbientLightColor(const vec3_t *color)
{
    (void)color;
    assert(!"R_Cmd_SetAmbientLightColor is a render command identity");
}

void R_Cmd_SetLightEmitColor(const vec3_t *color)
{
    (void)color;
    assert(!"R_Cmd_SetLightEmitColor is a render command identity");
}

void R_Cmd_SetLightPos(const vec3_t *pos)
{
    (void)pos;
    assert(!"R_Cmd_SetLightPos is a render command identity");
}

void R_Cmd_SetScreenspaceDrawMode(void)
{
    assert(!"R_Cmd_SetScreenspaceDrawMode is a render command identity");
}

void R_Cmd_DrawBox2D(const vec2_t *screen_pos, const vec2_t *signed_size,
                    const vec3_t *color, const float *width)
{
    (void)screen_pos;
    (void)signed_size;
    (void)color;
    (void)width;
    assert(!"R_Cmd_DrawBox2D is a render command identity");
}

void R_Cmd_DrawLine(vec2_t endpoints[], const float *width, const vec3_t *color,
                   const struct map *map)
{
    (void)endpoints;
    (void)width;
    (void)color;
    (void)map;
    assert(!"R_Cmd_DrawLine is a render command identity");
}

void R_Cmd_DrawQuad(vec2_t corners[], const float *width, const vec3_t *color,
                   const struct map *map)
{
    (void)corners;
    (void)width;
    (void)color;
    (void)map;
    assert(!"R_Cmd_DrawQuad is a render command identity");
}

void R_Cmd_DrawOrigin(const void *render_private, mat4x4_t *model)
{
    (void)render_private;
    (void)model;
    assert(!"R_Cmd_DrawOrigin is a render command identity");
}

void R_Cmd_DrawRay(const vec3_t *origin, const vec3_t *dir, mat4x4_t *model,
                  const vec3_t *color, const float *t)
{
    (void)origin;
    (void)dir;
    (void)model;
    (void)color;
    (void)t;
    assert(!"R_Cmd_DrawRay is a render command identity");
}

void R_Cmd_DrawOBB(const struct aabb *aabb, const mat4x4_t *model)
{
    (void)aabb;
    (void)model;
    assert(!"R_Cmd_DrawOBB is a render command identity");
}

void R_Cmd_DrawSelectionCircle(const vec2_t *xz, const float *radius,
                              const float *width, const vec3_t *color,
                              const struct map *map)
{
    (void)xz;
    (void)radius;
    (void)width;
    (void)color;
    (void)map;
    assert(!"R_Cmd_DrawSelectionCircle is a render command identity");
}

void R_Cmd_DrawSelectionRectangle(const struct obb *box, const float *width,
                                 const vec3_t *color, const struct map *map)
{
    (void)box;
    (void)width;
    (void)color;
    (void)map;
    assert(!"R_Cmd_DrawSelectionRectangle is a render command identity");
}

void R_Cmd_DrawMapOverlayQuads(vec2_t *xz_corners, vec3_t *colors,
                              const size_t *count, mat4x4_t *model,
                              bool *on_water_surface, const struct map *map)
{
    (void)xz_corners;
    (void)colors;
    (void)count;
    (void)model;
    (void)on_water_surface;
    (void)map;
    assert(!"R_Cmd_DrawMapOverlayQuads is a render command identity");
}

void R_Cmd_DrawFlowField(vec2_t *xz_positions, vec2_t *xz_directions,
                        const size_t *count, mat4x4_t *model,
                        const struct map *map)
{
    (void)xz_positions;
    (void)xz_directions;
    (void)count;
    (void)model;
    (void)map;
    assert(!"R_Cmd_DrawFlowField is a render command identity");
}

void R_Cmd_DrawCombinedHRVO(vec2_t *apexes, vec2_t *left_rays,
                           vec2_t *right_rays, const size_t *num_vos,
                           const struct map *map)
{
    (void)apexes;
    (void)left_rays;
    (void)right_rays;
    (void)num_vos;
    (void)map;
    assert(!"R_Cmd_DrawCombinedHRVO is a render command identity");
}

void R_Cmd_DrawLoadingScreen(const char *path)
{
    (void)path;
    assert(!"R_Cmd_DrawLoadingScreen is a render command identity");
}

void R_Cmd_DrawHealthbars(const size_t *num_ents, GLfloat *ent_health_pc,
                         vec3_t *ent_top_pos_ws, int *yoffsets,
                         const struct camera *cam)
{
    (void)num_ents;
    (void)ent_health_pc;
    (void)ent_top_pos_ws;
    (void)yoffsets;
    (void)cam;
    assert(!"R_Cmd_DrawHealthbars is a render command identity");
}

void R_Cmd_DrawSkeleton(mat4x4_t *model, const struct skeleton *skel,
                       const struct camera *cam)
{
    (void)model;
    (void)skel;
    (void)cam;
    assert(!"R_Cmd_DrawSkeleton is a render command identity");
}

void R_Cmd_DrawModelToTexture(const void *render_private, const struct obb *obb,
                             struct ent_anim_rstate *anim_state,
                             const char *key)
{
    (void)render_private;
    (void)obb;
    (void)anim_state;
    (void)key;
    assert(!"R_Cmd_DrawModelToTexture is a render command identity");
}

void R_Cmd_DrawNormals(const void *render_private, mat4x4_t *model,
                      const bool *anim)
{
    (void)render_private;
    (void)model;
    (void)anim;
    assert(!"R_Cmd_DrawNormals is a render command identity");
}

void R_Cmd_DepthPassBegin(const vec3_t *light_pos, const vec3_t *cam_pos,
                         const vec3_t *cam_dir)
{
    (void)light_pos;
    (void)cam_pos;
    (void)cam_dir;
    assert(!"R_Cmd_DepthPassBegin is a render command identity");
}

void R_Cmd_DepthPassEnd(void)
{
    assert(!"R_Cmd_DepthPassEnd is a render command identity");
}

void R_Cmd_RenderDepthMap(const void *render_private, mat4x4_t *model)
{
    (void)render_private;
    (void)model;
    assert(!"R_Cmd_RenderDepthMap is a render command identity");
}

void R_Cmd_SetShadowsEnabled(const bool *on)
{
    (void)on;
    assert(!"R_Cmd_SetShadowsEnabled is a render command identity");
}

void R_Cmd_Batch_RenderDepthMap(struct render_input *in)
{
    (void)in;
    assert(!"R_Cmd_Batch_RenderDepthMap is a render command identity");
}

void R_Cmd_Batch_Draw(struct render_input *in)
{
    (void)in;
    assert(!"R_Cmd_Batch_Draw is a render command identity");
}

void R_Cmd_Batch_DrawWithID(struct render_input *in, enum batch_id *id)
{
    (void)in;
    (void)id;
    assert(!"R_Cmd_Batch_DrawWithID is a render command identity");
}

void R_Cmd_Batch_Reset(void)
{
    assert(!"R_Cmd_Batch_Reset is a render command identity");
}

void R_Cmd_Batch_AllocChunks(struct map_resolution *res)
{
    (void)res;
    assert(!"R_Cmd_Batch_AllocChunks is a render command identity");
}

void R_Cmd_AnimAppendData(GLfloat *data, size_t *size)
{
    (void)data;
    (void)size;
    assert(!"R_Cmd_AnimAppendData is a render command identity");
}

void R_Cmd_AnimSetUniforms(mat4x4_t *normal_mat,
                          struct anim_pose_data_desc *desc,
                          const uint32_t *uid)
{
    (void)normal_mat;
    (void)desc;
    (void)uid;
    assert(!"R_Cmd_AnimSetUniforms is a render command identity");
}

void R_Cmd_SpriteRenderBatch(struct sprite_desc *sprites, size_t *nsprites,
                            const struct camera *cam)
{
    (void)sprites;
    (void)nsprites;
    (void)cam;
    assert(!"R_Cmd_SpriteRenderBatch is a render command identity");
}

void R_Cmd_MapInit(const char map_texfiles[][256], const size_t *num_textures,
                  const struct map_resolution *res)
{
    (void)map_texfiles;
    (void)num_textures;
    (void)res;
    assert(!"R_Cmd_MapInit is a render command identity");
}

void R_Cmd_MapShutdown(void)
{
    assert(!"R_Cmd_MapShutdown is a render command identity");
}

void R_Cmd_MapBegin(const bool *shadows, const vec2_t *pos,
                   size_t *num_splats, const struct splatmap *splatmap,
                   const struct map_resolution *res, const struct map *map)
{
    (void)shadows;
    (void)pos;
    (void)num_splats;
    (void)splatmap;
    (void)res;
    (void)map;
    assert(!"R_Cmd_MapBegin is a render command identity");
}

void R_Cmd_MapEnd(void)
{
    assert(!"R_Cmd_MapEnd is a render command identity");
}

void R_Cmd_MapUpdateFog(void *buff, const size_t *size)
{
    (void)buff;
    (void)size;
    assert(!"R_Cmd_MapUpdateFog is a render command identity");
}

void R_Cmd_MapInvalidate(void)
{
    assert(!"R_Cmd_MapInvalidate is a render command identity");
}

void R_Cmd_Texture_GetOrLoad(const char *basedir, const char *name, GLuint *out)
{
    (void)basedir;
    (void)name;
    (void)out;
    assert(!"R_Cmd_Texture_GetOrLoad is a render command identity");
}

void R_Cmd_PositionsUploadData(vec3_t *posbuff, uint32_t *idbuff,
                              const size_t *nents, const struct map *map)
{
    (void)posbuff;
    (void)idbuff;
    (void)nents;
    (void)map;
    assert(!"R_Cmd_PositionsUploadData is a render command identity");
}

void R_Cmd_PositionsInvalidateData(void)
{
    assert(!"R_Cmd_PositionsInvalidateData is a render command identity");
}

void R_Cmd_MoveUpdateUniforms(const struct map_resolution *res, vec2_t *map_pos,
                             int *ticks_hz, int *nwork)
{
    (void)res;
    (void)map_pos;
    (void)ticks_hz;
    (void)nwork;
    assert(!"R_Cmd_MoveUpdateUniforms is a render command identity");
}

void R_Cmd_MoveUploadData(void *gpuid_buff, size_t *ndynamic_ents,
                         void *attr_buff, size_t *attr_buffsize,
                         void *flock_buff, size_t *flock_buffsize,
                         void *cost_base_buff, size_t *cost_base_size,
                         void *blockers_buff, size_t *blockers_size)
{
    (void)gpuid_buff;
    (void)ndynamic_ents;
    (void)attr_buff;
    (void)attr_buffsize;
    (void)flock_buff;
    (void)flock_buffsize;
    (void)cost_base_buff;
    (void)cost_base_size;
    (void)blockers_buff;
    (void)blockers_size;
    assert(!"R_Cmd_MoveUploadData is a render command identity");
}

void R_Cmd_MoveInvalidateData(void)
{
    assert(!"R_Cmd_MoveInvalidateData is a render command identity");
}

void R_Cmd_MoveDispatchWork(const size_t *nents)
{
    (void)nents;
    assert(!"R_Cmd_MoveDispatchWork is a render command identity");
}

void R_Cmd_MoveReadNewVelocities(void *out, const size_t *nwork,
                                const size_t *maxout)
{
    (void)out;
    (void)nwork;
    (void)maxout;
    assert(!"R_Cmd_MoveReadNewVelocities is a render command identity");
}

void R_Cmd_MovePollCompletion(SDL_atomic_t *out)
{
    (void)out;
    assert(!"R_Cmd_MovePollCompletion is a render command identity");
}

void R_Cmd_MoveClearState(void)
{
    assert(!"R_Cmd_MoveClearState is a render command identity");
}

void R_Cmd_WaterInit(void)
{
    assert(!"R_Cmd_WaterInit is a render command identity");
}

void R_Cmd_WaterShutdown(void)
{
    assert(!"R_Cmd_WaterShutdown is a render command identity");
}

void R_Cmd_DrawWater(const struct render_input *in, const bool *refraction,
                    const bool *reflection)
{
    (void)in;
    (void)refraction;
    (void)reflection;
    assert(!"R_Cmd_DrawWater is a render command identity");
}

void R_Cmd_TileDrawSelected(const struct tile_desc *in,
                           const void *chunk_rprivate, mat4x4_t *model,
                           const int *tiles_per_chunk_x,
                           const int *tiles_per_chunk_z)
{
    (void)in;
    (void)chunk_rprivate;
    (void)model;
    (void)tiles_per_chunk_x;
    (void)tiles_per_chunk_z;
    assert(!"R_Cmd_TileDrawSelected is a render command identity");
}

void R_Cmd_TileUpdate(void *chunk_rprivate, const struct map *map,
                     const struct tile_desc *desc)
{
    (void)chunk_rprivate;
    (void)map;
    (void)desc;
    assert(!"R_Cmd_TileUpdate is a render command identity");
}

void R_Cmd_TilePatchVertsBlend(void *chunk_rprivate, const struct map *map,
                              const struct tile_desc *tile)
{
    (void)chunk_rprivate;
    (void)map;
    (void)tile;
    assert(!"R_Cmd_TilePatchVertsBlend is a render command identity");
}

void R_Cmd_TilePatchVertsSmooth(void *chunk_rprivate, const struct map *map,
                               const struct tile_desc *tile)
{
    (void)chunk_rprivate;
    (void)map;
    (void)tile;
    assert(!"R_Cmd_TilePatchVertsSmooth is a render command identity");
}

void R_Cmd_MinimapBake(const struct map *map, void **chunk_rprivates,
                      mat4x4_t *chunk_model_mats)
{
    (void)map;
    (void)chunk_rprivates;
    (void)chunk_model_mats;
    assert(!"R_Cmd_MinimapBake is a render command identity");
}

void R_Cmd_MinimapUpdateChunk(const struct map *map, void *chunk_rprivate,
                             mat4x4_t *chunk_model, const int *chunk_r,
                             const int *chunk_c)
{
    (void)map;
    (void)chunk_rprivate;
    (void)chunk_model;
    (void)chunk_r;
    (void)chunk_c;
    assert(!"R_Cmd_MinimapUpdateChunk is a render command identity");
}

void R_Cmd_MinimapRender(const struct map *map, const struct camera *cam,
                        vec2_t *center_pos, const int *side_len_px,
                        vec4_t *border_clr)
{
    (void)map;
    (void)cam;
    (void)center_pos;
    (void)side_len_px;
    (void)border_clr;
    assert(!"R_Cmd_MinimapRender is a render command identity");
}

void R_Cmd_MinimapRenderUnits(const struct map *map, vec2_t *center_pos,
                             const int *side_len_px, size_t *nunits,
                             vec2_t *posbuff, vec3_t *colorbuff)
{
    (void)map;
    (void)center_pos;
    (void)side_len_px;
    (void)nunits;
    (void)posbuff;
    (void)colorbuff;
    assert(!"R_Cmd_MinimapRenderUnits is a render command identity");
}

void R_Cmd_MinimapFree(void)
{
    assert(!"R_Cmd_MinimapFree is a render command identity");
}

void R_Cmd_UI_Init(void)
{
    assert(!"R_Cmd_UI_Init is a render command identity");
}

void R_Cmd_UI_Shutdown(void)
{
    assert(!"R_Cmd_UI_Shutdown is a render command identity");
}

void R_Cmd_UI_Render(const struct nk_draw_list *dl)
{
    (void)dl;
    assert(!"R_Cmd_UI_Render is a render command identity");
}

void R_Cmd_UI_UploadFontAtlas(void *image, const int *w, const int *h)
{
    (void)image;
    (void)w;
    (void)h;
    assert(!"R_Cmd_UI_UploadFontAtlas is a render command identity");
}

void R_Cmd_SkyboxLoad(const char *dir, const char *extension)
{
    (void)dir;
    (void)extension;
    assert(!"R_Cmd_SkyboxLoad is a render command identity");
}

void R_Cmd_DrawSkybox(const struct camera *cam)
{
    (void)cam;
    assert(!"R_Cmd_DrawSkybox is a render command identity");
}

void R_Cmd_SkyboxFree(void)
{
    assert(!"R_Cmd_SkyboxFree is a render command identity");
}

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
    float curr_ratio = res.as_vec2.x / res.as_vec2.y;
    float new_ratio = new_val->as_vec2.x / new_val->as_vec2.y;
    if(fabs(new_ratio - curr_ratio) < EPSILON)
        return;

    struct sval new_res = {.type = ST_TYPE_VEC2};
    if(new_ratio > curr_ratio) {
        new_res.as_vec2 = (vec2_t){
            .x = res.as_vec2.x,
            .y = res.as_vec2.y / (new_ratio / curr_ratio)
        };
    }else{
        new_res.as_vec2 = (vec2_t){
            .x = res.as_vec2.x / (curr_ratio / new_ratio),
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

#if PF_RENDER_BACKEND_OPENGL
    extern void R_GL_SetViewport(int *x, int *y, int *w, int *h);
    extern void R_GL_SwapchainSetRes(int *x, int *y);

    int viewport[4] = {0, 0, width, height};
    R_PushCmd((struct rcmd){
        .func = R_GL_SetViewport,
        .nargs = 4,
        .args = {
            R_PushArg(&viewport[0], sizeof(viewport[0])),
            R_PushArg(&viewport[1], sizeof(viewport[1])),
            R_PushArg(&viewport[2], sizeof(viewport[2])),
            R_PushArg(&viewport[3], sizeof(viewport[3])),
        },
    });
    R_PushCmd((struct rcmd){
        .func = R_GL_SwapchainSetRes,
        .nargs = 2,
        .args = {
            R_PushArg(&viewport[2], sizeof(viewport[2])),
            R_PushArg(&viewport[3], sizeof(viewport[3]))
        }
    });
#endif
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

static bool bool_val_validate(const struct sval *new_val)
{
    return new_val->type == ST_TYPE_BOOL;
}

static void vsync_commit(const struct sval *new_val)
{
    R_PushCmd((struct rcmd){
        .func = R_Backend_CommandSetSwapInterval,
        .nargs = 1,
        .args = {R_PushArg(&new_val->as_bool, sizeof(bool))},
    });
}

static bool int_val_validate(const struct sval *new_val)
{
    return new_val->type == ST_TYPE_INT;
}

static void debug_logmask_commit(const struct sval *new_val)
{
    R_PushCmd((struct rcmd){
        .func = R_Backend_CommandSetDebugLogMask,
        .nargs = 1,
        .args = {R_PushArg(&new_val->as_int, sizeof(int))},
    });
}

static void trace_gpu_commit(const struct sval *new_val)
{
    R_PushCmd((struct rcmd){
        .func = R_Backend_CommandSetTraceGPU,
        .nargs = 1,
        .args = {R_PushArg(&new_val->as_bool, sizeof(bool))},
    });
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_Init(const char *base_path)
{
    (void)base_path;

    ss_e status;
    SDL_DisplayMode dm;
    SDL_GetDesktopDisplayMode(0, &dm);

    status = Settings_Create((struct setting){
        .name = "pf.video.aspect_ratio",
        .val = (struct sval){
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
        .val = (struct sval){
            .type = ST_TYPE_VEC2,
            .as_vec2 = res_default
        },
        .prio = 1,
        .validate = res_validate,
        .commit = res_commit,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.video.display_mode",
        .val = (struct sval){
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
        .val = (struct sval){
            .type = ST_TYPE_BOOL,
            .as_bool = false
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.video.vsync",
        .val = (struct sval){
            .type = ST_TYPE_BOOL,
            .as_bool = true
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = vsync_commit,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.video.water_reflection",
        .val = (struct sval){
            .type = ST_TYPE_BOOL,
            .as_bool = true
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.video.water_refraction",
        .val = (struct sval){
            .type = ST_TYPE_BOOL,
            .as_bool = true
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.debug.render_log_mask",
        .val = (struct sval){
            .type = ST_TYPE_INT,
            .as_int = 0x1,
        },
        .prio = 0,
        .validate = int_val_validate,
        .commit = debug_logmask_commit,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.debug.trace_gpu",
        .val = (struct sval){
            .type = ST_TYPE_BOOL,
            .as_bool = false,
        },
        .prio = 0,
        .validate = bool_val_validate,
        .commit = trace_gpu_commit,
    });
    assert(status == SS_OKAY);

    return true;
}

SDL_Thread *R_Run(struct render_sync_state *rstate)
{
    return R_Backend_Run(rstate);
}

void *R_PushArg(const void *src, size_t size)
{
    struct render_workspace *ws = (SDL_ThreadID() == g_render_thread_id) ? G_GetRenderWS()
                                                                         : G_GetSimWS();
    void *ret = stalloc(&ws->args, size);
    if(!ret)
        return ret;

    memcpy(ret, src, size);
    return ret;
}

void *R_AllocArg(size_t size)
{
    struct render_workspace *ws = (SDL_ThreadID() == g_render_thread_id) ? G_GetRenderWS()
                                                                         : G_GetSimWS();
    return stalloc(&ws->args, size);
}

void R_PushCmd(struct rcmd cmd)
{
    if(SDL_ThreadID() == g_render_thread_id) {
        R_Backend_DispatchCmd(cmd);
        return;
    }

    queue_rcmd_push(&G_GetSimWS()->commands, &cmd);
}

void R_PushCmdImmediate(struct rcmd cmd)
{
    if(SDL_ThreadID() == g_render_thread_id) {
        R_Backend_DispatchCmd(cmd);
        return;
    }

    queue_rcmd_push(&G_GetRenderWS()->commands, &cmd);
}

void R_PushCmdImmediateFront(struct rcmd cmd)
{
    if(SDL_ThreadID() == g_render_thread_id) {
        R_Backend_DispatchCmd(cmd);
        return;
    }

    queue_rcmd_push_front(&G_GetRenderWS()->commands, &cmd);
}

bool R_InitWS(struct render_workspace *ws)
{
    if(!stalloc_init(&ws->args))
        goto fail_args;

    if(!queue_rcmd_init(&ws->commands, 2048))
        goto fail_queue;

    return true;

fail_queue:
    stalloc_destroy(&ws->args);
fail_args:
    return false;
}

void R_DestroyWS(struct render_workspace *ws)
{
    queue_rcmd_destroy(&ws->commands);
    stalloc_destroy(&ws->args);
}

void R_ClearWS(struct render_workspace *ws)
{
    queue_rcmd_clear(&ws->commands);
    stalloc_clear(&ws->args);
}

const char *R_GetInfo(enum render_info attr)
{
    return R_Backend_GetInfo(attr);
}

void R_InitAttributes(void)
{
    R_Backend_InitAttributes();
}

bool R_ComputeShaderSupported(void)
{
    return R_Backend_ComputeShaderSupported();
}

Uint32 R_WindowFlags(void)
{
    return R_Backend_WindowFlags();
}

void R_WindowDrawableSize(SDL_Window *window, int *out_w, int *out_h)
{
    R_Backend_WindowDrawableSize(window, out_w, out_h);
}

void R_PresentWindow(SDL_Window *window)
{
    R_Backend_PresentWindow(window);
}

void R_Yield(void)
{
    R_Backend_Yield();
}
