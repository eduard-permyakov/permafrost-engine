#ifndef RENDER_H
#define RENDER_H

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

struct pfobj_hdr;
struct entity;

/* Performs one-time initialization of the rendering subsystem.
 */
bool   R_Init(void);

/* Immediately performs the OpenGL draw calls in order to render the entity
 * based on the contents of its' private data.
 */
void   R_GL_Draw(struct entity *ent);

/* Computes the size (in bytes) that is required to store all the rendering subsystem
 * data from a PF Object file.
 */
size_t R_AL_PrivBuffSizeFromHeader(const struct pfobj_hdr *header);

/* Consumes lines of the stream and uses them to populate the private data stored 
 * in priv_buff. 
 */
bool   R_AL_InitPrivFromStream(const struct pfobj_hdr *header, FILE *stream, void *priv_buff);

/* Dumps private render data in PF Object format.
 */
void   R_AL_DumpPrivate(FILE *stream, void *priv_data);

#endif
