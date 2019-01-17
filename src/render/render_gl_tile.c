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

#include "render_gl.h"
#include "render_private.h"
#include "mesh.h"
#include "vertex.h"
#include "shader.h"
#include "material.h"
#include "gl_assert.h"
#include "gl_uniforms.h"
#include "public/render.h"
#include "../map/public/tile.h"
#include "../map/public/map.h"
#include "../collision.h"
#include "../camera.h"
#include "../config.h"

#include <GL/glew.h>

#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
  

#define ARR_SIZE(a)                 (sizeof(a)/sizeof(a[0]))

#define MAG(x, y)                   sqrt(pow(x,2) + pow(y,2))

#define VEC3_EQUAL(a, b)            (0 == memcmp((a).raw, (b).raw, sizeof((a).raw)))

#define INDICES_MASK_8(a, b)        (uint8_t)( (((a) & 0xf) << 4) | ((b) & 0xf) )

#define INDICES_MASK_32(a, b, c, d) (uint32_t)( (((a) & 0xff) << 24) | (((b) & 0xff) << 16) | (((c) & 0xff) << 8) | (((d) & 0xff) << 0) )

#define SAME_INDICES_32(i)          (  (( (i) >> 0) & 0xffff) == (( (i) >> 16) & 0xfffff) \
                                    && (( (i) >> 0) & 0xff  ) == (( (i) >> 8 ) & 0xff   ) \
                                    && (( (i) >> 0) & 0xf   ) == (( (i) >> 4 ) & 0xf    ) )

/* We take the directions to be relative to a normal vector facing outwards
 * from the plane of the face. West is to the right, east is to the left,
 * north is top, south is bottom. */
struct face{
    struct vertex nw, ne, se, sw; 
};

struct tile_adj_info{
    const struct tile *tile;
    uint8_t middle_mask, top_left_mask, top_right_mask, bot_left_mask, bot_right_mask;
    int top_center_idx, bot_center_idx, left_center_idx, right_center_idx;
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void r_gl_tile_top_normals(const struct tile *tile, vec3_t out_tri_normals[2], bool *out_tri_left)
{
    switch(tile->type) {
    case TILETYPE_FLAT: {
        out_tri_normals[0]  = (vec3_t) {0.0f, 1.0f, 0.0f};
        out_tri_normals[1]  = (vec3_t) {0.0f, 1.0f, 0.0f};

        *out_tri_left = true;
        break;
    }
    case TILETYPE_RAMP_SN: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, Z_COORDS_PER_TILE);

        out_tri_normals[0] = (vec3_t) {0.0f, sin(normal_angle), cos(normal_angle)};
        out_tri_normals[1] = (vec3_t) {0.0f, sin(normal_angle), cos(normal_angle)};

        *out_tri_left = true;
        break;
    }
    case TILETYPE_RAMP_NS: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, Z_COORDS_PER_TILE);
    
        out_tri_normals[0] = (vec3_t) {0.0f, sin(normal_angle), -cos(normal_angle)};
        out_tri_normals[1] = (vec3_t) {0.0f, sin(normal_angle), -cos(normal_angle)};

        *out_tri_left = true;
        break;
    }
    case TILETYPE_RAMP_EW: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, X_COORDS_PER_TILE);
    
        out_tri_normals[0] = (vec3_t) {-cos(normal_angle), sin(normal_angle), 0.0f};
        out_tri_normals[1] = (vec3_t) {-cos(normal_angle), sin(normal_angle), 0.0f};

        *out_tri_left = true;
        break;
    }
    case TILETYPE_RAMP_WE: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, X_COORDS_PER_TILE);
    
        out_tri_normals[0] = (vec3_t) {cos(normal_angle), sin(normal_angle), 0.0f};
        out_tri_normals[1] = (vec3_t) {cos(normal_angle), sin(normal_angle), 0.0f};

        *out_tri_left = true;
        break;
    }
    case TILETYPE_CORNER_CONCAVE_SW: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {0.0f, 1.0f, 0.0f};
        out_tri_normals[1] = (vec3_t) {cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                       cos(normal_angle) * sin(M_PI/4.0f)};

        *out_tri_left = false;
        break; 
    }
    case TILETYPE_CORNER_CONVEX_SW: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                       cos(normal_angle) * sin(M_PI/4.0f)};
        out_tri_normals[1] = (vec3_t) {0.0f, 1.0f, 0.0f};

        *out_tri_left = false;
        break; 
    }
    case TILETYPE_CORNER_CONCAVE_SE: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {0.0f, 1.0f, 0.0f};
        out_tri_normals[1] = (vec3_t) {-cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                        cos(normal_angle) * sin(M_PI/4.0f)};

        *out_tri_left = true;
        break; 
    }
    case TILETYPE_CORNER_CONVEX_SE: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {-cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                        cos(normal_angle) * sin(M_PI/4.0f)};
        out_tri_normals[1] = (vec3_t) {0.0f, 1.0f, 0.0f};

        *out_tri_left = true;
        break; 
    }
    case TILETYPE_CORNER_CONCAVE_NW: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) { cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                       -cos(normal_angle) * sin(M_PI/4.0f)};
        out_tri_normals[1] = (vec3_t) {0.0f, 1.0f, 0.0f};

        *out_tri_left = true;
        break; 
    }
    case TILETYPE_CORNER_CONVEX_NW: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {0.0f, 1.0f, 0.0f};
        out_tri_normals[1] = (vec3_t) { cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                       -cos(normal_angle) * sin(M_PI/4.0f)};

        *out_tri_left = true;
        break; 
    }
    case TILETYPE_CORNER_CONCAVE_NE: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {-cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                       -cos(normal_angle) * sin(M_PI/4.0f)};
        out_tri_normals[1] = (vec3_t) {0.0f, 1.0f, 0.0f};

        *out_tri_left = false;
        break; 
    }
    case TILETYPE_CORNER_CONVEX_NE: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, 
            MAG(X_COORDS_PER_TILE, Z_COORDS_PER_TILE)/2.0f );

        out_tri_normals[0] = (vec3_t) {0.0f, 1.0f, 0.0f};
        out_tri_normals[1] = (vec3_t) {-cos(normal_angle) * cos(M_PI/4.0f), sin(normal_angle), 
                                       -cos(normal_angle) * sin(M_PI/4.0f)};

        *out_tri_left = false;
        break; 
    }
    default: assert(0);
    }

    PFM_Vec3_Normal(out_tri_normals, out_tri_normals);
    PFM_Vec3_Normal(out_tri_normals + 1, out_tri_normals + 1);
}

static vec3_t r_gl_tile_middle_normal(const struct tile *tile)
{
    vec3_t ret;
    switch(tile->type) {
    case TILETYPE_FLAT: {
    case TILETYPE_CORNER_CONCAVE_SW:
    case TILETYPE_CORNER_CONVEX_SW:
    case TILETYPE_CORNER_CONCAVE_SE:
    case TILETYPE_CORNER_CONVEX_SE:
    case TILETYPE_CORNER_CONCAVE_NW:
    case TILETYPE_CORNER_CONVEX_NW:
    case TILETYPE_CORNER_CONCAVE_NE:
    case TILETYPE_CORNER_CONVEX_NE:
        ret = (vec3_t) {0.0f, 1.0f, 0.0f}; 
        break;
    }
    case TILETYPE_RAMP_SN: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, Z_COORDS_PER_TILE);
        ret = (vec3_t) {0.0f, sin(normal_angle), cos(normal_angle)};
        break;
    }
    case TILETYPE_RAMP_NS: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, Z_COORDS_PER_TILE);
        ret = (vec3_t) {0.0f, sin(normal_angle), -cos(normal_angle)};
        break;
    }
    case TILETYPE_RAMP_EW: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, X_COORDS_PER_TILE);
        ret = (vec3_t) {-cos(normal_angle), sin(normal_angle), 0.0f};
        break;
    }
    case TILETYPE_RAMP_WE: {
        float normal_angle = M_PI/2.0f - atan2(tile->ramp_height * Y_COORDS_PER_TILE, X_COORDS_PER_TILE);
        ret = (vec3_t) {cos(normal_angle), sin(normal_angle), 0.0f};
        break;
    }
    default: assert(0); 
    }

    PFM_Vec3_Normal(&ret, &ret);
    return ret;
}

static void r_gl_tile_mat_indices(struct tile_adj_info *inout, bool *out_top_tri_left_aligned)
{
    assert(inout->tile);

    vec3_t top_tri_normals[2];
    bool   top_tri_left_aligned;
    r_gl_tile_top_normals(inout->tile, top_tri_normals, out_top_tri_left_aligned);

    GLint tri_mats[2] = {
        fabs(top_tri_normals[0].y) < 1.0 && (inout->tile->ramp_height > 1) ? inout->tile->sides_mat_idx : inout->tile->top_mat_idx,
        fabs(top_tri_normals[1].y) < 1.0 && (inout->tile->ramp_height > 1) ? inout->tile->sides_mat_idx : inout->tile->top_mat_idx,
    };

    /*
     * CONFIG 1 (left-aligned)   CONFIG 2
     * (nw)      (ne)            (nw)      (ne)
     * +---------+               +---------+
     * |       / |               | \       |
     * |     /   |               |   \     |
     * |   /     |               |     \   |
     * | /       |               |       \ |
     * +---------+               +---------+
     * (sw)      (se)            (sw)      (se)
     */
    inout->middle_mask = INDICES_MASK_8(tri_mats[0], tri_mats[1]);
    inout->bot_center_idx = tri_mats[0];
    inout->top_center_idx = tri_mats[1];

    if(!(*out_top_tri_left_aligned)) {
        inout->top_left_mask     = INDICES_MASK_8(tri_mats[1], tri_mats[0]);
        inout->top_right_mask    = INDICES_MASK_8(tri_mats[1], tri_mats[1]);
        inout->bot_left_mask     = INDICES_MASK_8(tri_mats[0], tri_mats[0]);
        inout->bot_right_mask    = INDICES_MASK_8(tri_mats[0], tri_mats[1]);

        inout->left_center_idx  = tri_mats[0];
        inout->right_center_idx = tri_mats[1];
    }else {
        inout->top_left_mask     = INDICES_MASK_8(tri_mats[1], tri_mats[1]);
        inout->top_right_mask    = INDICES_MASK_8(tri_mats[0], tri_mats[1]);
        inout->bot_left_mask     = INDICES_MASK_8(tri_mats[1], tri_mats[0]);
        inout->bot_right_mask    = INDICES_MASK_8(tri_mats[0], tri_mats[0]);

        inout->left_center_idx  = tri_mats[1];
        inout->right_center_idx = tri_mats[0];
    }
}

enum blend_mode r_gl_blendmode_for_provoking_vert(const struct vertex *vert)
{
    if(SAME_INDICES_32(vert->adjacent_mat_indices[0])
    && SAME_INDICES_32(vert->adjacent_mat_indices[1])
    && vert->adjacent_mat_indices[0] == vert->adjacent_mat_indices[1]
    && (vert->adjacent_mat_indices[0] & 0xf) == vert->material_idx) {
    
        return BLEND_MODE_NOBLEND;
    }else{
        return BLEND_MODE_BLUR; 
    }
}

static bool arr_contains(int *array, size_t size, int elem)
{
    for(int i = 0; i < size; i++) {
        if(array[i] == elem) 
            return true;
    }
    return false;
}

static int arr_indexof(int *array, size_t size, int elem)
{
    for(int i = 0; i < size; i++) {
        if(array[i] == elem) 
            return i;
    }
    return -1;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_TileDrawSelected(const struct tile_desc *in, const void *chunk_rprivate, mat4x4_t *model, 
                           int tiles_per_chunk_x, int tiles_per_chunk_z)
{
    struct vertex vbuff[VERTS_PER_TILE];
    vec3_t red = (vec3_t){1.0f, 0.0f, 0.0f};
    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;

    const struct render_private *priv = chunk_rprivate;
    size_t offset = (in->tile_r * tiles_per_chunk_x + in->tile_c) * VERTS_PER_TILE * sizeof(struct vertex);
    size_t length = VERTS_PER_TILE * sizeof(struct vertex);

    const struct vertex *vert_base = glMapNamedBufferRange(priv->mesh.VBO, offset, length, GL_MAP_READ_BIT);
    assert(vert_base);
    memcpy(vbuff, vert_base, sizeof(vbuff));
    glUnmapNamedBuffer(priv->mesh.VBO);

    /* Additionally, scale the tile selection mesh slightly around its' center. This is so that 
     * it is slightly larger than the actual tile underneath and can be rendered on top of it. */
    const float SCALE_FACTOR = 1.025f;
    mat4x4_t final_model;
    mat4x4_t scale, trans, trans_inv, tmp1, tmp2;
    PFM_Mat4x4_MakeScale(SCALE_FACTOR, SCALE_FACTOR, SCALE_FACTOR, &scale);

    vec3_t center = (vec3_t){
        ( 0.0f - (in->tile_c* X_COORDS_PER_TILE) - X_COORDS_PER_TILE/2.0f ), 
        (-1.0f * Y_COORDS_PER_TILE + Y_COORDS_PER_TILE/2.0f), 
        ( 0.0f + (in->tile_r* Z_COORDS_PER_TILE) + Z_COORDS_PER_TILE/2.0f),
    };
    PFM_Mat4x4_MakeTrans(-center.x, -center.y, -center.z, &trans);
    PFM_Mat4x4_MakeTrans( center.x,  center.y,  center.z, &trans_inv);

    PFM_Mat4x4_Mult4x4(&scale, &trans, &tmp1);
    PFM_Mat4x4_Mult4x4(&trans_inv, &tmp1, &tmp2);
    PFM_Mat4x4_Mult4x4(model, &tmp2, &final_model);

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    /* Attribute 0 - position */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void*)0);
    glEnableVertexAttribArray(0);

    /* Attribute 1 - texture coordinates */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, uv));
    glEnableVertexAttribArray(1);

    /* Attribute 2 - normal */
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, normal));
    glEnableVertexAttribArray(2);

    shader_prog = R_Shader_GetProgForName("mesh.static.tile-outline");
    glUseProgram(shader_prog);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, final_model.raw);

    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform3fv(loc, 1, red.raw);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, sizeof(vbuff), vbuff, GL_STATIC_DRAW);

    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, VERTS_PER_TILE);

cleanup:
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

void R_GL_TilePatchVertsBlend(GLuint VBO, const struct tile *tiles, int width, int height, int r, int c)
{
    GL_ASSERT_OK();
    const struct tile *curr_tile  = &tiles[r * width + c];
    const struct tile *top_tile   = (r > 0)          ? &tiles[(r - 1) * width + c] : NULL;
    const struct tile *bot_tile   = (r < height - 1) ? &tiles[(r + 1) * width + c] : NULL;
    const struct tile *left_tile  = (c > 0)          ? &tiles[r * width + (c - 1)] : NULL;
    const struct tile *right_tile = (c < width - 1)  ? &tiles[r * width + (c + 1)] : NULL;

    const struct tile *top_right_tile = (top_tile && right_tile) ? &tiles[(r - 1) * width + (c + 1)] : NULL;
    const struct tile *bot_right_tile = (bot_tile && right_tile) ? &tiles[(r + 1) * width + (c + 1)] : NULL;
    const struct tile *top_left_tile = (top_tile && left_tile)   ? &tiles[(r - 1) * width + (c - 1)] : NULL;
    const struct tile *bot_left_tile = (bot_tile && left_tile)   ? &tiles[(r + 1) * width + (c - 1)] : NULL;

    struct tile_adj_info curr = {.tile = curr_tile};
    bool top_tri_left_aligned;
    r_gl_tile_mat_indices(&curr, &top_tri_left_aligned);

    /* It may be possible that some of the adjacent tiles are NULL, such as when the current
     * tile as at a chunk edge. In that case, we have no neighbor tile to blend with. In 
     * that case, we make the tile's material go up to the very edge. */

    struct tile_adj_info 
        top = {
           .tile = top_tile,
           .bot_center_idx = curr.top_center_idx,
           .bot_left_mask = curr.top_left_mask,
           .bot_right_mask = curr.top_right_mask,
        },
        bot = {
           .tile = bot_tile,
           .top_center_idx = curr.bot_center_idx,
           .top_left_mask = curr.bot_left_mask,
           .top_right_mask = curr.bot_right_mask,
        },
        left = {
           .tile = left_tile,
           .right_center_idx = curr.left_center_idx,
           .top_right_mask = curr.top_left_mask,
           .bot_right_mask = curr.bot_left_mask,
        },
        right = {
           .tile = right_tile,
           .left_center_idx = curr.right_center_idx, 
           .bot_left_mask = curr.bot_right_mask,
           .top_left_mask = curr.top_right_mask,
        },
        top_right = { .tile = top_right_tile, },
        bot_right = { .tile = bot_right_tile, },
        top_left  = { .tile = top_left_tile, },
        bot_left  = { .tile = bot_left_tile, };

    struct tile_adj_info *adjacent[] = {&top, &bot, &left, &right, &top_right, &bot_right, &top_left, &bot_left};

    for(int i = 0; i < ARR_SIZE(adjacent); i++) {
        bool tmp;
        if(adjacent[i]->tile) {
            r_gl_tile_mat_indices(adjacent[i], &tmp);
        }
    }
    
    if(!top_right.tile) {
        top_right.bot_left_mask = top_tile ? INDICES_MASK_8(curr.top_center_idx, top.bot_center_idx)
                                           : INDICES_MASK_8(curr.right_center_idx, right.left_center_idx); 
    }

    if(!top_left.tile) {
        top_left.bot_right_mask = top_tile ? INDICES_MASK_8(curr.top_center_idx, top.bot_center_idx)
                                           : INDICES_MASK_8(curr.left_center_idx, left.right_center_idx);
    }

    if(!bot_right.tile) {
        bot_right.top_left_mask = bot_tile ? INDICES_MASK_8(curr.bot_center_idx, bot.top_center_idx)
                                           : INDICES_MASK_8(curr.right_center_idx, right.left_center_idx);
    }

    if(!bot_left.tile) {
        bot_left.top_right_mask = bot_tile ? INDICES_MASK_8(curr.bot_center_idx, bot.top_center_idx)
                                           : INDICES_MASK_8(curr.left_center_idx, left.right_center_idx);
    }

    /* Now, update all 4 triangles of the top face 
     *
     * Since 'adjacent_mat_indices' is a flat attribute, we only need to set 
     * it for the provoking vertex of each triangle.
     *
     * The first two 'adjacency_mat_indices' elements hold the 8 surrounding materials for 
     * the triangle's two non-central vertices. If the vertex is surrounded by only
     * 2 different materials, for example, then the weighting of each of these 
     * materials at the vertex is determened by the number of occurences of the 
     * material's index. The final material is the weighted average of the 8 materials,
     * which may contain repeated indices.
     *
     * The next element holds the materials at the midpoints of the edges of this tile and 
     * the last one holds the materials for the middle_mask of the tile.
     */
    size_t offset = VERTS_PER_TILE * (r * width + c) * sizeof(struct vertex);
    size_t length = VERTS_PER_TILE * sizeof(struct vertex);

    struct vertex *tile_verts_base = glMapNamedBufferRange(VBO, offset, length, GL_MAP_WRITE_BIT);
    assert(tile_verts_base);
    struct vertex *south_provoking = tile_verts_base + (5 * VERTS_PER_FACE);
    struct vertex *north_provoking = tile_verts_base + (5 * VERTS_PER_FACE) + 2*3;
    struct vertex *west_provoking  = tile_verts_base + (5 * VERTS_PER_FACE) + (top_tri_left_aligned ?  3*3 : 3*1);
    struct vertex *east_provoking  = tile_verts_base + (5 * VERTS_PER_FACE) + (top_tri_left_aligned ?  3*1 : 3*3);

    south_provoking->adjacent_mat_indices[0] = 
        INDICES_MASK_32(bot.top_left_mask, bot_left.top_right_mask, left.bot_right_mask, curr.bot_left_mask);
    south_provoking->adjacent_mat_indices[1] = 
        INDICES_MASK_32(bot_right.top_left_mask, bot.top_right_mask, curr.bot_right_mask, right.bot_left_mask);
    south_provoking->blend_mode = r_gl_blendmode_for_provoking_vert(south_provoking);

    north_provoking->adjacent_mat_indices[0] = 
        INDICES_MASK_32(curr.top_left_mask, left.top_right_mask, top_left.bot_right_mask, top.bot_left_mask);
    north_provoking->adjacent_mat_indices[1] = 
        INDICES_MASK_32(right.top_left_mask, curr.top_right_mask, top.bot_right_mask, top_right.bot_left_mask);
    north_provoking->blend_mode = r_gl_blendmode_for_provoking_vert(north_provoking);

    west_provoking->adjacent_mat_indices[0] = south_provoking->adjacent_mat_indices[0];
    west_provoking->adjacent_mat_indices[1] = north_provoking->adjacent_mat_indices[0];
    west_provoking->blend_mode = r_gl_blendmode_for_provoking_vert(west_provoking);

    east_provoking->adjacent_mat_indices[0] = south_provoking->adjacent_mat_indices[1];
    east_provoking->adjacent_mat_indices[1] = north_provoking->adjacent_mat_indices[1];
    east_provoking->blend_mode = r_gl_blendmode_for_provoking_vert(east_provoking);

    GLint adj_center_mask = INDICES_MASK_32(
        INDICES_MASK_8(curr.top_center_idx,     top.bot_center_idx),
        INDICES_MASK_8(curr.right_center_idx,   right.left_center_idx),
        INDICES_MASK_8(curr.bot_center_idx,     bot.top_center_idx),
        INDICES_MASK_8(curr.left_center_idx,    left.right_center_idx)
    );

    struct vertex *provoking[] = {south_provoking, north_provoking, west_provoking, east_provoking};
    for(int i = 0; i < ARR_SIZE(provoking); i++) {

        provoking[i]->adjacent_mat_indices[2] = adj_center_mask;
        provoking[i]->adjacent_mat_indices[3] = curr.middle_mask;
    }

    glUnmapNamedBuffer(VBO);
    GL_ASSERT_OK();
}

void R_GL_TileGetVertices(const struct tile *tile, struct vertex *out, size_t r, size_t c)
{
    /* Bottom face is always the same (just shifted over based on row and column), and the 
     * front, back, left, right faces just connect the top and bottom faces. The only 
     * variations are in the top face, which has some corners raised based on tile type. 
     */

    struct face bot = {
        .nw = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - ((c+1) * X_COORDS_PER_TILE), (-1.0f * Y_COORDS_PER_TILE), 
                                 0.0f + (r * Z_COORDS_PER_TILE) },
            .uv     = (vec2_t) { 0.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .ne = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - (c * X_COORDS_PER_TILE), (-1.0f * Y_COORDS_PER_TILE), 
                                 0.0f + (r * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .se = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - (c * X_COORDS_PER_TILE), (-1.0f * Y_COORDS_PER_TILE), 
                                 0.0f + ((r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .sw = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - ((c+1) * X_COORDS_PER_TILE), (-1.0f * Y_COORDS_PER_TILE), 
                                 0.0f + ((r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
    };

    /* Normals for top face get set at the end */
    struct face top = {
        .nw = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - (c * X_COORDS_PER_TILE),
                                 M_Tile_NWHeight(tile) * Y_COORDS_PER_TILE,
                                 0.0f + (r * Z_COORDS_PER_TILE) },
            .uv     = (vec2_t) { 0.0f, 1.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .ne = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - ((c+1) * X_COORDS_PER_TILE), 
                                 M_Tile_NEHeight(tile) * Y_COORDS_PER_TILE,
                                 0.0f + (r * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 1.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .se = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - ((c+1) * X_COORDS_PER_TILE), 
                                 M_Tile_SEHeight(tile) * Y_COORDS_PER_TILE,
                                 0.0f + ((r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .sw = (struct vertex) {
            .pos    = (vec3_t) { 0.0f - (c * X_COORDS_PER_TILE), 
                                 M_Tile_SWHeight(tile) * Y_COORDS_PER_TILE,
                                 0.0f + ((r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
    };

#define V_COORD(width, height) (((float)height)/width)

    GLint side_adjacent_indices = ((tile->sides_mat_idx & 0xf) << 0) | ((tile->sides_mat_idx & 0xf) << 4)
                                | ((tile->sides_mat_idx & 0xf) << 8) | ((tile->sides_mat_idx & 0xf) << 12);
    struct face back = {
        .nw = (struct vertex) {
            .pos    = top.nw.pos,
            .uv     = (vec2_t) { 0.0f, V_COORD(X_COORDS_PER_TILE, back.nw.pos.y) },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .ne = (struct vertex) {
            .pos    = top.ne.pos,
            .uv     = (vec2_t) { 1.0f, V_COORD(X_COORDS_PER_TILE, back.ne.pos.y) },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .se = (struct vertex) {
            .pos    = bot.nw.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .sw = (struct vertex) {
            .pos    = bot.ne.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
    };

    struct face front = {
        .nw = (struct vertex) {
            .pos    = top.sw.pos,
            .uv     = (vec2_t) { 0.0f, V_COORD(X_COORDS_PER_TILE, front.nw.pos.y) },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .ne = (struct vertex) {
            .pos    = top.se.pos,
            .uv     = (vec2_t) { 1.0f, V_COORD(X_COORDS_PER_TILE, front.ne.pos.y) },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .se = (struct vertex) {
            .pos    = bot.sw.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .sw = (struct vertex) {
            .pos    = bot.se.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
    };

    struct face left = {
        .nw = (struct vertex) {
            .pos    = top.sw.pos,
            .uv     = (vec2_t) { 0.0f, V_COORD(X_COORDS_PER_TILE, left.nw.pos.y) },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .ne = (struct vertex) {
            .pos    = top.nw.pos,
            .uv     = (vec2_t) { 1.0f, V_COORD(X_COORDS_PER_TILE, left.ne.pos.y) },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .se = (struct vertex) {
            .pos    = bot.ne.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .sw = (struct vertex) {
            .pos    = bot.se.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
    };

    struct face right = {
        .nw = (struct vertex) {
            .pos    = top.ne.pos,
            .uv     = (vec2_t) { 0.0f, V_COORD(X_COORDS_PER_TILE, right.nw.pos.y) },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .ne = (struct vertex) {
            .pos    = top.se.pos,
            .uv     = (vec2_t) { 1.0f, V_COORD(X_COORDS_PER_TILE, right.ne.pos.y) },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .se = (struct vertex) {
            .pos    = bot.sw.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .sw = (struct vertex) {
            .pos    = bot.nw.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
    };

#undef V_COORD

    struct face *faces[] = {
        &bot, &front, &back, &left, &right 
    };

    for(int i = 0; i < ARR_SIZE(faces); i++) {

        struct face *curr = faces[i];

        /* First triangle */
        memcpy(out + (i * VERTS_PER_FACE) + 0, &curr->nw, sizeof(struct vertex));
        memcpy(out + (i * VERTS_PER_FACE) + 1, &curr->ne, sizeof(struct vertex));
        memcpy(out + (i * VERTS_PER_FACE) + 2, &curr->sw, sizeof(struct vertex));

        /* Second triangle */
        memcpy(out + (i * VERTS_PER_FACE) + 3, &curr->se, sizeof(struct vertex));
        memcpy(out + (i * VERTS_PER_FACE) + 4, &curr->sw, sizeof(struct vertex));
        memcpy(out + (i * VERTS_PER_FACE) + 5, &curr->ne, sizeof(struct vertex));
    }

    /* Lastly, the top face. Unlike the other five faces, it can have different 
     * normals for its' two triangles, and the triangles can be arranged differently 
     * at corner tiles. 
     */

    vec3_t top_tri_normals[2];
    bool   top_tri_left_aligned;
    r_gl_tile_top_normals(tile, top_tri_normals, &top_tri_left_aligned);

    /*
     * CONFIG 1 (left-aligned)   CONFIG 2
     * (nw)      (ne)            (nw)      (ne)
     * +---------+               +---------+
     * |       / |               | \       |
     * |     /   |               |   \     |
     * |   /     |               |     \   |
     * | /       |               |       \ |
     * +---------+               +---------+
     * (sw)      (se)            (sw)      (se)
     */

    struct vertex *first_tri[3];
    struct vertex *second_tri[3];

    first_tri[0] = &top.sw;
    first_tri[1] = &top.se;
    second_tri[0] = &top.nw;
    second_tri[1] = &top.ne;

    if(top_tri_left_aligned) {

        first_tri[2] = &top.ne;
        second_tri[2] = &top.sw;
    }else {

        first_tri[2] = &top.nw; 
        second_tri[2] = &top.se;
    }

    float center_height = 
          TILETYPE_IS_RAMP(tile->type)          ? (tile->base_height + tile->ramp_height / 2.0f) 
        : TILETYPE_IS_CORNER_CONVEX(tile->type) ? (tile->base_height + tile->ramp_height) 
        : (tile->base_height);

    vec3_t center_vert_pos = (vec3_t) {
        top.nw.pos.x - X_COORDS_PER_TILE / 2.0f, 
        center_height * Y_COORDS_PER_TILE, 
        top.nw.pos.z + Z_COORDS_PER_TILE / 2.0f
    };
    struct vertex center_vert = (struct vertex) {
        .uv     = (vec2_t) {0.5f, 0.5f},
        .normal = r_gl_tile_middle_normal(tile),
    };

    /* First 'major' triangle */
    bool use_side_mat = fabs(top_tri_normals[0].y) < 1.0 && (tile->ramp_height > 1);
    int mat_idx = use_side_mat ? tile->sides_mat_idx : tile->top_mat_idx;

    for(int i = 0; i < 3; i++) {
        first_tri[i]->normal = top_tri_normals[0];
        first_tri[i]->material_idx = mat_idx;
    }
    center_vert.material_idx = mat_idx;
    center_vert.normal = top_tri_normals[0];
    /* When all 4 of the top face triangles share the exact same center vertex, there is potential for 
     * there to be a single texel-wide line in between the different triangles due to imprecision in
     * interpolation. To fix this, we make the triangles have a very slight overlap by moving the 
     * center vertex very slightly for the different triangles. The top face will look the same but 
     * this will guarantee that there are no artifacts. */
    center_vert.pos = (vec3_t){center_vert_pos.x, center_vert_pos.y, center_vert_pos.z - 0.005};

    memcpy(out + (5 * VERTS_PER_FACE) + 0, first_tri[0], sizeof(struct vertex));
    memcpy(out + (5 * VERTS_PER_FACE) + 1, first_tri[1], sizeof(struct vertex));
    memcpy(out + (5 * VERTS_PER_FACE) + 2, &center_vert, sizeof(struct vertex));

    memcpy(out + (5 * VERTS_PER_FACE) + 3, &center_vert, sizeof(struct vertex));
    memcpy(out + (5 * VERTS_PER_FACE) + 4, first_tri[2], sizeof(struct vertex));
    memcpy(out + (5 * VERTS_PER_FACE) + 5, (top_tri_left_aligned ? first_tri[1] : first_tri[0]), sizeof(struct vertex));

    /* Second 'major' triangle */
    use_side_mat = fabs(top_tri_normals[1].y) < 1.0 && (tile->ramp_height > 1);
    mat_idx = use_side_mat ? tile->sides_mat_idx : tile->top_mat_idx;

    for(int i = 0; i < 3; i++) {
        second_tri[i]->normal = top_tri_normals[1];
        second_tri[i]->material_idx = mat_idx;
    }
    center_vert.material_idx = mat_idx;
    center_vert.normal = top_tri_normals[1];
    center_vert.pos = (vec3_t){center_vert_pos.x, center_vert_pos.y, center_vert_pos.z + 0.005};

    memcpy(out + (5 * VERTS_PER_FACE) + 6, second_tri[0], sizeof(struct vertex));
    memcpy(out + (5 * VERTS_PER_FACE) + 7, second_tri[1], sizeof(struct vertex));
    memcpy(out + (5 * VERTS_PER_FACE) + 8, &center_vert, sizeof(struct vertex));

    memcpy(out + (5 * VERTS_PER_FACE) + 9, &center_vert, sizeof(struct vertex));
    memcpy(out + (5 * VERTS_PER_FACE) + 10, second_tri[2], sizeof(struct vertex));
    memcpy(out + (5 * VERTS_PER_FACE) + 11, (top_tri_left_aligned ? second_tri[0] : second_tri[1]), sizeof(struct vertex));

}

int R_GL_TileGetTriMesh(const struct tile_desc *in, const void *chunk_rprivate, 
                        mat4x4_t *model, int tiles_per_chunk_x, vec3_t out[])
{
    const struct render_private *priv = chunk_rprivate;

    size_t offset = (in->tile_r * tiles_per_chunk_x + in->tile_c) * VERTS_PER_TILE * sizeof(struct vertex);
    size_t length = VERTS_PER_TILE * sizeof(struct vertex);
    const struct vertex *vert_base = glMapNamedBufferRange(priv->mesh.VBO, offset, length, GL_MAP_READ_BIT);
    assert(vert_base);
    int i = 0;

    for(; i < VERTS_PER_TILE; i++) {
    
        vec4_t pos_homo = (vec4_t){vert_base[i].pos.x, vert_base[i].pos.y, vert_base[i].pos.z, 1.0f};
        vec4_t ws_pos_homo;
        PFM_Mat4x4_Mult4x1(model, &pos_homo, &ws_pos_homo);
        
        out[i] = (vec3_t){
            ws_pos_homo.x / ws_pos_homo.w, 
            ws_pos_homo.y / ws_pos_homo.w, 
            ws_pos_homo.z / ws_pos_homo.w
        };
    }

    glUnmapNamedBuffer(priv->mesh.VBO);
    assert(i % 3 == 0);
    return i;
}

void R_GL_TileUpdate(void *chunk_rprivate, int r, int c, int tiles_width, int tiles_height, 
                     const struct tile *tiles)
{
    struct render_private *priv = chunk_rprivate;
    const struct tile *tile = &tiles[r * tiles_width + c];

    size_t offset = (r * tiles_width + c) * VERTS_PER_TILE * sizeof(struct vertex);
    size_t length = VERTS_PER_TILE * sizeof(struct vertex);
    struct vertex *vert_base = glMapNamedBufferRange(priv->mesh.VBO, offset, length, GL_MAP_WRITE_BIT);
    assert(vert_base);
    
    R_GL_TileGetVertices(tile, vert_base, r, c);

    glUnmapNamedBuffer(priv->mesh.VBO);

    for(int r_curr = r - 1; r_curr < r + 2; r_curr++){
        for(int c_curr = c - 1; c_curr < c + 2; c_curr++) {
        
            if(r_curr < 0 || r_curr >= tiles_height)
                continue;
            if(c_curr < 0 || c_curr >= tiles_width)
                continue;

            R_GL_TilePatchVertsBlend(priv->mesh.VBO, tiles, tiles_width, tiles_height, r_curr, c_curr);
        }
    }
    GL_ASSERT_OK();
}

