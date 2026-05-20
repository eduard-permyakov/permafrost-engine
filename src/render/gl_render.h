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

#ifndef GL_RENDER_H
#define GL_RENDER_H

#include "public/render.h"
#include "../map/public/tile.h"
#include "../pf_math.h"

#include "gl_loader.h"

#include <stddef.h>
#include <stdbool.h>


#define POSE_BUFF_TUNINT (GL_TEXTURE11)
#if defined(__APPLE__) && defined(__aarch64__)
#define SPLAT_MAP_TUNIT  (GL_TEXTURE7)
#define SKYBOX_TUNIT     (GL_TEXTURE13)
#define HEIGHT_MAP_TUNIT (GL_TEXTURE6)
#define SHADOW_MAP_TUNIT (GL_TEXTURE4)
#else
#define SPLAT_MAP_TUNIT  (GL_TEXTURE12)
#define SKYBOX_TUNIT     (GL_TEXTURE13)
#define HEIGHT_MAP_TUNIT (GL_TEXTURE14)
#define SHADOW_MAP_TUNIT (GL_TEXTURE15)
#endif

struct render_private;
struct vertex;
struct tile;
struct tile_desc;
struct map;

/* General */

void   R_Cmd_Init(struct render_private *priv, const char *shader, const struct vertex *vbuff);
void   R_GL_Init_Impl(struct render_private *priv, const char *shader, const struct vertex *vbuff);
void   R_GL_GlobalConfig(void);
void   R_GL_SetViewport(int *x, int *y, int *w, int *h);
void   R_GL_Draw_Impl(const void *render_private, mat4x4_t *model,
                      const bool *translucent);
void   R_GL_BeginFrame_Impl(void);
void   R_GL_EndFrame_Impl(void);
void   R_GL_SetViewMatAndPos_Impl(const mat4x4_t *view, const vec3_t *pos);
void   R_GL_SetProj_Impl(const mat4x4_t *proj);
void   R_GL_SetAmbientLightColor_Impl(const vec3_t *color);
void   R_GL_SetLightEmitColor_Impl(const vec3_t *color);
void   R_GL_SetLightPos_Impl(const vec3_t *pos);
void   R_GL_SetScreenspaceDrawMode_Impl(void);
void   R_GL_DrawBox2D_Impl(const vec2_t *screen_pos, const vec2_t *signed_size,
                           const vec3_t *color, const float *width);
void   R_GL_DrawLine_Impl(vec2_t endpoints[], const float *width,
                          const vec3_t *color, const struct map *map);
void   R_GL_DrawQuad_Impl(vec2_t corners[], const float *width,
                          const vec3_t *color, const struct map *map);
void   R_GL_DrawOrigin_Impl(const void *render_private, mat4x4_t *model);
void   R_GL_DrawRay_Impl(const vec3_t *origin, const vec3_t *dir,
                         mat4x4_t *model, const vec3_t *color, const float *t);
void   R_GL_DrawOBB_Impl(const struct aabb *aabb, const mat4x4_t *model);
void   R_GL_DrawSelectionCircle_Impl(const vec2_t *xz, const float *radius,
                                     const float *width, const vec3_t *color,
                                     const struct map *map);
void   R_GL_DrawSelectionRectangle_Impl(const struct obb *box,
                                        const float *width, const vec3_t *color,
                                        const struct map *map);
void   R_GL_DrawMapOverlayQuads_Impl(vec2_t *xz_corners, vec3_t *colors,
                                     const size_t *count, mat4x4_t *model,
                                     bool *on_water_surface,
                                     const struct map *map);
void   R_GL_DrawFlowField_Impl(vec2_t *xz_positions, vec2_t *xz_directions,
                               const size_t *count, mat4x4_t *model,
                               const struct map *map);
void   R_GL_DrawCombinedHRVO_Impl(vec2_t *apexes, vec2_t *left_rays,
                                  vec2_t *right_rays, const size_t *num_vos,
                                  const struct map *map);
void   R_GL_DrawLoadingScreen_Impl(const char *path);
void   R_GL_DrawHealthbars_Impl(const size_t *num_ents, GLfloat *ent_health_pc,
                                vec3_t *ent_top_pos_ws, int *yoffsets,
                                const struct camera *cam);
void   R_GL_DrawSkeleton_Impl(mat4x4_t *model, const struct skeleton *skel,
                              const struct camera *cam);
void   R_GL_DrawModelToTexture_Impl(const void *render_private,
                                    const struct obb *obb,
                                    struct ent_anim_rstate *anim_state,
                                    const char *key);
void   R_GL_DrawNormals_Impl(const void *render_private, mat4x4_t *model,
                             const bool *anim);
void   R_GL_DepthPassBegin_Impl(const vec3_t *light_pos,
                                const vec3_t *cam_pos,
                                const vec3_t *cam_dir);
void   R_GL_DepthPassEnd_Impl(void);
void   R_GL_RenderDepthMap_Impl(const void *render_private, mat4x4_t *model);
void   R_GL_SetShadowsEnabled_Impl(const bool *on);
void   R_GL_Batch_Draw_Impl(struct render_input *in);
void   R_GL_Batch_DrawWithID_Impl(struct render_input *in, enum batch_id *id);
void   R_GL_Batch_RenderDepthMap_Impl(struct render_input *in);
void   R_GL_Batch_Reset_Impl(void);
void   R_GL_Batch_AllocChunks_Impl(struct map_resolution *res);
void   R_GL_AnimAppendData_Impl(GLfloat *data, size_t *size);
void   R_GL_AnimSetUniforms_Impl(mat4x4_t *normal_mat,
                                 struct anim_pose_data_desc *desc,
                                 const uint32_t *uid);
void   R_GL_SpriteRenderBatch_Impl(struct sprite_desc *sprites,
                                   size_t *nsprites,
                                   const struct camera *cam);
void   R_GL_TileDrawSelected_Impl(const struct tile_desc *in,
                                  const void *chunk_rprivate,
                                  mat4x4_t *model,
                                  const int *tiles_per_chunk_x,
                                  const int *tiles_per_chunk_z);
void   R_GL_MapInit_Impl(const char map_texfiles[][256],
                         const size_t *num_textures,
                         const struct map_resolution *res);
void   R_GL_MapShutdown_Impl(void);
void   R_GL_MapBegin_Impl(const bool *shadows, const vec2_t *pos,
                          size_t *num_splats,
                          const struct splatmap *splatmap,
                          const struct map_resolution *res,
                          const struct map *map);
void   R_GL_MapEnd_Impl(void);
void   R_GL_MapUpdateFog_Impl(void *buff, const size_t *size);
void   R_GL_MapInvalidate_Impl(void);

/* Position/movement */

void   R_GL_PositionsUploadData_Impl(vec3_t *posbuff, uint32_t *idbuff,
                                     const size_t *nents,
                                     const struct map *map);
void   R_GL_PositionsInvalidateData_Impl(void);
void   R_GL_MoveUpdateUniforms_Impl(const struct map_resolution *res,
                                    vec2_t *map_pos, int *ticks_hz,
                                    int *nwork);
void   R_GL_MoveUploadData_Impl(void *gpuid_buff, size_t *ndynamic_ents,
                                void *attr_buff, size_t *attr_buffsize,
                                void *flock_buff, size_t *flock_buffsize,
                                void *cost_base_buff, size_t *cost_base_size,
                                void *blockers_buff, size_t *blockers_size);
void   R_GL_MoveInvalidateData_Impl(void);
void   R_GL_MoveDispatchWork_Impl(const size_t *nents);
void   R_GL_MoveReadNewVelocities_Impl(void *out, const size_t *nwork,
                                       const size_t *maxout);
void   R_GL_MovePollCompletion_Impl(SDL_atomic_t *out);
void   R_GL_MoveClearState_Impl(void);

/* Shadows */

void   R_GL_InitShadows(void);
vec3_t R_GL_GetLightPos(void);
void   R_GL_SetLightSpaceTrans(const mat4x4_t *trans);
void   R_GL_ShadowMapBind(void);

/* Water */

void   R_GL_WaterInit_Impl(void);
void   R_GL_WaterShutdown_Impl(void);
void   R_GL_DrawWater_Impl(const struct render_input *in,
                            const bool *refraction, const bool *reflection);
void   R_GL_SetClipPlane(vec4_t plane_eq);

/* Minimap */

void   R_GL_MinimapBake_Impl(const struct map *map, void **chunk_rprivates,
                             mat4x4_t *chunk_model_mats);
void   R_GL_MinimapUpdateChunk_Impl(const struct map *map,
                                    void *chunk_rprivate,
                                    mat4x4_t *chunk_model,
                                    const int *chunk_r, const int *chunk_c);
void   R_GL_MinimapRender_Impl(const struct map *map,
                               const struct camera *cam, vec2_t *center_pos,
                               const int *side_len_px, vec4_t *border_clr);
void   R_GL_MinimapRenderUnits_Impl(const struct map *map,
                                    vec2_t *center_pos,
                                    const int *side_len_px, size_t *nunits,
                                    vec2_t *posbuff, vec3_t *colorbuff);
void   R_GL_MinimapFree_Impl(void);

/* UI */

void   R_GL_UI_Init_Impl(void);
void   R_GL_UI_Shutdown_Impl(void);
void   R_GL_UI_Render_Impl(const struct nk_draw_list *dl);
void   R_GL_UI_UploadFontAtlas_Impl(void *image, const int *w, const int *h);

/* Terrain */

void   R_GL_MapFogBindLast(GLuint tunit, GLuint shader_prog, const char *uname);
void   R_GL_MapUpdateFogClear(void);

/* Skybox */

void   R_GL_SkyboxLoad_Impl(const char *dir, const char *extension);
void   R_GL_SkyboxBind_Impl(void);
void   R_GL_DrawSkybox_Impl(const struct camera *cam);
void   R_GL_DrawSkyboxScaled_Impl(const struct camera *cam, float *map_width, float *map_height);
void   R_GL_SkyboxFree_Impl(void);


#endif
