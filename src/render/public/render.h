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

enum render_pass{
    RENDER_PASS_DEPTH,
    RENDER_PASS_REGULAR
};

/* Each face is made of 2 independent triangles. The top face is an exception, and is made up of 4 
 * triangles. This is to give each triangle a vertex which lies at the center of the tile in the XZ
 * dimensions.
 * This center vertex will have its' own texture coordinate (used for blending edges between tiles).
 * As well, the center vertex can have its' own normal for potentially "smooth" corner and ramp tiles. 
 */
#define VERTS_PER_FACE 6
#define VERTS_PER_TILE ((5 * VERTS_PER_FACE) + (4 * 3))


/*###########################################################################*/
/* RENDER GENERAL                                                            */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Performs one-time initialization of the rendering subsystem.
 * ---------------------------------------------------------------------------
 */
bool   R_Init(const char *base_path);

/*###########################################################################*/
/* RENDER TEXTURE                                                            */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Load the specified image file, create an OpenGL texture, and return the handle.
 * ---------------------------------------------------------------------------
 */
bool R_Texture_Load(const char *basedir, const char *name, GLuint *out);

/* ---------------------------------------------------------------------------
 * Free a previously loaded texture.
 * ---------------------------------------------------------------------------
 */
void R_Texture_Free(const char *name);

/* ---------------------------------------------------------------------------
 * Get the OpenGL handle of a previously loaded texture.
 * ---------------------------------------------------------------------------
 */
bool R_Texture_GetForName(const char *name, GLuint *out);

/*###########################################################################*/
/* RENDER OPENGL                                                             */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Performs the OpenGL draw calls in order to render the object based on the 
 * contents of its' private data.
 * ---------------------------------------------------------------------------
 */
void   R_GL_Draw(const void *render_private, mat4x4_t *model);

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
 * Set OpenGL inverse bind pose and pose matrices uniforms for animation-related 
 * shader programs.
 * ---------------------------------------------------------------------------
 */
void   R_GL_SetAnimUniforms(mat4x4_t *inv_bind_poses, mat4x4_t *curr_poses, size_t count);

/* ---------------------------------------------------------------------------
 * Set the global ambient color that will impact all models based on their 
 * materials. The color is an RGB floating-point multiplier. 
 * ---------------------------------------------------------------------------
 */
void   R_GL_SetAmbientLightColor(vec3_t color);

/* ---------------------------------------------------------------------------
 * Set the color of the global light source.
 * The color is an RGB floating-point multiplier. 
 * ---------------------------------------------------------------------------
 */
void   R_GL_SetLightEmitColor(vec3_t color);

/* ---------------------------------------------------------------------------
 * Set the light position that will impact all models. 
 * Only one light source is supported for the time being. 
 * This position must be in world space.
 * ---------------------------------------------------------------------------
 */
void   R_GL_SetLightPos(vec3_t pos);

/* ---------------------------------------------------------------------------
 * Render an entitiy's skeleton which is used for animation. 
 * The camera argument is for deriving the screenspace position of text labels
 * for the joint names. If 'cam' is NULL, the labels won't be rendered.
 *
 * NOTE: This is a low-performance routine that calls malloc/free at every call.
 * It should be used for debugging only.
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
void   R_GL_DrawNormals(const void *render_private, mat4x4_t *model, bool anim);

/* ---------------------------------------------------------------------------
 * Debugging utility to draw an infinite ray defined by an origin and a direction.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawRay(vec3_t origin, vec3_t dir, mat4x4_t *model, vec3_t color, float t);

/* ---------------------------------------------------------------------------
 * Render the oriented bounding box for collidable entities.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawOBB(const struct entity *ent);

/* ---------------------------------------------------------------------------
 * Render a 2D box on the screen. 'screen_pos' + 'signed_size' is the 'opposite'
 * corner of the box. Both are given in screen coordinates.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawBox2D(vec2_t screen_pos, vec2_t signed_size, vec3_t color, float width);

/* ---------------------------------------------------------------------------
 * Writes the framebuffer color region (0, 0, width, height) to a PPM file.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DumpFBColor_PPM(const char *filename, int width, int height);


/* ---------------------------------------------------------------------------
 * Writes the framebuffer depth region (0, 0, width, height) to a PPM file. 
 * 'linearize' should be set to true when perspective projection is used. In 
 * this case, 'near' and 'far' should be the near and far planes used for 
 * perspective projection. If 'linearize' is false, they are ignored.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DumpFBDepth_PPM(const char *filename, int width, int height, 
                            bool linearize, GLfloat near, GLfloat far);

/* ---------------------------------------------------------------------------
 * Render a selection circle over the map surface.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawSelectionCircle(vec2_t xz, float radius, float width, vec3_t color, 
                                const struct map *map);

/* ---------------------------------------------------------------------------
 * Render an array of translucent quads over the map surface. The quad corners are 
 * specified in chunk coordinates, with 4 corners per quad in the 'xz_corners'
 * buffer. 
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawMapOverlayQuads(vec2_t *xz_corners, vec3_t *colors, size_t count, mat4x4_t *model, 
                                const struct map *map);

/* ---------------------------------------------------------------------------
 * Render an array of 2d vectors over the map surface. The positions are 
 * specified in chunk coordinates.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawFlowField(vec2_t *xz_positions, vec2_t *xz_directions, size_t count,
                          mat4x4_t *model, const struct map *map);

/* ---------------------------------------------------------------------------
 * Update shader uniforms for Screenspace rendering. This function should be
 * called after all 3D rendering has completed and before any screenspace 
 * rendering. After it has been called, all rendered triangles should have 
 * 0 z-dimention. The x and y dimentions correspond to screenspace coordinates.
 * ---------------------------------------------------------------------------
 */
void   R_GL_SetScreenspaceDrawMode(void);

/*###########################################################################*/
/* RENDER TILES                                                              */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Draws a colored outline around the tile specified by the descriptor.
 * ---------------------------------------------------------------------------
 */
void   R_GL_TileDrawSelected(const struct tile_desc *in, const void *chunk_rprivate, mat4x4_t *model, 
                             int tiles_per_chunk_x, int tiles_per_chunk_z);

/* ---------------------------------------------------------------------------
 * Will output a trinagle mesh for a particular tile. The output will be an 
 * array of vertices in worldspace coordinates, with 3 consecutive vertices
 * defining a triangle. The return value is the number of vertices written,
 * it will be a multiple of 3.
 * ---------------------------------------------------------------------------
 */
int    R_GL_TileGetTriMesh(const struct tile_desc *in, const void *chunk_rprivate, 
                           mat4x4_t *model, int tiles_per_chunk_x, vec3_t out[]);

/* ---------------------------------------------------------------------------
 * Update a specific tile with new attributes and buffer the new vertex data.
 * Will also update surrounding tiles with new adjacency data.
 * ---------------------------------------------------------------------------
 */
void   R_GL_TileUpdate(void *chunk_rprivate, int r, int c, int tiles_width, int tiles_height, 
                       const struct tile *tiles);

/*###########################################################################*/
/* RENDER MINIMAP                                                            */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Will create a texture and mesh for the map and store them in a local context
 * for rendering later.
 * ---------------------------------------------------------------------------
 */
bool  R_GL_MinimapBake(void **chunk_rprivates, mat4x4_t *chunk_model_mats, 
                       size_t chunk_x, size_t chunk_z,
                       vec3_t map_center, vec2_t map_size);

/* ---------------------------------------------------------------------------
 * Update a chunk-sized region of the minimap texture with up-to-date mesh 
 * data.
 * ---------------------------------------------------------------------------
 */
bool  R_GL_MinimapUpdateChunk(const struct map *map, void *chunk_rprivate, mat4x4_t *chunk_model, 
                              vec3_t map_center, vec2_t map_size);

/* ---------------------------------------------------------------------------
 * Render the minimap centered at the specified screenscape coordinate.
 * This function will also render a box over the minimap that indicates the
 * region currently visible by the specified camera. If camera is NULL, no
 * box is drawn.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MinimapRender(const struct map *map, const struct camera *cam, vec2_t center_pos);

/* ---------------------------------------------------------------------------
 * Free the memory allocated by 'R_GL_MinimapBake'.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MinimapFree(void);

/*###########################################################################*/
/* RENDER TERRAIN                                                            */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Initialize map texture array with the specified list of textures.
 * ---------------------------------------------------------------------------
 */
bool  R_GL_MapInit(const char map_texfiles[][256], size_t num_textures);

/* ---------------------------------------------------------------------------
 * Call prior to rendering any map chunks. Activates the map rendering context.
 * Must be followed with a matching call to 'R_GL_MapEnd'.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MapBegin(void);

/* ---------------------------------------------------------------------------
 * Call after finishing rendering all map chunks.
 * ---------------------------------------------------------------------------
 */
void  R_GL_MapEnd(void);

/*###########################################################################*/
/* RENDER SHADOWS                                                            */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Set up the rendering context for the depth pass. This _must_ be called
 * before any calls to 'R_GL_RenderDepthMap'. Afterwards, there _must_ be 
 * a matching call to 'R_GL_DepthPassEnd'.
 * ---------------------------------------------------------------------------
 */
void R_GL_DepthPassBegin(void);

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

/*###########################################################################*/
/* RENDER ASSET LOADING                                                      */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Consumes lines of the stream and uses them to populate a new private context
 * for the model. The context is returned in a malloc'd buffer.
 * ---------------------------------------------------------------------------
 */
void  *R_AL_PrivFromStream(const char *base_path, const struct pfobj_hdr *header, SDL_RWops *stream);

/* ---------------------------------------------------------------------------
 * Dumps private render data in PF Object format.
 * ---------------------------------------------------------------------------
 */
void   R_AL_DumpPrivate(FILE *stream, void *priv_data);

/* ---------------------------------------------------------------------------
 * Gives size (in bytes) of buffer size required for the render private 
 * buffer for a renderable PFChunk.
 * ---------------------------------------------------------------------------
 */
size_t R_AL_PrivBuffSizeForChunk(size_t tiles_width, size_t tiles_height, size_t num_mats);

/* ---------------------------------------------------------------------------
 * Initialize private render buff for a PFChunk of the map. 
 *
 * This function will build the vertices and their vertices from the data
 * already parsed into the 'tiles'.
 * ---------------------------------------------------------------------------
 */
bool   R_AL_InitPrivFromTiles(const struct tile *tiles, size_t width, size_t height,
                              void *priv_buff, const char *basedir);

#endif
