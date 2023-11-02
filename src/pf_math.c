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

#include "pf_math.h"
#include <string.h>
#include <assert.h>

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

GLfloat PFM_Vec2_Dot(vec2_t *op1, vec2_t *op2)
{
    return op1->x * op2->x + 
           op1->y * op2->y;
}

void PFM_Vec2_Add(vec2_t *op1, vec2_t *op2, vec2_t *out)
{
    out->x = op1->x + op2->x; 
    out->y = op1->y + op2->y;
}

void PFM_Vec2_Sub(vec2_t *op1, vec2_t *op2, vec2_t *out)
{
    out->x = op1->x - op2->x;
    out->y = op1->y - op2->y;
}

void PFM_Vec2_Scale(vec2_t *op1, GLfloat scale, vec2_t *out)
{
    out->x = op1->x * scale;
    out->y = op1->y * scale;
}

GLfloat PFM_Vec2_Len(const vec2_t *op1)
{
    return sqrt(op1->x * op1->x + 
                op1->y * op1->y);
}

void PFM_Vec2_Normal(vec2_t *op1,  vec2_t *out)
{
    GLfloat len = PFM_Vec2_Len(op1); 

    out->x = op1->x / len;
    out->y = op1->y / len;
}

void PFM_Vec2_Dump(vec2_t *vec, FILE *dumpfile)
{
    fprintf(dumpfile, "(%.4f, %.4f)\n", vec->x, vec->y);
}

void PFM_Vec3_Cross(vec3_t *a, vec3_t *b, vec3_t *out)
{
    out->x =   a->y * b->z - a->z * b->y;
    out->y = -(a->x * b->z - a->z * b->x);
    out->z =   a->x * b->y - a->y * b->x;
}

GLfloat PFM_Vec3_Dot(vec3_t *op1, vec3_t *op2)
{
    return op1->x * op2->x +
           op1->y * op2->y +
           op1->z * op2->z;
}

void PFM_Vec3_Add(vec3_t *op1, vec3_t *op2, vec3_t *out)
{
    for(int i = 0; i < 3; i++)
        out->raw[i] = op1->raw[i] + op2->raw[i];
}

void PFM_Vec3_Sub(vec3_t *op1, vec3_t *op2, vec3_t *out)
{
    for(int i = 0; i < 3; i++)
        out->raw[i] = op1->raw[i] - op2->raw[i];
}

void PFM_Vec3_Scale(vec3_t *op1, GLfloat scale, vec3_t *out)
{
    for(int i = 0; i < 3; i++)
        out->raw[i] = op1->raw[i] * scale;
}

GLfloat PFM_Vec3_Len(const vec3_t *op1)
{
    return sqrt(op1->x * op1->x + 
                op1->y * op1->y +
                op1->z * op1->z);
}

void PFM_Vec3_Normal(vec3_t *op1, vec3_t *out)
{
    GLfloat len = PFM_Vec3_Len(op1); 

    for(int i = 0; i < 3; i++)
        out->raw[i] = op1->raw[i] / len;
}

void PFM_Vec3_Dump(vec3_t *vec, FILE *dumpfile)
{
    fprintf(dumpfile, "(%.4f, %.4f, %.4f)\n", vec->x, vec->y, vec->z);
}

GLfloat PFM_Vec4_Dot(vec4_t *op1,  vec4_t *op2, vec4_t *out)
{
    return op1->x * op2->x +
           op1->y * op2->y +
           op1->z * op2->z +
           op1->w * op2->w;
}

void PFM_Vec4_Add(vec4_t *op1, vec4_t *op2, vec4_t *out)
{
    for(int i = 0; i < 4; i++)
        out->raw[i] = op1->raw[i] + op2->raw[i];
}

void PFM_Vec4_Sub(vec4_t *op1, vec4_t *op2, vec4_t *out)
{
    for(int i = 0; i < 4; i++)
        out->raw[i] = op1->raw[i] - op2->raw[i];
}

void PFM_Vec4_Scale(vec4_t *op1, GLfloat scale, vec4_t *out)
{
    for(int i = 0; i < 4; i++)
        out->raw[i] = op1->raw[i] * scale;
}

GLfloat PFM_Vec4_Len(const vec4_t *op1)
{
    return sqrt(op1->x * op1->x + 
                op1->y * op1->y +
                op1->z * op1->z +
                op1->w * op1->w);
}

void PFM_Vec4_Normal(vec4_t *op1, vec4_t *out)
{
    GLfloat len = PFM_Vec4_Len(op1); 

    for(int i = 0; i < 4; i++)
        out->raw[i] = op1->raw[i] / len;
}

void PFM_Vec4_Dump(vec4_t *vec,  FILE *dumpfile)
{
    fprintf(dumpfile, "(%.4f, %.4f, %.4f, %.4f)\n", vec->x, vec->y, vec->z, vec->w);
}

void PFM_Mat3x3_Scale(mat3x3_t *op1, GLfloat scale, mat3x3_t *out)
{
    for(int i = 0; i < 9; i++) {
        out->raw[i] = op1->raw[i] * scale;
    }
}

void PFM_Mat3x3_Mult3x3 (mat3x3_t *op1, mat3x3_t *op2, mat3x3_t *out)
{
    for(int r = 0; r < 3; r++) {
        for(int c = 0; c < 3; c++) {
            out->cols[c][r] = 0.0f;
            for(int k = 0; k < 3; k++)
                out->cols[c][r] += op1->cols[k][r] * op2->cols[c][k]; 
        }
    }
}

void PFM_Mat3x3_Mult3x1 (mat3x3_t *op1,  vec3_t *op2,  vec3_t *out)
{
    for(int r = 0; r < 3; r++) {
        out->raw[r] = 0.0f;
        for(int c = 0; c < 3; c++)
            out->raw[r] += op1->cols[c][r] * op2->raw[c];
    }
}

void PFM_Mat3x3_Identity(mat3x3_t *out)
{
    memset(out, 0, sizeof(mat3x3_t));

    for(int i = 0; i < 3; i++)
        out->cols[i][i] = 1;
}

void PFM_Mat4x4_Scale(mat4x4_t *op1, GLfloat scale, mat4x4_t *out)
{
    for(int i = 0; i < 16; i++) {
        out->raw[i] = op1->raw[i] * scale;
    }
}

void PFM_Mat4x4_Mult4x4 (mat4x4_t *op1, mat4x4_t *op2, mat4x4_t *out)
{
    for(int r = 0; r < 4; r++) {
        for(int c = 0; c < 4; c++) {
            out->cols[c][r] = 0.0f;
            for(int k = 0; k < 4; k++)
                out->cols[c][r] += op1->cols[k][r] * op2->cols[c][k]; 
        }
    }
}

void PFM_Mat4x4_Mult4x1(mat4x4_t *op1, vec4_t *op2, vec4_t *out)
{
    for(int r = 0; r < 4; r++) {
        out->raw[r] = 0.0f;
        for(int c = 0; c < 4; c++)
            out->raw[r] += op1->cols[c][r] * op2->raw[c];
    }
}

void PFM_Mat4x4_Identity(mat4x4_t *out)
{
    memset(out, 0, sizeof(mat4x4_t));

    for(int i = 0; i < 4; i++)
        out->cols[i][i] = 1;
}

void PFM_Mat4x4_MakeScale(GLfloat s1, GLfloat s2, GLfloat s3, mat4x4_t *out)
{
    PFM_Mat4x4_Identity(out);

    out->cols[0][0] = s1;
    out->cols[1][1] = s2;
    out->cols[2][2] = s3;
}

void PFM_Mat4x4_MakeTrans(GLfloat tx, GLfloat ty, GLfloat tz, mat4x4_t *out)
{
    PFM_Mat4x4_Identity(out); 

    out->cols[3][0] = tx;
    out->cols[3][1] = ty;
    out->cols[3][2] = tz;
}

void PFM_Mat4x4_MakeRotX(GLfloat radians, mat4x4_t *out)
{
    PFM_Mat4x4_Identity(out);

    out->cols[1][1] =  cos(radians);
    out->cols[1][2] =  sin(radians);

    out->cols[2][1] = -sin(radians);
    out->cols[2][2] =  cos(radians);
}

void PFM_Mat4x4_MakeRotY(GLfloat radians, mat4x4_t *out)
{
    PFM_Mat4x4_Identity(out);

    out->cols[0][0] =  cos(radians);
    out->cols[0][2] = -sin(radians);

    out->cols[2][0] =  sin(radians);
    out->cols[2][2] =  cos(radians);
}

void PFM_Mat4x4_MakeRotZ(GLfloat radians, mat4x4_t *out)
{
    PFM_Mat4x4_Identity(out);

    out->cols[0][0] =  cos(radians);
    out->cols[0][1] =  sin(radians);

    out->cols[1][0] = -sin(radians);
    out->cols[1][1] =  cos(radians);
}

/* Algorithm taken from:
 * http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.184.3942&rep=rep1&type=pdf
 */
void PFM_Mat4x4_RotFromQuat(const quat_t *quat, mat4x4_t *out)
{
    PFM_Mat4x4_Identity(out);

    out->cols[0][0]  = 1 - 2*pow(quat->y, 2) - 2*pow(quat->z, 2);
    out->cols[1][0] = 2*quat->x*quat->y + 2*quat->w*quat->z;
    out->cols[2][0] = 2*quat->x*quat->z - 2*quat->w*quat->y;

    out->cols[0][1] = 2*quat->x*quat->y - 2*quat->w*quat->z;
    out->cols[1][1] = 1 - 2*pow(quat->x, 2) - 2*pow(quat->z, 2);
    out->cols[2][1] = 2*quat->y*quat->z + 2*quat->w*quat->x;

    out->cols[0][2] = 2*quat->x*quat->z + 2*quat->w*quat->y;
    out->cols[1][2] = 2*quat->y*quat->z - 2*quat->w*quat->x;
    out->cols[2][2] = 1 - 2*pow(quat->x, 2) - 2*pow(quat->y, 2);
}

void PFM_Mat4x4_RotFromEuler(GLfloat deg_x, GLfloat deg_y, GLfloat deg_z, mat4x4_t *out)
{
    mat4x4_t x, y, z, tmp;

    PFM_Mat4x4_MakeRotX(DEG_TO_RAD(deg_x), &x);
    PFM_Mat4x4_MakeRotY(DEG_TO_RAD(deg_y), &y);
    PFM_Mat4x4_MakeRotZ(DEG_TO_RAD(deg_z), &z);

    PFM_Mat4x4_Mult4x4(&y, &z, &tmp);
    PFM_Mat4x4_Mult4x4(&x, &tmp, out);
}

/* - fov_radians is the vertical FOV angle
 * - This is OpenGL specific, where the positive Z-axis is pointing out of 
 * the screen
 * - Vectors multiplied by this matrix will already be in NDC
 * 
 */
void PFM_Mat4x4_MakePerspective(GLfloat fov_radians, GLfloat aspect_ratio, 
                                GLfloat z_near, GLfloat z_far, mat4x4_t *out)
{
    /* This assumes symmetry (left = -right, top = -bottom) */ 
    GLfloat t = z_near * tan(fov_radians / 2);
    GLfloat r = t * aspect_ratio;

    memset(out->raw, 0, sizeof(out->raw));
    out->cols[0][0] = z_near / r;
    out->cols[1][1] = z_near / t;
    out->cols[2][2] = -(z_far + z_near) / (z_far - z_near);
    out->cols[2][3] = -1.0f;
    out->cols[3][2] = -(2.0f * z_far * z_near) / (z_far - z_near);
}

void PFM_Mat4x4_MakeOrthographic(GLfloat left, GLfloat right,
                                 GLfloat bot,  GLfloat top,
                                 GLfloat nearp, GLfloat farp, mat4x4_t *out)
{
    PFM_Mat4x4_Identity(out);
    out->cols[0][0] = 2.0f/(right - left);
    out->cols[1][1] = 2.0f/(top - bot);
    out->cols[2][2] = -2.0f/(farp - nearp);
    out->cols[3][0] = -(right + left)/(right - left);
    out->cols[3][1] = -(top + bot)/(top - bot);
    out->cols[3][2] = -(farp + nearp)/(farp - nearp);
}

void PFM_Mat4x4_MakeLookAt(vec3_t *camera_pos, vec3_t *target_pos, 
                           vec3_t *up, mat4x4_t *out)
{
    vec3_t camera_dir, right;

    PFM_Vec3_Sub(camera_pos, target_pos, &camera_dir);
    PFM_Vec3_Normal(&camera_dir, &camera_dir);
    PFM_Vec3_Cross(&camera_dir, up, &right);

    mat4x4_t axes, trans;
    PFM_Mat4x4_Identity(&axes);
    axes.cols[0][0] = right.x;
    axes.cols[1][0] = right.y;
    axes.cols[2][0] = right.z;

    axes.cols[0][1] = up->x;
    axes.cols[1][1] = up->y;
    axes.cols[2][1] = up->z;

    axes.cols[0][2] = camera_dir.x;
    axes.cols[1][2] = camera_dir.y;
    axes.cols[2][2] = camera_dir.z;

    PFM_Mat4x4_MakeTrans(-camera_pos->x, -camera_pos->y, -camera_pos->z, &trans);
    PFM_Mat4x4_Mult4x4(&axes, &trans, out);
}

/* Implementation derived from Mesa 3D implementation */
void PFM_Mat4x4_Inverse(mat4x4_t *in, mat4x4_t *out)
{
    double inv[16], det;
    int i;

    inv[0] = 
        in->raw[5]  * in->raw[10] * in->raw[15] - 
        in->raw[5]  * in->raw[11] * in->raw[14] - 
        in->raw[9]  * in->raw[6]  * in->raw[15] + 
        in->raw[9]  * in->raw[7]  * in->raw[14] +
        in->raw[13] * in->raw[6]  * in->raw[11] - 
        in->raw[13] * in->raw[7]  * in->raw[10];
    
    inv[4] = 
       -in->raw[4]  * in->raw[10] * in->raw[15] + 
        in->raw[4]  * in->raw[11] * in->raw[14] + 
        in->raw[8]  * in->raw[6]  * in->raw[15] - 
        in->raw[8]  * in->raw[7]  * in->raw[14] - 
        in->raw[12] * in->raw[6]  * in->raw[11] + 
        in->raw[12] * in->raw[7]  * in->raw[10];

    inv[8] = 
        in->raw[4]  * in->raw[9] * in->raw[15] - 
        in->raw[4]  * in->raw[11] * in->raw[13] - 
        in->raw[8]  * in->raw[5] * in->raw[15] + 
        in->raw[8]  * in->raw[7] * in->raw[13] + 
        in->raw[12] * in->raw[5] * in->raw[11] - 
        in->raw[12] * in->raw[7] * in->raw[9];
    
    inv[12] = 
       -in->raw[4]  * in->raw[9] * in->raw[14] + 
        in->raw[4]  * in->raw[10] * in->raw[13] +
        in->raw[8]  * in->raw[5] * in->raw[14] - 
        in->raw[8]  * in->raw[6] * in->raw[13] - 
        in->raw[12] * in->raw[5] * in->raw[10] + 
        in->raw[12] * in->raw[6] * in->raw[9];
    
    inv[1] = 
       -in->raw[1]  * in->raw[10] * in->raw[15] + 
        in->raw[1]  * in->raw[11] * in->raw[14] + 
        in->raw[9]  * in->raw[2] * in->raw[15] - 
        in->raw[9]  * in->raw[3] * in->raw[14] - 
        in->raw[13] * in->raw[2] * in->raw[11] + 
        in->raw[13] * in->raw[3] * in->raw[10];
    
    inv[5] = 
        in->raw[0]  * in->raw[10] * in->raw[15] - 
        in->raw[0]  * in->raw[11] * in->raw[14] - 
        in->raw[8]  * in->raw[2] * in->raw[15] + 
        in->raw[8]  * in->raw[3] * in->raw[14] + 
        in->raw[12] * in->raw[2] * in->raw[11] - 
        in->raw[12] * in->raw[3] * in->raw[10];
    
    inv[9] = 
       -in->raw[0]  * in->raw[9] * in->raw[15] + 
        in->raw[0]  * in->raw[11] * in->raw[13] + 
        in->raw[8]  * in->raw[1] * in->raw[15] - 
        in->raw[8]  * in->raw[3] * in->raw[13] - 
        in->raw[12] * in->raw[1] * in->raw[11] + 
        in->raw[12] * in->raw[3] * in->raw[9];
    
    inv[13] = 
        in->raw[0]  * in->raw[9] * in->raw[14] - 
        in->raw[0]  * in->raw[10] * in->raw[13] - 
        in->raw[8]  * in->raw[1] * in->raw[14] + 
        in->raw[8]  * in->raw[2] * in->raw[13] + 
        in->raw[12] * in->raw[1] * in->raw[10] - 
        in->raw[12] * in->raw[2] * in->raw[9];
    
    inv[2] = 
        in->raw[1]  * in->raw[6] * in->raw[15] - 
        in->raw[1]  * in->raw[7] * in->raw[14] - 
        in->raw[5]  * in->raw[2] * in->raw[15] + 
        in->raw[5]  * in->raw[3] * in->raw[14] + 
        in->raw[13] * in->raw[2] * in->raw[7] - 
        in->raw[13] * in->raw[3] * in->raw[6];
    
    inv[6] = 
       -in->raw[0]  * in->raw[6] * in->raw[15] + 
        in->raw[0]  * in->raw[7] * in->raw[14] + 
        in->raw[4]  * in->raw[2] * in->raw[15] - 
        in->raw[4]  * in->raw[3] * in->raw[14] - 
        in->raw[12] * in->raw[2] * in->raw[7] + 
        in->raw[12] * in->raw[3] * in->raw[6];
    
    inv[10] = 
        in->raw[0]  * in->raw[5] * in->raw[15] - 
        in->raw[0]  * in->raw[7] * in->raw[13] - 
        in->raw[4]  * in->raw[1] * in->raw[15] + 
        in->raw[4]  * in->raw[3] * in->raw[13] + 
        in->raw[12] * in->raw[1] * in->raw[7] - 
        in->raw[12] * in->raw[3] * in->raw[5];
    
    inv[14] = 
       -in->raw[0]  * in->raw[5] * in->raw[14] + 
        in->raw[0]  * in->raw[6] * in->raw[13] + 
        in->raw[4]  * in->raw[1] * in->raw[14] - 
        in->raw[4]  * in->raw[2] * in->raw[13] - 
        in->raw[12] * in->raw[1] * in->raw[6] + 
        in->raw[12] * in->raw[2] * in->raw[5];
    
    inv[3] = 
       -in->raw[1] * in->raw[6] * in->raw[11] + 
        in->raw[1] * in->raw[7] * in->raw[10] + 
        in->raw[5] * in->raw[2] * in->raw[11] - 
        in->raw[5] * in->raw[3] * in->raw[10] - 
        in->raw[9] * in->raw[2] * in->raw[7] + 
        in->raw[9] * in->raw[3] * in->raw[6];
    
    inv[7] = 
        in->raw[0] * in->raw[6] * in->raw[11] - 
        in->raw[0] * in->raw[7] * in->raw[10] - 
        in->raw[4] * in->raw[2] * in->raw[11] + 
        in->raw[4] * in->raw[3] * in->raw[10] + 
        in->raw[8] * in->raw[2] * in->raw[7] - 
        in->raw[8] * in->raw[3] * in->raw[6];
    
    inv[11] = 
       -in->raw[0] * in->raw[5] * in->raw[11] + 
        in->raw[0] * in->raw[7] * in->raw[9] + 
        in->raw[4] * in->raw[1] * in->raw[11] - 
        in->raw[4] * in->raw[3] * in->raw[9] - 
        in->raw[8] * in->raw[1] * in->raw[7] + 
        in->raw[8] * in->raw[3] * in->raw[5];
    
    inv[15] = 
        in->raw[0] * in->raw[5] * in->raw[10] - 
        in->raw[0] * in->raw[6] * in->raw[9] - 
        in->raw[4] * in->raw[1] * in->raw[10] + 
        in->raw[4] * in->raw[2] * in->raw[9] + 
        in->raw[8] * in->raw[1] * in->raw[6] - 
        in->raw[8] * in->raw[2] * in->raw[5];
    
    det = in->raw[0] * inv[0] + in->raw[1] * inv[4] + in->raw[2] * inv[8] + in->raw[3] * inv[12];
    assert(det != 0);
    
    det = 1.0 / det;
    
    for (i = 0; i < 16; i++)
        out->raw[i] = inv[i] * det;
}

void PFM_Mat4x4_Transpose(mat4x4_t *in, mat4x4_t *out)
{
    for(int r = 0; r < 4; r++) {
        for(int c = 0; c < 4; c++) {
        
            GLfloat tmp = out->cols[c][r];
            out->cols[c][r] = in->cols[r][c];
            in->cols[r][c] = tmp;
        }
    }
}

/* Algorithm from:  
 * http://www.euclideanspace.com/maths/geometry/rotations/conversions/quaternionToMatrix/ 
 */
void PFM_Quat_FromRotMat(mat4x4_t *mat, quat_t *out)
{
    GLfloat tr = mat->cols[0][0] + mat->cols[1][1] + mat->cols[2][2];

    if (tr > 0) {

        GLfloat S = sqrt(tr+1.0) * 2; // S=4*qw 
        out->w = 0.25 * S;
        out->x = (mat->cols[2][1] - mat->cols[1][2]) / S;
        out->y = (mat->cols[0][2] - mat->cols[2][0]) / S; 
        out->z = (mat->cols[1][0] - mat->cols[0][1]) / S; 

    } else if ((mat->cols[0][0] > mat->cols[1][1]) && (mat->cols[0][0] > mat->cols[2][2])) {

        GLfloat S = sqrt(1.0 + mat->cols[0][0] - mat->cols[1][1] - mat->cols[2][2]) * 2; // S=4*qx 
        out->w = (mat->cols[2][1] - mat->cols[1][2]) / S;
        out->x = 0.25 * S;
        out->y = (mat->cols[0][1] + mat->cols[1][0]) / S; 
        out->z = (mat->cols[0][2] + mat->cols[2][0]) / S; 

    } else if (mat->cols[1][1] > mat->cols[2][2]) {

        GLfloat S = sqrt(1.0 + mat->cols[1][1] - mat->cols[0][0] - mat->cols[2][2]) * 2; // S=4*qy
        out->w = (mat->cols[0][2] - mat->cols[2][0]) / S;
        out->x = (mat->cols[0][1] + mat->cols[1][0]) / S; 
        out->y = 0.25 * S;
        out->z = (mat->cols[1][2] + mat->cols[2][1]) / S; 

    } else {

        float S = sqrt(1.0 + mat->cols[2][2] - mat->cols[0][0] - mat->cols[1][1]) * 2; // S=4*qz
        out->w = (mat->cols[1][0] - mat->cols[0][1]) / S;
        out->x = (mat->cols[0][2] + mat->cols[2][0]) / S;
        out->y = (mat->cols[1][2] + mat->cols[2][1]) / S;
        out->z = 0.25 * S;
    }
}

/* Algorithm from:
 * https://en.wikipedia.org/wiki/Conversion_between_quaternions_and_Euler_angles
 */
void PFM_Quat_ToEuler(quat_t *q, float *out_roll, float *out_pitch, float *out_yaw)
{
    /* roll (x-axis rotation) */
    if(out_roll) {
        float sinr = 2.0f * (q->w * q->x + q->y * q->z);
        float cosr = 1.0f - 2.0f * (q->x * q->x + q->y * q->y);
        *out_roll = RAD_TO_DEG(atan2(sinr, cosr));
    }

    /* pitch (y-axis rotation) */
    if(out_pitch) {
        float sinp = 2.0f * (q->w * q->y - q->z * q->x);
        if (fabs(sinp) >= 1)
            *out_pitch = RAD_TO_DEG((sinp >= 0.0f) ? (M_PI / 2) : -(M_PI / 2)); // use 90 degrees if out of range
        else
            *out_pitch = RAD_TO_DEG(asin(sinp));
    }

    /* yaw (z-axis rotation) */
    if(out_yaw) {
        double siny = 2.0f * (q->w * q->z + q->x * q->y);
        double cosy = 1.0f - 2.0f * (q->y * q->y + q->z * q->z);
        *out_yaw = RAD_TO_DEG(atan2(siny, cosy));
    }
}

void PFM_Quat_MultQuat(quat_t *op1, quat_t *op2, quat_t *out)
{
    out->x = ( op1->x * op2->w) + (op1->y * op2->z) - (op1->z * op2->y) + (op1->w * op2->x);
    out->y = (-op1->x * op2->z) + (op1->y * op2->w) + (op1->z * op2->x) + (op1->w * op2->y);
    out->z = ( op1->x * op2->y) - (op1->y * op2->x) + (op1->z * op2->w) + (op1->w * op2->z);
    out->w = (-op1->x * op2->x) - (op1->y * op2->y) - (op1->z * op2->z) + (op1->w * op2->w);
}

void PFM_Quat_Normal(quat_t *op1, quat_t *out)
{
    GLfloat len = sqrt(
         op1->x * op1->x 
       + op1->y * op1->y
       + op1->z * op1->z 
       + op1->w * op1->w
    ); 
    out->x = op1->x / len;
    out->y = op1->y / len;
    out->z = op1->z / len;
    out->w = op1->w / len;
}

void PFM_Quat_Inverse(quat_t *op1, quat_t *out)
{
    out->x = -op1->x;
    out->y = -op1->y;
    out->z = -op1->z;
    out->w =  op1->w;
}

GLfloat PFM_Quat_PitchDiff(quat_t *op1, quat_t *op2)
{
    vec4_t front = (vec4_t){1.0f, 0.0f, 0.0f, 1.0f};
    vec4_t dir_homo;
    mat4x4_t mat;

    PFM_Mat4x4_RotFromQuat(op1, &mat);
    PFM_Mat4x4_Mult4x1(&mat, &front, &dir_homo);

    vec3_t dir1 = (vec3_t){
        dir_homo.x / dir_homo.w,
        dir_homo.y / dir_homo.w,
        dir_homo.z / dir_homo.w,
    };

    PFM_Mat4x4_RotFromQuat(op2, &mat);
    PFM_Mat4x4_Mult4x1(&mat, &front, &dir_homo);

    vec3_t dir2 = (vec3_t){
        dir_homo.x / dir_homo.w,
        dir_homo.y / dir_homo.w,
        dir_homo.z / dir_homo.w,
    };

    float dot = dir1.x * dir2.x + dir1.z * dir2.z;
    float det = dir1.x * dir2.z - dir1.z * dir2.x;
    return atan2(det, dot);
}

GLfloat PFM_BilinearInterp(GLfloat q11, GLfloat q12, GLfloat q21, GLfloat q22,
                           GLfloat x1,  GLfloat x2,  GLfloat y1,  GLfloat y2,
                           GLfloat x,   GLfloat y)
{
    float x2x1, y2y1, x2x, y2y, yy1, xx1;
    x2x1 = x2 - x1;
    y2y1 = y2 - y1;
    x2x = x2 - x;
    y2y = y2 - y;
    yy1 = y - y1;
    xx1 = x - x1;
    return 1.0 / (x2x1 * y2y1) * (
        q11 * x2x * y2y +
        q21 * xx1 * y2y +
        q12 * x2x * yy1 +
        q22 * xx1 * yy1
    );
}

