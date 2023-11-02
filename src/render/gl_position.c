/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2022-2023 Eduard Permyakov 
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

#include "public/render.h"
#include "gl_perf.h"
#include "gl_assert.h"
#include "gl_shader.h"
#include "gl_texture.h"
#include "gl_state.h"
#include "../main.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"


#define PIXELS_PER_TILE (8)
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX_TEX_RES     (4096)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static GLuint s_posbuff_tex;

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_PositionsUploadData(vec3_t *posbuff, uint32_t *idbuff, 
                              const size_t *nents, const struct map *map)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    struct map_resolution res;
    M_GetResolution(map, &res);

    const size_t resx = MIN(res.chunk_w * res.tile_w * PIXELS_PER_TILE, MAX_TEX_RES);
    const size_t resy = MIN(res.chunk_h * res.tile_h * PIXELS_PER_TILE, MAX_TEX_RES);

    /* Create a framebuffer with a resolution based on the map size */
    GLuint fbo;

    glGenTextures(1, &s_posbuff_tex);
    glBindTexture(GL_TEXTURE_2D, s_posbuff_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, resx, resy);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s_posbuff_tex, 0);
    GLenum draw_buffers[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, draw_buffers);

    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    /* Upload the vertex attributes */
    GLuint VAO, pos_VBO, id_VBO;
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &pos_VBO);
    glBindBuffer(GL_ARRAY_BUFFER, pos_VBO);
    glBufferData(GL_ARRAY_BUFFER, *nents * sizeof(vec3_t), posbuff, GL_STREAM_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);

    glGenBuffers(1, &id_VBO);
    glBindBuffer(GL_ARRAY_BUFFER, id_VBO);
    glBufferData(GL_ARRAY_BUFFER, *nents * sizeof(uint32_t), idbuff, GL_STREAM_DRAW);

    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(uint32_t), (void*)0);
    glEnableVertexAttribArray(1);

    /* Render the position vertex from a bird's eye view to the texture. 
     * The entity's attributes will be encoded in the output texture and can be 
     * indexed using the enity's position. For example, an entity directly in
     * the center of the map will have its' attributes stored in the center texel
     * of the output texture. We can use this texture to efficiently query for the 
     * presence of an entity at a specific location (texel), or for efficiently 
     * getting all the entities in a region.
     */
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    glViewport(0, 0, resx, resy);

    R_GL_StateSet(GL_U_MAP_POS, (struct uval){
        .type = UTYPE_VEC2,
        .val.as_vec2 = (vec2_t){M_GetPos(map).x, M_GetPos(map).z}
    });
    R_GL_StateSet(GL_U_MAP_RES, (struct uval){
        .type = UTYPE_IVEC4,
        .val.as_ivec4[0] = res.chunk_w,
        .val.as_ivec4[1] = res.chunk_h,
        .val.as_ivec4[2] = res.tile_w,
        .val.as_ivec4[3] = res.tile_h,
    });

    R_GL_Shader_Install("posbuff");
    glPointSize(1.0f);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_POINTS, 0, *nents);

    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

    /* Clean up */
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);  

    glDeleteFramebuffers(1, &fbo);
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &pos_VBO);
    glDeleteBuffers(1, &id_VBO);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_PositionsGetTexture(GLuint *out_tex_id)
{
    *out_tex_id = s_posbuff_tex;
}

void R_GL_PositionsInvalidateData(void)
{
    glDeleteTextures(1, &s_posbuff_tex);
    s_posbuff_tex = 0;
}

