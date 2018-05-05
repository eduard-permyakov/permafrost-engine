/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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

#include "mesh.h"
#include "vertex.h"
#include "texture.h"
#include "shader.h"
#include "public/render.h"
#include "../gl_uniforms.h"
#include "../map/public/tile.h"
#include "../camera.h"
#include "../pf_math.h"
#include "../config.h"

#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#define MAX(a, b)            ((a) > (b) ? (a) : (b))
#define ARR_SIZE(a)          (sizeof(a)/sizeof(a[0])) 
#define MINIMAP_RES          (1024)
#define MINIMAP_SIZE         (256)
#define MINIMAP_BORDER_CLR   ((vec3_t){65.0f/256.0f, 65.0f/256.0f, 65.0f/256.0f})
#define MINIMAP_BORDER_WIDTH (3.0f)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

struct render_minimap_ctx{
    struct texture minimap_texture;
    struct mesh    minimap_mesh;
}s_ctx;

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_MinimapBake(void **chunk_rprivates, mat4x4_t *chunk_model_mats, 
                      size_t chunk_x, size_t chunk_z,
                      vec3_t map_center, vec2_t map_size)
{
    /* Create a new camera, with orthographic projection, centered 
     * over the map and facing straight down. */
    DECL_CAMERA_STACK(map_cam);
    memset(&map_cam, 0, g_sizeof_camera);

    vec3_t offset = (vec3_t){0.0f, 200.0f, 0.0f};
    PFM_Vec3_Add(&map_center, &offset, &map_center);

    Camera_SetPos((struct camera*)map_cam, map_center);
    Camera_SetPitchAndYaw((struct camera*)map_cam, -90.0f, 90.0f);

    float map_dim = MAX(map_size.raw[0], map_size.raw[1]);
    vec2_t bot_left  = (vec2_t){ -(map_dim/2),  (map_dim/2) };
    vec2_t top_right = (vec2_t){  (map_dim/2), -(map_dim/2) };
    Camera_TickFinishOrthographic((struct camera*)map_cam, bot_left, top_right);

    /* Next, create a new framebuffer and texture that we will render our chunk 
     * top-down view to. */
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);

    glGenTextures(1, &s_ctx.minimap_texture.id);
    glBindTexture(GL_TEXTURE_2D, s_ctx.minimap_texture.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, MINIMAP_RES, MINIMAP_RES, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s_ctx.minimap_texture.id, 0);

    GLenum draw_buffers[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, draw_buffers);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        goto fail_fb;

    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glViewport(0,0, MINIMAP_RES, MINIMAP_RES);

    /* Render the map top-down view to the texture. */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    for(int r = 0; r < chunk_z; r++) {
        for(int c = 0; c < chunk_x; c++) {
            R_GL_Draw(chunk_rprivates[r * chunk_x + c], chunk_model_mats + (r * chunk_x + c)); 
        }
    }

    s_ctx.minimap_texture.tunit = GL_TEXTURE0;
    R_Texture_AddExisting("__minimap__", s_ctx.minimap_texture.id);
    /* Re-bind the default framebuffer when we're done rendering */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);

    struct vertex map_verts[] = {
        (struct vertex) {
            .pos = (vec3_t) {0.0f, 0.0f, 0.0f}, 
            .uv =  (vec2_t) {0.0f, 0.0f},
        },
        (struct vertex) {
            .pos = (vec3_t) {0.0f, MINIMAP_SIZE, 0.0f}, 
            .uv =  (vec2_t) {0.0f, 1.0f},
        },
        (struct vertex) {
            .pos = (vec3_t) {MINIMAP_SIZE, MINIMAP_SIZE, 0.0f}, 
            .uv =  (vec2_t) {1.0f, 1.0f},
        },
        (struct vertex) {
            .pos = (vec3_t) {MINIMAP_SIZE, 0.0f, 0.0f}, 
            .uv =  (vec2_t) {1.0f, 0.0f},
        },
    };

    glGenVertexArrays(1, &s_ctx.minimap_mesh.VAO);
    glBindVertexArray(s_ctx.minimap_mesh.VAO);

    glGenBuffers(1, &s_ctx.minimap_mesh.VBO);
    glBindBuffer(GL_ARRAY_BUFFER, s_ctx.minimap_mesh.VBO);
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(map_verts) * sizeof(struct vertex), map_verts, GL_STATIC_DRAW);

    /* Attribute 0 - position */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void*)0);
    glEnableVertexAttribArray(0);

    /* Attribute 1 - texture coordinates */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, uv));
    glEnableVertexAttribArray(1);

    return true;

fail_fb:
    return false;
}

void R_GL_MinimapRender(vec2_t center_pos)
{
    mat4x4_t ortho;
    PFM_Mat4x4_MakeOrthographic(0.0f, CONFIG_RES_X, CONFIG_RES_Y, 0.0f, -1.0f, 1.0f, &ortho);
    R_GL_SetProj(&ortho);

    mat4x4_t identity;
    PFM_Mat4x4_Identity(&identity);
    vec3_t dummy_pos = (vec3_t){0.0f};
    R_GL_SetViewMatAndPos(&identity, &dummy_pos);

    float horiz_width = MINIMAP_SIZE / cos(M_PI/4.0f);

    mat4x4_t tilt, trans, model;
    PFM_Mat4x4_MakeRotZ(DEG_TO_RAD(-45.0f), &tilt);
    PFM_Mat4x4_MakeTrans(center_pos.x - horiz_width/2.0f, center_pos.y, 0.0f, &trans);
    PFM_Mat4x4_Mult4x4(&trans, &tilt, &model);

    /* We scale up the quad slightly and center it in the same position, then draw it behind 
     * the minimap to create the minimap border */
    float scale = ((float)(MINIMAP_SIZE + 2*MINIMAP_BORDER_WIDTH))/MINIMAP_SIZE;
    mat4x4_t border_trans, border_scale, border_model, tmp;
    PFM_Mat4x4_MakeScale(scale, scale, scale, &border_scale);
    PFM_Mat4x4_MakeTrans(center_pos.x - horiz_width/2.0f - MINIMAP_BORDER_WIDTH/cos(M_PI/4.0f), 
        center_pos.y, 0.0f, &border_trans);
    PFM_Mat4x4_Mult4x4(&tilt, &border_scale, &tmp);
    PFM_Mat4x4_Mult4x4(&border_trans, &tmp, &border_model);

    GLuint shader_prog;
    glBindVertexArray(s_ctx.minimap_mesh.VAO);

    /* First render a slightly larger colored quad as the border */
    glDisable(GL_DEPTH_TEST);

    shader_prog = R_Shader_GetProgForName("mesh.static.colored");
    glUseProgram(shader_prog);

    GLuint loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, border_model.raw);

    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform3fv(loc, 1, MINIMAP_BORDER_CLR.raw);

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    /* Now draw the minimap texture */
    shader_prog = R_Shader_GetProgForName("mesh.static.textured");
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model.raw);

    R_Texture_GL_Activate(&s_ctx.minimap_texture, shader_prog);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    glEnable(GL_DEPTH_TEST);
}

void R_GL_MinimapFree(void)
{
    assert(s_ctx.minimap_texture.id > 0);
    assert(s_ctx.minimap_mesh.VBO > 0);
    assert(s_ctx.minimap_mesh.VAO > 0);

    R_Texture_Free("__minimap__");
    glDeleteBuffers(1, &s_ctx.minimap_mesh.VAO);
    glDeleteBuffers(1, &s_ctx.minimap_mesh.VBO);
    memset(&s_ctx, 0, sizeof(s_ctx));
}

