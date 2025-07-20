/* 
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2025 Eduard Permyakov 
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

#include "gl_swapchain.h"
#include "gl_assert.h"
#include "gl_perf.h"
#include "gl_texture.h"
#include "gl_shader.h"
#include "public/render.h"
#include "../main.h"
#include "../lib/public/mem.h"

#include <assert.h>

#define FRAMES_IN_FLIGHT (2)
#define TIMEOUT_NS       (1*1000*1000*1000)
#define ARR_SIZE(a)      (sizeof(a)/sizeof(a[0]))

struct framebuffer{
    GLuint fbo;
    GLuint width, height;
    GLuint texture_color_buffer;
    GLuint depth_stencil_rbo;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static int                s_resx;
static int                s_resy;

static int                s_front_idx = 0;
static struct framebuffer s_images[FRAMES_IN_FLIGHT];
static GLsync             s_done_fences[FRAMES_IN_FLIGHT];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void framebuffer_init(struct framebuffer *fb, int width, int height)
{
    glGenFramebuffers(1, &fb->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);

    glGenTextures(1, &fb->texture_color_buffer);
    glBindTexture(GL_TEXTURE_2D, fb->texture_color_buffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb->texture_color_buffer, 0);

    glGenRenderbuffers(1, &fb->depth_stencil_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, fb->depth_stencil_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fb->depth_stencil_rbo);

    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    fb->width = width;
    fb->height = height;

    GL_ASSERT_OK();
}

static void framebuffer_destroy(struct framebuffer *fb)
{
    glDeleteFramebuffers(1, &fb->fbo);
    glDeleteTextures(1, &fb->texture_color_buffer);
    glDeleteRenderbuffers(1, &fb->depth_stencil_rbo);

    fb->fbo = 0;
    fb->texture_color_buffer = 0;
    fb->depth_stencil_rbo = 0;
}

static void framebuffer_bind(struct framebuffer *fb)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    glViewport(0, 0, fb->width, fb->height);
    GL_ASSERT_OK();
}

static void framebuffer_complete(struct framebuffer *fb)
{
    s_done_fences[s_front_idx] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void framebuffer_resize_maybe(struct framebuffer *fb)
{
    GLint width, height;
    glBindTexture(GL_TEXTURE_2D, fb->texture_color_buffer);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);

    if((width == s_resx) && (height == s_resy))
        return;

    framebuffer_destroy(fb);
    framebuffer_init(fb, s_resx, s_resy);
}

static void framebuffer_blit_to_screen(struct framebuffer *fb)
{
    GL_PERF_ENTER();
    GL_GPU_PERF_PUSH("blit"); 

    /* Blit by drawing a full-screen quad to the default framebuffer.
     * This is the fastest way.
     */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    R_GL_SetScreenspaceDrawMode();

    int width, height;
    Engine_WinDrawableSize(&width, &height);
    struct ui_vert vbuff[] = {
        { .screen_pos = { 0, 0 },           .uv = { 0.0f, 1.0f } },
        { .screen_pos = { width, 0 },       .uv = { 1.0f, 1.0f } },
        { .screen_pos = { width, height },  .uv = { 1.0f, 0.0f } },
        { .screen_pos = { 0, height },      .uv = { 0.0f, 0.0f } }
    };
    for(int i = 0; i < ARR_SIZE(vbuff); i++) {
        vbuff[i].color[0] = 0xff;
        vbuff[i].color[1] = 0xff;
        vbuff[i].color[2] = 0xff;
        vbuff[i].color[3] = 0xff;
    }

    /* OpenGL setup */
    GLuint VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(struct ui_vert), (void*)0);
    glEnableVertexAttribArray(0);  

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct ui_vert), 
        (void*)offsetof(struct ui_vert, uv));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(struct ui_vert), 
        (void*)offsetof(struct ui_vert, color));
    glEnableVertexAttribArray(2);

    /* set state */
    GLuint prog = R_GL_Shader_GetProgForName("ui");
    R_GL_Shader_InstallProg(prog);

    struct texture tex = (struct texture){
        .id = fb->texture_color_buffer,
        .tunit = GL_TEXTURE0
    };
    R_GL_Texture_Bind(&tex, prog);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, sizeof(vbuff), vbuff, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_FAN, 0, ARR_SIZE(vbuff));

    /* cleanup */
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    GL_ASSERT_OK();
    GL_GPU_PERF_POP();
    GL_PERF_RETURN_VOID();
}

static void framebuffer_dump_color_ppm(struct framebuffer *fb, const char *path)
{
    GLint width, height, iformat;
    glBindTexture(GL_TEXTURE_2D, fb->texture_color_buffer);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &iformat);

    unsigned char *data = malloc(width * height * 3);
    if(!data)
        return;

    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
    R_GL_Texture_WritePPM(path, data, width, height);

    glBindTexture(GL_TEXTURE_2D, 0);
    PF_FREE(data);
    GL_ASSERT_OK();
}

static void wait_frame_done(int i)
{
    if(!s_done_fences[i])
        return;

    GL_GPU_PERF_PUSH("wait for renderbuffer");
    GLenum result;
    GLenum flags = GL_SYNC_FLUSH_COMMANDS_BIT;
    do{
        result = glClientWaitSync(s_done_fences[i], flags, TIMEOUT_NS);
        flags = 0;
    }while((result != GL_ALREADY_SIGNALED) && (result != GL_CONDITION_SATISFIED));
    GL_GPU_PERF_POP();
}

static void destroy_fence(int i)
{
    if(!s_done_fences[i])
        return;

    glDeleteSync(s_done_fences[i]);
    s_done_fences[i] = 0;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_SwapchainInit(void)
{
    Engine_WinDrawableSize(&s_resx, &s_resy);
    for(int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        framebuffer_init(&s_images[i], s_resx, s_resy);
    }
    GL_ASSERT_OK();
    return true;
}

void R_GL_SwapchainShutdown(void)
{
    for(int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        wait_frame_done(i);
        destroy_fence(i);
        framebuffer_destroy(&s_images[i]);
    }
}

void R_GL_SwapchainSetRes(int *x, int *y)
{
    s_resx = *x;
    s_resy = *y;
}

void R_GL_SwapchainAcquireNext(void)
{
    /* If we are presenting the frames, this should have 
     * already been waited on during presentation, and we
     * should return immediately.
     */
    wait_frame_done(s_front_idx);
    framebuffer_resize_maybe(&s_images[s_front_idx]);
    framebuffer_bind(&s_images[s_front_idx]);
}

void R_GL_SwapchainPresentLast(void)
{
    int last_idx = s_front_idx;
    wait_frame_done(last_idx);
    destroy_fence(last_idx);
    framebuffer_blit_to_screen(&s_images[last_idx]);
}

void R_GL_SwapchainFinishCommands(void)
{
    framebuffer_complete(&s_images[s_front_idx]);
    s_front_idx = (s_front_idx + 1) % FRAMES_IN_FLIGHT;
}

