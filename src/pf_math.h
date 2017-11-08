#ifndef PF_MATH_H
#define PF_MATH_H

#include <GL/glew.h>   /* GLfloat definition */
#include <stdio.h>   /* FILE definition    */
#define __USE_MISC
#include <math.h>    /* M_PI definition    */

#define DEG_TO_RAD(_deg) ((_deg)*(M_PI/180.0f))

typedef union vec2{
    GLfloat raw[2];
    struct{
        GLfloat x, y; 
    };
}vec2_t;

typedef union vec3{
    GLfloat raw[3];
    struct{
        GLfloat x, y, z; 
    };
}vec3_t;

typedef union vec4{
    GLfloat raw[4];
    struct{
        GLfloat x, y, z, w; 
    };
}vec4_t;

typedef vec4_t quat_t;

typedef union mat3x3{
    GLfloat raw[9];
    GLfloat cols[3][3];
    struct{
        GLfloat m0, m1, m2,
                m3, m4, m5,
                m6, m7, m8;
    };
}mat3x3_t;

typedef union mat4x4{
    GLfloat raw[16];
    GLfloat cols[4][4];
    struct{
        GLfloat m0, m1, m2, m3,
                m4, m5, m6, m7,
                m8, m9, m10,m11,
                m12,m13,m14,m15;
    };
}mat4x4_t;

int     PFM_vec2_dot       (vec2_t *op1, vec2_t *op2);
void    PFM_vec2_add       (vec2_t *op1, vec2_t *op2, vec2_t *out);
void    PFM_vec2_sub       (vec2_t *op1, vec2_t *op2, vec2_t *out);
void    PFM_vec2_scale     (vec2_t *op1, GLfloat scale, vec2_t *out);
GLfloat PFM_vec2_len       (vec2_t *op1);
void    PFM_vec2_normal    (vec2_t *op1,  vec2_t *out);

void    PFM_vec3_cross     (vec3_t *a,   vec3_t *b,   vec3_t *out);
GLfloat PFM_vec3_dot       (vec3_t *op1, vec3_t *op2);
void    PFM_vec3_add       (vec3_t *op1, vec3_t *op2, vec3_t *out);
void    PFM_vec3_sub       (vec3_t *op1, vec3_t *op2, vec3_t *out);
void    PFM_vec3_scale     (vec3_t *op1, GLfloat scale, vec3_t *out);
GLfloat PFM_vec3_len       (vec3_t *op1);
void    PFM_vec3_normal    (vec3_t *op1, vec3_t *out);
void    PFM_vec3_dump      (vec3_t *vec, FILE *dumpfile);

GLfloat PFM_vec4_dot       (vec4_t *op1, vec4_t *op2, vec4_t *out);
void    PFM_vec4_add       (vec4_t *op1, vec4_t *op2, vec4_t *out);
void    PFM_vec4_sub       (vec4_t *op1, vec4_t *op2, vec4_t *out);
void    PFM_vec4_scale     (vec4_t *op1, GLfloat scale, vec4_t *out);
GLfloat PFM_vec4_len       (vec4_t *op1);
void    PFM_vec4_normal    (vec4_t *op1, vec4_t *out);
void 	PFM_vec4_dump	   (vec4_t *vec, FILE *dumpfile);

void    PFM_mat3x3_scale   (mat3x3_t *op1,  GLfloat scale, mat3x3_t *out);
void    PFM_mat3x3_mult3x3 (mat3x3_t *op1,  mat3x3_t *op2, mat3x3_t *out);
void    PFM_mat3x3_mult3x1 (mat3x3_t *op1,  vec3_t *op2,   vec3_t *out);
void    PFM_mat3x3_identity(mat3x3_t *out);

void    PFM_mat4x4_scale   (mat4x4_t *op1, GLfloat scale, mat4x4_t *out);
void    PFM_mat4x4_mult4x4 (mat4x4_t *op1, mat4x4_t *op2, mat4x4_t *out);
void    PFM_mat4x4_mult4x1 (mat4x4_t *op1, vec4_t   *op2, vec4_t   *out);
void    PFM_mat4x4_identity(mat4x4_t *out);

void    PFM_mat4x4_make_scale (GLfloat s1, GLfloat s2, GLfloat s3, mat4x4_t *out);
void    PFM_mat4x4_make_trans (GLfloat tx, GLfloat ty, GLfloat tz, mat4x4_t *out);
void    PFM_mat4x4_make_rot_x (GLfloat radians, mat4x4_t *out);
void    PFM_mat4x4_make_rot_y (GLfloat radians, mat4x4_t *out);
void    PFM_mat4x4_make_rot_z (GLfloat radians, mat4x4_t *out);
void    PFM_mat4x4_make_rot   (vec3_t *axis, GLfloat radians, mat4x4_t *out);

void    PFM_mat4x4_make_perspective (GLfloat fov_radians, GLfloat aspect_ratio, 
                                     GLfloat z_near, GLfloat z_far, mat4x4_t *out);
void    PFM_mat4x4_make_look_at     (vec3_t *camera_pos, vec3_t *target_pos, 
                                     vec3_t *up_dir, mat4x4_t *out);

#endif
