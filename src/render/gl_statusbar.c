/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2023 Eduard Permyakov 
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

#include "gl_vertex.h"
#include "gl_shader.h"
#include "gl_state.h"
#include "gl_assert.h"
#include "gl_perf.h"
#include "../camera.h"
#include "../pf_math.h"
#include "../config.h"
#include "../main.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/mem.h"

#include <GL/glew.h>
#include <assert.h>


#define ARR_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#define MAX_HBS     (256)

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_DrawHealthbars(const size_t *num_ents, GLfloat *ent_health_pc, 
                         vec3_t *ent_top_pos_ws, const struct camera *cam)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    int width, height;
    Engine_WinDrawableSize(&width, &height);

    /* Convert the worldspace positions to SDL screenspace positions */
    STALLOC(vec2_t, ent_top_pos_ss, *num_ents); /* Screen-space XY positions of the entity tops. */

    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view); 
    Camera_MakeProjMat(cam, &proj);

    for(int i = 0; i < *num_ents; i++) {
    
        vec4_t ent_top_homo = (vec4_t){ent_top_pos_ws[i].x, ent_top_pos_ws[i].y, ent_top_pos_ws[i].z, 1.0f};

        vec4_t clip, tmp;
        PFM_Mat4x4_Mult4x1(&view, &ent_top_homo, &tmp);
        PFM_Mat4x4_Mult4x1(&proj, &tmp, &clip);
        vec3_t ndc = (vec3_t){clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};

        float screen_x = (ndc.x + 1.0f) * width/2.0f;
        float screen_y = height - ((ndc.y + 1.0f) * height/2.0f);

        ent_top_pos_ss[i] = (vec2_t){screen_x, screen_y};
    }

    /* Create a buffer of mesh vertices for a healthbar centered at (0, 0).
     * Set uv attribute for each vertex - used in fragment shader to determine relative 
     * texel position within the quad. 
     */
    const struct textured_vert corners[] = {
        (struct textured_vert) {
            .pos = (vec3_t) {-1.0f, -1.0f, 0.0f}, 
            .uv =  (vec2_t) {0.0f, 0.0f},
        },
        (struct textured_vert) {
            .pos = (vec3_t) {-1.0f, 1.0f, 0.0f}, 
            .uv =  (vec2_t) {0.0f, 1.0f},
        },
        (struct textured_vert) {
            .pos = (vec3_t) {1.0f, 1.0f, 0.0f}, 
            .uv =  (vec2_t) {1.0f, 1.0f},
        },
        (struct textured_vert) {
            .pos = (vec3_t) {1.0f, -1.0f, 0.0f}, 
            .uv =  (vec2_t) {1.0f, 0.0f},
        },
    };

    const struct textured_vert vbuff[] = {
        corners[0], corners[1], corners[2],
        corners[2], corners[3], corners[0],
    };

    /* OpenGL setup */
    GLuint VAO, VBO;

    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(vbuff) * sizeof(struct textured_vert), vbuff, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct textured_vert), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct textured_vert), 
        (void*)offsetof(struct textured_vert, uv));
    glEnableVertexAttribArray(1);

    /* set uniforms */
    int w, h;
    Engine_WinDrawableSize(&w, &h);

    R_GL_StateSet(GL_U_CURR_RES, (struct uval){
        .type = UTYPE_IVEC2,
        .val.as_ivec2[0] = w,
        .val.as_ivec2[1] = h
    });

    size_t ndraw = MIN(*num_ents, MAX_HBS);
    R_GL_StateSetArray(GL_U_ENT_TOP_OFFSETS_SS, UTYPE_VEC2, ndraw, ent_top_pos_ss);
    R_GL_StateSetArray(GL_U_ENT_HEALTH_PC, UTYPE_FLOAT, ndraw, ent_health_pc);

    R_GL_Shader_Install("statusbar");

    /* Draw instances */
    glDrawArraysInstanced(GL_TRIANGLES, 0, ARR_SIZE(vbuff), *num_ents);

    /* cleanup */
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    STFREE(ent_top_pos_ss);
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

