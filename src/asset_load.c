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

#include "asset_load.h"
#include "entity.h"

#include "render/public/render.h"
#include "anim/public/anim.h"
#include "map/public/map.h"

#include <SDL.h>

#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#ifndef __USE_POSIX
    #define __USE_POSIX
#endif
#include <string.h>
#include <stdlib.h> 

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool al_parse_pfobj_header(SDL_RWops *stream, struct pfobj_hdr *out)
{
    char line[MAX_LINE_LEN];

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "version %f", &out->version))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_verts %d", &out->num_verts))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_joints %d", &out->num_joints))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_materials %d", &out->num_materials))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_as %d", &out->num_as))
        goto fail;

    if(out->num_as > MAX_ANIM_SETS)
        goto fail;

    READ_LINE(stream, line, fail);
    if(!(strstr(line, "frame_counts")))
        goto fail;

    char *string = line;
    char *saveptr;

    /* Consume the first token, the property name 'frame_counts' */
    string = strtok_r(line, " \t", &saveptr);
    for(int i = 0; i < out->num_as; i++) {

        string = strtok_r(NULL, " \t", &saveptr);
        if(!string)
            goto fail;

        if(!sscanf(string, "%d", &out->frame_counts[i]))
            goto fail;
    }

    return true;

fail:
    return false;
}

static bool al_parse_pfmap_header(SDL_RWops *stream, struct pfmap_hdr *out)
{
    char line[MAX_LINE_LEN];

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "version %f", &out->version))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_rows %d", &out->num_rows))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_cols %d", &out->num_cols))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_materials %d", &out->num_materials))
        goto fail;

    return true;

fail:
    return false;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

/*
 *  We compute the amount of memory needed for each entity ahead of 
 *  time. The we allocate a single buffer, which is appended to the 
 *  end of 'struct entity'. Resource fields of 'struct entity' such 
 *  as the vertex buffer pointer or the animation data pointer point 
 *  into this buffer.
 *
 *  This allows us to do a single malloc/free per each model while
 *  also not wasting any memory.
 */
struct entity *AL_EntityFromPFObj(const char *base_path, const char *pfobj_name, const char *name)
{
    struct entity *ret;
    SDL_RWops *stream;
    struct pfobj_hdr header;
    size_t alloc_size;

    char pfobj_path[BASEDIR_LEN * 2];
    assert( strlen(base_path) + strlen(pfobj_name) + 1 < sizeof(pfobj_path) );
    strcpy(pfobj_path, base_path);
    strcat(pfobj_path, "/");
    strcat(pfobj_path, pfobj_name);

    stream = SDL_RWFromFile(pfobj_path, "r");
    if(!stream)
        goto fail_parse; 

    if(!al_parse_pfobj_header(stream, &header))
        goto fail_parse;

    size_t render_buffsz = R_AL_PrivBuffSizeFromHeader(&header);
    size_t anim_buffsz = A_AL_PrivBuffSizeFromHeader(&header);

    ret = malloc(sizeof(struct entity) + render_buffsz + anim_buffsz);
    if(!ret)
        goto fail_alloc;
    ret->render_private = ret + 1;
    ret->anim_private = ((char*)ret->render_private) + render_buffsz;

    if(!R_AL_InitPrivFromStream(&header, base_path, stream, ret->render_private))
        goto fail_init;

    if(!A_AL_InitPrivFromStream(&header, stream, ret->anim_private))
        goto fail_init;

    /* Entities with no joints and no animation sets are considered static. 
     * Otherwise, they must have both a nonzero number ofjoints and 
     * animation sets to be considered valid considered valid. */
    //TODO: nicer way - we should still be able to render animated pfobj as static
    ret->animated = (header.num_joints > 0);
    assert(!ret->animated || header.num_as > 0);

    assert( strlen(name) < sizeof(ret->name) );
    strcpy(ret->name, name);

    assert( strlen(base_path) < sizeof(ret->basedir) );
    strcpy(ret->basedir, base_path);

    SDL_RWclose(stream);

    ret->uid = Entity_NewUID();
    return ret;

fail_init:
    free(ret);
fail_alloc:
fail_parse:
    SDL_RWclose(stream);
fail_open:
    return NULL;
}

void AL_EntityFree(struct entity *entity)
{
    //TODO: Clean up OpenGL buffers
    free(entity);
}

struct map *AL_MapFromPFMap(const char *base_path, const char *pfmap_name)
{
    struct map *ret;
    SDL_RWops *stream;

    char pfmap_path[BASEDIR_LEN * 2];
    assert( strlen(base_path) + strlen(pfmap_name) + 1 < sizeof(pfmap_path) );
    strcpy(pfmap_path, base_path);
    strcat(pfmap_path, "/");
    strcat(pfmap_path, pfmap_name);

    stream = SDL_RWFromFile(pfmap_path, "r");
    if(!stream)
        goto fail_open;

    struct pfmap_hdr header;
    if(!al_parse_pfmap_header(stream, &header))
        goto fail_parse;

    ret = malloc(M_AL_BuffSizeFromHeader(&header));
    if(!ret)
        goto fail_alloc;

    if(!M_AL_InitMapFromStream(&header, base_path, stream, ret))
        goto fail_init;

    SDL_RWclose(stream);
    return ret;

fail_init:
    free(ret);
fail_alloc:
fail_parse:
    SDL_RWclose(stream);
fail_open:
    return NULL;
}

struct map *AL_MapFromPFMapString(const char *str)
{

}

void AL_MapFree(struct map *map)
{
    //TODO: Clean up OpenGL buffers
    free(map);
}

bool AL_ReadLine(SDL_RWops *stream, char *outbuff)
{
    bool done = false;
    int idx = 0;
    do {
         
        if(!SDL_RWread(stream, outbuff + idx, 1, 1))
            return false; 

        if(outbuff[idx] == '\n') {
            outbuff[++idx] = '\0';
            return true;
        }
        
        idx++; 
    }while(idx < MAX_LINE_LEN);

    return false;
}

