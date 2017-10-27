#include "asset_load.h"
#include "entity.h"

#include <stdio.h>
#define __USE_POSIX
#include <string.h>

#define MAX_ANIM_SETS 16
#define MAX_LINE_LEN  128

#define READ_LINE(file, buff, fail_label)       \
    do{                                         \
        if(!fgets(buff, MAX_LINE_LEN, file))    \
            goto fail_label;                    \
        buff[MAX_LINE_LEN - 1] = '\0';          \
    }while(0)

struct pfobj_hdr{
    float    version; 
    unsigned num_verts;
    unsigned num_joints;
    unsigned num_as;
    unsigned frame_counts[MAX_ANIM_SETS];
};

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
 *  end of 'struct entity' and accessed by the flexible array member 
 *  'mem'. Resource fields of 'struct entity' such as the vertex 
 *  buffer pointer or the animation data pointer point into this buffer.
 *
 *  This allows us to do a single malloc/free per each model while
 *  also not wasting any memory.
 * */
#if 0
static size_t al_alloc_size_from_hdr(const struct pfobj_hdr *header)
{
    size_t ret = 0;

    ret += sizeof(struct entity);
    ret += header->num_verts  * sizeof(struct vertex);
    ret += header->num_as     * sizeof(struct anim_clip);
    ret += header->num_joints * sizeof(struct joint);

    /*
     * For each frame of each animation clip, we also require:
     *
     *    1. a 'struct anim_sample' (for referencing this frame's SQT array)
     *    2. num_joint number of 'struct SQT's (each joint's transform
     *       for the current frame)
     */
    for(unsigned as_idx  = 0; as_idx  < header->num_as; as_idx++) {

        ret += header->frame_counts[as_idx] * 
               (sizeof(struct anim_sample) + header->num_joints * sizeof(struct SQT));
    }

    return ret;
}
#endif

struct entity *AL_EntityFromPFObj(const char *pfobj_path)
{
    struct entity *ret;
    FILE *stream;
    struct pfobj_hdr header;
    size_t alloc_size;

    stream = fopen(pfobj_path, "r");
    if(!stream){
        goto fail_fopen; 
    }

    if(!al_parse_header(stream, &header))
        goto fail_parse_hdr;

    printf("v: %f, nv: %d, nj: %d, ac: %d\n",
        header.version,
        header.num_verts,
        header.num_joints,
        header.num_as);
    for(int i = 0; i < header.num_as; i++) {
        printf("%d ", header.frame_counts[i]); 
    }
    printf("\n");

#if 0
    alloc_size = al_alloc_size_from_hdr(&header);
    printf("alloc sz: %zu bytes\n", alloc_size);
#endif

fail_alloc:
fail_parse_hdr:
fail_fopen:
    return NULL;
}

void ALEntityFree(struct entity *entity)
{

}

