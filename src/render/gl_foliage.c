/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
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
 *  Linking this software statically or dynamically with other modules is making
 *  a combined work based on this software. Thus, the terms and conditions of
 *  the GNU General Public License cover the whole combination.
 *
 *  As a special exception, the copyright holders of Permafrost Engine give
 *  you permission to link Permafrost Engine with independent modules to produce
 *  an executable, regardless of the license terms of these independent
 *  modules, and to copy and distribute the resulting executable under
 *  terms of your choice, provided that you also meet, for each linked
 *  independent module, the terms and conditions of the license of that
 *  module. An independent module is a module which is not derived from
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may
 *  extend this exception to your version of Permafrost Engine, but you are not
 *  obliged to do so. If you do not wish to do so, delete this exception
 *  statement from your version.
 *
 */

#define MEM_FILE_SYS MEM_SYS_RENDER
#define MEM_FILE_SUB MEM_SUB_RENDER_GL_FOLIAGE

#include "render_private.h"
#include "gl_render.h"
#include "gl_shader.h"
#include "gl_state.h"
#include "gl_vertex.h"
#include "gl_assert.h"
#include "gl_perf.h"
#include "gl_material.h"
#include "../camera.h"
#include "../main.h"
#include "../map/public/tile.h"
#include "../lib/public/mem.h"

#include <GL/glew.h>

#include <string.h>
#include <assert.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_FOLIAGE)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_FOLIAGE)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_FOLIAGE)

#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

/* Shared mesh VBO and its stride; set once in Init, used in every SetChunk */
static GLuint   s_mesh_vbo      = 0;
static GLsizei  s_vertex_stride = 0;

/* Per-chunk VAOs: each has attribs 0-3 (mesh) + 4-5 (instances) fully set up */
static GLuint  *s_chunk_vaos    = NULL;
/* Per-chunk instance VBOs (interleaved vec3 pos | float rot_y) */
static GLuint  *s_instance_vbos = NULL;
static size_t  *s_chunk_counts  = NULL;
static size_t   s_num_chunks    = 0;
static GLsizei  s_num_verts     = 0;
static GLuint   s_grass_tex     = 0;

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_MapFoliageInit(void *priv_arg, const size_t *num_chunks)
{
    ASSERT_IN_RENDER_THREAD();

    const struct render_private *priv = (const struct render_private *)priv_arg;

    s_num_chunks    = *num_chunks;
    s_num_verts     = (GLsizei)priv->mesh.num_verts;
    s_grass_tex     = priv->materials[0].texture.id;
    s_mesh_vbo      = priv->mesh.VBO;
    s_vertex_stride = (GLsizei)priv->vertex_stride;

    s_chunk_vaos    = PF_CALLOC(s_num_chunks, sizeof(GLuint));
    s_instance_vbos = PF_CALLOC(s_num_chunks, sizeof(GLuint));
    s_chunk_counts  = PF_CALLOC(s_num_chunks, sizeof(size_t));
    if(!s_chunk_vaos || !s_instance_vbos || !s_chunk_counts)
        goto fail_alloc;

    GL_ASSERT_OK();
    return;

fail_alloc:
    PF_FREE(s_chunk_vaos);
    PF_FREE(s_instance_vbos);
    PF_FREE(s_chunk_counts);
    s_chunk_vaos    = NULL;
    s_instance_vbos = NULL;
    s_chunk_counts  = NULL;
    s_num_chunks    = 0;
}

void R_GL_MapFoliageSetChunk(const size_t *chunk_idx, const vec3_t *positions,
                             const float *rotations, const size_t *count)
{
    ASSERT_IN_RENDER_THREAD();

    size_t idx = *chunk_idx;
    size_t n   = *count;
    assert(idx < s_num_chunks);

    s_chunk_counts[idx] = n;

    if(n == 0) {
        if(s_instance_vbos[idx]) {
            glDeleteBuffers(1, &s_instance_vbos[idx]);
            s_instance_vbos[idx] = 0;
        }
        if(s_chunk_vaos[idx]) {
            glDeleteVertexArrays(1, &s_chunk_vaos[idx]);
            s_chunk_vaos[idx] = 0;
        }
        return;
    }

    /* Upload interleaved [vec3 pos | float rot_y] instance data */
    const size_t inst_stride = sizeof(vec3_t) + sizeof(float);
    STALLOC(char, inst_data, n * inst_stride);

    for(size_t i = 0; i < n; i++) {
        char *base = inst_data + i * inst_stride;
        memcpy(base,                  &positions[i], sizeof(vec3_t));
        memcpy(base + sizeof(vec3_t), &rotations[i], sizeof(float));
    }

    if(!s_instance_vbos[idx])
        glGenBuffers(1, &s_instance_vbos[idx]);

    glBindBuffer(GL_ARRAY_BUFFER, s_instance_vbos[idx]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * inst_stride), inst_data, GL_STATIC_DRAW);
    STFREE(inst_data);

    /* (Re)build the per-chunk VAO with all 6 attribs fully configured */
    if(!s_chunk_vaos[idx])
        glGenVertexArrays(1, &s_chunk_vaos[idx]);

    glBindVertexArray(s_chunk_vaos[idx]);

    /* Attribs 0-3: per-vertex data from the shared mesh VBO */
    glBindBuffer(GL_ARRAY_BUFFER, s_mesh_vbo);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, s_vertex_stride, (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, s_vertex_stride,
        (void*)offsetof(struct vertex, uv));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, s_vertex_stride,
        (void*)offsetof(struct vertex, normal));
    glEnableVertexAttribArray(2);

    glVertexAttribIPointer(3, 1, GL_INT, s_vertex_stride,
        (void*)offsetof(struct vertex, material_idx));
    glEnableVertexAttribArray(3);

    /* Attribs 4-5: per-instance data from this chunk's instance VBO */
    const GLsizei inst_stride_gl = (GLsizei)(sizeof(vec3_t) + sizeof(float));
    glBindBuffer(GL_ARRAY_BUFFER, s_instance_vbos[idx]);

    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, inst_stride_gl, (void*)0);
    glEnableVertexAttribArray(4);
    glVertexAttribDivisor(4, 1);

    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, inst_stride_gl,
        (void*)sizeof(vec3_t));
    glEnableVertexAttribArray(5);
    glVertexAttribDivisor(5, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GL_ASSERT_OK();
}

void R_GL_MapFoliageShutdown(void)
{
    ASSERT_IN_RENDER_THREAD();

    for(size_t i = 0; i < s_num_chunks; i++) {
        if(s_instance_vbos[i]) {
            glDeleteBuffers(1, &s_instance_vbos[i]);
            s_instance_vbos[i] = 0;
        }
        if(s_chunk_vaos[i]) {
            glDeleteVertexArrays(1, &s_chunk_vaos[i]);
            s_chunk_vaos[i] = 0;
        }
    }
    PF_FREE(s_chunk_vaos);
    PF_FREE(s_instance_vbos);
    PF_FREE(s_chunk_counts);
    s_chunk_vaos    = NULL;
    s_instance_vbos = NULL;
    s_chunk_counts  = NULL;

    s_num_chunks    = 0;
    s_num_verts     = 0;
    s_grass_tex     = 0;
    s_mesh_vbo      = 0;
    s_vertex_stride = 0;
}

void R_GL_MapFoliageBeginDraw(const struct camera *cam, const float *scale,
                              const struct map_resolution *res, const vec2_t *map_pos)
{
    ASSERT_IN_RENDER_THREAD();

    GL_PERF_PUSH_GROUP(0, "map_foliage");

    Camera_TickFinishPerspective((struct camera*)cam);

    R_GL_StateSet("cover_scale", (struct uval){
        .type         = UTYPE_FLOAT,
        .val.as_float = *scale,
    });
    R_GL_StateSet(GL_U_TEXTURE0, (struct uval){
        .type       = UTYPE_INT,
        .val.as_int = 0,
    });

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_grass_tex);

    R_GL_Shader_Install("foliage");
    R_GL_ShadowMapBind();

    /* Bind the fog-of-war state so the shader can hide/dim fogged foliage. */
    GLuint prog = R_GL_Shader_GetProgForName("foliage");
    R_GL_StateSet(GL_U_MAP_RES, (struct uval){
        .type            = UTYPE_IVEC4,
        .val.as_ivec4[0] = res->chunk_w,
        .val.as_ivec4[1] = res->chunk_h,
        .val.as_ivec4[2] = res->tile_w,
        .val.as_ivec4[3] = res->tile_h,
    });
    R_GL_StateInstall(GL_U_MAP_RES, prog);
    R_GL_StateSet(GL_U_MAP_POS, (struct uval){
        .type        = UTYPE_VEC2,
        .val.as_vec2 = *map_pos,
    });
    R_GL_StateInstall(GL_U_MAP_POS, prog);
    R_GL_MapFogBindLast(GL_TEXTURE5, prog, "visbuff");

    glDisable(GL_CULL_FACE);
}

void R_GL_MapFoliageDrawChunk(const size_t *chunk_idx)
{
    ASSERT_IN_RENDER_THREAD();

    size_t idx = *chunk_idx;
    assert(idx < s_num_chunks);

    size_t n = s_chunk_counts[idx];
    if(n == 0 || !s_chunk_vaos[idx])
        return;

    glBindVertexArray(s_chunk_vaos[idx]);
    glDrawArraysInstanced(GL_TRIANGLES, 0, s_num_verts, (GLsizei)n);
    glBindVertexArray(0);
    GL_ASSERT_OK();
}

void R_GL_MapFoliageEndDraw(void)
{
    ASSERT_IN_RENDER_THREAD();

    glEnable(GL_CULL_FACE);
    glBindTexture(GL_TEXTURE_2D, 0);

    GL_PERF_POP_GROUP();
    GL_ASSERT_OK();
}

