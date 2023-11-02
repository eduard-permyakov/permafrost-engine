/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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
#include "gl_mesh.h"
#include "gl_vertex.h"
#include "gl_shader.h"
#include "gl_material.h"
#include "gl_assert.h"
#include "gl_state.h"
#include "gl_perf.h"
#include "public/render.h"
#include "../map/public/tile.h"
#include "../map/public/map.h"
#include "../phys/public/collision.h"
#include "../camera.h"
#include "../config.h"
#include "../main.h"
#include "../perf.h"

#include <GL/glew.h>

#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
  

#define ARR_SIZE(a)                 (sizeof(a)/sizeof(a[0]))
#define MAG(x, y)                   sqrt(pow(x,2) + pow(y,2))
#define VEC3_EQUAL(a, b)            (0 == memcmp((a).raw, (b).raw, sizeof((a).raw)))

#define CPY2(dst, src)      \
    do{                     \
        dst[0] = src[0];    \
        dst[1] = src[1];    \
    }while(0)

#define INDICES_MASK_16(a, b)       (uint16_t)( (((a) & 0xff) << 8) \
                                              |  ((b) & 0xff) << 0)

#define INDICES_MASK_32(a, b)       (uint32_t)( (((a) & 0xffff) << 16) \
                                              |  ((b) & 0xffff) << 0 )

#define SAME_INDICES_32(i)          (  ((i) & 0xffff) == (((i) >> 16) & 0xffff) \
                                    && ((i) & 0xff  ) == (((i) >> 8 ) & 0xff  ) )

/* We take the directions to be relative to a normal vector facing outwards
 * from the plane of the face. West is to the right, east is to the left,
 * north is top, south is bottom. */
struct face{
    struct terrain_vert nw, ne, se, sw; 
};

struct tile_adj_info{
    const struct tile *tile;
    uint16_t middle_mask; 
    uint16_t top_left_mask;
    uint16_t top_right_mask;
    uint16_t bot_left_mask;
    uint16_t bot_right_mask;
    int top_center_idx;
    int bot_center_idx;
    int left_center_idx;
    int right_center_idx;
};

struct tri{
    struct terrain_vert verts[3];
};

/* Each top face is made up of 8 triangles, in the following configuration:
 *   +------+------+
 *   |\     |     /|
 *   |  \   |   /  |
 *   |    \ | /    |
 *   +------+------+
 *   |    / | \    |
 *   |  /   |   \  |
 *   |/     |     \|
 *   +------+------+
 * Each face can be thought of as being made of of 4 "major" triangles,
 * each of which has its' own adjacency info as a flat attribute. The 4 major
 * triangles are the minimal configuration that is necessary for the blending
 * system to work.
 *   +------+------+
 *   |\           /|
 *   |  \   2   /  |
 *   |    \   /    |
 *   +  1  >+<  3  +
 *   |    /   \    |
 *   |  /   0   \  |
 *   |/           \|
 *   +------+------+
 * The "major" trinagles can be futher subdivided. The triangles they are divided 
 * into must inherit the flat adjacency attributes and interpolate their positions, 
 * uv coorinates, and normals. In our case, we futher subdivide each of the major
 * triangles into 2 triangles. This is to give an extra vertex on the midpoint 
 * of each edge. When smoothing the normals, this extra point having its' own 
 * normal is essential. Care must be taken to ensure the appropriate winding order
 * for each triangle for backface culling!
 */
union top_face_vbuff{
    struct terrain_vert verts[VERTS_PER_TOP_FACE];
    struct tri tris[VERTS_PER_TOP_FACE/3];
    struct{
        /* Tri 0 */
        struct terrain_vert se0; 
        struct terrain_vert s0;
        struct terrain_vert center0;
        /* Tri 1 */
        struct terrain_vert center1;
        struct terrain_vert s1;
        struct terrain_vert sw0;
        /* Tri 2 */
        struct terrain_vert sw1;
        struct terrain_vert w0;
        struct terrain_vert center2;
        /* Tri 3 */
        struct terrain_vert center3;
        struct terrain_vert w1;
        struct terrain_vert nw0;
        /* Tri 4 */
        struct terrain_vert nw1;
        struct terrain_vert n0;
        struct terrain_vert center4;
        /* Tri 5 */
        struct terrain_vert center5;
        struct terrain_vert n1;
        struct terrain_vert ne0;
        /* Tri 6 */
        struct terrain_vert ne1;
        struct terrain_vert e0;
        struct terrain_vert center6;
        /* Tri 7 */
        struct terrain_vert center7;
        struct terrain_vert e1;
        struct terrain_vert se1;
    };
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void tile_top_normals(const struct tile *tile, vec3_t out_tri_normals[], bool *out_tri_left)
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

static void tile_smooth_normals_corner(struct tile *adj_cw[], struct terrain_vert *inout)
{
    enum{
        ADJ_CW_IDX_TOP_LEFT  = 0,
        ADJ_CW_IDX_TOP_RIGHT = 1,
        ADJ_CW_IDX_BOT_RIGHT = 2,
        ADJ_CW_IDX_BOT_LEFT  = 3,
    };
    vec3_t norm_total = {0};

    for(int i = 0; i < 4; i++) {

        if(!adj_cw[i])
            continue;
    
        vec3_t normals[2];
        bool top_tri_left_aligned;
        tile_top_normals(adj_cw[i], normals, &top_tri_left_aligned);

        switch(i) {
        case ADJ_CW_IDX_TOP_LEFT: 
            PFM_Vec3_Add(&norm_total, normals + 1, &norm_total);
            PFM_Vec3_Add(&norm_total, normals + (top_tri_left_aligned ? 1 : 0), &norm_total);
            break;
        case ADJ_CW_IDX_TOP_RIGHT:
            PFM_Vec3_Add(&norm_total, normals + 1, &norm_total);
            PFM_Vec3_Add(&norm_total, normals + (top_tri_left_aligned ? 0 : 1), &norm_total);
            break;
        case ADJ_CW_IDX_BOT_RIGHT:
            PFM_Vec3_Add(&norm_total, normals + 0, &norm_total);
            PFM_Vec3_Add(&norm_total, normals + (top_tri_left_aligned ? 0 : 1), &norm_total);
            break;
        case ADJ_CW_IDX_BOT_LEFT:
            PFM_Vec3_Add(&norm_total, normals + 0, &norm_total);
            PFM_Vec3_Add(&norm_total, normals + (top_tri_left_aligned ? 1 : 0), &norm_total);
            break;
        default: assert(0);
        }
    }

    PFM_Vec3_Normal(&norm_total, &norm_total);
    inout->normal = norm_total;
}

static void tile_smooth_normals_edge(struct tile *adj_lrtb[], struct terrain_vert *inout)
{
    vec3_t norm_total = {0};
    assert((!!adj_lrtb[0] + !!adj_lrtb[1] + !!adj_lrtb[2] + !!adj_lrtb[3]) <= 2);

    for(int i = 0; i < 4; i++) {

        if(!adj_lrtb[i])
            continue;
    
        vec3_t normals[2];
        bool top_tri_left_aligned;
        tile_top_normals(adj_lrtb[i], normals, &top_tri_left_aligned);

        PFM_Vec3_Add(&norm_total, normals + 0, &norm_total);
        PFM_Vec3_Add(&norm_total, normals + 1, &norm_total);
    }

    assert(PFM_Vec3_Len(&norm_total) > 0);
    PFM_Vec3_Normal(&norm_total, &norm_total);
    inout->normal = norm_total;
}

static void tile_mat_indices(struct tile_adj_info *inout, bool *out_top_tri_left_aligned)
{
    assert(inout->tile);

    vec3_t top_tri_normals[2];
    bool   top_tri_left_aligned;
    tile_top_normals(inout->tile, top_tri_normals, out_top_tri_left_aligned);

    GLuint tri_mats[2] = {
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
    inout->middle_mask = INDICES_MASK_16(tri_mats[0], tri_mats[1]);
    inout->bot_center_idx = tri_mats[0];
    inout->top_center_idx = tri_mats[1];

    if(!(*out_top_tri_left_aligned)) {
        inout->top_left_mask     = INDICES_MASK_16(tri_mats[1], tri_mats[0]);
        inout->top_right_mask    = INDICES_MASK_16(tri_mats[1], tri_mats[1]);
        inout->bot_left_mask     = INDICES_MASK_16(tri_mats[0], tri_mats[0]);
        inout->bot_right_mask    = INDICES_MASK_16(tri_mats[0], tri_mats[1]);

        inout->left_center_idx  = tri_mats[0];
        inout->right_center_idx = tri_mats[1];
    }else {
        inout->top_left_mask     = INDICES_MASK_16(tri_mats[1], tri_mats[1]);
        inout->top_right_mask    = INDICES_MASK_16(tri_mats[0], tri_mats[1]);
        inout->bot_left_mask     = INDICES_MASK_16(tri_mats[1], tri_mats[0]);
        inout->bot_right_mask    = INDICES_MASK_16(tri_mats[0], tri_mats[0]);

        inout->left_center_idx  = tri_mats[1];
        inout->right_center_idx = tri_mats[0];
    }
}

/* When all the materials for the tile are the same, we don't have to perform 
 * blending in the shader. This aids performance. */
enum blend_mode optimal_blendmode(const struct terrain_vert *vert)
{
    if(SAME_INDICES_32(vert->c1_indices[0])
    && vert->c1_indices[0] == vert->c2_indices[0]
    && vert->c1_indices[0] == vert->c2_indices[1]
    && vert->c1_indices[0] == vert->tb_indices
    && vert->c1_indices[0] == vert->lr_indices) {
    
        return BLEND_MODE_NOBLEND;
    }else{
        return vert->blend_mode; 
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

static int arr_indexof(const int *array, size_t size, int elem)
{
    for(int i = 0; i < size; i++) {
        if(array[i] == elem) 
            return i;
    }
    return -1;
}

static int arr_min(const int array[], size_t size)
{
    int min = array[0];
    for(int i = 1; i < size; i++) {
        if(array[i] < min)
            min = array[i];
    }
    return min;
}

static float tile_min_visible_height(const struct map *map, struct tile_desc tile)
{
    struct map_resolution res;
    M_GetResolution(map, &res);

    struct tile *curr_tile  = NULL,
                *top_tile   = NULL,
                *bot_tile   = NULL,
                *left_tile  = NULL,
                *right_tile = NULL;

    int ret = M_TileForDesc(map, tile, &curr_tile);
    assert(ret);

    struct tile_desc ref = tile;
    ret = M_Tile_RelativeDesc(res, &ref, 0, -1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &top_tile); 
        assert(ret);
    }

    ref = tile;
    ret = M_Tile_RelativeDesc(res, &ref, 0, 1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &bot_tile); 
        assert(ret);
    }

    ref = tile;
    ret = M_Tile_RelativeDesc(res, &ref, -1, 0);
    if(ret) {
        ret = M_TileForDesc(map, ref, &left_tile); 
        assert(ret);
    }

    ref = tile;
    ret = M_Tile_RelativeDesc(res, &ref, 1, 0);
    if(ret) {
        ret = M_TileForDesc(map, ref, &right_tile); 
        assert(ret);
    }

    int heights[] = {

        M_Tile_NWHeight(curr_tile),
        M_Tile_NEHeight(curr_tile),
        M_Tile_SEHeight(curr_tile),
        M_Tile_SWHeight(curr_tile),

        left_tile ? M_Tile_NEHeight(left_tile) : -1,
        left_tile ? M_Tile_SEHeight(left_tile) : -1,

        top_tile ? M_Tile_SWHeight(top_tile) : -1,
        top_tile ? M_Tile_SEHeight(top_tile) : -1,

        right_tile ? M_Tile_NWHeight(right_tile) : -1,
        right_tile ? M_Tile_SWHeight(right_tile) : -1,

        bot_tile  ? M_Tile_NWHeight(bot_tile) : -1,
        bot_tile  ? M_Tile_NEHeight(bot_tile) : -1,
    };

    return arr_min(heights, ARR_SIZE(heights)) * Y_COORDS_PER_TILE;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_TileDrawSelected(const struct tile_desc *in, const void *chunk_rprivate, mat4x4_t *model, 
                           const int *tiles_per_chunk_x, const int *tiles_per_chunk_z)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    struct terrain_vert vbuff[VERTS_PER_TILE];
    vec4_t red = (vec4_t){1.0f, 0.0f, 0.0f, 1.0};
    GLuint VAO, VBO;

    const struct render_private *priv = chunk_rprivate;
    size_t offset = (in->tile_r * (*tiles_per_chunk_x) + in->tile_c) * VERTS_PER_TILE * sizeof(struct terrain_vert);
    size_t length = VERTS_PER_TILE * sizeof(struct terrain_vert);

    glBindBuffer(GL_ARRAY_BUFFER, priv->mesh.VBO);
    const struct terrain_vert *vert_base = glMapBufferRange(GL_ARRAY_BUFFER, offset, length, GL_MAP_READ_BIT);
    assert(vert_base);
    memcpy(vbuff, vert_base, sizeof(vbuff));
    glUnmapBuffer(GL_ARRAY_BUFFER);

    /* Additionally, scale the tile selection mesh slightly around its' center. This is so that 
     * it is slightly larger than the actual tile underneath and can be rendered on top of it. */
    const float SCALE_FACTOR = 1.025f;
    mat4x4_t final_model;
    mat4x4_t scale, trans, trans_inv, tmp1, tmp2;
    PFM_Mat4x4_MakeScale(SCALE_FACTOR, SCALE_FACTOR, SCALE_FACTOR, &scale);

    vec3_t center = (vec3_t){
        ( 0.0f - (in->tile_c* X_COORDS_PER_TILE) - X_COORDS_PER_TILE/2.0f ), 
        (-TILE_DEPTH * Y_COORDS_PER_TILE - Y_COORDS_PER_TILE/2.0f), 
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
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct terrain_vert), (void*)0);
    glEnableVertexAttribArray(0);

    /* Attribute 1 - texture coordinates */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct terrain_vert), 
        (void*)offsetof(struct terrain_vert, uv));
    glEnableVertexAttribArray(1);

    /* Attribute 2 - normal */
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(struct terrain_vert), 
        (void*)offsetof(struct terrain_vert, normal));
    glEnableVertexAttribArray(2);

    /* Set uniforms */
    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = final_model
    });

    R_GL_StateSet(GL_U_COLOR, (struct uval){
        .type = UTYPE_VEC4,
        .val.as_vec4 = red
    });

    R_GL_Shader_Install("mesh.static.tile-outline");

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, sizeof(vbuff), vbuff, GL_STATIC_DRAW);

    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, VERTS_PER_TILE);

    /* cleanup */
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    GL_PERF_RETURN_VOID();
}

void R_GL_TilePatchVertsBlend(void *chunk_rprivate, const struct map *map, const struct tile_desc *tile)
{
    ASSERT_IN_RENDER_THREAD();

    const struct render_private *priv = chunk_rprivate;
    GLuint VBO = priv->mesh.VBO;

    struct map_resolution res;
    M_GetResolution(map, &res);

    struct tile *curr_tile      = NULL,
                *top_tile       = NULL,
                *bot_tile       = NULL,
                *left_tile      = NULL,
                *right_tile     = NULL,
                *top_right_tile = NULL,
                *bot_right_tile = NULL,
                *top_left_tile  = NULL,
                *bot_left_tile  = NULL;

    int ret = M_TileForDesc(map, *tile, &curr_tile);
    assert(ret);

    struct tile_desc ref = *tile;
    ret = M_Tile_RelativeDesc(res, &ref, 0, -1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &top_tile); 
        assert(ret);
    }

    ref = *tile;
    ret = M_Tile_RelativeDesc(res, &ref, 0, 1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &bot_tile); 
        assert(ret);
    }

    ref = *tile;
    ret = M_Tile_RelativeDesc(res, &ref, -1, 0);
    if(ret) {
        ret = M_TileForDesc(map, ref, &left_tile); 
        assert(ret);
    }

    ref = *tile;
    ret = M_Tile_RelativeDesc(res, &ref, 1, 0);
    if(ret) {
        ret = M_TileForDesc(map, ref, &right_tile); 
        assert(ret);
    }

    ref = *tile;
    ret = M_Tile_RelativeDesc(res, &ref, 1, -1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &top_right_tile); 
        assert(ret);
    }

    ref = *tile;
    ret = M_Tile_RelativeDesc(res, &ref, 1, 1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &bot_right_tile); 
        assert(ret);
    }

    ref = *tile;
    ret = M_Tile_RelativeDesc(res, &ref, -1, -1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &top_left_tile);
        assert(ret);
    }

    ref = *tile;
    ret = M_Tile_RelativeDesc(res, &ref, -1, 1);
    if(ret) {
        ret = M_TileForDesc(map, ref, &bot_left_tile); 
        assert(ret);
    }

    struct tile_adj_info curr = {.tile = curr_tile};
    bool top_tri_left_aligned;
    tile_mat_indices(&curr, &top_tri_left_aligned);

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
            tile_mat_indices(adjacent[i], &tmp);
        }
    }
    
    if(!top_right.tile) {
        top_right.bot_left_mask = top_tile ? INDICES_MASK_16(curr.top_center_idx, top.bot_center_idx)
                                           : INDICES_MASK_16(curr.right_center_idx, right.left_center_idx); 
    }

    if(!top_left.tile) {
        top_left.bot_right_mask = top_tile ? INDICES_MASK_16(curr.top_center_idx, top.bot_center_idx)
                                           : INDICES_MASK_16(curr.left_center_idx, left.right_center_idx);
    }

    if(!bot_right.tile) {
        bot_right.top_left_mask = bot_tile ? INDICES_MASK_16(curr.bot_center_idx, bot.top_center_idx)
                                           : INDICES_MASK_16(curr.right_center_idx, right.left_center_idx);
    }

    if(!bot_left.tile) {
        bot_left.top_right_mask = bot_tile ? INDICES_MASK_16(curr.bot_center_idx, bot.top_center_idx)
                                           : INDICES_MASK_16(curr.left_center_idx, left.right_center_idx);
    }

    /* Now, update all triangles of the top face 
     *
     * Since all the material index attributes are flat attribute, we only need to set 
     * them for the provoking vertex of each triangle.
     *
     * 'c1_indices' and 'c2_indices' hold the 8 surrounding materials for the triangle's 
     * two non-central vertices. If the vertex is surrounded by only 2 different materials, 
     * for example, then the weighting of each of these materials at the vertex is determened 
     * by the number of occurences of the material's index. The final material is the 
     * weighted average of the 8 materials, which may contain repeated indices.
     *
     * 'tb_indices' and 'lr_indices' hold the materials at the midpoints of the edges of this 
     * tile and 'middle_indices' hold the materials for the center of the tile.
     */
    size_t offset = VERTS_PER_TILE * (tile->tile_r * TILES_PER_CHUNK_WIDTH + tile->tile_c) * sizeof(struct terrain_vert);
    size_t length = VERTS_PER_TILE * sizeof(struct terrain_vert);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    struct terrain_vert *tile_verts_base = glMapBufferRange(GL_ARRAY_BUFFER, offset, length, GL_MAP_WRITE_BIT);
    GL_ASSERT_OK();
    assert(tile_verts_base);

    struct terrain_vert *south_provoking[2] = {tile_verts_base + (4 * VERTS_PER_SIDE_FACE) + 0*3,
                                               tile_verts_base + (4 * VERTS_PER_SIDE_FACE) + 1*3};
    struct terrain_vert *west_provoking[2]  = {tile_verts_base + (4 * VERTS_PER_SIDE_FACE) + 2*3,
                                               tile_verts_base + (4 * VERTS_PER_SIDE_FACE) + 3*3};
    struct terrain_vert *north_provoking[2] = {tile_verts_base + (4 * VERTS_PER_SIDE_FACE) + 4*3,
                                               tile_verts_base + (4 * VERTS_PER_SIDE_FACE) + 5*3};
    struct terrain_vert *east_provoking[2]  = {tile_verts_base + (4 * VERTS_PER_SIDE_FACE) + 6*3,
                                               tile_verts_base + (4 * VERTS_PER_SIDE_FACE) + 7*3};

    for(int i = 0; i < 2; i++) {

        south_provoking[i]->c1_indices[0] = INDICES_MASK_32(bot.top_left_mask, bot_left.top_right_mask);
        south_provoking[i]->c1_indices[1] = INDICES_MASK_32(left.bot_right_mask, curr.bot_left_mask);

        south_provoking[i]->c2_indices[0] = INDICES_MASK_32(bot_right.top_left_mask, bot.top_right_mask);
        south_provoking[i]->c2_indices[1] = INDICES_MASK_32(curr.bot_right_mask, right.bot_left_mask);

        north_provoking[i]->c1_indices[0] = INDICES_MASK_32(curr.top_left_mask, left.top_right_mask);
        north_provoking[i]->c1_indices[1] = INDICES_MASK_32(top_left.bot_right_mask, top.bot_left_mask);

        north_provoking[i]->c2_indices[0] = INDICES_MASK_32(right.top_left_mask, curr.top_right_mask);
        north_provoking[i]->c2_indices[1] = INDICES_MASK_32(top.bot_right_mask, top_right.bot_left_mask);

        CPY2(west_provoking[i]->c1_indices, south_provoking[0]->c1_indices);
        CPY2(west_provoking[i]->c2_indices, north_provoking[0]->c1_indices);

        CPY2(east_provoking[i]->c1_indices, south_provoking[0]->c2_indices);
        CPY2(east_provoking[i]->c2_indices, north_provoking[0]->c2_indices);
    }

    GLuint tb_mask = INDICES_MASK_32(
        INDICES_MASK_16(curr.top_center_idx, top.bot_center_idx),
        INDICES_MASK_16(curr.bot_center_idx, bot.top_center_idx)
    );
    GLuint lr_mask = INDICES_MASK_32(
        INDICES_MASK_16(curr.left_center_idx,  left.right_center_idx),
        INDICES_MASK_16(curr.right_center_idx, right.left_center_idx)
    );

    struct terrain_vert *provoking[] = {
        south_provoking[0], south_provoking[1], 
        north_provoking[0], north_provoking[1], 
        west_provoking[0],  west_provoking[1], 
        east_provoking[0],  east_provoking[1]
    };

    for(int i = 0; i < ARR_SIZE(provoking); i++) {

        provoking[i]->tb_indices = tb_mask;
        provoking[i]->lr_indices = lr_mask;
        provoking[i]->middle_indices = curr.middle_mask;
        provoking[i]->blend_mode = optimal_blendmode(provoking[i]);
    }

    glUnmapBuffer(GL_ARRAY_BUFFER);
    GL_ASSERT_OK();
}

void R_GL_TilePatchVertsSmooth(void *chunk_rprivate, const struct map *map, const struct tile_desc *tile)
{
    ASSERT_IN_RENDER_THREAD();

    const struct render_private *priv = chunk_rprivate;
    GLuint VBO = priv->mesh.VBO;

    size_t offset = VERTS_PER_TILE * (tile->tile_r * TILES_PER_CHUNK_WIDTH + tile->tile_c) * sizeof(struct terrain_vert);
    size_t length = VERTS_PER_TILE * sizeof(struct terrain_vert);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    union top_face_vbuff *tfvb = glMapBufferRange(GL_ARRAY_BUFFER, offset, length, GL_MAP_WRITE_BIT);
    GL_ASSERT_OK();
    assert(tfvb);
    tfvb = (union top_face_vbuff*)(((struct terrain_vert*)tfvb) + (4 * VERTS_PER_SIDE_FACE));

    struct map_resolution res;
    M_GetResolution(map, &res);

    struct tile *curr_tile = NULL;
    M_TileForDesc(map, *tile, &curr_tile);
    assert(curr_tile);

    vec3_t normals[2];
    bool top_tri_left_aligned;
    tile_top_normals(curr_tile, normals, &top_tri_left_aligned);
    
    struct tile *tiles[4] = {0};
    struct tile_desc td;

    /* NW (top-left) corner */
    memset(tiles, 0, sizeof(tiles));
    td = *tile; if(M_Tile_RelativeDesc(res, &td, -1, -1)) M_TileForDesc(map, td, &tiles[0]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0, -1)) M_TileForDesc(map, td, &tiles[1]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[2]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td, -1,  0)) M_TileForDesc(map, td, &tiles[3]);
    tile_smooth_normals_corner(tiles, &tfvb->nw0);
    tile_smooth_normals_corner(tiles, &tfvb->nw1);

    /* NE (top-right) corner */
    memset(tiles, 0, sizeof(tiles));
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0, -1)) M_TileForDesc(map, td, &tiles[0]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  1, -1)) M_TileForDesc(map, td, &tiles[1]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  1,  0)) M_TileForDesc(map, td, &tiles[2]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[3]);
    tile_smooth_normals_corner(tiles, &tfvb->ne0);
    tile_smooth_normals_corner(tiles, &tfvb->ne1);

    /* SE (bot-right) corner */
    memset(tiles, 0, sizeof(tiles));
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[0]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  1,  0)) M_TileForDesc(map, td, &tiles[1]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  1,  1)) M_TileForDesc(map, td, &tiles[2]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0,  1)) M_TileForDesc(map, td, &tiles[3]);
    tile_smooth_normals_corner(tiles, &tfvb->se0);
    tile_smooth_normals_corner(tiles, &tfvb->se1);

    /* SW (bot-left) corner */
    memset(tiles, 0, sizeof(tiles));
    td = *tile; if(M_Tile_RelativeDesc(res, &td, -1,  0)) M_TileForDesc(map, td, &tiles[0]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[1]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0,  1)) M_TileForDesc(map, td, &tiles[2]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td, -1,  1)) M_TileForDesc(map, td, &tiles[3]);
    tile_smooth_normals_corner(tiles, &tfvb->sw0);
    tile_smooth_normals_corner(tiles, &tfvb->sw1);

    /* Top edge */
    memset(tiles, 0, sizeof(tiles));
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0, -1)) M_TileForDesc(map, td, &tiles[2]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[3]);
    tile_smooth_normals_edge(tiles, &tfvb->n0);
    tile_smooth_normals_edge(tiles, &tfvb->n1);

    /* Bot edge */
    memset(tiles, 0, sizeof(tiles));
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[2]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0,  1)) M_TileForDesc(map, td, &tiles[3]);
    tile_smooth_normals_edge(tiles, &tfvb->s0);
    tile_smooth_normals_edge(tiles, &tfvb->s1);

    /* Left edge */
    memset(tiles, 0, sizeof(tiles));
    td = *tile; if(M_Tile_RelativeDesc(res, &td, -1,  0)) M_TileForDesc(map, td, &tiles[0]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[1]);
    tile_smooth_normals_edge(tiles, &tfvb->w0);
    tile_smooth_normals_edge(tiles, &tfvb->w1);

    /* Right edge */
    memset(tiles, 0, sizeof(tiles));
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  0,  0)) M_TileForDesc(map, td, &tiles[0]);
    td = *tile; if(M_Tile_RelativeDesc(res, &td,  1,  0)) M_TileForDesc(map, td, &tiles[1]);
    tile_smooth_normals_edge(tiles, &tfvb->e0);
    tile_smooth_normals_edge(tiles, &tfvb->e1);

    /* Center */
    vec3_t center_norm = {0};
    PFM_Vec3_Add(&center_norm, normals + 0, &center_norm);
    PFM_Vec3_Add(&center_norm, normals + 1, &center_norm);
    PFM_Vec3_Normal(&center_norm, &center_norm);

    tfvb->center0.normal = center_norm;
    tfvb->center1.normal = center_norm;
    tfvb->center2.normal = center_norm;
    tfvb->center3.normal = center_norm;
    tfvb->center4.normal = center_norm;
    tfvb->center5.normal = center_norm;
    tfvb->center6.normal = center_norm;
    tfvb->center7.normal = center_norm;

    glUnmapBuffer(GL_ARRAY_BUFFER);
    GL_ASSERT_OK();
}

void R_GL_TileUpdate(void *chunk_rprivate, const struct map *map, const struct tile_desc *desc)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    struct render_private *priv = chunk_rprivate;

    struct tile *tile;
    int ret = M_TileForDesc(map, *desc, &tile);
    assert(ret);

    size_t offset = (desc->tile_r * TILES_PER_CHUNK_WIDTH + desc->tile_c) * VERTS_PER_TILE * sizeof(struct terrain_vert);
    size_t length = VERTS_PER_TILE * sizeof(struct terrain_vert);
    glBindBuffer(GL_ARRAY_BUFFER, priv->mesh.VBO);
    struct terrain_vert *vert_base = glMapBufferRange(GL_ARRAY_BUFFER, offset, length, GL_MAP_WRITE_BIT);
    assert(vert_base);
    
    R_TileGetVertices(map, *desc, vert_base);
    glUnmapBuffer(GL_ARRAY_BUFFER);

    R_GL_TilePatchVertsBlend(chunk_rprivate, map, desc);
    if(tile->blend_normals) {
        R_GL_TilePatchVertsSmooth(chunk_rprivate, map, desc);
    }

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_TileGetVertices(const struct map *map, struct tile_desc td, struct terrain_vert *out)
{
    PERF_ENTER();

    struct tile *tile;
    int ret = M_TileForDesc(map, td, &tile);
    assert(ret);

    /* Use the smallest possible size for the side faces of the tile. This saves us 
     * some fragment processing by not drawing side faces that are not visible. */
    float min_vis_height = tile_min_visible_height(map, td);

    /* Bottom face is always the same (just shifted over based on row and column), and the 
     * front, back, left, right faces just connect the top and bottom faces. The only 
     * variations are in the top face, which has some corners raised based on tile type. 
     */

    struct face bot = {
        .nw = (struct terrain_vert) {
            .pos    = (vec3_t) { 0.0f - ((td.tile_c+1) * X_COORDS_PER_TILE), 
                                 min_vis_height,
                                 0.0f + (td.tile_r * Z_COORDS_PER_TILE) },
            .uv     = (vec2_t) { 0.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .ne = (struct terrain_vert) {
            .pos    = (vec3_t) { 0.0f - (td.tile_c * X_COORDS_PER_TILE), 
                                 min_vis_height, 
                                 0.0f + (td.tile_r * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 1.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .se = (struct terrain_vert) {
            .pos    = (vec3_t) { 0.0f - (td.tile_c * X_COORDS_PER_TILE), 
                                 min_vis_height, 
                                 0.0f + ((td.tile_r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .sw = (struct terrain_vert) {
            .pos    = (vec3_t) { 0.0f - ((td.tile_c+1) * X_COORDS_PER_TILE),
                                 min_vis_height, 
                                 0.0f + ((td.tile_r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, -1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
    };

    /* Normals for top face get set at the end */
    struct face top = {
        .nw = (struct terrain_vert) {
            .pos    = (vec3_t) { 0.0f - (td.tile_c * X_COORDS_PER_TILE),
                                 M_Tile_NWHeight(tile) * Y_COORDS_PER_TILE,
                                 0.0f + (td.tile_r * Z_COORDS_PER_TILE) },
            .uv     = (vec2_t) { 0.0f, 1.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .ne = (struct terrain_vert) {
            .pos    = (vec3_t) { 0.0f - ((td.tile_c+1) * X_COORDS_PER_TILE), 
                                 M_Tile_NEHeight(tile) * Y_COORDS_PER_TILE,
                                 0.0f + (td.tile_r * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 1.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .se = (struct terrain_vert) {
            .pos    = (vec3_t) { 0.0f - ((td.tile_c+1) * X_COORDS_PER_TILE), 
                                 M_Tile_SEHeight(tile) * Y_COORDS_PER_TILE,
                                 0.0f + ((td.tile_r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
        .sw = (struct terrain_vert) {
            .pos    = (vec3_t) { 0.0f - (td.tile_c * X_COORDS_PER_TILE), 
                                 M_Tile_SWHeight(tile) * Y_COORDS_PER_TILE,
                                 0.0f + ((td.tile_r+1) * Z_COORDS_PER_TILE) }, 
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .material_idx  = tile->top_mat_idx,
        },
    };

#define V_COORD(width, height) (((float)height)/width)

    struct face back = {
        .nw = (struct terrain_vert) {
            .pos    = top.ne.pos,
            .uv     = (vec2_t) { 0.0f, V_COORD(X_COORDS_PER_TILE, top.ne.pos.y) },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .ne = (struct terrain_vert) {
            .pos    = top.nw.pos,
            .uv     = (vec2_t) { 1.0f, V_COORD(X_COORDS_PER_TILE, top.nw.pos.y) },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .se = (struct terrain_vert) {
            .pos    = bot.ne.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .sw = (struct terrain_vert) {
            .pos    = bot.nw.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, -1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
    };

    struct face front = {
        .nw = (struct terrain_vert) {
            .pos    = top.sw.pos,
            .uv     = (vec2_t) { 0.0f, V_COORD(X_COORDS_PER_TILE, top.sw.pos.y) },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .ne = (struct terrain_vert) {
            .pos    = top.se.pos,
            .uv     = (vec2_t) { 1.0f, V_COORD(X_COORDS_PER_TILE, top.se.pos.y) },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .se = (struct terrain_vert) {
            .pos    = bot.sw.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .sw = (struct terrain_vert) {
            .pos    = bot.se.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 0.0f, 0.0f, 1.0f },
            .material_idx  = tile->sides_mat_idx,
        },
    };

    struct face left = {
        .nw = (struct terrain_vert) {
            .pos    = top.nw.pos,
            .uv     = (vec2_t) { 0.0f, V_COORD(X_COORDS_PER_TILE, top.nw.pos.y) },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .ne = (struct terrain_vert) {
            .pos    = top.sw.pos,
            .uv     = (vec2_t) { 1.0f, V_COORD(X_COORDS_PER_TILE, top.sw.pos.y) },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .se = (struct terrain_vert) {
            .pos    = bot.se.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .sw = (struct terrain_vert) {
            .pos    = bot.ne.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { 1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
    };

    struct face right = {
        .nw = (struct terrain_vert) {
            .pos    = top.se.pos,
            .uv     = (vec2_t) { 0.0f, V_COORD(X_COORDS_PER_TILE, top.se.pos.y) },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .ne = (struct terrain_vert) {
            .pos    = top.ne.pos,
            .uv     = (vec2_t) { 1.0f, V_COORD(X_COORDS_PER_TILE, top.ne.pos.y) },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .se = (struct terrain_vert) {
            .pos    = bot.nw.pos,
            .uv     = (vec2_t) { 1.0f, 0.0f },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
        .sw = (struct terrain_vert) {
            .pos    = bot.sw.pos,
            .uv     = (vec2_t) { 0.0f, 0.0f },
            .normal = (vec3_t) { -1.0f, 0.0f, 0.0f },
            .material_idx  = tile->sides_mat_idx,
        },
    };

#undef V_COORD

    struct face *faces[] = {
        &front, &back, &left, &right 
    };

    for(int i = 0; i < ARR_SIZE(faces); i++) {

        struct face *curr = faces[i];

        /* First triangle */
        memcpy(out + (i * VERTS_PER_SIDE_FACE) + 0, &curr->nw, sizeof(struct terrain_vert));
        memcpy(out + (i * VERTS_PER_SIDE_FACE) + 1, &curr->ne, sizeof(struct terrain_vert));
        memcpy(out + (i * VERTS_PER_SIDE_FACE) + 2, &curr->sw, sizeof(struct terrain_vert));

        /* Second triangle */
        memcpy(out + (i * VERTS_PER_SIDE_FACE) + 3, &curr->se, sizeof(struct terrain_vert));
        memcpy(out + (i * VERTS_PER_SIDE_FACE) + 4, &curr->sw, sizeof(struct terrain_vert));
        memcpy(out + (i * VERTS_PER_SIDE_FACE) + 5, &curr->ne, sizeof(struct terrain_vert));
    }

    /* Lastly, the top face. Unlike the other five faces, it can have different 
     * normals for its' two triangles, and the triangles can be arranged differently 
     * at corner tiles. 
     */

    vec3_t top_tri_normals[2];
    bool   top_tri_left_aligned;
    tile_top_normals(tile, top_tri_normals, &top_tri_left_aligned);

    /*
     * CONFIG 1 (left-aligned)   CONFIG 2
     * (nw)      (ne)            (nw)      (ne)
     * +---------+               +---------+
     * |Tri1   / |               | \   Tri1|
     * |     /   |               |   \     |
     * |   /     |               |     \   |
     * | /   Tri0|               |Tri0   \ |
     * +---------+               +---------+
     * (sw)      (se)            (sw)      (se)
     */

    float center_height = 
          TILETYPE_IS_RAMP(tile->type)          ? (tile->base_height + tile->ramp_height / 2.0f) 
        : TILETYPE_IS_CORNER_CONVEX(tile->type) ? (tile->base_height + tile->ramp_height) 
        : (tile->base_height);

    vec3_t center_vert_pos = (vec3_t) {
        top.nw.pos.x - X_COORDS_PER_TILE / 2.0f, 
        center_height * Y_COORDS_PER_TILE, 
        top.nw.pos.z + Z_COORDS_PER_TILE / 2.0f
    };

    bool tri0_side_mat = fabs(top_tri_normals[0].y) < 1.0 && (tile->ramp_height > 1);
    bool tri1_side_mat = fabs(top_tri_normals[1].y) < 1.0 && (tile->ramp_height > 1);
	int tri0_idx = tri0_side_mat ? tile->sides_mat_idx : tile->top_mat_idx;
	int tri1_idx = tri1_side_mat ? tile->sides_mat_idx : tile->top_mat_idx;

    struct terrain_vert center_vert_tri0 = (struct terrain_vert) {
        .pos    = center_vert_pos,
        .uv     = (vec2_t){0.5f, 0.5f},
        .normal = top_tri_normals[0],
        .material_idx = tri0_idx
    };
    struct terrain_vert center_vert_tri1 = (struct terrain_vert) {
        .pos    = center_vert_pos,
        .uv     = (vec2_t){0.5f, 0.5f},
        .normal = top_tri_normals[1],
        .material_idx = tri1_idx
    };

    struct terrain_vert north_vert = (struct terrain_vert) {
        .pos    = (vec3_t){
            (top.ne.pos.x + top.nw.pos.x)/2, 
            (top.ne.pos.y + top.nw.pos.y)/2, 
            (top.ne.pos.z + top.nw.pos.z)/2},
        .uv     = (vec2_t){0.5f, 1.0f}, 
        .normal = top_tri_normals[1],
        .material_idx = tri1_idx
    };
    struct terrain_vert south_vert = (struct terrain_vert) {
        .pos    = (vec3_t){
            (top.se.pos.x + top.sw.pos.x)/2, 
            (top.se.pos.y + top.sw.pos.y)/2, 
            (top.se.pos.z + top.sw.pos.z)/2},
        .uv     = (vec2_t){0.5f, 0.0f},
        .normal = top_tri_normals[0],
        .material_idx = tri0_idx
    };
    struct terrain_vert west_vert = (struct terrain_vert) {
        .pos    = (vec3_t){
            (top.sw.pos.x + top.nw.pos.x)/2, 
            (top.sw.pos.y + top.nw.pos.y)/2, 
            (top.sw.pos.z + top.nw.pos.z)/2},
        .uv     = (vec2_t){0.0f, 0.5f},
        .normal = top_tri_left_aligned ? top_tri_normals[1] : top_tri_normals[0],
        .material_idx = (top_tri_left_aligned ? tri1_idx : tri0_idx),
    };
    struct terrain_vert east_vert = (struct terrain_vert) {
        .pos    = (vec3_t){
            (top.se.pos.x + top.ne.pos.x)/2, 
            (top.se.pos.y + top.ne.pos.y)/2, 
            (top.se.pos.z + top.ne.pos.z)/2},
        .uv     = (vec2_t){1.0f, 0.5f},
        .normal = top_tri_left_aligned ? top_tri_normals[0] : top_tri_normals[1],
        .material_idx = (top_tri_left_aligned ? tri0_idx : tri1_idx),
    };

    assert(sizeof(union top_face_vbuff) == VERTS_PER_TOP_FACE * sizeof(struct terrain_vert));
    union top_face_vbuff *tfvb = (union top_face_vbuff*)(out + 4 * VERTS_PER_SIDE_FACE);
    tfvb->se0 = top.se;
    tfvb->s0 = south_vert;
    tfvb->center0 = center_vert_tri0;
    tfvb->center1 = center_vert_tri0;
    tfvb->s1 = south_vert;
    tfvb->sw0 = top.sw;
    tfvb->sw1 = top.sw;
    tfvb->w0 = west_vert;
    tfvb->center2 = top_tri_left_aligned ? center_vert_tri1 : center_vert_tri0;
    tfvb->center3 = top_tri_left_aligned ? center_vert_tri1 : center_vert_tri0;
    tfvb->w1 = west_vert;
    tfvb->nw0 = top.nw;
    tfvb->nw1 = top.nw;
    tfvb->n0 = north_vert;
    tfvb->center4 = center_vert_tri1;
    tfvb->center5 = center_vert_tri1;
    tfvb->n1 = north_vert;
    tfvb->ne0 = top.ne;
    tfvb->ne1 = top.ne;
    tfvb->e0 = east_vert;
    tfvb->center6 = top_tri_left_aligned ? center_vert_tri0 : center_vert_tri1;
    tfvb->center7 = top_tri_left_aligned ? center_vert_tri0 : center_vert_tri1;
    tfvb->e1 = east_vert;
    tfvb->se1 = top.se;

    /* Give a slight overlap to the triangles of the top face to make sure there 
     * no gap can appear between adjacent triangles due to interpolation errors */
    tfvb->center0.pos.z -= 0.005;
    tfvb->center1.pos.z -= 0.005;
    tfvb->center2.pos.x -= 0.005;
    tfvb->center3.pos.x -= 0.005;
    tfvb->center4.pos.z += 0.005;
    tfvb->center5.pos.z += 0.005;
    tfvb->center6.pos.x += 0.005;
    tfvb->center7.pos.x += 0.005;

    if(top_tri_left_aligned) {
        tfvb->se0.material_idx = tri0_idx;
        tfvb->sw0.material_idx = tri0_idx;
        tfvb->sw1.material_idx = tri1_idx;
        tfvb->nw0.material_idx = tri1_idx;
        tfvb->nw1.material_idx = tri1_idx;
        tfvb->ne0.material_idx = tri1_idx;
        tfvb->ne1.material_idx = tri0_idx;
        tfvb->se1.material_idx = tri0_idx;

        tfvb->se0.normal = top_tri_normals[0];
        tfvb->sw0.normal = top_tri_normals[0];
        tfvb->sw1.normal = top_tri_normals[1];
        tfvb->nw0.normal = top_tri_normals[1];
        tfvb->nw1.normal = top_tri_normals[1];
        tfvb->ne0.normal = top_tri_normals[1];
        tfvb->ne1.normal = top_tri_normals[0];
        tfvb->se1.normal = top_tri_normals[0];
    }else{
        tfvb->se0.material_idx = tri0_idx;
        tfvb->sw0.material_idx = tri0_idx;
        tfvb->sw1.material_idx = tri0_idx;
        tfvb->nw0.material_idx = tri0_idx;
        tfvb->nw1.material_idx = tri1_idx;
        tfvb->ne0.material_idx = tri1_idx;
        tfvb->ne1.material_idx = tri1_idx;
        tfvb->se1.material_idx = tri1_idx;

        tfvb->se0.normal = top_tri_normals[0];
        tfvb->sw0.normal = top_tri_normals[0];
        tfvb->sw1.normal = top_tri_normals[0];
        tfvb->nw0.normal = top_tri_normals[0];
        tfvb->nw1.normal = top_tri_normals[1];
        tfvb->ne0.normal = top_tri_normals[1];
        tfvb->ne1.normal = top_tri_normals[1];
        tfvb->se1.normal = top_tri_normals[1];
    }

    for(struct terrain_vert *curr_provoking = out; 
        curr_provoking < out + (4 * VERTS_PER_SIDE_FACE); 
        curr_provoking += 3) {

        curr_provoking->blend_mode = BLEND_MODE_NOBLEND;
    }
    for(struct terrain_vert *curr_provoking = out + (4 * VERTS_PER_SIDE_FACE); 
        curr_provoking < out + VERTS_PER_TILE; 
        curr_provoking += 3) {

        curr_provoking->blend_mode = tile->blend_mode;
    }

    PERF_RETURN_VOID();
}

int R_TileGetTriMesh(const struct map *map, struct tile_desc *td, mat4x4_t *model, vec3_t out[])
{
    PERF_ENTER();

    struct terrain_vert verts[VERTS_PER_TILE];
    R_TileGetVertices(map, *td, verts);
    int i = 0;

    for(; i < ARR_SIZE(verts); i++) {

        vec4_t pos_homo = (vec4_t){verts[i].pos.x, verts[i].pos.y, verts[i].pos.z, 1.0f};
        vec4_t ws_pos_homo;
        PFM_Mat4x4_Mult4x1(model, &pos_homo, &ws_pos_homo);

        out[i] = (vec3_t){
            ws_pos_homo.x / ws_pos_homo.w, 
            ws_pos_homo.y / ws_pos_homo.w, 
            ws_pos_homo.z / ws_pos_homo.w
        };
    }

    assert(i % 3 == 0);
    PERF_RETURN(i);
}

