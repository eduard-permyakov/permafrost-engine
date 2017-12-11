#ifndef GL_UNIFORMS_H
#define GL_UNIFORMS_H

/* Written by camera once per frame */
#define GL_U_PROJECTION     "projection"
#define GL_U_VIEW           "view"

/* Written to by render subsystem for every entity */
#define GL_U_MODEL          "model"
#define GL_U_COLOR          "color"
#define GL_U_MATERIALS      "materials"

/* Written by anim subsystem for every entity */
#define GL_U_INV_BIND_MATS  "anim_inv_bind_mats"
#define GL_U_CURR_POSE_MATS "anim_curr_pose_mats"

/* 8 texture slots that get set for each entity */
#define GL_U_TEXTURE0       "texture0"
#define GL_U_TEXTURE1       "texture1"
#define GL_U_TEXTURE2       "texture2"
#define GL_U_TEXTURE3       "texture3"
#define GL_U_TEXTURE4       "texture4"
#define GL_U_TEXTURE5       "texture5"
#define GL_U_TEXTURE6       "texture6"
#define GL_U_TEXTURE7       "texture7"

#endif
