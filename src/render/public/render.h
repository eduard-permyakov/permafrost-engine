#ifndef RENDER_H
#define RENDER_H

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

struct pfobj_hdr;

/* Performs one-time initialization of the rendering subsystem.
 */
bool   R_Init(void);

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
