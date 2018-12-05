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

#include "render_gl_shadows.h"
#include "public/render.h"
#include "render_gl.h"
#include "render_private.h"
#include "gl_uniforms.h"
#include "gl_assert.h"
#include "../pf_math.h"
#include "../config.h"
#include "../game/public/game.h"

#include <GL/glew.h>

#include <assert.h>


/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static GLuint s_depth_map_FBO;
static GLuint s_depth_map_tex;
static bool   s_depth_pass_active = false;

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_InitShadows(void)
{
    glGenFramebuffers(1, &s_depth_map_FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, s_depth_map_FBO);

    glGenTextures(1, &s_depth_map_tex);
    glBindTexture(GL_TEXTURE_2D, s_depth_map_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32, 
                 CONFIG_RES_X, CONFIG_RES_Y, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, s_depth_map_tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);  
    GL_ASSERT_OK();
}

void R_GL_DepthPassBegin(void)
{
    assert(!s_depth_pass_active);
    s_depth_pass_active = true;

    /* The parameters for the orthographic matrix are chosen such that the orthographic 
     * frustum completely encapsulates the part of the scene seen by the RTS camera. 
     * Note that when in first-person view, this means that some objects will not cast shadows
     * as they are outside the light's view, especially if the camera is looking in the opposite
     * direction of the light. This is a sacrifice we accept, as making a shadow map of the entire
     * scene is expensive and will yield a lower-precision shadow map. */
    mat4x4_t light_proj;
    PFM_Mat4x4_MakeOrthographic(-160.0f, 160.0f, 160.0f, -160.0f, -1.0f, 400.0f, &light_proj);

    mat4x4_t light_view;

    vec3_t cam_pos = G_ActiveCamPos();
    vec3_t light_pos = R_GL_GetLightPos();

    vec3_t origin = (vec3_t){0.0f, 0.0f, 0.0f};
    vec3_t right = (vec3_t){-1.0f, 0.0f, 0.0f};

    vec3_t light_dir = R_GL_GetLightPos();
    PFM_Vec3_Normal(&light_dir, &light_dir);
    PFM_Vec3_Scale(&light_dir, -1.0f, &light_dir);

    vec3_t up;
    PFM_Vec3_Cross(&light_dir, &right, &up);

    vec3_t target;
    PFM_Vec3_Add(&cam_pos, &light_dir, &target);

    /* Since, for shadow mapping, we treat our light source as a directional light, 
     * we only care about direction of the light rays, not the absolute position of 
     * the light source. Thus, we render the shadow map from the position of the camera,
     * just looking in the direction of the light source */
    PFM_Mat4x4_MakeLookAt(&cam_pos, &target, &up, &light_view);

    mat4x4_t light_space_trans;
    PFM_Mat4x4_Mult4x4(&light_proj, &light_view, &light_space_trans);
    R_GL_SetLightSpaceTrans(&light_space_trans);

    glBindFramebuffer(GL_FRAMEBUFFER, s_depth_map_FBO);
    glClear(GL_DEPTH_BUFFER_BIT);
    GL_ASSERT_OK();
}

void R_GL_DepthPassEnd(void)
{
    assert(s_depth_pass_active);
    s_depth_pass_active = false;

    R_GL_SetShadowMap(s_depth_map_tex);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);  

    GL_ASSERT_OK();
}

void R_GL_RenderDepthMap(const void *render_private, mat4x4_t *model)
{
    assert(s_depth_pass_active);
    GL_ASSERT_OK();

    const struct render_private *priv = render_private;
    GLuint loc;

    glUseProgram(priv->shader_prog_dp);

    loc = glGetUniformLocation(priv->shader_prog_dp, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model->raw);

    glBindVertexArray(priv->mesh.VAO);
    glDrawArrays(GL_TRIANGLES, 0, priv->mesh.num_verts);

    GL_ASSERT_OK();
}

