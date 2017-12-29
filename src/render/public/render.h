#ifndef RENDER_H
#define RENDER_H

#include "../../pf_math.h"

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

struct pfobj_hdr;
struct entity;
struct skeleton;


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
 * Performs the OpenGL draw calls in order to render the entity based on the 
 * contents of its' private data.
 * ---------------------------------------------------------------------------
 */
void   R_GL_Draw(struct entity *ent);

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
 * Helpers to set OpenGL uniforms for particular shader programs.
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
void   R_GL_DrawOrigin(const struct entity *ent);

/* ---------------------------------------------------------------------------
 * Debugging utility to draw normals as yellow rays going out from the model.
 * ---------------------------------------------------------------------------
 */
void   R_GL_DrawNormals(const struct entity *ent);


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

#endif
