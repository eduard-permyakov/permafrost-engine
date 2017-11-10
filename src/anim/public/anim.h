#ifndef ANIM_H
#define ANIM_H

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

struct pfobj_hdr;
struct entity;
struct skeleton;


/* Simple utility to get a reference to the skeleton structure. It shoould not be modified 
 * or freed.
 */
const struct skeleton *A_GetSkeleton(struct entity *ent);

/* Computes the size (in bytes) that is required to store all the animation subsystem
 * data from a PF Object file.
 */
size_t A_AL_PrivBuffSizeFromHeader(const struct pfobj_hdr *header);

/* Consumes lines of the stream and uses them to populate the private data stored 
 * in priv_buff.
 */
bool   A_AL_InitPrivFromStream(const struct pfobj_hdr *header, FILE *stream, void *priv_buff);

/* Dumps private animation data in PF Object format.
 */
void   A_AL_DumpPrivate(FILE *stream, void *priv_data);

#endif
