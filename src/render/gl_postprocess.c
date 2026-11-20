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
#define MEM_FILE_SUB MEM_SUB_RENDER_GL_POSTPROCESS

#include "gl_postprocess.h"
#include "gl_assert.h"

#define GPU_MEM_FILE_SYS GPU_MEM_SYS_GL_POSTPROCESS
#include "gl_mem.h"
#include "gl_perf.h"
#include "gl_shader.h"
#include "gl_state.h"
#include "render_private.h"
#include "public/render.h"
#include "../main.h"
#include "../mem.h"
#include "../camera.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/stb_image.h"

#include <assert.h>
#include <string.h>
#include <math.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_POSTPROCESS)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_POSTPROCESS)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_POSTPROCESS)

#define LUT_DIM         (64)
#define DITHER_DIM      (4)
#define WORLD_TUNIT     (GL_TEXTURE0)
#define GRADE_TUNIT     (GL_TEXTURE1)
#define PALETTE_TUNIT   (GL_TEXTURE2)
#define MAX(a, b)       ((a) > (b) ? (a) : (b))

struct postprocess_ctx{
    GLuint empty_vao;
    GLuint fbo;
    GLuint color_tex;
    GLuint depth_stencil_tex;
    /* Where the antialiased world goes on its way to being graded */
    GLuint aa_fbo;
    GLuint aa_tex;
    int    width, height;
    GLuint grade_lut;
    GLuint palette_lut;
    /* The directory the LUTs came from, or the one that last failed to load */
    char   look[256];
    bool   look_loaded;
    /* Whether the world being drawn is to be graded to the look */
    bool   graded;
    /* The framebuffer the world was redirected from, with its viewport */
    GLint  dst_fbo;
    GLint  dst_viewport[4];
};

/*****************************************************************************/
/* GLOBAL VARIABLES                                                          */
/*****************************************************************************/

float g_world_scale = 1.0f;

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct postprocess_ctx s_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/* The strip is LUT_DIM slices side by side, x = blue * LUT_DIM + red, y = green */
static GLuint lut_load(const char *dir, const char *name, GLint filter)
{
    char path[512];
    pf_snprintf(path, sizeof(path), "%s/%s/%s", g_basepath, dir, name);

    int width, height, nchannels;
    unsigned char *strip = stbi_load(path, &width, &height, &nchannels, 3);
    if(!strip)
        return 0;

    GLuint ret = 0;
    unsigned char *cube = NULL;
    if(width != LUT_DIM * LUT_DIM || height != LUT_DIM)
        goto out;

    cube = PF_MALLOC(LUT_DIM * LUT_DIM * LUT_DIM * 3);
    if(!cube)
        goto out;

    /* The engine has images loaded bottom row first */
    for(int b = 0; b < LUT_DIM; b++) {
    for(int g = 0; g < LUT_DIM; g++) {
        size_t row = LUT_DIM - 1 - g;
        memcpy(cube + ((size_t)b * LUT_DIM + g) * LUT_DIM * 3,
               strip + (row * width + b * LUT_DIM) * 3, LUT_DIM * 3);
    }}

    glGenTextures(1, &ret);
    glBindTexture(GL_TEXTURE_3D, ret);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB8, LUT_DIM, LUT_DIM, LUT_DIM, 0, 
        GL_RGB, GL_UNSIGNED_BYTE, cube);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_3D, 0);

out:
    PF_FREE(cube);
    stbi_image_free(strip);
    return ret;
}

static void look_free(void)
{
    if(s_ctx.grade_lut)
        glDeleteTextures(1, &s_ctx.grade_lut);
    if(s_ctx.palette_lut)
        glDeleteTextures(1, &s_ctx.palette_lut);
    s_ctx.grade_lut = 0;
    s_ctx.palette_lut = 0;
    s_ctx.look_loaded = false;
}

static bool look_ensure(const char *look)
{
    if(0 == strcmp(look, s_ctx.look))
        return s_ctx.look_loaded;

    look_free();
    pf_snprintf(s_ctx.look, sizeof(s_ctx.look), "%s", look);

    s_ctx.grade_lut = lut_load(look, "grade.png", GL_LINEAR);
    s_ctx.palette_lut = lut_load(look, "palette.png", GL_NEAREST);
    if(!s_ctx.grade_lut || !s_ctx.palette_lut) {
        look_free();
        return false;
    }
    s_ctx.look_loaded = true;
    return true;
}

static void target_free(void)
{
    if(s_ctx.fbo)
        glDeleteFramebuffers(1, &s_ctx.fbo);
    if(s_ctx.aa_fbo)
        glDeleteFramebuffers(1, &s_ctx.aa_fbo);
    if(s_ctx.color_tex)
        glDeleteTextures(1, &s_ctx.color_tex);
    if(s_ctx.aa_tex)
        glDeleteTextures(1, &s_ctx.aa_tex);
    if(s_ctx.depth_stencil_tex)
        glDeleteTextures(1, &s_ctx.depth_stencil_tex);
    s_ctx.fbo = 0;
    s_ctx.aa_fbo = 0;
    s_ctx.color_tex = 0;
    s_ctx.aa_tex = 0;
    s_ctx.depth_stencil_tex = 0;
    s_ctx.width = 0;
    s_ctx.height = 0;
}

/* Filtered, for the antialiasing pass. The grading pass fetches whole texels. */
static GLuint color_tex_make(int width, int height)
{
    GLuint ret;
    glGenTextures(1, &ret);
    glBindTexture(GL_TEXTURE_2D, ret);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return ret;
}

static void target_ensure(int width, int height)
{
    if(s_ctx.fbo && width == s_ctx.width && height == s_ctx.height)
        return;
    target_free();

    s_ctx.color_tex = color_tex_make(width, height);
    s_ctx.aa_tex = color_tex_make(width, height);

    glGenTextures(1, &s_ctx.depth_stencil_tex);
    glBindTexture(GL_TEXTURE_2D, s_ctx.depth_stencil_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0,
        GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &s_ctx.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_ctx.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 
        s_ctx.color_tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
        s_ctx.depth_stencil_tex, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glGenFramebuffers(1, &s_ctx.aa_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_ctx.aa_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 
        s_ctx.aa_tex, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    s_ctx.width = width;
    s_ctx.height = height;
    GL_ASSERT_OK();
}

static void fullscreen_draw(void)
{
    if(!s_ctx.empty_vao)
        glGenVertexArrays(1, &s_ctx.empty_vao);
    glBindVertexArray(s_ctx.empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

/* Draws 'tex' antialiased over the whole of the bound framebuffer */
static void antialias_draw(GLuint tex, int width, int height)
{
    glActiveTexture(WORLD_TUNIT);
    glBindTexture(GL_TEXTURE_2D, tex);

    R_GL_StateSet(GL_U_POST_WORLD_TEX, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = WORLD_TUNIT - GL_TEXTURE0
    });
    R_GL_StateSet(GL_U_POST_OUT_SIZE, (struct uval){
        .type = UTYPE_VEC2,
        .val.as_vec2 = (vec2_t){width, height}
    });
    R_GL_Shader_Install("fxaa");
    fullscreen_draw();
}

/* How far, in pixels of the reduced target, the dither pattern is moved to
 * ride with the ground as the camera pans. Only an orthographic view moves 
 * the ground by the same amount everywhere, so any other gets no offset.
 */
static void dither_offset(const struct camera *cam, int out[2])
{
    out[0] = 0;
    out[1] = 0;
    if(Camera_GetProjection(cam) != CAM_PROJ_ORTHOGRAPHIC)
        return;

    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view);
    Camera_MakeProjMat(cam, &proj);

    vec4_t origin = (vec4_t){0.0f, 0.0f, 0.0f, 1.0f}, tmp, clip;
    PFM_Mat4x4_Mult4x1(&view, &origin, &tmp);
    PFM_Mat4x4_Mult4x1(&proj, &tmp, &clip);

    float px = (clip.x / clip.w * 0.5f + 0.5f) * s_ctx.width;
    float py = (clip.y / clip.w * 0.5f + 0.5f) * s_ctx.height;
    long ox = lroundf(px), oy = lroundf(py);

    out[0] = (int)(((-ox % DITHER_DIM) + DITHER_DIM) % DITHER_DIM);
    out[1] = (int)(((-oy % DITHER_DIM) + DITHER_DIM) % DITHER_DIM);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_PostProcess_Shutdown(void)
{
    ASSERT_IN_RENDER_THREAD();
    look_free();
    target_free();
    if(s_ctx.empty_vao)
        glDeleteVertexArrays(1, &s_ctx.empty_vao);
    s_ctx = (struct postprocess_ctx){0};
}

bool R_GL_PostProcess_BeginWorld(const char *look, float scale, bool antialias)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();
    assert(scale >= 1.0f);

    s_ctx.graded = look && look_ensure(look);
    if(!s_ctx.graded) {
        if(!antialias)
            GL_PERF_RETURN(false);
        scale = 1.0f;
    }

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s_ctx.dst_fbo);
    glGetIntegerv(GL_VIEWPORT, s_ctx.dst_viewport);

    int width = MAX(lroundf(s_ctx.dst_viewport[2] / scale), 1);
    int height = MAX(lroundf(s_ctx.dst_viewport[3] / scale), 1);
    target_ensure(width, height);

    glBindFramebuffer(GL_FRAMEBUFFER, s_ctx.fbo);
    glViewport(0, 0, width, height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    g_world_scale = (float)s_ctx.dst_viewport[2] / width;

    GL_ASSERT_OK();
    GL_PERF_RETURN(true);
}

bool R_GL_PostProcess_Graded(void)
{
    return s_ctx.graded;
}

void R_GL_PostProcess_Resolve(const struct camera *cam, float dither, bool antialias)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();
    GL_PERF_PUSH_GROUP(0, "postprocess");

    const GLint *vp = s_ctx.dst_viewport;
    GLuint world_tex = s_ctx.color_tex;
    g_world_scale = 1.0f;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    /* Edges are smoothed ahead of the grade, the way a picture drawn large 
     * and scaled down has them smooth before its colours are reduced.
     */
    if(antialias && s_ctx.graded) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_ctx.aa_fbo);
        antialias_draw(s_ctx.color_tex, s_ctx.width, s_ctx.height);
        world_tex = s_ctx.aa_tex;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, s_ctx.dst_fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);

    if(!s_ctx.graded) {
        antialias_draw(s_ctx.color_tex, vp[2], vp[3]);
    }else{
        glActiveTexture(WORLD_TUNIT);
        glBindTexture(GL_TEXTURE_2D, world_tex);
        glActiveTexture(GRADE_TUNIT);
        glBindTexture(GL_TEXTURE_3D, s_ctx.grade_lut);
        glActiveTexture(PALETTE_TUNIT);
        glBindTexture(GL_TEXTURE_3D, s_ctx.palette_lut);

        int offset[2];
        dither_offset(cam, offset);

        R_GL_StateSet(GL_U_POST_WORLD_TEX, (struct uval){
            .type = UTYPE_INT,
            .val.as_int = WORLD_TUNIT - GL_TEXTURE0
        });
        R_GL_StateSet(GL_U_POST_GRADE_LUT, (struct uval){
            .type = UTYPE_INT,
            .val.as_int = GRADE_TUNIT - GL_TEXTURE0
        });
        R_GL_StateSet(GL_U_POST_PALETTE_LUT, (struct uval){
            .type = UTYPE_INT,
            .val.as_int = PALETTE_TUNIT - GL_TEXTURE0
        });
        R_GL_StateSet(GL_U_POST_OUT_SIZE, (struct uval){
            .type = UTYPE_VEC2,
            .val.as_vec2 = (vec2_t){vp[2], vp[3]}
        });
        R_GL_StateSet(GL_U_POST_DITHER_OFFSET, (struct uval){
            .type = UTYPE_IVEC2,
            .val.as_ivec2[0] = offset[0],
            .val.as_ivec2[1] = offset[1]
        });
        R_GL_StateSet(GL_U_POST_DITHER, (struct uval){
            .type = UTYPE_FLOAT,
            .val.as_float = dither
        });
        R_GL_Shader_Install("postprocess");
        fullscreen_draw();

        glActiveTexture(GRADE_TUNIT);
        glBindTexture(GL_TEXTURE_3D, 0);
        glActiveTexture(PALETTE_TUNIT);
        glBindTexture(GL_TEXTURE_3D, 0);
    }
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);

    GL_PERF_POP_GROUP();
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_PostProcess_CopyDepth(void)
{
    ASSERT_IN_RENDER_THREAD();
    const GLint *vp = s_ctx.dst_viewport;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_ctx.fbo);
    glBlitFramebuffer(0, 0, s_ctx.width, s_ctx.height, 
        vp[0], vp[1], vp[0] + vp[2], vp[1] + vp[3], GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    GL_ASSERT_OK();
}

void R_GL_PostProcess_GradeBind(GLenum tunit, bool on)
{
    ASSERT_IN_RENDER_THREAD();
    on = on && s_ctx.graded;

    glActiveTexture(tunit);
    glBindTexture(GL_TEXTURE_3D, on ? s_ctx.grade_lut : 0);
    glActiveTexture(GL_TEXTURE0);

    R_GL_StateSet(GL_U_POST_GRADE_LUT, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = tunit - GL_TEXTURE0
    });
    R_GL_StateSet(GL_U_POST_GRADE_ON, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = on
    });
}
