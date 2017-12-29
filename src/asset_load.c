#include "asset_load.h"
#include "entity.h"

#include "render/public/render.h"
#include "anim/public/anim.h"

#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#define __USE_POSIX
#include <string.h>
#include <stdlib.h> 


#define READ_LINE(file, buff, fail_label)       \
    do{                                         \
        if(!fgets(buff, MAX_LINE_LEN, file))    \
            goto fail_label;                    \
        buff[MAX_LINE_LEN - 1] = '\0';          \
    }while(0)

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool al_parse_header(FILE *stream, struct pfobj_hdr *out)
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
    FILE *stream;
    struct pfobj_hdr header;
    size_t alloc_size;

    char pfobj_path[BASEDIR_LEN * 2];
    assert( strlen(base_path) + strlen(pfobj_name) + 1 < sizeof(pfobj_path) );
    strcpy(pfobj_path, base_path);
    strcat(pfobj_path, "/");
    strcat(pfobj_path, pfobj_name);

    stream = fopen(pfobj_path, "r");
    if(!stream)
        goto fail_parse; 

    if(!al_parse_header(stream, &header))
        goto fail_parse;

    printf("v: %f, nv: %d, nj: %d, nm: %d, ac: %d\n",
        header.version,
        header.num_verts,
        header.num_joints,
        header.num_materials,
        header.num_as);
    for(int i = 0; i < header.num_as; i++) {
        printf("%d ", header.frame_counts[i]); 
    }
    printf("\n");
    
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

    assert( strlen(name) < sizeof(ret->name) );
    strcpy(ret->name, name);

    assert( strlen(base_path) < sizeof(ret->basedir) );
    strcpy(ret->basedir, base_path);

    return ret;

fail_init:
    free(ret);
fail_alloc:
fail_parse:
    return NULL;
}

void AL_EntityFree(struct entity *entity)
{
    free(entity);
}

