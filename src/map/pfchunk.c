#include "pfchunk.h"
#include "../render/public/render.h"

#include <stdlib.h>

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

struct pfchunk *M_PFChunk_New(size_t num_mats)
{
    size_t alloc_sz = sizeof(struct pfchunk) +
                      R_AL_PrivBuffSizeForChunk(TILES_PER_CHUNK_WIDTH,
                                                TILES_PER_CHUNK_HEIGHT,
                                                num_mats);

    struct pfchunk *ret = calloc(1, alloc_sz);
    if(!ret)
        return NULL;
    ret->render_private = ret + 1;

    return ret; 
}

void M_PFChunk_Free(struct pfchunk *chunk)
{
    free(chunk);
}

