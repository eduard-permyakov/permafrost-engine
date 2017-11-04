#include "public/render.h"
#include "render_private.h"
#include "vertex.h"
#include "render_gl.h"

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

static bool al_read_face(FILE *stream, struct face *out)
{
    char line[MAX_LINE_LEN];

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "f %d %d %d", &out->vertex_indeces[0],
                                   &out->vertex_indeces[1], 
                                   &out->vertex_indeces[2])) {
        goto fail;
    }

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

    priv->mesh.num_verts = header->num_verts;
    priv->mesh.vbuff = (void*)(priv + 1);
    priv->mesh.num_faces = header->num_faces;
    priv->mesh.ebuff = (void*)((char*)priv->mesh.vbuff + vbuff_sz);

    for(int i = 0; i < header->num_verts; i++) {
        if(!al_read_vertex(stream, &priv->mesh.vbuff[i]))
            goto fail;
    }

    for(int i = 0; i < header->num_faces; i++) {
        if(!al_read_face(stream, &priv->mesh.ebuff[i])) 
            goto fail;
    }

    GL_Init(priv);

    return true;

fail:
    return false;
}

void R_AL_DumpPrivate(FILE *stream, void *priv_data)
{
    struct render_private *priv = priv_data;

    for(int i = 0; i < priv->mesh.num_verts; i++) {

        struct vertex *v = &priv->mesh.vbuff[i];

        fprintf(stream, "v %.6f %.6f %.6f\n", v->pos.x, v->pos.y, v->pos.z); 
        fprintf(stream, "vt %.6f %.6f \n", v->uv.x, v->uv.y); 

        struct joint_weight empty = {0}; 

        fprintf(stream, "vw");
        for(int j = 0; j < 4; j++) {
            struct joint_weight *jw = &priv->mesh.vbuff[i].weights[j];

            if(memcmp(jw, &empty, sizeof(struct joint_weight)) != 0) {
                fprintf(stream, " %d/%.6f", jw->joint_idx, jw->weight); 
            }
        }
        fprintf(stream, "\n");
    }

    for(int i = 0; i < priv->mesh.num_faces; i++) {

        struct face *f = &priv->mesh.ebuff[i];

        fprintf(stream, "f %d %d %d\n",
            f->vertex_indeces[0], f->vertex_indeces[1], f->vertex_indeces[2]);
    }
}

