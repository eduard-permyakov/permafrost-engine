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
#include "../map/public/tile.h"

#include "../asset_load.h"

#include <ctype.h>
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

#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))

/* For rendering the map */
#define VERTS_PER_FACE 6
#define FACES_PER_TILE 6

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

static bool al_vertices_from_tile(const struct tile *tile, struct vertex *out, 
                                  size_t r, size_t c)
{
    /* We take the directions to be relative to a normal vector facing outwards
     * from the plane of the face. West is to the right, east is to the left,
     * north is top, south is bottom. */
    struct face{
        struct vertex nw, ne, se, sw; 
    };

    struct face bot = {
        .nw = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - ((c+1) * X_COORDS_PER_TILE), (-1.0f * Y_COORDS_PER_TILE), 
                                 0.0f + (r * Z_COORDS_PER_TILE) },
            .uv     = (vec2_t) { 0.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .ne = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - (c * X_COORDS_PER_TILE), (-1.0f * Y_COORDS_PER_TILE), 
                                 0.0f + (r * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .se = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - (c * X_COORDS_PER_TILE), (-1.0f * Y_COORDS_PER_TILE), 
                                 0.0f + ((r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .sw = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - ((c+1) * X_COORDS_PER_TILE), -1.0f, 0.0f + ((r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
    };

    struct face top = {
        .nw = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - (c * X_COORDS_PER_TILE), (tile->base_height * Y_COORDS_PER_TILE), 
                                 0.0f + (r * Z_COORDS_PER_TILE) },
            .uv     = (vec2_t) { 0.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, 1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .ne = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - ((c+1) * X_COORDS_PER_TILE), (tile->base_height * Y_COORDS_PER_TILE), 
                                 0.0f + (r * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, 1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .se = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - ((c+1) * X_COORDS_PER_TILE), (tile->base_height * Y_COORDS_PER_TILE), 
                                 0.0f + ((r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .sw = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - (c * X_COORDS_PER_TILE), (tile->base_height * Y_COORDS_PER_TILE), 
                                 0.0f + ((r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
    };

    struct face back = {
        .nw = (struct vertex) {
            .pos    = top.nw.pos,
            .uv     = (vec2_t) { 0.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .ne = (struct vertex) {
            .pos    = top.ne.pos,
            .uv     = (vec2_t) { 1.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .se = (struct vertex) {
            .pos    = bot.nw.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .sw = (struct vertex) {
            .pos    = bot.ne.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
    };

    struct face front = {
        .nw = (struct vertex) {
            .pos    = top.sw.pos,
            .uv     = (vec2_t) { 0.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .ne = (struct vertex) {
            .pos    = top.se.pos,
            .uv     = (vec2_t) { 1.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .se = (struct vertex) {
            .pos    = bot.sw.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .sw = (struct vertex) {
            .pos    = bot.se.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
    };

    struct face left = {
        .nw = (struct vertex) {
            .pos    = top.sw.pos,
            .uv     = (vec2_t) { 0.0f, 1.0f },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .ne = (struct vertex) {
            .pos    = top.nw.pos,
            .uv     = (vec2_t) { 1.0f, 1.0f },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .se = (struct vertex) {
            .pos    = bot.ne.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .sw = (struct vertex) {
            .pos    = bot.se.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
    };

    struct face right = {
        .nw = (struct vertex) {
            .pos    = top.ne.pos,
            .uv     = (vec2_t) { 0.0f, 1.0f },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .ne = (struct vertex) {
            .pos    = top.se.pos,
            .uv     = (vec2_t) { 1.0f, 1.0f },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .se = (struct vertex) {
            .pos    = bot.sw.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
        .sw = (struct vertex) {
            .pos    = bot.nw.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
            .joint_indices = {0},
            .weights       = {0}
        },
    };

    struct face *faces[] = {
        &top, &bot, &front, &back, &left, &right 
    };

    for(int i = 0; i < ARR_SIZE(faces); i++) {
    
        struct face *curr = faces[i];

        /* First triangle */
        memcpy(out + (i * VERTS_PER_FACE) + 0, &curr->nw, sizeof(struct vertex));
        memcpy(out + (i * VERTS_PER_FACE) + 1, &curr->ne, sizeof(struct vertex));
        memcpy(out + (i * VERTS_PER_FACE) + 2, &curr->sw, sizeof(struct vertex));

        /* Second triangle */
        memcpy(out + (i * VERTS_PER_FACE) + 3, &curr->se, sizeof(struct vertex));
        memcpy(out + (i * VERTS_PER_FACE) + 4, &curr->sw, sizeof(struct vertex));
        memcpy(out + (i * VERTS_PER_FACE) + 5, &curr->ne, sizeof(struct vertex));
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
    void *unused_base = priv_buff + sizeof(struct render_private);
    size_t vbuff_sz = header->num_verts * sizeof(struct vertex);

    priv->mesh.num_verts = header->num_verts;
    priv->mesh.vbuff = unused_base;
    unused_base += vbuff_sz;

    priv->num_materials = header->num_materials;
    priv->materials = unused_base;

    for(int i = 0; i < header->num_verts; i++) {
        if(!al_read_vertex(stream, &priv->mesh.vbuff[i]))
            goto fail;
    }

    for(int i = 0; i < header->num_materials; i++) {

        priv->materials[i].texture.tunit = GL_TEXTURE0 + i;
        if(!al_read_material(stream, basedir, &priv->materials[i])) 
            goto fail;
    }

    R_GL_Init(priv);

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
    void *unused_base = priv_buff + sizeof(struct render_private);
    size_t vbuff_sz = num_verts * sizeof(struct vertex);

    priv->mesh.num_verts = num_verts;
    priv->mesh.vbuff = unused_base;
    unused_base += vbuff_sz;

    priv->num_materials = num_mats;
    priv->materials = unused_base;

    for(int r = 0; r < height; r++) {
        for(int c = 0; c < width; c++) {

            const struct tile *curr = &tiles[r * width + c];
            struct vertex *vert_base = &priv->mesh.vbuff[ (r * width + c) * VERTS_PER_FACE * FACES_PER_TILE ];

            if(!al_vertices_from_tile(curr, vert_base, r, c))
                goto fail; 
        }
    }

    for(int i = 0; i < num_mats; i++) {

        priv->materials[i].texture.tunit = GL_TEXTURE0 + i;
        if(!al_read_material(mats_stream, basedir, &priv->materials[i])) 
            goto fail;
    }

    R_GL_Init(priv);

    return true;

fail:
    return false;
}

