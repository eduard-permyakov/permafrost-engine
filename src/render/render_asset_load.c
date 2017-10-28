#include "public/render.h"
#include "render_private.h"
#include "vertex.h"

#include "../asset_load.h"

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

static bool al_read_vertex(FILE *stream, struct vertex *out)
{
    char line[MAX_LINE_LEN];

    READ_LINE(stream, line, fail); 
    if(!sscanf(line, "v %f %f %f", &out->pos.x, &out->pos.y, &out->pos.z))
        goto fail;

    READ_LINE(stream, line, fail); 
    if(!sscanf(line, "vt %f %f", &out->uv.x, &out->uv.y))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!strstr(line, "vw"))
        goto fail;

    char *string = line;
    char *saveptr;
    int i;

    /* Write 0.0 weights by default */
    memset(out->weights, 0, sizeof(out->weights));

    /* Consume the first token, the attribute name 'vw' */
    string = strtok_r(line, " \t", &saveptr);
    for(i = 0; i < 4; i++) {

        string = strtok_r(NULL, " \t", &saveptr);
        if(!string)
            break;

        if(!sscanf(string, "%d/%f", &out->weights[i].joint_idx, &out->weights[i].weight))
            goto fail;
    }

    if(i == 0)
        goto fail;

    return true;

fail:
    return false;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

size_t R_AL_PrivBuffSizeFromHeader(const struct pfobj_hdr *header)
{
    size_t ret = 0;

    ret += sizeof(struct render_private);
    ret += header->num_verts * sizeof(struct vertex);
    ret += header->num_faces * sizeof(struct face);

    return ret;
}

bool R_AL_InitPrivFromStream(const struct pfobj_hdr *header, FILE *stream, void *priv_buff)
{
    struct render_private *priv = priv_buff;
    size_t vbuff_sz = header->num_verts * sizeof(struct vertex);

    priv->mesh.vbuff = (void*)(priv + 1);
    priv->mesh.ebuff = (void*)((char*)priv->mesh.vbuff + vbuff_sz);

    for(int i = 0; i < header->num_verts; i++) {
        al_read_vertex(stream, &priv->mesh.vbuff[i]);
    }

    return true;

fail:
    return false;
}

