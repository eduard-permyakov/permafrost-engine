/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2023 Eduard Permyakov 
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
#include "gl_texture.h"
#include "gl_assert.h"
#include "gl_state.h"
#include "gl_shader.h"
#include "gl_render.h"
#include "gl_perf.h"
#include "../main.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/stb_image.h"
#include "../lib/public/mem.h"

#include <assert.h>

#include <GL/glew.h>

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct render_ui_ctx{
    GLuint font_tex;
    GLuint VBO;
    GLuint EBO;
    GLuint VAO;
}s_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void exec_draw_commands(const struct nk_draw_list *dl, GLuint shader_prog)
{
    GL_PERF_ENTER();

    int w, h;
    Engine_WinDrawableSize(&w, &h);

    struct nk_vec2i curr_vres = (struct nk_vec2i){w, h};
    const struct nk_draw_command *cmd;
    const nk_draw_index *offset = NULL;

    mat4x4_t ortho;
    PFM_Mat4x4_MakeOrthographic(0.0f, curr_vres.x, curr_vres.y, 0.0f, -1.0f, 1.0f, &ortho);

    R_GL_StateSet(GL_U_PROJECTION, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = ortho
    });
    R_GL_StateInstall(GL_U_PROJECTION, R_GL_Shader_GetCurrActive());

    for(cmd = nk__draw_list_begin(dl, dl->buffer); cmd; 
        cmd = nk__draw_list_next(cmd, dl->buffer, dl)) {

        if(cmd->userdata.ptr) {
        
            struct nk_command_userdata *ud = cmd->userdata.ptr;
            switch(ud->type) {

            case NK_COMMAND_SET_VRES: {

                curr_vres = ud->vec2i;

                PFM_Mat4x4_MakeOrthographic(0.0f, ud->vec2i.x, ud->vec2i.y, 0.0f, -1.0f, 1.0f, &ortho);
                R_GL_StateSet(GL_U_PROJECTION, (struct uval){
                    .type = UTYPE_MAT4,
                    .val.as_mat4 = ortho
                });
                R_GL_StateInstall(GL_U_PROJECTION, R_GL_Shader_GetCurrActive());

                PF_FREE(ud);
                continue;
            }
            case NK_COMMAND_IMAGE_TEXPATH: {

                stbi_set_flip_vertically_on_load(false);
                R_GL_Texture_GetOrLoad(g_basepath, ud->texpath, (GLuint*)&cmd->texture.id);
                stbi_set_flip_vertically_on_load(true);
                break;
            }
            default: assert(0);
            }

            PF_FREE(ud);
        }

        if(!cmd->elem_count) 
            continue;

        struct texture tex = (struct texture){cmd->texture.id, GL_TEXTURE0};
        R_GL_Texture_Bind(&tex, shader_prog);

        glScissor((GLint)(cmd->clip_rect.x / (float)curr_vres.x * w),
            h - (GLint)((cmd->clip_rect.y + cmd->clip_rect.h) / (float)curr_vres.y * h),
            (GLint)(cmd->clip_rect.w / (float)curr_vres.x * w),
            (GLint)(cmd->clip_rect.h / (float)curr_vres.y * h));
        glDrawElements(GL_TRIANGLES, (GLsizei)cmd->elem_count, GL_UNSIGNED_INT, offset);

        offset += cmd->elem_count;
    }

    GL_PERF_RETURN_VOID();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

int R_UI_GetFontTexID(void)
{
    assert(s_ctx.font_tex > 0);
    return s_ctx.font_tex;
}

void R_GL_UI_Init(void)
{
    ASSERT_IN_RENDER_THREAD();

    /* buffer setup */
    GLsizei vs = sizeof(struct ui_vert);
    size_t vp = offsetof(struct ui_vert, screen_pos);
    size_t vt = offsetof(struct ui_vert, uv);
    size_t vc = offsetof(struct ui_vert, color);

    glGenBuffers(1, &s_ctx.VBO);
    glGenBuffers(1, &s_ctx.EBO);
    glGenVertexArrays(1, &s_ctx.VAO);

    glBindVertexArray(s_ctx.VAO);
    glBindBuffer(GL_ARRAY_BUFFER, s_ctx.VBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ctx.EBO);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, vs, (void*)vp);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, vs, (void*)vt);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, vs, (void*)vc);

    /* unbind context */
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    GL_ASSERT_OK();
}

void R_GL_UI_Shutdown(void)
{
    ASSERT_IN_RENDER_THREAD();

    if(s_ctx.font_tex) {
        glDeleteTextures(1, &s_ctx.font_tex);
    }
    glDeleteBuffers(1, &s_ctx.VBO);
    glDeleteBuffers(1, &s_ctx.EBO);
    glDeleteVertexArrays(1, &s_ctx.VAO);

    GL_ASSERT_OK();
}

void R_GL_UI_Render(const struct nk_draw_list *dl)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    /* setup global state */
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);

    int w, h, x = 0, y = 0;
    Engine_WinDrawableSize(&w, &h);
    R_GL_SetViewport(&x, &y, &w, &h);

    /* setup program */
    GLuint shader_prog = R_GL_Shader_GetProgForName("ui");
    assert(shader_prog);
    R_GL_Shader_InstallProg(shader_prog);

    /* setup buffers */
    glBindVertexArray(s_ctx.VAO);
    glBindBuffer(GL_ARRAY_BUFFER, s_ctx.VBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ctx.EBO);

    glBufferData(GL_ARRAY_BUFFER, dl->vertices->memory.size, dl->vertices->memory.ptr, GL_STREAM_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, dl->elements->memory.size, dl->elements->memory.ptr, GL_STREAM_DRAW);

    /* iterate over and execute each draw command */
    exec_draw_commands(dl, shader_prog);

    /* cleanup state */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_UI_UploadFontAtlas(void *image, const int *w, const int *h)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    glGenTextures(1, &s_ctx.font_tex);
    glBindTexture(GL_TEXTURE_2D, s_ctx.font_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)*w, (GLsizei)*h, 0,
                GL_RGBA, GL_UNSIGNED_BYTE, image);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

