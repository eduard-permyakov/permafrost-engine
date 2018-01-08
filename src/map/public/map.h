#ifndef MAP_H
#define MAP_H

#include "../../pf_math.h"

#include <stdio.h>
#include <stdbool.h>

struct pfchunk;
struct pfmap_hdr;
struct map;

/*###########################################################################*/
/* MAP GENERAL                                                               */
/*###########################################################################*/

/* ------------------------------------------------------------------------
 * This renders all the chunks at once, which is wasteful when there are 
 * many off-screen chunks.
 * TODO: Rendering of visible chunks only
 * ------------------------------------------------------------------------
 */
void   M_RenderEntireMap(const struct map *map);
void   M_CenterAtOrigin(struct map *map);

/*###########################################################################*/
/* MAP ASSET LOADING                                                         */
/*###########################################################################*/

bool   M_AL_InitMapFromStream(const struct pfmap_hdr *header, const char *basedir,
                              FILE *stream, const char *pfmat_name, void *outmap);
size_t M_AL_BuffSizeFromHeader(const struct pfmap_hdr *header);

/* ------------------------------------------------------------------------
 * Writes the map in PFMap format.
 * ------------------------------------------------------------------------
 */
void   M_AL_DumpMap(FILE *stream, const struct map *map);

#endif
