/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2020 Eduard Permyakov 
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

enum render_pass{
    RENDER_PASS_DEPTH,
    RENDER_PASS_REGULAR
};

struct ui_vert{
    vec2_t  screen_pos;
    vec2_t  uv;
    uint8_t color[4];
};

#define VERTS_PER_SIDE_FACE (6)
#define VERTS_PER_TOP_FACE  (24)
#define VERTS_PER_TILE      (4 * VERTS_PER_SIDE_FACE + VERTS_PER_TOP_FACE)
#define TILE_DEPTH          (3)
#define MAX_MATERIALS       (16)


/*###########################################################################*/
/* RENDER OPENGL                                                             */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Performs the OpenGL draw calls in order to render the object based on the 
 * contents of its' private data.
 * ---------------------------------------------------------------------------
 */
void   R_GL_Draw(const void *render_private, mat4x4_t *model, const bool *translucent);

/* ---------------------------------------------------------------------------
 * Clear the draw buffer and set up the global OpenGL state at the beginning 
 * of the frame.
 * ---------------------------------------------------------------------------
 */
void   R_GL_BeginFrame(void);

/* ---------------------------------------------------------------------------
 * Sets the view matrix for all relevant shader programs. 
 * ---------------------------------------------------------------------------
 */
void   R_GL_SetViewMatAndPos(const mat4x4_t *view, const vec3_t *pos);

/* ---------------------------------------------------------------------------
 * Sets the projection matrix for all relevant shader programs.
 * ---------------------------------------------------------------------------
 */
void   R_GL_SetProj(const mat4x4_t *proj);

/* ---------------------------------------------------------------------------
 * Set OpenGL uniforms for animation-related shader programs.
 * ---------------------------------------------------------------------------
 */
void   R_GL_SetAnimUniforms(mat4x4_t *inv_bind_poses, mat4x4_t *curr_poses, 
                            mat4x4_t *normal_mat, const size_t *count);

/* ---------------------------------------------------------------------------
 * Set the global ambient color that will impact all models based on their 
 * materials. The color is an RGB floating-point multiplier. 
 * ---------------------------------------------------------------------------
 */
void   R_GL_SetAmbientLightColor(const vec3_t *color);

/* ---------------------------------------------------------------------------
 * Set the color of the global light source.
 * The color is an RGB floating-point multiplier. 
 * ---------------------------------------------------------------------------
 */
void   R_GL_SetLightEmitColor(const vec3_t *color);

/* ---------------------------------------------------------------------------
 * Set the light position that will impact all models. 
 * Only one light source is supported for the time being. 
 * This position must be in world space.
 * ---------------------------------------------------------------------------
 */
void   R_GL_SetLightPos(const vec3_t *pos);

/* ---------------------------------------------------------------------------
 * Render an entitiy's skeleton which is used for animation. 
 * The camera argument is for deriving the screenspace position of text labels
 * for the joint names. If 'cam' is NULL, the labels won't be rendered.
 *
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawSkeleton(const struct entity *ent, const struct skeleton *skel, 
                         const struct camera *cam);

/* ---------------------------------------------------------------------------
 * Debugging utility to draw X(red), Y(green), Z(blue) axes at the origin
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawOrigin(const void *render_private, mat4x4_t *model);

/* ---------------------------------------------------------------------------
 * Debugging utility to draw normals as yellow rays going out from the model.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawNormals(const void *render_private, mat4x4_t *model, const bool *anim);

/* ---------------------------------------------------------------------------
 * Debugging utility to draw an infinite ray defined by an origin and a direction.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawRay(const vec3_t *origin, const vec3_t *dir, mat4x4_t *model, 
                    const vec3_t *color, const float *t);

/* ---------------------------------------------------------------------------
 * Render the oriented bounding box for collidable entities.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawOBB(const struct aabb *aabb, const mat4x4_t *model);

/* ---------------------------------------------------------------------------
 * Render a 2D box on the screen. 'screen_pos' + 'signed_size' is the 'opposite'
 * corner of the box. Both are given in screen coordinates.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawBox2D(const vec2_t *screen_pos, const vec2_t *signed_size, 
                      const vec3_t *color, const float *width);

/* ---------------------------------------------------------------------------
 * Writes the framebuffer color region (0, 0, width, height) to a PPM file.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DumpFBColor_PPM(const char *filename, const int *width, const int *height);


/* ---------------------------------------------------------------------------
 * Writes the framebuffer depth region (0, 0, width, height) to a PPM file. 
 * 'linearize' should be set to true when perspective projection is used. In 
 * this case, 'near' and 'far' should be the near and far planes used for 
 * perspective projection. If 'linearize' is false, they are ignored.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DumpFBDepth_PPM(const char *filename, const int *width, const int *height, 
                            const bool *linearize, const GLfloat *nearp, const GLfloat *farp);

/* ---------------------------------------------------------------------------
 * Render a selection circle over the map surface.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawSelectionCircle(const vec2_t *xz, const float *radius, const float *width, 
                                const vec3_t *color, const struct map *map);

/* ---------------------------------------------------------------------------
 * Render a selection rectangle over the map surface.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawSelectionRectangle(const struct obb *box, const float *width, 
                                   const vec3_t *color, const struct map *map);

/* ---------------------------------------------------------------------------
 * Render a line over the map surface.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawLine(vec2_t endpoints[], const float *width, const vec3_t *color, 
                     const struct map *map);

/* ---------------------------------------------------------------------------
 * Render a quadrilateral over the map surface.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawQuad(vec2_t corners[], const float *width, const vec3_t *color, 
                     const struct map *map);

/* ---------------------------------------------------------------------------
 * Render an array of translucent quads over the map surface. The quad corners are 
 * specified in chunk coordinates, with 4 corners per quad in the 'xz_corners'
 * buffer. 
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawMapOverlayQuads(vec2_t *xz_corners, vec3_t *colors, const size_t *count, mat4x4_t *model, 
                                const struct map *map);

/* ---------------------------------------------------------------------------
 * Render an array of 2d vectors over the map surface. The positions are 
 * specified in chunk coordinates.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawFlowField(vec2_t *xz_positions, vec2_t *xz_directions, const size_t *count,
                          mat4x4_t *model, const struct map *map);

/* ---------------------------------------------------------------------------
 * Update shader uniforms for Screenspace rendering. This function should be
 * called after all 3D rendering has completed and before any screenspace 
 * rendering. After it has been called, all rendered triangles should have 
 * 0 z-dimention. The x and y dimentions correspond to screenspace coordinates.
 * ---------------------------------------------------------------------------
 */
void   R_GL_SetScreenspaceDrawMode(void);

/* ---------------------------------------------------------------------------
 * Render the loading screen image over the entire viewport. This will configure
 * the OpenGL state for screenspace rendering.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawLoadingScreen(void);

/* ---------------------------------------------------------------------------
 * Draws the healthbars for the following 'num_ents'. 'ent_health_pc' must be
 * initialized to a buffer of 'num_ents' floats (health percentages) and 
 * 'ent_top_pos_ws' must be initialized to a buffer of 'num_ents' worldspace
 * positions (the top center of the entity's OBB).
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawHealthbars(const size_t *num_ents, GLfloat *ent_health_pc, 
                           vec3_t *ent_top_pos_ws, const struct camera *cam);

/* ---------------------------------------------------------------------------
 * Render an entity's combined hybrid reciprocal velocity obstacle. (the union
 * of all dynamic neighbours' HRVOs and static neighbours' VOs). There should
 * be exactly 'num_vos' elements in the 'apexes' 'left_rays' and 'right_rays'
 * input arrays.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawCombinedHRVO(vec2_t *apexes, vec2_t *left_rays, vec2_t *right_rays, 
                             const size_t *num_vos, const struct map *map);

/* ---------------------------------------------------------------------------
 * Get a nanosecond-resolution GPU timestamp for a previously created
 * cookie. The cookie is invalidated.
 * ---------------------------------------------------------------------------
 */
void   R_GL_TimestampForCookie(uint32_t *cookie, uint64_t *out);

/*###########################################################################*/
/* RENDER TILES                                                              */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Draws a colored outline around the tile specified by the descriptor.
 * ---------------------------------------------------------------------------
 */
void   R_GL_TileDrawSelected(const struct tile_desc *in, const void *chunk_rprivate, mat4x4_t *model, 
                             const int *tiles_per_chunk_x, const int *tiles_per_chunk_z);


/* ---------------------------------------------------------------------------
 * Update a specific tile with new attributes and buffer the new vertex data.
 * Will also update surrounding tiles with new adjacency data.
 * ---------------------------------------------------------------------------
 */
void   R_GL_TileUpdate(void *chunk_rprivate, const struct map *map, const struct tile_desc *desc);

/*###########################################################################*/
/* RENDER MINIMAP                                                            */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Will create a texture and mesh for the map and store them in a local context
 * for rendering later.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MinimapBake(const struct map *map, void **chunk_rprivates, 
                       mat4x4_t *chunk_model_mats);

/* ---------------------------------------------------------------------------
 * Update a chunk-sized region of the minimap texture with up-to-date mesh 
 * data.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MinimapUpdateChunk(const struct map *map, void *chunk_rprivate, 
                              mat4x4_t *chunk_model, const int *chunk_r, const int *chunk_c);

/* ---------------------------------------------------------------------------
 * Render the minimap centered at the specified (virtual) screenscape coordinate.
 * The map's virtual minimap resolution will be used. This function will also 
 * render a box over the minimap that indicates the region currently visible by 
 * the specified camera. If camera is NULL, no box is drawn.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MinimapRender(const struct map *map, const struct camera *cam, 
                         vec2_t *center_pos, const int *side_len_px, vec4_t *border_clr);

/* ---------------------------------------------------------------------------
 * Render the specified unit positions in the minimap region.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MinimapRenderUnits(const struct map *map, vec2_t *center_pos, 
                              const int *side_len_px, size_t *nunits, 
                              vec2_t *posbuff, vec3_t *colorbuff);

/* ---------------------------------------------------------------------------
 * Free the memory allocated by 'R_GL_MinimapBake'.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MinimapFree(void);

/* ---------------------------------------------------------------------------
 * Patch the vertices for a particular tile to have adjacency information
 * about the neighboring tiles, to be used for smooth blending.
 * Tiles which border tiles with different materials will get blending
 * 'turned on' by setting a vertex attribute.
 * ---------------------------------------------------------------------------
 */
void  R_GL_TilePatchVertsBlend(void *chunk_rprivate, const struct map *map, const struct tile_desc *tile);

/* ---------------------------------------------------------------------------
 * Updated a tile's verticies to be the average of all normals at that location,
 * thereby giving the appearance of smooth edges when lighting shading is applied.
 * ---------------------------------------------------------------------------
 */
void  R_GL_TilePatchVertsSmooth(void *chunk_rprivate, const struct map *map, const struct tile_desc *tile);

/*###########################################################################*/
/* RENDER TERRAIN                                                            */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Initialize map texture array with the specified list of textures.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MapInit(const char map_texfiles[][256], const size_t *num_textures, 
                   const struct map_resolution *res);

/* ---------------------------------------------------------------------------
 * Free the resources reserved by R_GL_MapInit.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MapShutdown(void);

/* ---------------------------------------------------------------------------
 * Call prior to rendering any map chunks. Activates the map rendering context.
 * Must be followed with a matching call to 'R_GL_MapEnd'.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MapBegin(const bool *shadows, const vec2_t *pos);

/* ---------------------------------------------------------------------------
 * Call after finishing rendering all map chunks.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MapEnd(void);

/* ---------------------------------------------------------------------------
 * Send the current-frame fog-of-war information to the rendering susbsystem.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MapUpdateFog(void *buff, const size_t *size);

/* ---------------------------------------------------------------------------
 * Must be Called once per frame when we are sure there will be no more draw 
 * commands touching the map data.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MapInvalidate(void);

/*###########################################################################*/
/* RENDER SHADOWS                                                            */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Set up the rendering context for the depth pass. This _must_ be called
 * before any calls to 'R_GL_RenderDepthMap'. Afterwards, there _must_ be 
 * a matching call to 'R_GL_DepthPassEnd'.
 * ---------------------------------------------------------------------------
 */
void R_GL_DepthPassBegin(const vec3_t *light_pos, const vec3_t *cam_pos, const vec3_t *cam_dir);

/* ---------------------------------------------------------------------------
 * Set up the rendering context for normal rendering. This _must_ be called
 * after all calls to 'R_GL_RenderDepthMap' complete.
 * ---------------------------------------------------------------------------
 */
void R_GL_DepthPassEnd(void);

/* ---------------------------------------------------------------------------
 * Update the depth map for the mesh. The depth map will then be used for 
 * rendering shadows on the 'regular' render pass.
 * ---------------------------------------------------------------------------
 */
void R_GL_RenderDepthMap(const void *render_private, mat4x4_t *model);

/* ---------------------------------------------------------------------------
 * Return the frustum of the light source used for rendering the shadow map.
 * An up-to-date frustum is generated during 'R_GL_DepthPassBegin'
 * ---------------------------------------------------------------------------
 */
void R_GL_GetLightFrustum(struct frustum *out);

/* ---------------------------------------------------------------------------
 * Disable or enable shadows for a particular renderable object.
 * ---------------------------------------------------------------------------
 */
void R_GL_SetShadowsEnabled(void *render_private, const bool *on);

/*###########################################################################*/
/* RENDER WATER                                                              */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Initialize the water rendering context.
 * ---------------------------------------------------------------------------
 */
void R_GL_WaterInit(void);

/* ---------------------------------------------------------------------------
 * Free all resources claimed by 'R_GL_WaterInit'
 * ---------------------------------------------------------------------------
 */
void R_GL_WaterShutdown(void);

/* ---------------------------------------------------------------------------
 * Renders the water layer for the given map.
 * ---------------------------------------------------------------------------
 */
void R_GL_DrawWater(const struct render_input *in, const bool *refraction, const bool *reflection);


/*###########################################################################*/
/* RENDER UI                                                                 */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Initialize UI rendering context.
 * ---------------------------------------------------------------------------
 */
void R_GL_UI_Init(void);

/* ---------------------------------------------------------------------------
 * Tear down UI rendering context. (free resources claimed by R_GL_UI_Init)
 * ---------------------------------------------------------------------------
 */
void R_GL_UI_Shutdown(void);

/* ---------------------------------------------------------------------------
 * Render the UI from the draw commands generated by the nukear calls during
 * a single simulation tick.
 * ---------------------------------------------------------------------------
 */
void R_GL_UI_Render(const struct nk_draw_list *dl);

/* ---------------------------------------------------------------------------
 * Upload the specified font atlas texture
 * ---------------------------------------------------------------------------
 */
void R_GL_UI_UploadFontAtlas(void *image, const int *w, const int *h);


/*###########################################################################*/
/* RENDER BATCH                                                              */
/*###########################################################################*/

enum batch_id{
    /* ID of 0 has a special meaning */
    BATCH_ID_PROJECTILE = 0x1,
};

/* ---------------------------------------------------------------------------
 * Draw all the camera-visible entities in the render input, making use of 
 * draw command batching to reduce driver overhead. This is equivalent to calling
 * R_GL_Draw(...) for every camera-visible entitiy. Meshes and textures that are 
 * not part of any batch will be added to the batches dynamically.
 * ---------------------------------------------------------------------------
 */
void R_GL_Batch_Draw(struct render_input *in);

/* ---------------------------------------------------------------------------
 * Like 'R_GL_Batch_Draw' but using the specified batch instead of per-chunk batches.
 * ---------------------------------------------------------------------------
 */
void R_GL_Batch_DrawWithID(struct render_input *in, enum batch_id *id);

/* ---------------------------------------------------------------------------
 * Update the depth map for every light-visible entity in the render input.
 * This is the equivalent of calling R_GL_RenderDepthMap(...) for every
 * light-visible entity.
 * ---------------------------------------------------------------------------
 */
void R_GL_Batch_RenderDepthMap(struct render_input *in);

/* ---------------------------------------------------------------------------
 * Free all the resources used by live batches. Free all the per-chunk batches,
 * resetting the state of the module to that at initialization time.
 * ---------------------------------------------------------------------------
 */
void R_GL_Batch_Reset(void);

/* ---------------------------------------------------------------------------
 * Allocate a new batch for every chunk of the map. Allocating batch
 * resources has significant overhead so it's advantageous to do this upfront.
 * ---------------------------------------------------------------------------
 */
void R_GL_Batch_AllocChunks(struct map_resolution *res);


/*###########################################################################*/
/* RENDER POSITION                                                           */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Render the entity attributes to a texture based on the entity's map-space 
 * position. The resulting texture can be queried with R_GL_PositionsGet and 
 * used for further computations on the GPU.
 * ---------------------------------------------------------------------------
 */
void R_GL_PositionsUpload(vec3_t *posbuff, uint32_t *idbuff, 
                          const size_t *nents, const struct map *map);

/* ---------------------------------------------------------------------------
 * Get the ID of the texture rendered to by R_GL_PositionsRender.
 * ---------------------------------------------------------------------------
 */
void R_GL_PositionsGet(GLuint *out_tex_id);

/* ---------------------------------------------------------------------------
 * Free resources previously allocated by R_GL_PositionsUpload.
 * ---------------------------------------------------------------------------
 */
void R_GL_PositionsInvalidate(void);


/*###########################################################################*/
/* RENDER MOVEMENT                                                           */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Upload the movement input state to a shader storage buffer object.
 * ---------------------------------------------------------------------------
 */
void R_GL_MoveUpload(void *buff, size_t *buffsize);

/* ---------------------------------------------------------------------------
 * Free resources previously allocated by R_GL_MoveUpload.
 * ---------------------------------------------------------------------------
 */
void R_GL_MoveInvalidate(void);


#endif

