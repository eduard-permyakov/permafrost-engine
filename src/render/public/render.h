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

#ifndef RENDER_H
#define RENDER_H

#include "../../pf_math.h"
#include "../../sprite.h"

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

#include <SDL.h> /* for SDL_RWops */

struct pfobj_hdr;
struct entity;
struct skeleton;
struct tile;
struct tile_desc;
struct map;
struct camera;
struct frustum;
struct render_input;
struct nk_draw_list;
struct map_resolution;
struct obb;
struct aabb;
struct ent_anim_rstate;
struct anim_pose_data_desc;
struct splatmap;

enum render_pass{
    RENDER_PASS_DEPTH,
    RENDER_PASS_REGULAR
};

struct ui_vert{
    vec2_t  screen_pos;
    vec2_t  uv;
    uint8_t color[4];
};

struct sprite_desc{
    struct sprite_sheet_desc sheet;
    size_t                   frame;
    vec2_t                   ws_size;
    vec3_t                   ws_pos;
};

#define VERTS_PER_SIDE_FACE  (6)
#define VERTS_PER_TOP_FACE   (24)
#define VERTS_PER_TILE       (4 * VERTS_PER_SIDE_FACE + VERTS_PER_TOP_FACE)
#define TILE_DEPTH           (3)
#define MAX_MATERIALS        (16)

/*###########################################################################*/
/* RENDER COMMANDS                                                           */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Render command identity for drawing an object based on the contents of its
 * private render data. The active backend owns execution.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_Draw(const void *render_private, mat4x4_t *model, const bool *translucent);

/* ---------------------------------------------------------------------------
 * Render command identity for frame begin. The active backend dispatches this
 * to its own frame setup implementation.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_BeginFrame(void);

/* ---------------------------------------------------------------------------
 * Render command identity for frame end. The active backend dispatches this to
 * its own frame completion implementation.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_EndFrame(void);

/* ---------------------------------------------------------------------------
 * Render command identity for backend view matrix / camera-position state.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_SetViewMatAndPos(const mat4x4_t *view, const vec3_t *pos);

/* ---------------------------------------------------------------------------
 * Render command identity for backend projection matrix state.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_SetProj(const mat4x4_t *proj);

/* ---------------------------------------------------------------------------
 * Render command identity for global ambient color state.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_SetAmbientLightColor(const vec3_t *color);

/* ---------------------------------------------------------------------------
 * Render command identity for global light color state.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_SetLightEmitColor(const vec3_t *color);

/* ---------------------------------------------------------------------------
 * Render command identity for global light position state.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_SetLightPos(const vec3_t *pos);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing an entitiy's skeleton which is used for
 * animation.
 * The camera argument is for deriving the screenspace position of text labels
 * for the joint names. If 'cam' is NULL, the labels won't be rendered.
 *
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawSkeleton(mat4x4_t *model, const struct skeleton *skel,
                         const struct camera *cam);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing the model to a texture and saving the
 * output texture to the specified key.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawModelToTexture(const void *render_private, const struct obb *obb,
                               struct ent_anim_rstate *anim_state, const char *key);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing X(red), Y(green), Z(blue) axes at the
 * origin.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawOrigin(const void *render_private, mat4x4_t *model);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing normals as yellow rays going out from
 * the model.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawNormals(const void *render_private, mat4x4_t *model, const bool *anim);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing an infinite ray defined by an origin and
 * a direction.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawRay(const vec3_t *origin, const vec3_t *dir, mat4x4_t *model,
                    const vec3_t *color, const float *t);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing an oriented bounding box for collidable
 * entities.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawOBB(const struct aabb *aabb, const mat4x4_t *model);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing a 2D screen-space box. 'screen_pos' +
 * 'signed_size' is the 'opposite' corner of the box.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawBox2D(const vec2_t *screen_pos, const vec2_t *signed_size,
                      const vec3_t *color, const float *width);

/* ---------------------------------------------------------------------------
 * OpenGL-only debug helper: writes the framebuffer color region
 * (0, 0, width, height) to a PPM file.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DumpFBColor_PPM(const char *filename, const int *width, const int *height);


/* ---------------------------------------------------------------------------
 * OpenGL-only debug helper: writes the framebuffer depth region
 * (0, 0, width, height) to a PPM file.
 * 'linearize' should be set to true when perspective projection is used. In
 * this case, 'near' and 'far' should be the near and far planes used for
 * perspective projection. If 'linearize' is false, they are ignored.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DumpFBDepth_PPM(const char *filename, const int *width, const int *height,
                            const bool *linearize, const GLfloat *nearp, const GLfloat *farp);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing a selection circle over the map surface.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawSelectionCircle(const vec2_t *xz, const float *radius, const float *width,
                                const vec3_t *color, const struct map *map);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing a selection rectangle over the map surface.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawSelectionRectangle(const struct obb *box, const float *width,
                                   const vec3_t *color, const struct map *map);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing a line over the map surface.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawLine(vec2_t endpoints[], const float *width, const vec3_t *color,
                     const struct map *map);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing a quadrilateral over the map surface.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawQuad(vec2_t corners[], const float *width, const vec3_t *color,
                     const struct map *map);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing an array of translucent quads over the
 * map surface. The quad corners are specified in chunk coordinates, with 4
 * corners per quad in the 'xz_corners' buffer.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawMapOverlayQuads(vec2_t *xz_corners, vec3_t *colors, const size_t *count, mat4x4_t *model,
                                bool *on_water_surface, const struct map *map);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing an array of 2d vectors over the map
 * surface. The positions are specified in chunk coordinates.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawFlowField(vec2_t *xz_positions, vec2_t *xz_directions, const size_t *count,
                          mat4x4_t *model, const struct map *map);

/* ---------------------------------------------------------------------------
 * Render command identity for switching the active backend into screen-space
 * rendering after 3D rendering has completed.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_SetScreenspaceDrawMode(void);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing the loading screen image over the entire
 * viewport.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawLoadingScreen(const char *path);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing healthbars for the following 'num_ents'.
 * 'ent_health_pc' must be initialized to a buffer of 'num_ents' floats (health
 * percentages) and 'ent_top_pos_ws' must be initialized to a buffer of
 * 'num_ents' worldspace positions (the top center of the entity's OBB).
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawHealthbars(const size_t *num_ents, GLfloat *ent_health_pc,
                           vec3_t *ent_top_pos_ws, int *yoffsets, const struct camera *cam);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing an entity's combined hybrid reciprocal
 * velocity obstacle. (the union of all dynamic neighbours' HRVOs and static
 * neighbours' VOs). There should be exactly 'num_vos' elements in the
 * 'apexes' 'left_rays' and 'right_rays' input arrays.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_DrawCombinedHRVO(vec2_t *apexes, vec2_t *left_rays, vec2_t *right_rays,
                             const size_t *num_vos, const struct map *map);

/* ---------------------------------------------------------------------------
 * Get a nanosecond-resolution GPU timestamp for a previously created
 * cookie. The cookie is invalidated.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_TimestampForCookie(uint32_t *cookie, uint64_t *out);

/*###########################################################################*/
/* RENDER TILES                                                              */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Render command identity for drawing a colored outline around the tile
 * specified by the descriptor.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_TileDrawSelected(const struct tile_desc *in, const void *chunk_rprivate, mat4x4_t *model,
                             const int *tiles_per_chunk_x, const int *tiles_per_chunk_z);


/* ---------------------------------------------------------------------------
 * Render command identity for updating a specific tile with new attributes and
 * buffering the new vertex data. Will also update surrounding tiles with new
 * adjacency data.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_TileUpdate(void *chunk_rprivate, const struct map *map, const struct tile_desc *desc);

/*###########################################################################*/
/* RENDER MINIMAP                                                            */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Render command identity for creating a texture and mesh for the map and
 * storing them in a local context for rendering later.
 * ---------------------------------------------------------------------------
 */
void  R_Cmd_MinimapBake(const struct map *map, void **chunk_rprivates,
                       mat4x4_t *chunk_model_mats);

/* ---------------------------------------------------------------------------
 * Render command identity for updating a chunk-sized region of the minimap
 * texture with up-to-date mesh data.
 * ---------------------------------------------------------------------------
 */
void  R_Cmd_MinimapUpdateChunk(const struct map *map, void *chunk_rprivate,
                              mat4x4_t *chunk_model, const int *chunk_r, const int *chunk_c);

/* ---------------------------------------------------------------------------
 * Render command identity for rendering the minimap centered at the specified
 * virtual screenscape coordinate. The map's virtual minimap resolution will be
 * used. This function will also render a box over the minimap that indicates
 * the region currently visible by the specified camera. If camera is NULL, no
 * box is drawn.
 * ---------------------------------------------------------------------------
 */
void  R_Cmd_MinimapRender(const struct map *map, const struct camera *cam,
                         vec2_t *center_pos, const int *side_len_px, vec4_t *border_clr);

/* ---------------------------------------------------------------------------
 * Render command identity for rendering the specified unit positions in the
 * minimap region.
 * ---------------------------------------------------------------------------
 */
void  R_Cmd_MinimapRenderUnits(const struct map *map, vec2_t *center_pos,
                              const int *side_len_px, size_t *nunits,
                              vec2_t *posbuff, vec3_t *colorbuff);

/* ---------------------------------------------------------------------------
 * Render command identity for freeing memory allocated by 'R_Cmd_MinimapBake'.
 * ---------------------------------------------------------------------------
 */
void  R_Cmd_MinimapFree(void);

/* ---------------------------------------------------------------------------
 * Render command identity for patching the vertices for a particular tile to
 * have adjacency information about the neighboring tiles, to be used for smooth
 * blending.
 * Tiles which border tiles with different materials will get blending
 * 'turned on' by setting a vertex attribute.
 * ---------------------------------------------------------------------------
 */
void  R_Cmd_TilePatchVertsBlend(void *chunk_rprivate, const struct map *map, const struct tile_desc *tile);

/* ---------------------------------------------------------------------------
 * Render command identity for updating a tile's vertices to be the average of
 * all normals at that location, thereby giving the appearance of smooth edges
 * when lighting shading is applied.
 * ---------------------------------------------------------------------------
 */
void  R_Cmd_TilePatchVertsSmooth(void *chunk_rprivate, const struct map *map, const struct tile_desc *tile);

/*###########################################################################*/
/* RENDER TERRAIN                                                            */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Render command identity for initializing map texture arrays with the
 * specified list of textures.
 * ---------------------------------------------------------------------------
 */
void  R_Cmd_MapInit(const char map_texfiles[][256], const size_t *num_textures,
                   const struct map_resolution *res);

/* ---------------------------------------------------------------------------
 * Render command identity for freeing the resources reserved by R_Cmd_MapInit.
 * ---------------------------------------------------------------------------
 */
void  R_Cmd_MapShutdown(void);

/* ---------------------------------------------------------------------------
 * Render command identity for activating the map rendering context before
 * rendering any map chunks. Must be followed with a matching call to
 * 'R_Cmd_MapEnd'.
 * ---------------------------------------------------------------------------
 */
void  R_Cmd_MapBegin(const bool *shadows, const vec2_t *pos,
                    size_t *num_splats, const struct splatmap *splatmap,
                    const struct map_resolution *res, const struct map *map);

/* ---------------------------------------------------------------------------
 * Render command identity for finishing map chunk rendering.
 * ---------------------------------------------------------------------------
 */
void  R_Cmd_MapEnd(void);

/* ---------------------------------------------------------------------------
 * Render command identity for sending current-frame fog-of-war information to
 * the rendering susbsystem.
 * ---------------------------------------------------------------------------
 */
void  R_Cmd_MapUpdateFog(void *buff, const size_t *size);

/* ---------------------------------------------------------------------------
 * Render command identity called once per frame when there will be no more draw
 * commands touching the map data.
 * ---------------------------------------------------------------------------
 */
void  R_Cmd_MapInvalidate(void);

/*###########################################################################*/
/* RENDER SHADOWS                                                            */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Render command identity for setting up the rendering context for the depth
 * pass. This _must_ be called before any calls to 'R_Cmd_RenderDepthMap'.
 * Afterwards, there _must_ be a matching call to 'R_Cmd_DepthPassEnd'.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_DepthPassBegin(const vec3_t *light_pos, const vec3_t *cam_pos, const vec3_t *cam_dir);

/* ---------------------------------------------------------------------------
 * Render command identity for restoring normal rendering after all calls to
 * 'R_Cmd_RenderDepthMap' complete.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_DepthPassEnd(void);

/* ---------------------------------------------------------------------------
 * Render command identity for updating the depth map for the mesh. The depth
 * map will then be used for rendering shadows on the 'regular' render pass.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_RenderDepthMap(const void *render_private, mat4x4_t *model);

/* ---------------------------------------------------------------------------
 * OpenGL shadow diagnostics for the current depth pass.
 * ---------------------------------------------------------------------------
 */
void R_GL_ShadowStatsAddStatic(unsigned draws, size_t verts);
void R_GL_ShadowStatsAddAnim(unsigned draws, size_t verts);

/* ---------------------------------------------------------------------------
 * OpenGL shadow helper: return the frustum of the light source used for
 * rendering the shadow map. An up-to-date frustum is generated during
 * 'R_Cmd_DepthPassBegin'.
 * ---------------------------------------------------------------------------
 */
void R_GL_GetLightFrustum(struct frustum *out);

/* ---------------------------------------------------------------------------
 * Render command identity for disabling or enabling shadows.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_SetShadowsEnabled(const bool *on);

/*###########################################################################*/
/* RENDER WATER                                                              */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Render command identity for initializing the water rendering context.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_WaterInit(void);

/* ---------------------------------------------------------------------------
 * Render command identity for freeing resources claimed by 'R_Cmd_WaterInit'.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_WaterShutdown(void);

/* ---------------------------------------------------------------------------
 * Render command identity for rendering the water layer for the given map.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_DrawWater(const struct render_input *in, const bool *refraction, const bool *reflection);


/*###########################################################################*/
/* RENDER UI                                                                 */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Render command identity for initializing the UI rendering context.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_UI_Init(void);

/* ---------------------------------------------------------------------------
 * Render command identity for tearing down the UI rendering context.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_UI_Shutdown(void);

/* ---------------------------------------------------------------------------
 * Render command identity for rendering a Nuklear draw list.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_UI_Render(const struct nk_draw_list *dl);

/* ---------------------------------------------------------------------------
 * Render command identity for uploading the specified font atlas texture.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_UI_UploadFontAtlas(void *image, const int *w, const int *h);


/*###########################################################################*/
/* RENDER BATCH                                                              */
/*###########################################################################*/

enum batch_id{
    /* ID of 0 has a special meaning */
    BATCH_ID_PROJECTILE = 0x1,
};

/* ---------------------------------------------------------------------------
 * Render command identity for drawing all the camera-visible entities in the
 * render input, making use of draw command batching to reduce driver overhead.
 * This is equivalent to calling R_Cmd_Draw(...) for every camera-visible entity.
 * Meshes and textures that are not part of any batch will be added to the
 * batches dynamically.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_Batch_Draw(struct render_input *in);

/* ---------------------------------------------------------------------------
 * Render command identity for drawing with the specified batch instead of
 * per-chunk batches.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_Batch_DrawWithID(struct render_input *in, enum batch_id *id);

/* ---------------------------------------------------------------------------
 * Render command identity for updating the depth map for every light-visible
 * entity in the render input. This is the equivalent of calling
 * R_Cmd_RenderDepthMap(...) for every light-visible entity.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_Batch_RenderDepthMap(struct render_input *in);

/* ---------------------------------------------------------------------------
 * Render command identity for freeing all resources used by live batches and
 * all per-chunk batches, resetting the state of the module to that at
 * initialization time.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_Batch_Reset(void);

/* ---------------------------------------------------------------------------
 * Render command identity for allocating a new batch for every chunk of the
 * map. Allocating batch resources has significant overhead so it's advantageous
 * to do this upfront.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_Batch_AllocChunks(struct map_resolution *res);


/*###########################################################################*/
/* RENDER POSITION                                                           */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Render command identity for uploading entity position data for optional
 * GPU-side movement computations.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_PositionsUploadData(vec3_t *posbuff, uint32_t *idbuff,
                              const size_t *nents, const struct map *map);

/* ---------------------------------------------------------------------------
 * OpenGL-only helper: get the ID of the position ID-map texture.
 * ---------------------------------------------------------------------------
 */
void R_GL_PositionsGetTexture(GLuint *out_tex_id);

/* ---------------------------------------------------------------------------
 * Render command identity for freeing resources previously allocated by
 * R_Cmd_PositionsUploadData.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_PositionsInvalidateData(void);


/*###########################################################################*/
/* RENDER MOVEMENT                                                           */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Render command identity for updating optional GPU movement parameters.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_MoveUpdateUniforms(const struct map_resolution *res, vec2_t *map_pos,
                             int *ticks_hz, int *nwork);

/* ---------------------------------------------------------------------------
 * Render command identity for uploading optional GPU movement input state.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_MoveUploadData(void *gpuid_buff, size_t *ndynamic_ents,
                         void *attr_buff, size_t *attr_buffsize,
                         void *flock_buff, size_t *flock_buffsize,
                         void *cost_base_buff, size_t *cost_base_size,
                         void *blockers_buff, size_t *blockers_size);

/* ---------------------------------------------------------------------------
 * Render command identity for freeing optional GPU movement resources.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_MoveInvalidateData(void);

/* ---------------------------------------------------------------------------
 * Render command identity for dispatching optional GPU movement work.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_MoveDispatchWork(const size_t *nents);

/* ---------------------------------------------------------------------------
 * Render command identity for reading optional GPU movement results.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_MoveReadNewVelocities(void *out, const size_t *nwork, const size_t *maxout);

/* ---------------------------------------------------------------------------
 * Render command identity for polling optional GPU movement completion.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_MovePollCompletion(SDL_atomic_t *out);

/* ---------------------------------------------------------------------------
 * Render command identity for cleaning optional GPU movement state.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_MoveClearState(void);

/*###########################################################################*/
/* RENDER SKYBOX                                                             */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Render command identity for loading the skybox face images.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_SkyboxLoad(const char *dir, const char *extension);

/* ---------------------------------------------------------------------------
 * Render command identity for rendering the skybox.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_DrawSkybox(const struct camera *cam);

/* ---------------------------------------------------------------------------
 * Render command identity for releasing GPU resources used by the skybox.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_SkyboxFree(void);

/*###########################################################################*/
/* RENDER ANIM                                                               */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Render command identity for appending pose data to the active backend's
 * animation-pose store.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_AnimAppendData(GLfloat *data, size_t *size);

/* ---------------------------------------------------------------------------
 * Render command identity for setting the active animated entity state before
 * an animated mesh draw.
 * ---------------------------------------------------------------------------
 */
void   R_Cmd_AnimSetUniforms(mat4x4_t *normal_mat, struct anim_pose_data_desc *desc, const uint32_t *uid);

/*###########################################################################*/
/* RENDER SWAPCHAIN                                                          */
/*###########################################################################*/

/* OpenGL-only command used by the OpenGL loading-screen path. */
void R_Cmd_SwapchainPresentLast(void);

/*###########################################################################*/
/* RENDER SPRITE                                                             */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Render command identity for drawing the active world-space sprite batch.
 * ---------------------------------------------------------------------------
 */
void R_Cmd_SpriteRenderBatch(struct sprite_desc *sprites, size_t *nsprites,
                            const struct camera *cam);

#endif
