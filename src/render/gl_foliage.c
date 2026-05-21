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

#include <GL/glew.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

/* VAO that combines the grass mesh VBO with the per-instance data VBO */
static GLuint  s_cover_vao     = 0;
/* VBO holding interleaved per-instance [vec3 pos | float rot_y] data */
static GLuint  s_instance_vbo  = 0;
static size_t  s_num_instances = 0;
static GLsizei s_num_verts     = 0;
static GLuint  s_grass_tex     = 0;

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_MapFoliageInit(void *priv_arg, const vec3_t *positions,
                       const float *rotations, const size_t *count)
{
    ASSERT_IN_RENDER_THREAD();

    const struct render_private *priv = (const struct render_private *)priv_arg;

    s_num_instances = *count;
    s_num_verts     = (GLsizei)priv->mesh.num_verts;
    s_grass_tex     = priv->materials[0].texture.id;

    if(s_num_instances == 0)
        return;

    /* Create a foliage VAO that combines the shared mesh VBO with a new
     * per-instance data VBO. We set up attributes 0-3 from the mesh
     * VBO identically to how R_GL_Init does it for static meshes. */
    glGenVertexArrays(1, &s_cover_vao);
    glBindVertexArray(s_cover_vao);

    /* Bind the mesh VBO and configure per-vertex attributes */
    glBindBuffer(GL_ARRAY_BUFFER, priv->mesh.VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, priv->vertex_stride, (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, priv->vertex_stride,
        (void*)offsetof(struct vertex, uv));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, priv->vertex_stride,
        (void*)offsetof(struct vertex, normal));
    glEnableVertexAttribArray(2);

    glVertexAttribIPointer(3, 1, GL_INT, priv->vertex_stride,
        (void*)offsetof(struct vertex, material_idx));
    glEnableVertexAttribArray(3);

    /* Build interleaved per-instance buffer: [vec3 pos][float rot_y] */
    const size_t inst_stride = sizeof(vec3_t) + sizeof(float);
    char *inst_data = malloc(s_num_instances * inst_stride);
    if(!inst_data)
        goto fail_alloc;

    for(size_t i = 0; i < s_num_instances; i++) {
        char *base = inst_data + i * inst_stride;
        memcpy(base,                    &positions[i], sizeof(vec3_t));
        memcpy(base + sizeof(vec3_t),   &rotations[i], sizeof(float));
    }

    glGenBuffers(1, &s_instance_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s_num_instances * inst_stride),
        inst_data, GL_STATIC_DRAW);
    free(inst_data);

    /* Attrib 4: per-instance world position (vec3), advance once per instance */
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, (GLsizei)inst_stride, (void*)0);
    glEnableVertexAttribArray(4);
    glVertexAttribDivisor(4, 1);

    /* Attrib 5: per-instance Y rotation (float), advance once per instance */
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, (GLsizei)inst_stride,
        (void*)sizeof(vec3_t));
    glEnableVertexAttribArray(5);
    glVertexAttribDivisor(5, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GL_ASSERT_OK();
    return;

fail_alloc:
    glDeleteVertexArrays(1, &s_cover_vao);
    s_cover_vao = 0;
    s_num_instances = 0;
}

void R_GL_MapFoliageShutdown(void)
{
    ASSERT_IN_RENDER_THREAD();

    if(s_instance_vbo) {
        glDeleteBuffers(1, &s_instance_vbo);
        s_instance_vbo = 0;
    }
    if(s_cover_vao) {
        glDeleteVertexArrays(1, &s_cover_vao);
        s_cover_vao = 0;
    }
    s_num_instances = 0;
    s_num_verts     = 0;
    s_grass_tex     = 0;
}

void R_GL_MapFoliageDraw(const struct camera *cam, const float *scale)
{
    ASSERT_IN_RENDER_THREAD();

    if(!s_cover_vao || s_num_instances == 0 || s_grass_tex == 0)
        return;

    GL_PERF_PUSH_GROUP(0, "map_foliage");

    /* Ensure view/projection are set from this frame's camera */
    Camera_TickFinishPerspective((struct camera*)cam);

    R_GL_Shader_Install("foliage");
    GLuint prog = R_GL_Shader_GetCurrActive();

    R_GL_StateInstall(GL_U_VIEW,          prog);
    R_GL_StateInstall(GL_U_PROJECTION,    prog);
    R_GL_StateInstall(GL_U_LS_TRANS,      prog);
    R_GL_StateInstall(GL_U_AMBIENT_COLOR, prog);
    R_GL_StateInstall(GL_U_LIGHT_POS,     prog);
    R_GL_StateInstall(GL_U_LIGHT_COLOR,   prog);
    R_GL_StateInstall(GL_U_SHADOWS_ON,    prog);
    R_GL_StateInstall(GL_U_SHADOW_MAP,    prog);

    R_GL_StateSet("cover_scale", (struct uval){
        .type       = UTYPE_FLOAT,
        .val.as_float = *scale,
    });
    R_GL_StateInstall("cover_scale", prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_grass_tex);
    R_GL_StateSet(GL_U_TEXTURE0, (struct uval){
        .type    = UTYPE_INT,
        .val.as_int = 0,
    });
    R_GL_StateInstall(GL_U_TEXTURE0, prog);

    /* Grass cross-quads need both face directions lit */
    glDisable(GL_CULL_FACE);

    glBindVertexArray(s_cover_vao);
    glDrawArraysInstanced(GL_TRIANGLES, 0, s_num_verts, (GLsizei)s_num_instances);
    glBindVertexArray(0);

    glEnable(GL_CULL_FACE);
    glBindTexture(GL_TEXTURE_2D, 0);

    GL_PERF_POP_GROUP();
    GL_ASSERT_OK();
}
