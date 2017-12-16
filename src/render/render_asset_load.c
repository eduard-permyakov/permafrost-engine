#include "public/render.h"
#include "render_private.h"
#include "vertex.h"
#include "material.h"
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

#define STREVAL(a) STR(a)
#define STR(a) #a

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
    if(!sscanf(line, "vn %f %f %f", &out->normal.x, &out->normal.y, &out->normal.z))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!strstr(line, "vw"))
        goto fail;

    char *string = line;
    char *saveptr;
    int i;

    /* Write 0.0 weights by default */
    memset(out->weights, 0, sizeof(out->weights));
    memset(out->joint_indices, 0, sizeof(out->joint_indices));

    /* Consume the first token, the attribute name 'vw' */
    string = strtok_r(line, " \t", &saveptr);
    for(i = 0; i < 4; i++) {

        string = strtok_r(NULL, " \t", &saveptr);
        if(!string)
            break;

        if(!sscanf(string, "%d/%f", &out->joint_indices[i], &out->weights[i]))
            goto fail;
    }

    if(i == 0)
        goto fail;

    READ_LINE(stream, line, fail); 
    if(!sscanf(line, "vm %d", &out->material_idx))
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

static bool al_read_material(FILE *stream, const char *basedir, struct material *out)
{
    char line[MAX_LINE_LEN];

    /* Consume the first line with the name - we don't use it currently */
    READ_LINE(stream, line, fail);

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " ambient %f", &out->ambient_intensity))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " diffuse %f %f %f", &out->diffuse_clr.x, &out->diffuse_clr.y, &out->diffuse_clr.z))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " specular %f %f %f", &out->specular_clr.x, &out->specular_clr.y, &out->specular_clr.z))
        goto fail;

    char texname[32];
    READ_LINE(stream, line, fail);
    if(!sscanf(line, " texture %" STREVAL(sizeof(texname)) "s",  texname))
        goto fail;
    texname[sizeof(texname)-1] = '\0';

    if(!R_Texture_GetForName(texname, &out->texture.id) &&
       !R_Texture_Load(basedir, texname, &out->texture.id))
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
    ret += header->num_materials * sizeof(struct material);

    return ret;
}

/*
 * Render private buff layout:
 *
 *  +---------------------------------+ <-- base
 *  | struct render_private[1]        |
 *  +---------------------------------+
 *  | struct vertex[num_verts]        |
 *  +---------------------------------+
 *  | struct face[num_faces]          |
 *  +---------------------------------+
 *  | struct material[num_materials]  |
 *  +---------------------------------+
 *
 */

bool R_AL_InitPrivFromStream(const struct pfobj_hdr *header, const char *basedir, FILE *stream, void *priv_buff)
{
    struct render_private *priv = priv_buff;
    void *unused_base = priv_buff + sizeof(struct render_private);
    size_t vbuff_sz = header->num_verts * sizeof(struct vertex);

    priv->mesh.num_verts = header->num_verts;
    priv->mesh.vbuff = unused_base;
    unused_base += vbuff_sz;

    priv->mesh.num_faces = header->num_faces;
    priv->mesh.ebuff = unused_base;
    unused_base += header->num_faces * sizeof(struct face);

    priv->num_materials = header->num_materials;
    priv->materials = unused_base;

    for(int i = 0; i < header->num_verts; i++) {
        if(!al_read_vertex(stream, &priv->mesh.vbuff[i]))
            goto fail;
    }

    for(int i = 0; i < header->num_faces; i++) {
        if(!al_read_face(stream, &priv->mesh.ebuff[i])) 
            goto fail;
    }

    for(int i = 0; i < header->num_materials; i++) {

        priv->materials[i].texture.tunit = GL_TEXTURE0 + i;
        if(!al_read_material(stream, basedir, &priv->materials[i])) 
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

        fprintf(stream, "vw");
        for(int j = 0; j < 4; j++) {

            if(v->weights[j]) {
                fprintf(stream, " %d/%.6f", v->joint_indices[j], v->weights[j]);
            }
        }
        fprintf(stream, "\n");

        fprintf(stream, "vm %d\n", v->material_idx); 
    }

    for(int i = 0; i < priv->mesh.num_faces; i++) {

        struct face *f = &priv->mesh.ebuff[i];

        fprintf(stream, "f %d %d %d\n",
            f->vertex_indeces[0], f->vertex_indeces[1], f->vertex_indeces[2]);
    }
}

