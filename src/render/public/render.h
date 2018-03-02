/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017 Eduard Permyakov 
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

#ifndef RENDER_H
#define RENDER_H

#include "../../pf_math.h"

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

struct pfobj_hdr;
struct entity;
struct skeleton;
struct tile;
struct tile_desc;


/*###########################################################################*/
/* RENDER GENERAL                                                            */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Performs one-time initialization of the rendering subsystem.
 * ---------------------------------------------------------------------------
 */
bool   R_Init(const char *base_path);


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
 * Helpers to set OpenGL uniforms for animation-related shader programs.
 * ---------------------------------------------------------------------------
 */
void   R_GL_SetAnimUniformMat4x4Array(mat4x4_t *data, size_t count, const char *uname);
void   R_GL_SetAnimUniformVec4Array(vec4_t *data, size_t count, const char *uname);

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
 *
 * NOTE: This is a low-performance routine that calls malloc/free at every call.
 * It should be used for debugging only.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawSkeleton(const struct entity *ent, const struct skeleton *skel);

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
void   R_GL_DrawRay(vec3_t origin, vec3_t dir, mat4x4_t *model);

/* ---------------------------------------------------------------------------
 * Draws a colored outline around the tile specified by the descriptor.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawTileSelected(const struct tile_desc *in, const void *chunk_rprivate, 
                             mat4x4_t *model, int tiles_per_chunk_x, int tiles_per_chunk_z);


/*###########################################################################*/
/* RENDER ASSET LOADING                                                      */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Computes the size (in bytes) that is required to store all the rendering 
 * subsystem data from a PF Object file.
 * ---------------------------------------------------------------------------
 */
size_t R_AL_PrivBuffSizeFromHeader(const struct pfobj_hdr *header);

/* ---------------------------------------------------------------------------
 * Consumes lines of the stream and uses them to populate the private data 
 * stored in priv_buff. 
 * ---------------------------------------------------------------------------
 */
bool   R_AL_InitPrivFromStream(const struct pfobj_hdr *header, const char *basedir, 
                               FILE *stream, void *priv_buff);

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
 * 
 * Material data is read from a separate stream.
 * ---------------------------------------------------------------------------
 */
bool   R_AL_InitPrivFromTilesAndMats(FILE *mats_stream, size_t num_mats, 
                                     const struct tile *tiles, size_t width, size_t height,
                                     void *priv_buff, const char *basedir);

#endif
