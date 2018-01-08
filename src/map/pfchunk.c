/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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

