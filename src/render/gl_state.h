/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2023 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or *  (at your option) any later version.
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

#ifndef GL_STATE_H
#define GL_STATE_H

#include "../pf_math.h"
#include <stdbool.h>
#include <GL/glew.h>

#define GL_U_PROJECTION         "projection"
#define GL_U_VIEW               "view"
#define GL_U_VIEW_POS           "view_pos"
#define GL_U_VIEW_DIR           "view_dir"
#define GL_U_VIEW_ROT_MAT       "view_rot"
#define GL_U_MODEL              "model"
#define GL_U_MATERIALS          "materials"
#define GL_U_INV_BIND_MATS      "anim_inv_bind_mats"
#define GL_U_CURR_POSE_MATS     "anim_curr_pose_mats"
#define GL_U_NORMAL_MAT         "anim_normal_mat"
#define GL_U_TEXTURE0           "texture0"
#define GL_U_TEXTURE1           "texture1"
#define GL_U_TEXTURE2           "texture2"
#define GL_U_TEXTURE3           "texture3"
#define GL_U_TEXTURE4           "texture4"
#define GL_U_TEXTURE5           "texture5"
#define GL_U_TEXTURE6           "texture6"
#define GL_U_TEXTURE7           "texture7"
#define GL_U_TEXTURE8           "texture8"
#define GL_U_TEXTURE9           "texture9"
#define GL_U_TEXTURE10          "texture10"
#define GL_U_TEXTURE11          "texture11"
#define GL_U_TEXTURE12          "texture12"
#define GL_U_TEXTURE13          "texture13"
#define GL_U_TEXTURE14          "texture14"
#define GL_U_TEXTURE15          "texture15"
#define GL_U_TEX_ARRAY0         "tex_array0"
#define GL_U_TEX_ARRAY1         "tex_array1"
#define GL_U_TEX_ARRAY2         "tex_array2"
#define GL_U_TEX_ARRAY3         "tex_array3"
#define GL_U_AMBIENT_COLOR      "ambient_color"
#define GL_U_LIGHT_POS          "light_pos"
#define GL_U_LIGHT_COLOR        "light_color"
#define GL_U_LS_TRANS           "light_space_transform"
#define GL_U_SHADOW_MAP         "shadow_map"
#define GL_U_HEIGHT_MAP         "height_map"
#define GL_U_SPLAT_MAP          "splat_map"
#define GL_U_SKYBOX             "skybox"
#define GL_U_ENT_TOP_OFFSETS_SS "ent_top_offsets_ss"
#define GL_U_ENT_HEALTH_PC      "ent_health_pc"
#define GL_U_CURR_RES           "curr_res"
#define GL_U_COLOR              "color"
#define GL_U_CLIP_PLANE0        "clip_plane0"
#define GL_U_MOVE_FACTOR        "water_move_factor"
#define GL_U_DUDV_MAP           "water_dudv_map"
#define GL_U_NORMAL_MAP         "water_normal_map"
#define GL_U_REFRACT_TEX        "refraction_tex"
#define GL_U_REFLECT_TEX        "reflection_tex"
#define GL_U_REFRACT_DEPTH      "refraction_depth"
#define GL_U_CAM_NEAR           "cam_near"
#define GL_U_CAM_FAR            "cam_far"
#define GL_U_WATER_TILING       "water_tiling"
#define GL_U_MAP_RES            "map_resolution"
#define GL_U_MAP_POS            "map_pos"
#define GL_U_ATTR_STRIDE        "attr_stride"
#define GL_U_ATTR_OFFSET        "attr_offset"
#define GL_U_SPLATS             "splats"
#define GL_U_POSEBUFF           "posebuff"
#define GL_U_INV_BIND_MAT_OFFSET "inv_bind_mats_offset"
#define GL_U_CURR_POSE_MAT_OFFSET "curr_pose_mats_offset"
#define GL_U_TICKS_HZ           "ticks_hz"
#define GL_U_SHADOWS_ON         "shadows_on"
#define GL_U_NUM_SIM_ENTS       "num_sim_ents"
#define GL_U_SPRITES            "sprites"
#define GL_U_SPRITE_SHEET       "sprite_sheet"
#define GL_U_SPRITE_NROWS       "sprite_nrows"
#define GL_U_SPRITE_NCOLS       "sprite_ncols"

enum utype{
    UTYPE_FLOAT,
    UTYPE_VEC2,
    UTYPE_VEC3,
    UTYPE_VEC4,
    UTYPE_INT,
    UTYPE_IVEC2,
    UTYPE_IVEC3,
    UTYPE_IVEC4,
    UTYPE_MAT3,
    UTYPE_MAT4,
    UTYPE_MAT4_ARR,
    UTYPE_COMPOSITE,
    UTYPE_ARRAY,
    UTYPE_BLOCK_BINDING,
};

struct mdesc{
    const char *name;
    enum utype  type; 
    ptrdiff_t   offset;
};

struct uval{
    enum utype type;
    union{
        GLfloat  as_float;
        vec2_t   as_vec2;
        vec3_t   as_vec3;
        vec4_t   as_vec4;
        GLint    as_int;
        GLint    as_ivec2[2];
        GLint    as_ivec3[3];
        GLint    as_ivec4[4];
        mat3x3_t as_mat3;
        mat4x4_t as_mat4;
    }val;
};

bool R_GL_StateInit(void);
void R_GL_StateShutdown(void);

void R_GL_StateSet(const char *uname, struct uval val);
bool R_GL_StateGet(const char *uname, struct uval *out);
void R_GL_StateSetArray(const char *uname, enum utype itemtype, size_t size, void *data);
void R_GL_StateSetComposite(const char *uname, const struct mdesc *descs, 
                            size_t itemsize, size_t nitems, void *data);
void R_GL_StateSetBlockBinding(const char *uname, GLuint binding);

/* The shader program must have been used before installing the uniforms */
void R_GL_StateInstall(const char *uname, GLuint shader_prog);

void R_GL_StatePushRenderTarget(GLuint fbo);
void R_GL_StatePopRenderTarget(void);

#endif

