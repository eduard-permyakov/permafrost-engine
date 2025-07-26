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

#include "public/render.h"
#include "public/render_ctrl.h"
#include "gl_perf.h"
#include "gl_assert.h"
#include "gl_shader.h"
#include "gl_texture.h"
#include "gl_state.h"
#include "gl_vertex.h"
#include "gl_perf.h"
#include "../main.h"
#include "../camera.h"
#include "../lib/public/pf_string.h"

#define ARR_SIZE(a)    (sizeof(a)/sizeof((a)[0]))
#define MIN(a, b)      ((a) < (b) ? (a) : (b))
#define MAX_DRAW_CALLS (512)
#define MAX_SPRITES    (1024)

struct draw_call_desc{
    size_t begin_idx;
    size_t end_idx;
};

/* OpenGL std140 layout */
struct gpu_sprite_desc{
    vec3_t ws_pos;
    float  __pad0;
    vec2_t ws_size;
    int   frame_idx;
    float __pad1[1];
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void sort_by_sprite(struct sprite_desc *sprites, size_t nsprites,
                           struct draw_call_desc out[MAX_DRAW_CALLS], size_t *nout)
{
    if(nsprites == 0) {
        *nout = 0;
        return;
    }

    int i = 1;
    while(i < nsprites) {
        int j = i;
        const char *first = sprites[i].sheet.filename;
        const char *second = sprites[j].sheet.filename;
        while(j > 0 && strcmp(first, second)) {

            struct sprite_desc tmp = sprites[j - 1];
            sprites[j - 1] = sprites[j];
            sprites[j] = tmp;
            j--;
        }
        i++;
    }

    size_t count = 0;
    size_t begin_idx = 0;
    for(int i = 1; i < nsprites; i++) {
        if(0 != strcmp(sprites[i].sheet.filename, sprites[i - 1].sheet.filename)) {
            size_t end_idx = i - 1;
            out[count++] = (struct draw_call_desc){begin_idx, end_idx};
            begin_idx = i;
            if(count == MAX_DRAW_CALLS)
                break;
        }
    }
    out[count++] = (struct draw_call_desc){begin_idx, nsprites-1};
    *nout = count;
}

static void do_draw_call(struct draw_call_desc desc, struct sprite_sheet_desc sheet,
                         const struct sprite_desc *sprites)
{
    /* Get the texture for the filename */
    char path[512];
    pf_snprintf(path, sizeof(path), "assets/sprites/%s", sheet.filename);

    GLuint tex;
    R_GL_Texture_GetOrLoad(g_basepath, path, &tex);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    /* Set all  uniforms */
    R_GL_StateSet(GL_U_SPRITE_SHEET, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = 0
    });
    R_GL_StateSet(GL_U_SPRITE_NROWS, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = sheet.nrows
    });
    R_GL_StateSet(GL_U_SPRITE_NCOLS, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = sheet.ncols
    });

    /* Upload per-instance attributes to uniform buffer */
    struct gpu_sprite_desc descs[MAX_SPRITES];
    size_t nents = desc.end_idx - desc.begin_idx + 1;
    nents = MIN(nents, MAX_SPRITES);
    for(int i = 0; i < nents; i++) {
        descs[i] = (struct gpu_sprite_desc){
            .ws_pos = sprites[i].ws_pos,
            .ws_size = sprites[i].ws_size,
            .frame_idx = sprites[i].frame,
        };
    }

    GLuint UBO;
    glGenBuffers(1, &UBO);
    glBindBuffer(GL_UNIFORM_BUFFER, UBO);
    glBufferData(GL_UNIFORM_BUFFER, MAX_SPRITES * sizeof(struct gpu_sprite_desc), 0, GL_STREAM_DRAW);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, nents * sizeof(struct gpu_sprite_desc), descs);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, UBO);
    R_GL_StateSetBlockBinding(GL_U_SPRITES, 0);

    /* Setup OpenGL billboard geometry */
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

    GLuint VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(vbuff) * sizeof(struct textured_vert), 
        vbuff, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct textured_vert), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct textured_vert), 
        (void*)offsetof(struct textured_vert, uv));
    glEnableVertexAttribArray(1);

    /* Invoke sprite shader */
    R_GL_Shader_Install("sprite.batched");
    glDrawArraysInstanced(GL_TRIANGLES, 0, ARR_SIZE(vbuff), nents);

    /* Cleanup resources */
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    glDeleteBuffers(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glDeleteBuffers(1, &UBO);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_SpriteRenderBatch(struct sprite_desc *sprites, size_t *nsprites,
                            const struct camera *cam)
{
    GL_PERF_PUSH_GROUP(0, "sprite");
    /* First sort the sprites by distinct sprite sheet */
    size_t ndescs = 0;
    struct draw_call_desc descs[MAX_DRAW_CALLS];
    sort_by_sprite(sprites, *nsprites, descs, &ndescs);

    /* Set the camera uniforms */
    Camera_TickFinishPerspective((struct camera*)cam);
    vec3_t dir = Camera_GetDir(cam);
    R_GL_StateSet(GL_U_VIEW_DIR, (struct uval){
        .type = UTYPE_VEC3,
        .val.as_vec3 = dir
    });

    /* Then render each sheet using instancing */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    for(int i = 0; i < ndescs; i++) {
        struct draw_call_desc curr = descs[i];
        struct sprite_sheet_desc *sheet = &sprites[curr.begin_idx].sheet;
        do_draw_call(curr, *sheet, sprites);
    }
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    GL_PERF_POP_GROUP();
}

