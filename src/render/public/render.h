#ifndef RENDER_H
#define RENDER_H

#include "../../pf_math.h"

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

struct pfobj_hdr;
struct entity;
struct skeleton;

/* Performs one-time initialization of the rendering subsystem.
 */
bool   R_Init(void);

/* Immediately performs the OpenGL draw calls in order to render the entity
 * based on the contents of its' private data.
 */
void   R_GL_Draw(struct entity *ent);

void   R_GL_SetView(const mat4x4_t *view, const char *shader_name);
void   R_GL_SetProj(const mat4x4_t *proj, const char *shader_name);
void   R_GL_SetUniformMat4x4Array(mat4x4_t *data, size_t count, const char *uname, const char *shader_name);

void   R_GL_SetAmbientLightColor(vec3_t color, const char *shader_name);

/* Render an entitiy's skeleton which is used for animation. 
 *
 * NOTE: This is a low-performance routine that calls malloc/free at every call.
 * It should be used for debugging only.
 */
void   R_GL_DrawSkeleton(const struct entity *ent, const struct skeleton *skel);

/* Debugging utility to draw X,Y,Z axes at the origin
 */
void   R_GL_DrawOrigin(const struct entity *ent);

/* Computes the size (in bytes) that is required to store all the rendering subsystem
 * data from a PF Object file.
 */
size_t R_AL_PrivBuffSizeFromHeader(const struct pfobj_hdr *header);

/* Consumes lines of the stream and uses them to populate the private data stored 
 * in priv_buff. 
 */
bool   R_AL_InitPrivFromStream(const struct pfobj_hdr *header, const char *basedir, 
                               FILE *stream, void *priv_buff);

/* Dumps private render data in PF Object format.
 */
void   R_AL_DumpPrivate(FILE *stream, void *priv_data);

#endif
