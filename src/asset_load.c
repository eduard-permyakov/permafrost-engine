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
    if(!sscanf(line, "num_faces %d", &out->num_faces))
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
struct entity *AL_EntityFromPFObj(const char *pfobj_path, const char *name, size_t namelen)
{
    struct entity *ret;
    FILE *stream;
    struct pfobj_hdr header;
    size_t alloc_size;

    stream = fopen(pfobj_path, "r");
    if(!stream){
        goto fail; 
    }

    if(!al_parse_header(stream, &header))
        goto fail;

    printf("v: %f, nv: %d, nj: %d, nf: %d, nm: %d, ac: %d\n",
        header.version,
        header.num_verts,
        header.num_joints,
        header.num_faces,
        header.num_materials,
        header.num_as);
    for(int i = 0; i < header.num_as; i++) {
        printf("%d ", header.frame_counts[i]); 
    }
    printf("\n");
    
    char *tmpbuff = malloc(32*1024*512);
    size_t render_buffsz = R_AL_PrivBuffSizeFromHeader((const struct pfobj_hdr*)&header);
    size_t anim_buffsz = R_AL_PrivBuffSizeFromHeader((const struct pfobj_hdr*)&header);

    if(!R_AL_InitPrivFromStream((const struct pfobj_hdr*)&header, stream, tmpbuff))
        goto fail;

    if(!A_AL_InitPrivFromStream(&header, stream, tmpbuff + render_buffsz))
        goto fail;

    ret = malloc(sizeof(struct entity));
    ret->render_private = tmpbuff;
    ret->anim_private = tmpbuff + render_buffsz;

    assert(namelen < sizeof(ret->name));
    strncpy(ret->name, name, namelen);
    ret->name[sizeof(ret->name) - 1] = '\0';

    return ret;

fail:
    return NULL;
}

void ALEntityFree(struct entity *entity)
{

}

