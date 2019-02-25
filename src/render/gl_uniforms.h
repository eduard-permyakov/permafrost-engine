/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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

#ifndef GL_UNIFORMS_H
#define GL_UNIFORMS_H

/* Written by camera once per frame */
#define GL_U_PROJECTION     "projection"
#define GL_U_VIEW           "view"
#define GL_U_VIEW_POS       "view_pos"

/* Written to by render subsystem for every entity */
#define GL_U_MODEL          "model"
#define GL_U_COLOR          "color"
#define GL_U_MATERIALS      "materials"

/* Written by anim subsystem for every entity */
#define GL_U_INV_BIND_MATS  "anim_inv_bind_mats"
#define GL_U_CURR_POSE_MATS "anim_curr_pose_mats"

/* 8 texture slots that get set by render subsystem for each entity */
#define GL_U_TEXTURE0       "texture0"
#define GL_U_TEXTURE1       "texture1"
#define GL_U_TEXTURE2       "texture2"
#define GL_U_TEXTURE3       "texture3"
#define GL_U_TEXTURE4       "texture4"
#define GL_U_TEXTURE5       "texture5"
#define GL_U_TEXTURE6       "texture6"
#define GL_U_TEXTURE7       "texture7"
#define GL_U_TEXTURE8       "texture8"
#define GL_U_TEXTURE9       "texture9"
#define GL_U_TEXTURE10      "texture10"
#define GL_U_TEXTURE11      "texture11"
#define GL_U_TEXTURE12      "texture12"
#define GL_U_TEXTURE13      "texture13"
#define GL_U_TEXTURE14      "texture14"
#define GL_U_TEXTURE15      "texture15"

/* 1 array texture slot */
#define GL_U_TEX_ARRAY0     "tex_array0"

/* Global light parameters - affect all models */
#define GL_U_AMBIENT_COLOR  "ambient_color"
#define GL_U_LIGHT_POS      "light_pos"
#define GL_U_LIGHT_COLOR    "light_color"

/* Used for depth map rendering and testing */
#define GL_U_LS_TRANS       "light_space_transform"
#define GL_U_SHADOW_MAP     "shadow_map"

/* Used for rendering the status bars. */
#define GL_U_ENT_TOP_OFFSETS_SS "ent_top_offsets_ss"
#define GL_U_ENT_HEALTH_PC      "ent_health_pc"

#endif
