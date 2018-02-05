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

#include "public/render.h"
#include "render_private.h"
#include "vertex.h"
#include "material.h"
#include "render_gl.h"

#include "../asset_load.h"
#include "../map/public/tile.h"

#include <ctype.h>
#define __USE_POSIX
#include <string.h>

#if defined(_WIN32)
    #define strtok_r strtok_s
#endif


#define READ_LINE(file, buff, fail_label)       \
    do{                                         \
        if(!fgets(buff, MAX_LINE_LEN, file))    \
            goto fail_label;                    \
        buff[MAX_LINE_LEN - 1] = '\0';          \
    }while(0)

#define STREVAL(a) STR(a)
#define STR(a) #a

#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))

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

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " texture %" STREVAL(sizeof(out->texname)) "s",  out->texname))
        goto fail;
    out->texname[sizeof(out->texname)-1] = '\0';

    if(!R_Texture_GetForName(out->texname, &out->texture.id) &&
       !R_Texture_Load(basedir, out->texname, &out->texture.id))
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
 *  | struct material[num_materials]  |
 *  +---------------------------------+
 *
 */

bool R_AL_InitPrivFromStream(const struct pfobj_hdr *header, const char *basedir, FILE *stream, void *priv_buff)
{
    struct render_private *priv = priv_buff;
    char *unused_base = (char*)priv_buff + sizeof(struct render_private);
    size_t vbuff_sz = header->num_verts * sizeof(struct vertex);

    priv->mesh.num_verts = header->num_verts;
    priv->mesh.vbuff = (void*)unused_base;
    unused_base += vbuff_sz;

    priv->num_materials = header->num_materials;
    priv->materials = (void*)unused_base;

    for(int i = 0; i < header->num_verts; i++) {
        if(!al_read_vertex(stream, &priv->mesh.vbuff[i]))
            goto fail;
    }

    for(int i = 0; i < header->num_materials; i++) {

        priv->materials[i].texture.tunit = GL_TEXTURE0 + i;
        if(!al_read_material(stream, basedir, &priv->materials[i])) 
            goto fail;
    }

    R_GL_Init(priv, (header->num_as > 0));

    return true;

fail:
    return false;
}

void R_AL_DumpPrivate(FILE *stream, void *priv_data)
{
    struct render_private *priv = priv_data;

    /* Write verticies */
    for(int i = 0; i < priv->mesh.num_verts; i++) {

        struct vertex *v = &priv->mesh.vbuff[i];

        fprintf(stream, "v %.6f %.6f %.6f\n", v->pos.x, v->pos.y, v->pos.z); 
        fprintf(stream, "vt %.6f %.6f \n", v->uv.x, v->uv.y); 
        fprintf(stream, "vn %.6f %.6f %.6f\n", v->normal.x, v->normal.y, v->normal.z);

        fprintf(stream, "vw ");
        for(int j = 0; j < 4; j++) {

            if(v->weights[j]) {
                fprintf(stream, "%d/%.6f ", v->joint_indices[j], v->weights[j]);
            }
        }
        fprintf(stream, "\n");

        fprintf(stream, "vm %d\n", v->material_idx); 
    }

    /* Write materials */
    for(int i = 0; i < priv->num_materials; i++) {
    
        struct material *m = &priv->materials[i];

        /* We don't keep track of the material names; this isn't strictly necessary */
        char name[32];
        snprintf(name, sizeof(name), "Material.%d", i + 1);

        fprintf(stream, "material %s\n", name);
        fprintf(stream, "\tambient %.6f\n", m->ambient_intensity);
        fprintf(stream, "\tdiffuse %.6f %.6f %.6f\n", 
            m->diffuse_clr.x, m->diffuse_clr.y, m->diffuse_clr.z);
        fprintf(stream, "\tspecular %.6f %.6f %.6f\n", 
            m->specular_clr.x, m->specular_clr.y, m->specular_clr.z);
        fprintf(stream, "\ttexture %s\n", m->texname);
    }
}

size_t R_AL_PrivBuffSizeForChunk(size_t tiles_width, size_t tiles_height, size_t num_mats)
{
    size_t ret = 0;

    ret += sizeof(struct render_private);
    /* We are going to draw each tile as a 6-sided quad, each quad consisting 
     * of two triangles */
    ret += (tiles_width * tiles_height) * sizeof(struct vertex) * VERTS_PER_FACE * FACES_PER_TILE;
    ret += sizeof(struct material) * num_mats;

    return ret;
}

bool R_AL_InitPrivFromTilesAndMats(FILE *mats_stream, size_t num_mats, 
                                  const struct tile *tiles, size_t width, size_t height, 
                                  void *priv_buff, const char *basedir)
{
    size_t num_verts = VERTS_PER_FACE * FACES_PER_TILE * (width * height);

    struct render_private *priv = priv_buff;
    char *unused_base = (char*)priv_buff + sizeof(struct render_private);
    size_t vbuff_sz = num_verts * sizeof(struct vertex);

    priv->mesh.num_verts = num_verts;
    priv->mesh.vbuff = (void*)unused_base;
    unused_base += vbuff_sz;

    priv->num_materials = num_mats;
    priv->materials = (void*)unused_base;

    for(int r = 0; r < height; r++) {
        for(int c = 0; c < width; c++) {

            const struct tile *curr = &tiles[r * width + c];
            struct vertex *vert_base = &priv->mesh.vbuff[ (r * width + c) * VERTS_PER_FACE * FACES_PER_TILE ];

            R_GL_VerticesFromTile(curr, vert_base, r, c);
        }
    }

    for(int i = 0; i < num_mats; i++) {

        priv->materials[i].texture.tunit = GL_TEXTURE0 + i;
        if(!al_read_material(mats_stream, basedir, &priv->materials[i])) 
            goto fail;
   }

    R_GL_Init(priv, false);

    return true;

fail:
    return false;
}

