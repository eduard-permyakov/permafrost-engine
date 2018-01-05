#ifndef MAP_H
#define MAP_H

#include <stdio.h>
#include <stdbool.h>

struct pfchunk;

struct map{
    /* ------------------------------------------------------------------------
     * Doubly-linked list of all pfchunks belonging to this map.
     * ------------------------------------------------------------------------
     */
    struct pfchunk *head, *tail;
    /* ------------------------------------------------------------------------
     * In numbers of chunks.
     * ------------------------------------------------------------------------
     */
    size_t width, height;
};


/*###########################################################################*/
/* MAP ASSET LOADING                                                         */
/*###########################################################################*/

/* TODO: We are using a single PFChunk for now - next this will be initialized
 * from a PFMap stream, which contains different PFChunk references and how 
 * they are interconnected with one another. 
 *
 * 'num_mats' will also come from the PFMap file...
 */
bool M_AL_InitMapFromStreams(FILE *pfchunk_stream, FILE *pfmat_stream, struct map *out,
                             size_t num_mats);

/* ------------------------------------------------------------------------
 * Writes the map in PFMap format.
 * ------------------------------------------------------------------------
 */
void M_AL_DumpMap(FILE *stream, const struct map *map);

#endif
