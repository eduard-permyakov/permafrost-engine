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

#include "public/map.h"
#include "pfchunk.h"
#include "../asset_load.h"
#include "../render/public/render.h"
#include "map_private.h"

#include <stdlib.h>
#include <assert.h>
#define __USE_POSIX
#include <string.h>

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

bool m_al_parse_tile(const char *str, struct tile *out)
{
    if(strlen(str) != 6)
        goto fail;

    char type_hexstr[2] = {str[0], '\0'};

    out->type          = (enum tiletype) strtol(type_hexstr, NULL, 16);
    out->pathable      = (bool)          (str[1] - '0');
    out->base_height   = (int)           (str[2] - '0');
    out->top_mat_idx   = (int)           (str[3] - '0');
    out->sides_mat_idx = (int)           (str[4] - '0');
    out->ramp_height   = (int)           (str[5] - '0');

    return true;

fail:
    return false;
}

bool m_al_read_row(SDL_RWops *stream, struct tile *out)
{
    char line[MAX_LINE_LEN];

    READ_LINE(stream, line, fail); 

    char *string = line;
    char *saveptr;
    /* String points to the first token - the first tile of this row */
    string = strtok_r(line, " \t\n", &saveptr);

    for(int i = 0; i < TILES_PER_CHUNK_WIDTH; i++) {

        if(!string)
            goto fail;

        if(!m_al_parse_tile(string, out + i))
            goto fail;
        string = strtok_r(NULL, " \t\n", &saveptr);
    }

    /* That should have been it for this line */
    if(string != NULL)
        goto fail;

    return true;

fail:
    return false;
}

bool m_al_read_pfchunk(SDL_RWops *stream, struct pfchunk *out)
{
    for(int i = 0; i < TILES_PER_CHUNK_HEIGHT; i++) {
        
        if(!m_al_read_row(stream, out->tiles + (i * TILES_PER_CHUNK_WIDTH)))
            goto fail;
    }

    return true;

fail:
    return false;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/
 
bool M_AL_InitMapFromStream(const struct pfmap_hdr *header, const char *basedir,
                            SDL_RWops *stream, void *outmap)
{
    struct map *map = outmap;
    char line[MAX_LINE_LEN];

    map->width = header->num_cols;
    map->height = header->num_rows;
    map->pos = (vec3_t) {0.0f, 0.0f, 0.0f};

    size_t num_chunks = header->num_rows * header->num_cols;

    char *unused_base = (char*)(map + 1);
    unused_base += num_chunks * sizeof(struct pfchunk);

    for(int i = 0; i < num_chunks; i++) {

        map->chunks[i].render_private = (void*)unused_base;

        if(!m_al_read_pfchunk(stream, map->chunks + i))
            goto fail;

        int num_mats;
        READ_LINE(stream, line, fail); 
        if(!sscanf(line, "chunk_materials %d", &num_mats))
            goto fail;

        size_t renderbuff_sz = R_AL_PrivBuffSizeForChunk(
                               TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT, num_mats);
        unused_base += renderbuff_sz;

        if(!R_AL_InitPrivFromTilesAndMats(stream, num_mats, 
                                          map->chunks[i].tiles, TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT,
                                          map->chunks[i].render_private, basedir)) {
            goto fail;
        }
    }

    return true;

fail:
    return false;
}

size_t M_AL_BuffSizeFromHeader(const struct pfmap_hdr *header)
{
    size_t num_chunks = header->num_rows * header->num_cols;
    size_t avg_num_mats = ceil(header->num_materials / (float)num_chunks);

    return sizeof(struct map) + num_chunks * 
           (sizeof(struct pfchunk) + R_AL_PrivBuffSizeForChunk(
                                     TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT, avg_num_mats));
}

void M_AL_DumpMap(FILE *stream, const struct map *map)
{
    for(int i = 0; i < (map->width * map->height); i++) {

        const struct pfchunk *curr = &map->chunks[i];
    
        for(int r = 0; r < TILES_PER_CHUNK_HEIGHT; r++) {
            for(int c = 0; c < TILES_PER_CHUNK_WIDTH; c++) {

                const struct tile *tile = &curr->tiles[r * TILES_PER_CHUNK_WIDTH + c];

                char type_hex[2];
                assert(tile->type >= 0 && tile->type <= 16);
                snprintf(type_hex, sizeof(type_hex), "%1x", tile->type);

                fprintf(stream, "%c%c%c%c%c%c", (char) type_hex[0],
                                                (char) (tile->pathable)      + '0',
                                                (char) (tile->base_height)   + '0',
                                                (char) (tile->top_mat_idx)   + '0',
                                                (char) (tile->sides_mat_idx) + '0',
                                                (char) (tile->ramp_height)   + '0');

                if(c != (TILES_PER_CHUNK_WIDTH - 1))
                    fprintf(stream, " ");
            }
            fprintf(stream, "\n");
        }
    }
}

