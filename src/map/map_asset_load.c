#include "public/map.h"
#include "pfchunk.h"
#include "../asset_load.h"
#include "../render/public/render.h"

#define __USE_POSIX
#include <string.h>


#define READ_LINE(file, buff, fail_label)       \
    do{                                         \
        if(!fgets(buff, MAX_LINE_LEN, file))    \
            goto fail_label;                    \
        buff[MAX_LINE_LEN - 1] = '\0';          \
    }while(0)

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

bool m_al_parse_tile(const char *str, struct tile *out)
{
    if(strlen(str) != 6)
        goto fail;

    out->type          = (enum tiletype) (str[0] - '0');
    out->pathable      = (bool)          (str[1] - '0');
    out->base_height   = (int)           (str[2] - '0');
    out->top_mat_idx   = (int)           (str[3] - '0');
    out->sides_mat_idx = (int)           (str[4] - '0');
    out->ramp_height   = (int)           (str[5] - '0');

    return true;

fail:
    return false;
}

bool m_al_read_row(FILE *stream, struct tile *out)
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

bool m_al_read_pfchunk(FILE *stream, struct pfchunk *out)
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

bool M_AL_InitMapFromStreams(FILE *pfchunk_stream, FILE *pfmat_stream, struct map *out, 
                             size_t num_mats)
{
    struct pfchunk *chunk = M_PFChunk_New(num_mats);
    if(!chunk)
        goto fail_alloc;

    if(!m_al_read_pfchunk(pfchunk_stream, chunk))
        goto fail_read;

    const char *basedir = "/home/eduard/engine/assets/maps/grass-cliffs-1";
    if(!R_AL_InitPrivFromTilesAndMats(pfmat_stream, num_mats, 
                                      chunk->tiles, TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT,
                                      chunk->render_private, basedir)) {
        goto fail_init_render;                                  
    }

    out->head = chunk;
    out->tail = chunk;
    out->width = 1;
    out->height = 1;

    return true;

fail_init_render:
fail_read:
    M_PFChunk_Free(chunk);
fail_alloc:
    return false;
}

void M_AL_DumpMap(FILE *stream, const struct map *map)
{
    struct pfchunk *curr = map->head;

    while(curr) {

        for(int r = 0; r < TILES_PER_CHUNK_HEIGHT; r++) {
            for(int c = 0; c < TILES_PER_CHUNK_WIDTH; c++) {

                const struct tile *tile = &curr->tiles[r * TILES_PER_CHUNK_WIDTH + c];
                fprintf(stream, "%c%c%c%c%c", (char) (tile->type)          + '0',
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
     
        curr = curr->next;
    }
}

