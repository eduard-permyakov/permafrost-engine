#ifndef GL_UNIFORMS_H
#define GL_UNIFORMS_H

/* Written by camera once per frame */
#define GL_U_PROJECTION "projection"
#define GL_U_VIEW       "view"

/* Written to by render subsystem for every entity */
#define GL_U_MODEL      "model"
#define GL_U_COLOR      "color"

/* Written by anim subsystem for every entity */
#define GL_U_INV_BIND_MATS  "anim_inv_bind_mats"
#define GL_U_CURR_POSE_MATS "anim_curr_pose_mats"

#endif
