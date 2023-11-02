/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2023 Eduard Permyakov 
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

#include "public/render_ctrl.h"
#include "gl_shader.h"
#include "gl_assert.h"
#include "gl_state.h"
#include "gl_material.h"
#include "../main.h"
#include "../lib/public/pf_string.h"

#include <SDL.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#define SHADER_PATH_LEN 128
#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))

struct uniform{
    int           type;
    const char   *name;
};

struct shader{
    GLint           prog_id;
    const char     *name;
    const char     *vertex_path;
    const char     *geo_path;
    const char     *frag_path;
    const char     *compute_path;
    struct uniform *uniforms;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static GLuint s_curr_prog = 0;

/* Shader 'prog_id' will be initialized by R_GL_Shader_InitAll */
static struct shader s_shaders[] = {
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "mesh.static.colored",
        .vertex_path    = "shaders/vertex/basic.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/colored.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC4,      GL_U_COLOR,            },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "mesh.static.textured",
        .vertex_path    = "shaders/vertex/static.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/textured.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_VEC3,      GL_U_VIEW_POS          },
            { UTYPE_INT,       GL_U_TEX_ARRAY0        },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "mesh.static.textured-phong",
        .vertex_path    = "shaders/vertex/static.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/textured-phong.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_VEC3,      GL_U_AMBIENT_COLOR     },
            { UTYPE_VEC3,      GL_U_LIGHT_COLOR       },
            { UTYPE_VEC3,      GL_U_LIGHT_POS         },
            { UTYPE_VEC3,      GL_U_VIEW_POS          },
            { UTYPE_INT,       GL_U_TEX_ARRAY0        },
            { UTYPE_COMPOSITE, GL_U_MATERIALS,        },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "mesh.static.tile-outline",
        .vertex_path    = "shaders/vertex/static.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/tile-outline.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC4,      GL_U_COLOR,            },
            {0},
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "mesh.animated.textured-phong",
        .vertex_path    = "shaders/vertex/skinned.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/textured-phong.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_ARRAY,     GL_U_CURR_POSE_MATS    },
            { UTYPE_ARRAY,     GL_U_INV_BIND_MATS     },
            { UTYPE_MAT4,      GL_U_NORMAL_MAT        },
            { UTYPE_VEC3,      GL_U_AMBIENT_COLOR     },
            { UTYPE_VEC3,      GL_U_LIGHT_COLOR       },
            { UTYPE_VEC3,      GL_U_LIGHT_POS         },
            { UTYPE_VEC3,      GL_U_VIEW_POS          },
            { UTYPE_INT,       GL_U_TEX_ARRAY0        },
            { UTYPE_COMPOSITE, GL_U_MATERIALS,        },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "mesh.static.normals.colored",
        .vertex_path    = "shaders/vertex/static.glsl",
        .geo_path       = "shaders/geometry/normals.glsl",
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/colored.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_VEC4,      GL_U_COLOR,            },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "mesh.animated.normals.colored",
        .vertex_path    = "shaders/vertex/skinned.glsl",
        .geo_path       = "shaders/geometry/normals.glsl",
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/colored.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_ARRAY,     GL_U_CURR_POSE_MATS    },
            { UTYPE_ARRAY,     GL_U_INV_BIND_MATS     },
            { UTYPE_MAT4,      GL_U_NORMAL_MAT        },
            { UTYPE_VEC4,      GL_U_COLOR,            },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "mesh.static.colored-per-vert",
        .vertex_path    = "shaders/vertex/colored.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/colored-per-vert.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "terrain",
        .vertex_path    = "shaders/vertex/terrain.glsl",
        .compute_path   = NULL,
        .geo_path       = NULL,
        .frag_path      = "shaders/fragment/terrain.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_VEC3,      GL_U_AMBIENT_COLOR     },
            { UTYPE_VEC3,      GL_U_LIGHT_COLOR       },
            { UTYPE_VEC3,      GL_U_LIGHT_POS         },
            { UTYPE_VEC3,      GL_U_VIEW_POS          },
            { UTYPE_INT,       GL_U_TEX_ARRAY0        },
            { UTYPE_INT,       "visbuff",             },
            { UTYPE_INT,       "visbuff_offset",      },
            { UTYPE_IVEC4,     GL_U_MAP_RES,          },
            { UTYPE_VEC2,      GL_U_MAP_POS,          },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "terrain-shadowed",
        .vertex_path    = "shaders/vertex/terrain-shadowed.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/terrain-shadowed.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_MAT4,      GL_U_LS_TRANS          },
            { UTYPE_VEC3,      GL_U_AMBIENT_COLOR     },
            { UTYPE_VEC3,      GL_U_LIGHT_COLOR       },
            { UTYPE_VEC3,      GL_U_LIGHT_POS         },
            { UTYPE_VEC3,      GL_U_VIEW_POS          },
            { UTYPE_INT,       GL_U_TEX_ARRAY0        },
            { UTYPE_INT,       "visbuff",             },
            { UTYPE_INT,       "visbuff_offset",      },
            { UTYPE_IVEC4,     GL_U_MAP_RES,          },
            { UTYPE_VEC2,      GL_U_MAP_POS,          },
            { UTYPE_INT,       GL_U_SHADOW_MAP        },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "mesh.static.depth",
        .vertex_path    = "shaders/vertex/depth.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/passthrough.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_LS_TRANS          },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "batched.mesh.static.depth",
        .vertex_path    = "shaders/vertex/depth-batched.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/passthrough.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_MAT4,      GL_U_LS_TRANS          },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_INT,       GL_U_TEX_ARRAY0        },
            { UTYPE_INT,       GL_U_TEX_ARRAY1        },
            { UTYPE_INT,       GL_U_TEX_ARRAY2        },
            { UTYPE_INT,       GL_U_TEX_ARRAY3        },
            { UTYPE_INT,       "attrbuff"             },
            { UTYPE_INT,       "attrbuff_offset"      },
            { UTYPE_INT,       GL_U_ATTR_STRIDE       },
            { UTYPE_INT,       GL_U_ATTR_OFFSET       },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "mesh.animated.depth",
        .vertex_path    = "shaders/vertex/skinned-depth.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/passthrough.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_MAT4,      GL_U_LS_TRANS          },
            { UTYPE_ARRAY,     GL_U_CURR_POSE_MATS    },
            { UTYPE_ARRAY,     GL_U_INV_BIND_MATS     },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "batched.mesh.animated.depth",
        .vertex_path    = "shaders/vertex/skinned-depth-batched.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/passthrough.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_MAT4,      GL_U_LS_TRANS          },
            { UTYPE_INT,       GL_U_TEX_ARRAY0        },
            { UTYPE_INT,       GL_U_TEX_ARRAY1        },
            { UTYPE_INT,       GL_U_TEX_ARRAY2        },
            { UTYPE_INT,       GL_U_TEX_ARRAY3        },
            { UTYPE_INT,       "attrbuff"             },
            { UTYPE_INT,       "attrbuff_offset"      },
            { UTYPE_INT,       GL_U_ATTR_STRIDE       },
            { UTYPE_INT,       GL_U_ATTR_OFFSET       },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "mesh.static.textured-phong-shadowed",
        .vertex_path    = "shaders/vertex/static-shadowed.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/textured-phong-shadowed.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC3,      GL_U_AMBIENT_COLOR     },
            { UTYPE_VEC3,      GL_U_LIGHT_COLOR       },
            { UTYPE_VEC3,      GL_U_LIGHT_POS         },
            { UTYPE_VEC3,      GL_U_VIEW_POS          },
            { UTYPE_MAT4,      GL_U_LS_TRANS          },
            { UTYPE_COMPOSITE, GL_U_MATERIALS,        },
            { UTYPE_INT,       GL_U_SHADOW_MAP        },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "batched.mesh.static.textured-phong-shadowed",
        .vertex_path    = "shaders/vertex/static-shadowed-batched.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/textured-phong-shadowed-batched.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_MAT4,      GL_U_LS_TRANS          },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_VEC3,      GL_U_AMBIENT_COLOR     },
            { UTYPE_VEC3,      GL_U_LIGHT_COLOR       },
            { UTYPE_VEC3,      GL_U_LIGHT_POS         },
            { UTYPE_VEC3,      GL_U_VIEW_POS          },
            { UTYPE_INT,       GL_U_SHADOW_MAP        },
            { UTYPE_INT,       GL_U_TEX_ARRAY0        },
            { UTYPE_INT,       GL_U_TEX_ARRAY1        },
            { UTYPE_INT,       GL_U_TEX_ARRAY2        },
            { UTYPE_INT,       GL_U_TEX_ARRAY3        },
            { UTYPE_INT,       "attrbuff"             },
            { UTYPE_INT,       "attrbuff_offset"      },
            { UTYPE_INT,       GL_U_ATTR_STRIDE       },
            { UTYPE_INT,       GL_U_ATTR_OFFSET       },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "mesh.animated.textured-phong-shadowed",
        .vertex_path    = "shaders/vertex/skinned-shadowed.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/textured-phong-shadowed.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_MAT4,      GL_U_LS_TRANS          },
            { UTYPE_ARRAY,     GL_U_CURR_POSE_MATS    },
            { UTYPE_ARRAY,     GL_U_INV_BIND_MATS     },
            { UTYPE_MAT4,      GL_U_NORMAL_MAT        },
            { UTYPE_VEC3,      GL_U_AMBIENT_COLOR     },
            { UTYPE_VEC3,      GL_U_LIGHT_COLOR       },
            { UTYPE_VEC3,      GL_U_LIGHT_POS         },
            { UTYPE_VEC3,      GL_U_VIEW_POS          },
            { UTYPE_INT,       GL_U_TEX_ARRAY0        },
            { UTYPE_COMPOSITE, GL_U_MATERIALS,        },
            { UTYPE_INT,       GL_U_SHADOW_MAP        },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "batched.mesh.animated.textured-phong-shadowed",
        .vertex_path    = "shaders/vertex/skinned-shadowed-batched.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/textured-phong-shadowed-batched.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_MAT4,      GL_U_LS_TRANS          },
            { UTYPE_VEC3,      GL_U_AMBIENT_COLOR     },
            { UTYPE_VEC3,      GL_U_LIGHT_COLOR       },
            { UTYPE_VEC3,      GL_U_LIGHT_POS         },
            { UTYPE_VEC3,      GL_U_VIEW_POS          },
            { UTYPE_INT,       GL_U_TEX_ARRAY0        },
            { UTYPE_INT,       GL_U_TEX_ARRAY1        },
            { UTYPE_INT,       GL_U_TEX_ARRAY2        },
            { UTYPE_INT,       GL_U_TEX_ARRAY3        },
            { UTYPE_INT,       GL_U_SHADOW_MAP        },
            { UTYPE_INT,       "attrbuff"             },
            { UTYPE_INT,       "attrbuff_offset"      },
            { UTYPE_INT,       GL_U_ATTR_STRIDE       },
            { UTYPE_INT,       GL_U_ATTR_OFFSET       },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "statusbar",
        .vertex_path    = "shaders/vertex/statusbar.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/statusbar.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_IVEC2,     GL_U_CURR_RES          },
            { UTYPE_ARRAY,     GL_U_ENT_TOP_OFFSETS_SS},
            { UTYPE_ARRAY,     GL_U_ENT_HEALTH_PC     },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "water",
        .vertex_path    = "shaders/vertex/water.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/water.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC3,      GL_U_LIGHT_POS         },
            { UTYPE_VEC3,      GL_U_VIEW_POS          },
            { UTYPE_IVEC2,     GL_U_WATER_TILING      },
            { UTYPE_INT,       GL_U_DUDV_MAP          },
            { UTYPE_INT,       GL_U_NORMAL_MAP        },
            { UTYPE_INT,       GL_U_REFRACT_TEX       },
            { UTYPE_INT,       GL_U_REFLECT_TEX       },
            { UTYPE_FLOAT,     GL_U_MOVE_FACTOR       },
            { UTYPE_FLOAT,     GL_U_CAM_NEAR          },
            { UTYPE_FLOAT,     GL_U_CAM_FAR           },
            { UTYPE_VEC3,      GL_U_LIGHT_COLOR       },
            { UTYPE_INT,       "visbuff"              },
            { UTYPE_INT,       "visbuff_offset"       },
            { UTYPE_IVEC4,     GL_U_MAP_RES,          },
            { UTYPE_VEC2,      GL_U_MAP_POS,          },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "ui",
        .vertex_path    = "shaders/vertex/ui.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/ui.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_INT,       GL_U_TEXTURE0          },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "minimap",
        .vertex_path    = "shaders/vertex/static.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/minimap.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            { UTYPE_VEC4,      GL_U_CLIP_PLANE0       },
            { UTYPE_VEC3,      GL_U_LIGHT_POS         },
            { UTYPE_VEC3,      GL_U_VIEW_POS          },
            { UTYPE_INT,       GL_U_TEXTURE0          },
            { UTYPE_INT,       "visbuff"              },
            { UTYPE_INT,       "visbuff_offset"       },
            { UTYPE_IVEC4,     GL_U_MAP_RES,          },
            { UTYPE_VEC2,      GL_U_MAP_POS,          },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "minimap-units",
        .vertex_path    = "shaders/vertex/colored-instanced.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/colored-per-vert.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_MAT4,      GL_U_MODEL             },
            { UTYPE_MAT4,      GL_U_VIEW              },
            { UTYPE_MAT4,      GL_U_PROJECTION        },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "posbuff",
        .vertex_path    = "shaders/vertex/posbuff.glsl",
        .geo_path       = NULL,
        .compute_path   = NULL,
        .frag_path      = "shaders/fragment/posbuff.glsl",
        .uniforms       = (struct uniform[]){
            { UTYPE_IVEC4,     GL_U_MAP_RES,          },
            { UTYPE_VEC2,      GL_U_MAP_POS           },
            {0}
        },
    },
    {
        .prog_id        = (intptr_t)NULL,
        .name           = "movement",
        .vertex_path    = NULL,
        .geo_path       = NULL,
        .compute_path   = "shaders/compute/movement.glsl",
        .frag_path      = NULL,
        .uniforms       = (struct uniform[]){
            {0}
        },
    },
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

const char *shader_text_load(const char *path)
{
    ASSERT_IN_RENDER_THREAD();

    SDL_RWops *stream = SDL_RWFromFile(path, "r");
    if(!stream){
        return NULL;
    }

    const Sint64 fsize = SDL_RWsize(stream);    
    char *ret = malloc(fsize + 1);
    if(!ret) {
        SDL_RWclose(stream);
        return NULL; 
    }
    char *out = ret;

    Sint64 read, read_total = 0;
    while(read_total < fsize) {
        read = SDL_RWread(stream, out, 1, fsize - read_total); 

        if(read == 0)
            break;

        read_total += read;
        out        += read;
    }
    SDL_RWclose(stream);

    if(read_total != fsize){
        return NULL;
    }

    out[0] = '\0';
    return ret;
}

static bool shader_init(const char *text, GLuint *out, GLint type)
{
    ASSERT_IN_RENDER_THREAD();

    char info[512];
    GLint success;

    *out = glCreateShader(type);
    glShaderSource(*out, 1, &text, NULL);
    glCompileShader(*out);

    glGetShaderiv(*out, GL_COMPILE_STATUS, &success);
    if(!success) {

        glGetShaderInfoLog(*out, sizeof(info), NULL, info);
        pf_strlcat(info, "\n", sizeof(info));
        PRINT(info);
        return false;
    }

    return true;
}

static bool shader_load_and_init(const char *path, GLuint *out, GLint type)
{
    ASSERT_IN_RENDER_THREAD();
    char buff[512];

    const char *text = shader_text_load(path);
    if(!text) {
        pf_snprintf(buff, sizeof(buff), "Could not load shader at: %s\n", path);
        PRINT(buff);
        goto fail;
    }
    
    if(!shader_init(text, out, type)){
        pf_snprintf(buff, sizeof(buff), "Could not compile shader at: %s\n", path);
        PRINT(buff);
        goto fail;
    }

    free((char*)text);
    return true;

fail:
    free((char*)text);
    return false;
}

static bool shader_make_prog(const GLuint vertex_shader, const GLuint geo_shader, 
                             const GLuint compute_shader, const GLuint frag_shader, GLint *out)
{
    ASSERT_IN_RENDER_THREAD();

    char info[512];
    GLint success;

    *out = glCreateProgram();
    if(vertex_shader) {
        glAttachShader(*out, vertex_shader);
    }
    if(geo_shader) {
        glAttachShader(*out, geo_shader); 
    }
    if(compute_shader) {
        glAttachShader(*out, compute_shader);
    }
    if(frag_shader) {
        glAttachShader(*out, frag_shader);
    }
    glLinkProgram(*out);

    glGetProgramiv(*out, GL_LINK_STATUS, &success);
    if(!success) {

        glGetProgramInfoLog(*out, sizeof(info), NULL, info);
        pf_strlcat(info, "\n", sizeof(info));
        PRINT(info);
        return false;
    }

    return true;
}

static const struct shader *shader_for_name(const char *name)
{
    ASSERT_IN_RENDER_THREAD();

    for(int i = 0; i < ARR_SIZE(s_shaders); i++) {

        const struct shader *curr = &s_shaders[i];
        if(!strcmp(curr->name, name))
            return curr;
    }
    return NULL;
}

static const struct shader *shader_for_prog(GLuint prog)
{
    ASSERT_IN_RENDER_THREAD();

    for(int i = 0; i < ARR_SIZE(s_shaders); i++) {

        const struct shader *curr = &s_shaders[i];
        if(curr->prog_id == prog)
            return curr;
    }
    return NULL;
}

static void shader_install(const struct shader *shader)
{
    const struct uniform *curr = shader->uniforms;

    if(s_curr_prog != shader->prog_id) {
        glUseProgram(shader->prog_id);
        s_curr_prog = shader->prog_id;
    }

    while(curr->name) {

        R_GL_StateInstall(curr->name, shader->prog_id);
        curr++;
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_Shader_InitAll(const char *base_path)
{
    ASSERT_IN_RENDER_THREAD();

    for(int i = 0; i < ARR_SIZE(s_shaders); i++){

        char path[512];
        struct shader *res = &s_shaders[i];
        GLuint vertex = 0, geometry = 0, fragment = 0, compute = 0;

        if(res->vertex_path) {
            pf_snprintf(path, sizeof(path), "%s/%s", base_path, res->vertex_path);
            if(!shader_load_and_init(path, &vertex, GL_VERTEX_SHADER)) {
                PRINT("Failed to load and init vertex shader.\n");
                goto fail;
            }
        }
        assert(!res->vertex_path || vertex > 0);

        if(res->geo_path) {
            pf_snprintf(path, sizeof(path), "%s/%s", base_path, res->geo_path);
            if(!shader_load_and_init(path, &geometry, GL_GEOMETRY_SHADER)) {
                PRINT("Failed to load and init geometry shader.\n");
                goto fail;
            }
        }
        assert(!res->geo_path || geometry > 0);

        if(res->frag_path) {
            pf_snprintf(path, sizeof(path), "%s/%s", base_path, res->frag_path);
            if(!shader_load_and_init(path, &fragment, GL_FRAGMENT_SHADER)) {
                PRINT("Failed to load and init fragment shader.\n");
                goto fail;
            }
        }
        assert(!res->frag_path || fragment > 0);

        if(res->compute_path) {

            if(!R_ComputeShaderSupported()) {
                char buff[512];
                pf_snprintf(buff, sizeof(buff), "No compute shader support on the current platform. "
                    "Skipping shader '%s'.\n", res->name);
                PRINT(buff);
                continue;
            }

            pf_snprintf(path, sizeof(path), "%s/%s", base_path, res->compute_path);
            if(!shader_load_and_init(path, &compute, GL_COMPUTE_SHADER)) {
                PRINT("Failed to load and init compute shader.\n");
                goto fail;
            }
        }
        assert(!res->compute_path || compute > 0);

        if(!shader_make_prog(vertex, geometry, compute, fragment, &res->prog_id)) {
            char buff[512];
            pf_snprintf(buff, sizeof(buff), "Failed to make shader program %d of %d.\n",
                i + 1, (int)ARR_SIZE(s_shaders));
            PRINT(buff);
            goto fail;
        }

        if(vertex)      glDeleteShader(vertex);
        if(geometry)    glDeleteShader(geometry);
        if(fragment)    glDeleteShader(fragment);
        if(compute)     glDeleteShader(compute);
        continue;

    fail:
        if(vertex)      glDeleteShader(vertex);
        if(geometry)    glDeleteShader(geometry);
        if(fragment)    glDeleteShader(fragment);
        if(compute)     glDeleteShader(compute);
        return false;
    }

    return true;
}

GLint R_GL_Shader_GetProgForName(const char *name)
{
    ASSERT_IN_RENDER_THREAD();

    for(int i = 0; i < ARR_SIZE(s_shaders); i++) {

        const struct shader *curr = &s_shaders[i];

        if(!strcmp(curr->name, name))
            return curr->prog_id;
    }
    
    return -1;
}

const char *R_GL_Shader_GetName(GLuint prog)
{
    ASSERT_IN_RENDER_THREAD();

    for(int i = 0; i < ARR_SIZE(s_shaders); i++) {

        const struct shader *curr = &s_shaders[i];
        if(curr->prog_id == prog)
            return curr->name;
    }
    
    return NULL;
}
    
void R_GL_Shader_Install(const char *name)
{
    ASSERT_IN_RENDER_THREAD();

    const struct shader *shader = shader_for_name(name);
    shader_install(shader);
}

void R_GL_Shader_InstallProg(GLuint prog)
{
    ASSERT_IN_RENDER_THREAD();

    const struct shader *shader = shader_for_prog(prog);
    shader_install(shader);
}

GLuint R_GL_Shader_GetCurrActive(void)
{
    return s_curr_prog;
}

