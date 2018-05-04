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

#include "render_gl.h"
#include "render_private.h"
#include "mesh.h"
#include "vertex.h"
#include "shader.h"
#include "material.h"
#include "gl_assert.h"
#include "public/render.h"
#include "../entity.h"
#include "../gl_uniforms.h"
#include "../camera.h"
#include "../config.h"
#include "../anim/public/skeleton.h"
#include "../anim/public/anim.h"
#include "../map/public/tile.h"
#include "../map/public/map.h"

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

static void r_gl_set_materials(GLuint shader_prog, size_t num_mats, const struct material *mats)
{
    for(size_t i = 0; i < num_mats; i++) {
    
        const struct material *mat = &mats[i];
        const size_t nmembers = 3; 

        const struct member_desc{
            const GLchar *name; 
            size_t        size;
            ptrdiff_t     offset;
        }descs[] = {
            {"ambient_intensity", 1, offsetof(struct material, ambient_intensity) },
            {"diffuse_clr",       3, offsetof(struct material, diffuse_clr)       },
            {"specular_clr",      3, offsetof(struct material, specular_clr)      }
        };

        for(size_t j = 0; j < nmembers; j++) {
        
            char locbuff[64];
            GLuint loc;

            snprintf(locbuff, sizeof(locbuff), "%s[%zu].%s", GL_U_MATERIALS, i, descs[j].name);
            locbuff[sizeof(locbuff)-1] = '\0';

            loc = glGetUniformLocation(shader_prog, locbuff);
            switch(descs[j].size) {
            case 1: glUniform1fv(loc, 1, (void*) ((char*)mat + descs[j].offset) ); break;
            case 3: glUniform3fv(loc, 1, (void*) ((char*)mat + descs[j].offset) ); break;
            default: assert(0);
            }

        }
    }
}

static void r_gl_set_uniform_mat4x4_array(mat4x4_t *data, size_t count, 
                                          const char *uname, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = R_Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, uname);
    glUniformMatrix4fv(loc, count, GL_FALSE, (void*)data);
}

static void r_gl_set_uniform_vec4_array(vec4_t *data, size_t count, 
                                        const char *uname, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = R_Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, uname);
    glUniform4fv(loc, count, (void*)data);
}

static void r_gl_set_view(const mat4x4_t *view, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = R_Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, GL_U_VIEW);
    glUniformMatrix4fv(loc, 1, GL_FALSE, view->raw);
}

static void r_gl_set_proj(const mat4x4_t *proj, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = R_Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, GL_U_PROJECTION);
    glUniformMatrix4fv(loc, 1, GL_FALSE, proj->raw);
}

static void r_gl_set_view_pos(const vec3_t *pos, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = R_Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, GL_U_VIEW_POS);
    glUniform3fv(loc, 1, pos->raw);
}

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

void R_GL_Init(struct render_private *priv, const char *shader, const struct vertex *vbuff)
{
    struct mesh *mesh = &priv->mesh;

    glGenVertexArrays(1, &mesh->VAO);
    glBindVertexArray(mesh->VAO);

    glGenBuffers(1, &mesh->VBO);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->VBO);
    glBufferData(GL_ARRAY_BUFFER, mesh->num_verts * sizeof(struct vertex), vbuff, GL_STATIC_DRAW);

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

    /* Attribute 3 - material index */
    glVertexAttribIPointer(3, 1, GL_INT, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, material_idx));
    glEnableVertexAttribArray(3);

    if(0 == strcmp("mesh.animated.textured-phong", shader)) {
    
        /* Attribute 4 - joint indices */
        glVertexAttribPointer(4, 4, GL_INT, GL_FALSE, sizeof(struct vertex),
            (void*)offsetof(struct vertex, joint_indices));
        glEnableVertexAttribArray(4);  

        /* Attribute 5 - joint weights */
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(struct vertex),
            (void*)offsetof(struct vertex, weights));
        glEnableVertexAttribArray(5);  

    }else if(0 == strcmp("terrain", shader)) {

        /* Attribute 4 - tile texture blend mode */
        glVertexAttribIPointer(4, 1, GL_INT, sizeof(struct vertex), 
            (void*)offsetof(struct vertex, blend_mode));
        glEnableVertexAttribArray(4);
         
        /* Attribute 5 - adjacent material indices */
        glVertexAttribIPointer(5, 4, GL_INT, sizeof(struct vertex), 
            (void*)offsetof(struct vertex, adjacent_mat_indices));
        glEnableVertexAttribArray(5);
    }

    priv->shader_prog = R_Shader_GetProgForName(shader);
}

void R_GL_Draw(const void *render_private, mat4x4_t *model)
{
    const struct render_private *priv = render_private;
    GLuint loc;

    glUseProgram(priv->shader_prog);

    loc = glGetUniformLocation(priv->shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model->raw);

    r_gl_set_materials(priv->shader_prog, priv->num_materials, priv->materials);

    for(int i = 0; i < priv->num_materials; i++) {
        R_Texture_GL_Activate(&priv->materials[i].texture, priv->shader_prog);
    }

    glBindVertexArray(priv->mesh.VAO);
    glDrawArrays(GL_TRIANGLES, 0, priv->mesh.num_verts);
}

void R_GL_SetViewMatAndPos(const mat4x4_t *view, const vec3_t *pos)
{
    const char *shaders[] = {
        "mesh.static.colored",
        "mesh.static.textured",
        "mesh.static.textured-phong",
        "mesh.static.tile-outline",
        "mesh.static.normals.colored",
        "mesh.animated.textured-phong",
        "mesh.animated.normals.colored",
        "terrain"
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {

        r_gl_set_view(view, shaders[i]);
        r_gl_set_view_pos(pos, shaders[i]);
    }
}

void R_GL_SetProj(const mat4x4_t *proj)
{
    const char *shaders[] = {
        "mesh.static.colored",
        "mesh.static.textured",
        "mesh.static.textured-phong",
        "mesh.static.tile-outline",
        "mesh.static.normals.colored",
        "mesh.animated.textured-phong",
        "mesh.animated.normals.colored",
        "terrain"
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++)
        r_gl_set_proj(proj, shaders[i]);
}

void R_GL_SetAnimUniformMat4x4Array(mat4x4_t *data, size_t count, const char *uname)
{
    const char *shaders[] = {
        "mesh.animated.textured-phong",
        "mesh.animated.normals.colored",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++)
        r_gl_set_uniform_mat4x4_array(data, count, uname, shaders[i]);
}

void R_GL_SetAnimUniformVec4Array(vec4_t *data, size_t count, const char *uname)
{
    const char *shaders[] = {
        "mesh.animated.textured-phong",
        "mesh.animated.normals.colored",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++)
        r_gl_set_uniform_vec4_array(data, count, uname, shaders[i]);
}

void R_GL_SetAmbientLightColor(vec3_t color)
{
    const char *shaders[] = {
        "mesh.static.textured-phong",
        "mesh.animated.textured-phong",
        "terrain"
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {
    
        GLuint loc, shader_prog;

        shader_prog = R_Shader_GetProgForName(shaders[i]);
        glUseProgram(shader_prog);

        loc = glGetUniformLocation(shader_prog, GL_U_AMBIENT_COLOR);
        glUniform3fv(loc, 1, color.raw);
    }
}

void R_GL_SetLightEmitColor(vec3_t color)
{
    const char *shaders[] = {
        "mesh.static.textured-phong",
        "mesh.animated.textured-phong",
        "terrain"
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {
    
        GLuint loc, shader_prog;

        shader_prog = R_Shader_GetProgForName(shaders[i]);
        glUseProgram(shader_prog);

        loc = glGetUniformLocation(shader_prog, GL_U_LIGHT_COLOR);
        glUniform3fv(loc, 1, color.raw);
    }
}

void R_GL_SetLightPos(vec3_t pos)
{
    const char *shaders[] = {
        "mesh.static.textured-phong",
        "mesh.animated.textured-phong",
        "terrain"
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {

        GLuint loc, shader_prog;
    
        shader_prog = R_Shader_GetProgForName(shaders[i]);
        glUseProgram(shader_prog);

        loc = glGetUniformLocation(shader_prog, GL_U_LIGHT_POS);
        glUniform3fv(loc, 1, pos.raw);
    }
}

void R_GL_DrawSkeleton(const struct entity *ent, const struct skeleton *skel)
{
    vec3_t *vbuff;
    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;
    vec3_t green = (vec3_t){0.0f, 1.0f, 0.0f};

    /* Our vbuff looks like this:
     * +----------------+-------------+--------------+-----
     * | joint root 0   | joint tip 0 | joint root 1 | ...
     * +----------------+-------------+--------------+-----
     */
    vbuff = calloc(skel->num_joints * 2, sizeof(vec3_t));

    for(int i = 0, vbuff_idx = 0; i < skel->num_joints; i++, vbuff_idx +=2) {

        struct joint *curr = &skel->joints[i];
        struct SQT *sqt = &skel->bind_sqts[i];

        vec4_t homo = (vec4_t){0.0f, 0.0f, 0.0f, 1.0f}; 
        vec4_t result;

        mat4x4_t bind_pose;
        PFM_Mat4x4_Inverse(&skel->inv_bind_poses[i], &bind_pose);

        /* The root of the bone in object space */
        PFM_Mat4x4_Mult4x1(&bind_pose, &homo, &result);
        vbuff[vbuff_idx] = (vec3_t){result.x, result.y ,result.z};
    
        /* The tip of the bone in object space */
        homo = (vec4_t){curr->tip.x, curr->tip.y, curr->tip.z, 1.0f}; 
        PFM_Mat4x4_Mult4x1(&bind_pose, &homo, &result);
        vbuff[vbuff_idx + 1] = (vec3_t){result.x, result.y ,result.z};
    }
 
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, skel->num_joints * sizeof(vec3_t) * 2, vbuff, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    shader_prog = R_Shader_GetProgForName("mesh.static.colored");
    glUseProgram(shader_prog);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform3fv(loc, 1, green.raw);

    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);

    mat4x4_t model;
    Entity_ModelMatrix(ent, &model);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model.raw);

    glPointSize(5.0f);

    glBindVertexArray(VAO);
    glDrawArrays(GL_POINTS, 0, skel->num_joints * 2);
    glDrawArrays(GL_LINES, 0, skel->num_joints * 2);

cleanup:
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    free(vbuff);
}

void R_GL_DrawOrigin(const void *render_private, mat4x4_t *model)
{
    vec3_t vbuff[2];
    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;

    vec3_t red   = (vec3_t){1.0f, 0.0f, 0.0f};
    vec3_t green = (vec3_t){0.0f, 1.0f, 0.0f};
    vec3_t blue  = (vec3_t){0.0f, 0.0f, 1.0f};

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    shader_prog = R_Shader_GetProgForName("mesh.static.colored");
    glUseProgram(shader_prog);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model->raw);

    /* Set line width */
    GLfloat old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(3.0f);

    /* Render the 3 axis lines at the origin */
    vbuff[0] = (vec3_t){0.0f, 0.0f, 0.0f};
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);

    for(int i = 0; i < 3; i++) {

        switch(i) {
        case 0:
            vbuff[1] = (vec3_t){1.0f, 0.0f, 0.0f}; 
            glUniform3fv(loc, 1, red.raw);
            break;
        case 1:
            vbuff[1] = (vec3_t){0.0f, 1.0f, 0.0f}; 
            glUniform3fv(loc, 1, green.raw);
            break;
        case 2:
            vbuff[1] = (vec3_t){0.0f, 0.0f, 1.0f}; 
            glUniform3fv(loc, 1, blue.raw);
            break;
        }
    
        glBufferData(GL_ARRAY_BUFFER, 2 * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);

        glBindVertexArray(VAO);
        glDrawArrays(GL_LINES, 0, 2);
    }
    glLineWidth(old_width);

cleanup:
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

void R_GL_DrawRay(vec3_t origin, vec3_t dir, mat4x4_t *model)
{
    vec3_t vbuff[2];
    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;
    vec3_t red = (vec3_t){1.0f, 0.0f, 0.0f};

    vbuff[0] = origin; 
    PFM_Vec3_Normal(&dir, &dir);
    PFM_Vec3_Scale(&dir, 1000.0f, &dir);
    PFM_Vec3_Add(&origin, &dir, &vbuff[1]);

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    shader_prog = R_Shader_GetProgForName("mesh.static.colored");
    glUseProgram(shader_prog);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model->raw);

    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform3fv(loc, 1, red.raw);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, 2 * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);

    glBindVertexArray(VAO);
    glDrawArrays(GL_LINES, 0, 2);

cleanup:
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

void R_GL_DrawTileSelected(const struct tile_desc *in, const void *chunk_rprivate, mat4x4_t *model, 
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

void R_GL_DrawNormals(const void *render_private, mat4x4_t *model, bool anim)
{
    const struct render_private *priv = render_private;


    GLuint normals_shader = anim ? R_Shader_GetProgForName("mesh.animated.normals.colored")
                                 : R_Shader_GetProgForName("mesh.static.normals.colored");
    assert(normals_shader);
    glUseProgram(normals_shader);

    GLuint loc;
    vec3_t yellow = (vec3_t){1.0f, 1.0f, 0.0f};

    loc = glGetUniformLocation(normals_shader, GL_U_COLOR);
    glUniform3fv(loc, 1, yellow.raw);

    loc = glGetUniformLocation(normals_shader, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model->raw);

    glBindVertexArray(priv->mesh.VAO);
    glDrawArrays(GL_TRIANGLES, 0, priv->mesh.num_verts);
}

void R_GL_PatchTileVertsBlend(GLuint VBO, const struct tile *tiles, int width, int height, int r, int c)
{
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
    //&vbuff[VERTS_PER_TILE * (r * width + c)];
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

    glFlushMappedNamedBufferRange(VBO, offset, length);
    glUnmapNamedBuffer(VBO);
}

void R_GL_VerticesFromTile(const struct tile *tile, struct vertex *out, size_t r, size_t c)
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

    struct vertex center_vert = (struct vertex) {
        .pos    = (vec3_t) {top.nw.pos.x - X_COORDS_PER_TILE / 2.0f, 
                            center_height * Y_COORDS_PER_TILE, 
                            top.nw.pos.z + Z_COORDS_PER_TILE / 2.0f},
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

    memcpy(out + (5 * VERTS_PER_FACE) + 6, second_tri[0], sizeof(struct vertex));
    memcpy(out + (5 * VERTS_PER_FACE) + 7, second_tri[1], sizeof(struct vertex));
    memcpy(out + (5 * VERTS_PER_FACE) + 8, &center_vert, sizeof(struct vertex));

    memcpy(out + (5 * VERTS_PER_FACE) + 9, &center_vert, sizeof(struct vertex));
    memcpy(out + (5 * VERTS_PER_FACE) + 10, second_tri[2], sizeof(struct vertex));
    memcpy(out + (5 * VERTS_PER_FACE) + 11, (top_tri_left_aligned ? second_tri[0] : second_tri[1]), sizeof(struct vertex));

}

int R_GL_TriMeshForTile(const struct tile_desc *in, const void *chunk_rprivate, 
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

void R_GL_DumpFramebuffer_PPM(const char *filename, int width, int height)
{
    long img_size = width * height * 3;
    unsigned char *data = malloc(img_size);
    if(!data) {
        return;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, data);

    FILE *file = fopen(filename, "wb");
    if(!file) {
        free(data); 
        return;
    }

    fprintf(file, "P6\n%d %d\n%d\n", width, height, 255);
    for(int i = 0; i < height; i++) {
        for(int j = 0; j < width; j++) {

            static unsigned char color[3];
            color[0] = data[3*i*width + 3*j    ];
            color[1] = data[3*i*width + 3*j + 1];
            color[2] = data[3*i*width + 3*j + 2];
            fwrite(color, 1, 3, file);
        }
    }

    fclose(file);
    free(data);
}

void R_GL_UpdateTile(void *chunk_rprivate, int r, int c, int tiles_width, int tiles_height, 
                     const struct tile *tiles)
{
    struct render_private *priv = chunk_rprivate;
    const struct tile *tile = &tiles[r * tiles_width + c];

    size_t offset = (r * tiles_width + c) * VERTS_PER_TILE * sizeof(struct vertex);
    size_t length = VERTS_PER_TILE * sizeof(struct vertex);
    struct vertex *vert_base = glMapNamedBufferRange(priv->mesh.VBO, offset, length, GL_MAP_WRITE_BIT);
    assert(vert_base);
    
    R_GL_VerticesFromTile(tile, vert_base, r, c);

    glFlushMappedNamedBufferRange(priv->mesh.VBO, offset, length);
    glUnmapNamedBuffer(priv->mesh.VBO);

    for(int r_curr = r - 1; r_curr < r + 2; r_curr++){
        for(int c_curr = c - 1; c_curr < c + 2; c_curr++) {
        
            if(r_curr < 0 || r_curr >= tiles_height)
                continue;
            if(c_curr < 0 || c_curr >= tiles_width)
                continue;

            R_GL_PatchTileVertsBlend(priv->mesh.VBO, tiles, tiles_width, tiles_height, r_curr, c_curr);
        }
    }
}

void *R_GL_BakeChunk(const void *chunk_rprivate_tiles, vec3_t chunk_center, mat4x4_t *model,
                     int tiles_per_chunk_x, int tiles_per_chunk_z, const struct tile *tiles,
                     int chunk_r, int chunk_c)
{
    /* Note that we already include the phong lighting information in the pre-baked chunk. This
     * means that the pre-baked terrain cannot change lighting in real-time. It is possible 
     * to render the top surface texture with lighting disabled and then light in in real-time
     * but this makes dealing with all the different tile materials a headache. We would either
     * need to blend them so there are no sharp edges (slow) or use a single material for the 
     * entire top surface.*/

    const struct render_private *og_priv = chunk_rprivate_tiles;
    glUseProgram(og_priv->shader_prog);

    /* Create a new camera, with orthographic projection, centered 
     * over the chunk and facing straight down. */
    DECL_CAMERA_STACK(chunk_cam);
    memset(&chunk_cam, 0, g_sizeof_camera);

    vec3_t offset = (vec3_t){0.0f, 200.0f, 0.0f};
    PFM_Vec3_Add(&chunk_center, &offset, &chunk_center);

    Camera_SetPos((struct camera*)chunk_cam, chunk_center);
    Camera_SetPitchAndYaw((struct camera*)chunk_cam, -90.0f, 90.0f);

    vec2_t bot_left  = (vec2_t){-(X_COORDS_PER_TILE * tiles_per_chunk_x/2),  (Z_COORDS_PER_TILE * tiles_per_chunk_z/2)};
    vec2_t top_right = (vec2_t){ (X_COORDS_PER_TILE * tiles_per_chunk_x/2), -(Z_COORDS_PER_TILE * tiles_per_chunk_z/2)};
    Camera_TickFinishOrthographic((struct camera*)chunk_cam, bot_left, top_right);

    /* Next, create a new framebuffer and texture that we will render our chunk 
     * top-down view to. */
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);

    GLuint rendered_tex;
    glGenTextures(1, &rendered_tex);

    glBindTexture(GL_TEXTURE_2D, rendered_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, CONFIG_BAKED_TILE_TEX_RES * tiles_per_chunk_x, CONFIG_BAKED_TILE_TEX_RES * tiles_per_chunk_z, 
        0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, rendered_tex, 0);

    GLenum draw_buffers[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, draw_buffers);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        goto fail_fb;

    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glViewport(0,0, CONFIG_BAKED_TILE_TEX_RES * tiles_per_chunk_x, CONFIG_BAKED_TILE_TEX_RES * tiles_per_chunk_x);

    /* Render the chunk top-down view to the texture. */
    R_GL_Draw(chunk_rprivate_tiles, model);

    /* Re-bind the default framebuffer when we're done rendering */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Now construct our new 'fast' render context. We have some unused memory at the 
     * end of the buffer but this is not a concern. */
    size_t max_buff_sz = sizeof(struct render_private)
                       + MATERIALS_PER_CHUNK * sizeof(struct material);
    struct render_private *ret = malloc(max_buff_sz);
    if(!ret)
        goto fail_alloc_ret;

    /*
     * Recall: render private buff layout:
     *
     *  +---------------------------------+ <-- base
     *  | struct render_private[1]        |
     *  +---------------------------------+
     *  | struct material[num_materials]  |
     *  +---------------------------------+
     */

    struct vertex *vbuff = malloc(og_priv->mesh.num_verts * sizeof(struct vertex));
    if(!vbuff)
        goto fail_alloc_vbuff;
    struct vertex *vbuff_curr = vbuff;

    /* First pass over the tiles - figure out which materials we need to keep */
    int side_mats_set[MATERIALS_PER_CHUNK];
    int num_side_mats = 0;

    for(int r = 0; r < tiles_per_chunk_z; r++) {
        for(int c = 0; c < tiles_per_chunk_x; c++) {

            const struct tile *curr_tile = &tiles[r * tiles_per_chunk_x + c];    

            if(!arr_contains(side_mats_set, num_side_mats, curr_tile->sides_mat_idx)) {
                side_mats_set[num_side_mats++] = curr_tile->sides_mat_idx;

                /* We need at least one free material slot for the baked top face texture. */
                if(num_side_mats > (MATERIALS_PER_CHUNK-1))
                    goto fail_side_mats_count;
            }
        }
    }

    int top_mat_idx = num_side_mats;
    assert(top_mat_idx >= 0 && top_mat_idx < MATERIALS_PER_CHUNK);

    /* Second pass over the tiles - fill the vbuff_curr and patch UV coordinates */
    int num_verts = 0;

    for(int r = 0; r < tiles_per_chunk_z; r++) {
        for(int c = 0; c < tiles_per_chunk_x; c++) {
            
            const struct tile *curr_tile = &tiles[r * tiles_per_chunk_x + c];

            struct vertex curr_tile_vbuff[VERTS_PER_TILE];
            R_GL_VerticesFromTile(curr_tile, curr_tile_vbuff, r, c);

            /* Order of faces for each tile in the vbuff_curr: bot, front, back, left, right, top.
             * Recall that the top face has double the number of triangles and vertices. */

            if(M_Tile_FrontFaceVisible(tiles, r, c)) {
                memcpy(vbuff_curr, curr_tile_vbuff + (VERTS_PER_FACE * 1), sizeof(struct vertex) * VERTS_PER_FACE); 
                for(int i = 0; i < VERTS_PER_FACE; i++) {
                    vbuff_curr[i].material_idx = arr_indexof(side_mats_set, num_side_mats, vbuff_curr[i].material_idx);
                    assert(vbuff_curr[i].material_idx >= 0);
                }
                num_verts += VERTS_PER_FACE; 
                vbuff_curr += VERTS_PER_FACE;
            }

            if(M_Tile_BackFaceVisible(tiles, r, c)) {
                memcpy(vbuff_curr, curr_tile_vbuff + (VERTS_PER_FACE * 2), sizeof(struct vertex) * VERTS_PER_FACE); 
                for(int i = 0; i < VERTS_PER_FACE; i++) {
                    vbuff_curr[i].material_idx = arr_indexof(side_mats_set, num_side_mats, vbuff_curr[i].material_idx);
                    assert(vbuff_curr[i].material_idx >= 0);
                }
                num_verts += VERTS_PER_FACE; 
                vbuff_curr += VERTS_PER_FACE;
            }

            if(M_Tile_LeftFaceVisible(tiles, r, c)) {
                memcpy(vbuff_curr, curr_tile_vbuff + (VERTS_PER_FACE * 3), sizeof(struct vertex) * VERTS_PER_FACE); 
                for(int i = 0; i < VERTS_PER_FACE; i++) {
                    vbuff_curr[i].material_idx = arr_indexof(side_mats_set, num_side_mats, vbuff_curr[i].material_idx);
                    assert(vbuff_curr[i].material_idx >= 0);
                }
                num_verts += VERTS_PER_FACE; 
                vbuff_curr += VERTS_PER_FACE;
            }

            if(M_Tile_RightFaceVisible(tiles, r, c)) {
                memcpy(vbuff_curr, curr_tile_vbuff + (VERTS_PER_FACE * 4), sizeof(struct vertex) * VERTS_PER_FACE); 
                for(int i = 0; i < VERTS_PER_FACE; i++) {
                    vbuff_curr[i].material_idx = arr_indexof(side_mats_set, num_side_mats, vbuff_curr[i].material_idx);
                    assert(vbuff_curr[i].material_idx >= 0);
                }
                num_verts += VERTS_PER_FACE; 
                vbuff_curr += VERTS_PER_FACE;
            }

            struct vertex sw = curr_tile_vbuff[(VERTS_PER_FACE * 5) + 0];
            struct vertex se = curr_tile_vbuff[(VERTS_PER_FACE * 5) + 1];
            struct vertex nw = curr_tile_vbuff[(VERTS_PER_FACE * 5) + 6];
            struct vertex ne = curr_tile_vbuff[(VERTS_PER_FACE * 5) + 7];

            /* Patch the UV coordinates of the top face */
            float u_frac = (1.0f / tiles_per_chunk_x);
            float v_frac = (1.0f / tiles_per_chunk_z);

            sw.material_idx = top_mat_idx;
            se.material_idx = top_mat_idx;
            nw.material_idx = top_mat_idx;
            ne.material_idx = top_mat_idx;

            sw.uv = (vec2_t){ c    * u_frac, (r+1) * v_frac};
            se.uv = (vec2_t){(c+1) * u_frac, (r+1) * v_frac};
            nw.uv = (vec2_t){ c    * u_frac, r     * v_frac};
            ne.uv = (vec2_t){(c+1) * u_frac, r     * v_frac};

            vec3_t top_tri_normals[2];
            bool   top_tri_left_aligned;
            r_gl_tile_top_normals(curr_tile, top_tri_normals, &top_tri_left_aligned);

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

            vbuff_curr[0] = sw;
            vbuff_curr[1] = se;
            vbuff_curr[3] = nw;
            vbuff_curr[4] = ne;

            if(top_tri_left_aligned) {
                vbuff_curr[2] = ne;
                vbuff_curr[5] = sw;
            }else {
                vbuff_curr[2] = nw; 
                vbuff_curr[5] = se;
            }

            for(int i = 0; i < 3; i++)
                vbuff_curr[i].normal = top_tri_normals[0];

            for(int i = 3; i < 6; i++)
                vbuff_curr[i].normal = top_tri_normals[1];

            num_verts += 6;
            vbuff_curr += 6;
        }
    }

    ret->mesh.num_verts = num_verts;
    ret->materials = (void*)(ret + 1);
    ret->num_materials = num_side_mats + 1;

    for(int i = 0; i < num_side_mats; i++) {
        ret->materials[i] = og_priv->materials[side_mats_set[i]];
        ret->materials[i].texture.tunit = GL_TEXTURE0 + i;
    }

    char texname[32];
    snprintf(texname, sizeof(texname), "__baked_chunk__.%d.%d", chunk_r, chunk_c);
    texname[sizeof(texname)-1] = '\0';
    R_Texture_AddExisting(texname, rendered_tex);

    ret->materials[top_mat_idx].texture.id = rendered_tex;
    ret->materials[top_mat_idx].texture.tunit = GL_TEXTURE0 + top_mat_idx;

    R_GL_Init(ret, "mesh.static.textured", vbuff);
    free(vbuff);
    return ret;

fail_side_mats_count:
    free(vbuff);
fail_alloc_vbuff:
    free(ret);
fail_alloc_ret:
    glDeleteTextures(1, &rendered_tex); 
    glDeleteFramebuffers(1, &fb);
fail_fb:
    return NULL;
}

