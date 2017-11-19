#include "pf_math.h"
#include <string.h>
#include <assert.h>

int PFM_vec2_dot(vec2_t *op1, vec2_t *op2)
{
    return op1->x * op2->x + 
           op1->y * op1->y;
}

void PFM_vec2_add(vec2_t *op1, vec2_t *op2, vec2_t *out)
{
    out->x = op1->x + op2->y; 
    out->y = op1->y + op1->y;
}

void PFM_vec2_sub(vec2_t *op1, vec2_t *op2, vec2_t *out)
{
    out->x = op1->x - op2->x;
    out->y = op1->y - op2->y;
}

void PFM_vec2_scale(vec2_t *op1, GLfloat scale, vec2_t *out)
{
    out->x = op1->x * scale;
    out->y = op1->y * scale;
}

GLfloat PFM_vec2_len(vec2_t *op1)
{
    return sqrt(op1->x * op1->x + 
                op1->y * op1->y);
}

void PFM_vec2_normal(vec2_t *op1,  vec2_t *out)
{
    GLfloat len = PFM_vec2_len(op1); 

    out->x = op1->x / len;
    out->y = op1->y / len;
}

void PFM_vec3_cross(vec3_t *a, vec3_t *b, vec3_t *out)
{
    out->x =   a->y * b->z - a->z * b->y;
    out->y = -(a->x * b->z - a->z * b->x);
    out->z =   a->x * b->y - a->y * b->x;
}

GLfloat PFM_vec3_dot(vec3_t *op1, vec3_t *op2)
{
    return op1->x * op2->x +
           op1->y * op2->y +
           op1->z * op2->z;
}

void PFM_vec3_add(vec3_t *op1, vec3_t *op2, vec3_t *out)
{
    for(int i = 0; i < 3; i++)
        out->raw[i] = op1->raw[i] + op2->raw[i];
}

void PFM_vec3_sub(vec3_t *op1, vec3_t *op2, vec3_t *out)
{
    for(int i = 0; i < 3; i++)
        out->raw[i] = op1->raw[i] - op2->raw[i];
}

void PFM_vec3_scale(vec3_t *op1, GLfloat scale, vec3_t *out)
{
    for(int i = 0; i < 3; i++)
        out->raw[i] = op1->raw[i] * scale;
}

GLfloat PFM_vec3_len (vec3_t *op1)
{
    return sqrt(op1->x * op1->x + 
                op1->y * op1->y +
                op1->z * op1->z);
}

void PFM_vec3_normal(vec3_t *op1, vec3_t *out)
{
    GLfloat len = PFM_vec3_len(op1); 

    for(int i = 0; i < 3; i++)
        out->raw[i] = op1->raw[i] / len;
}

void PFM_vec3_dump(vec3_t *vec, FILE *dumpfile)
{
    fprintf(dumpfile, "(%.4f, %.4f, %.4f)\n", vec->x, vec->y, vec->z);
}

GLfloat PFM_vec4_dot(vec4_t *op1,  vec4_t *op2, vec4_t *out)
{
    return op1->x * op2->x +
           op1->y * op2->y +
           op1->z * op2->z +
           op1->w * op2->w;
}

void PFM_vec4_add(vec4_t *op1, vec4_t *op2, vec4_t *out)
{
    for(int i = 0; i < 4; i++)
        out->raw[i] = op1->raw[i] + op2->raw[i];
}

void PFM_vec4_sub(vec4_t *op1, vec4_t *op2, vec4_t *out)
{
    for(int i = 0; i < 4; i++)
        out->raw[i] = op1->raw[i] - op2->raw[i];
}

void PFM_vec4_scale(vec4_t *op1, GLfloat scale, vec4_t *out)
{
    for(int i = 0; i < 4; i++)
        out->raw[i] = op1->raw[i] * scale;
}

GLfloat PFM_vec4_len(vec4_t *op1)
{
    return sqrt(op1->x * op1->x + 
                op1->y * op1->y +
                op1->z * op1->z +
                op1->w * op1->w);
}

void PFM_vec4_normal(vec4_t *op1, vec4_t *out)
{
    GLfloat len = PFM_vec4_len(op1); 

    for(int i = 0; i < 4; i++)
        out->raw[i] = op1->raw[i] / len;
}

void PFM_vec4_dump(vec4_t *vec,  FILE *dumpfile)
{
    fprintf(dumpfile, "(%.4f, %.4f, %.4f, %.4f)\n", vec->x, vec->y, vec->z, vec->w);
}

void PFM_mat3x3_scale(mat3x3_t *op1, GLfloat scale, mat3x3_t *out)
{
    for(int i = 0; i < 9; i++) {
        out->raw[i] = op1->raw[i] * scale;
    }
}

void PFM_mat3x3_mult3x3 (mat3x3_t *op1, mat3x3_t *op2, mat3x3_t *out)
{
    for(int r = 0; r < 3; r++) {
        for(int c = 0; c < 3; c++) {
            out->cols[c][r] = 0.0f;
            for(int k = 0; k < 3; k++)
                out->cols[c][r] += op1->cols[k][r] * op2->cols[c][k]; 
        }
    }
}

void PFM_mat3x3_mult3x1 (mat3x3_t *op1,  vec3_t *op2,  vec3_t *out)
{
    for(int r = 0; r < 3; r++) {
        out->raw[r] = 0.0f;
        for(int c = 0; c < 3; c++)
            out->raw[r] += op1->cols[c][r] * op2->raw[c];
    }
}

void PFM_mat3x3_identity(mat3x3_t *out)
{
    memset(out, 0, sizeof(mat3x3_t));

    for(int i = 0; i < 3; i++)
        out->cols[i][i] = 1;
}

void PFM_mat4x4_scale(mat4x4_t *op1, GLfloat scale, mat4x4_t *out)
{
    for(int i = 0; i < 16; i++) {
        out->raw[i] = op1->raw[i] * scale;
    }
}

void PFM_mat4x4_mult4x4 (mat4x4_t *op1, mat4x4_t *op2, mat4x4_t *out)
{
    for(int r = 0; r < 4; r++) {
        for(int c = 0; c < 4; c++) {
            out->cols[c][r] = 0.0f;
            for(int k = 0; k < 4; k++)
                out->cols[c][r] += op1->cols[k][r] * op2->cols[c][k]; 
        }
    }
}

void PFM_mat4x4_mult4x1(mat4x4_t *op1, vec4_t *op2, vec4_t *out)
{
    for(int r = 0; r < 4; r++) {
        out->raw[r] = 0.0f;
        for(int c = 0; c < 4; c++)
            out->raw[r] += op1->cols[c][r] * op2->raw[c];
    }
}

void PFM_mat4x4_identity(mat4x4_t *out)
{
    memset(out, 0, sizeof(mat4x4_t));

    for(int i = 0; i < 4; i++)
        out->cols[i][i] = 1;
}

void PFM_mat4x4_make_scale(GLfloat s1, GLfloat s2, GLfloat s3, mat4x4_t *out)
{
    PFM_mat4x4_identity(out);

    out->cols[0][0] = s1;
    out->cols[1][1] = s2;
    out->cols[2][2] = s3;
}

void PFM_mat4x4_make_trans(GLfloat tx, GLfloat ty, GLfloat tz, mat4x4_t *out)
{
    PFM_mat4x4_identity(out); 

    out->cols[3][0] = tx;
    out->cols[3][1] = ty;
    out->cols[3][2] = tz;
}

void PFM_mat4x4_make_rot_x(GLfloat radians, mat4x4_t *out)
{
    PFM_mat4x4_identity(out);

    out->cols[1][1] =  cos(radians);
    out->cols[1][2] =  sin(radians);

    out->cols[2][1] = -sin(radians);
    out->cols[2][2] =  cos(radians);
}

void PFM_mat4x4_make_rot_y(GLfloat radians, mat4x4_t *out)
{
    PFM_mat4x4_identity(out);

    out->cols[0][0] =  cos(radians);
    out->cols[0][2] = -sin(radians);

    out->cols[2][0] =  sin(radians);
    out->cols[2][2] =  cos(radians);
}

void PFM_mat4x4_make_rot_z(GLfloat radians, mat4x4_t *out)
{
    PFM_mat4x4_identity(out);

    out->cols[0][0] =  cos(radians);
    out->cols[0][1] =  sin(radians);

    out->cols[1][0] = -sin(radians);
    out->cols[1][1] =  cos(radians);
}

void PFM_mat4x4_make_rot(vec3_t *axis, GLfloat radians, mat4x4_t *out)
{
    //TODO: http://paulbourke.net/geometry/rotate/
}

/* Algorithm taken from:
 * http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.184.3942&rep=rep1&type=pdf
 */
void PFM_mat4x4_rot_from_quat(const quat_t *quat, mat4x4_t *out)
{
    PFM_mat4x4_identity(out);

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

void PFM_mat4x4_rot_from_euler(GLfloat deg_x, GLfloat deg_y, GLfloat deg_z, mat4x4_t *out)
{
    mat4x4_t x, y, z, tmp;

    PFM_mat4x4_make_rot_x(DEG_TO_RAD(deg_x), &x);
    PFM_mat4x4_make_rot_y(DEG_TO_RAD(deg_y), &y);
    PFM_mat4x4_make_rot_z(DEG_TO_RAD(deg_z), &z);

    PFM_mat4x4_mult4x4(&y, &z, &tmp);
    PFM_mat4x4_mult4x4(&x, &tmp, out);
}

/* - fov_radians is the vertical FOV angle
 * - This is OpenGL specific, where the positive Z-axis is pointing out of 
 * the screen
 * - Vectors multiplied by this matrix will already be in NDC
 * 
 */
void PFM_mat4x4_make_perspective(GLfloat fov_radians, GLfloat aspect_ratio, 
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

void PFM_mat4x4_make_look_at(vec3_t *camera_pos, vec3_t *target_pos, 
                             vec3_t *up, mat4x4_t *out)
{
    vec3_t camera_dir, right;

    PFM_vec3_sub(camera_pos, target_pos, &camera_dir);
    PFM_vec3_normal(&camera_dir, &camera_dir);
    PFM_vec3_cross(&camera_dir, up, &right);

    mat4x4_t axes, trans;
    PFM_mat4x4_identity(&axes);
    axes.cols[0][0] = right.x;
    axes.cols[1][0] = right.y;
    axes.cols[2][0] = right.z;

    axes.cols[0][1] = up->x;
    axes.cols[1][1] = up->y;
    axes.cols[2][1] = up->z;

    axes.cols[0][2] = camera_dir.x;
    axes.cols[1][2] = camera_dir.y;
    axes.cols[2][2] = camera_dir.z;

    PFM_mat4x4_make_trans(-camera_pos->x, -camera_pos->y, -camera_pos->z, &trans);
    PFM_mat4x4_mult4x4(&axes, &trans, out);
}

/* Implementation derived from Mesa 3D implementation */
void PFM_mat4x4_inverse(mat4x4_t *in, mat4x4_t *out)
{
    double inv[16], det;
    int i;

    inv[0] = in->raw[5]  * in->raw[10] * in->raw[15] - 
             in->raw[5]  * in->raw[11] * in->raw[14] - 
             in->raw[9]  * in->raw[6]  * in->raw[15] + 
             in->raw[9]  * in->raw[7]  * in->raw[14] +
             in->raw[13] * in->raw[6]  * in->raw[11] - 
             in->raw[13] * in->raw[7]  * in->raw[10];
    
    inv[4] = -in->raw[4]  * in->raw[10] * in->raw[15] + 
              in->raw[4]  * in->raw[11] * in->raw[14] + 
              in->raw[8]  * in->raw[6]  * in->raw[15] - 
              in->raw[8]  * in->raw[7]  * in->raw[14] - 
              in->raw[12] * in->raw[6]  * in->raw[11] + 
              in->raw[12] * in->raw[7]  * in->raw[10];

    inv[8] = in->raw[4]  * in->raw[9] * in->raw[15] - 
             in->raw[4]  * in->raw[11] * in->raw[13] - 
             in->raw[8]  * in->raw[5] * in->raw[15] + 
             in->raw[8]  * in->raw[7] * in->raw[13] + 
             in->raw[12] * in->raw[5] * in->raw[11] - 
             in->raw[12] * in->raw[7] * in->raw[9];
    
    inv[12] = -in->raw[4]  * in->raw[9] * in->raw[14] + 
               in->raw[4]  * in->raw[10] * in->raw[13] +
               in->raw[8]  * in->raw[5] * in->raw[14] - 
               in->raw[8]  * in->raw[6] * in->raw[13] - 
               in->raw[12] * in->raw[5] * in->raw[10] + 
               in->raw[12] * in->raw[6] * in->raw[9];
    
    inv[1] = -in->raw[1]  * in->raw[10] * in->raw[15] + 
              in->raw[1]  * in->raw[11] * in->raw[14] + 
              in->raw[9]  * in->raw[2] * in->raw[15] - 
              in->raw[9]  * in->raw[3] * in->raw[14] - 
              in->raw[13] * in->raw[2] * in->raw[11] + 
              in->raw[13] * in->raw[3] * in->raw[10];
    
    inv[5] = in->raw[0]  * in->raw[10] * in->raw[15] - 
             in->raw[0]  * in->raw[11] * in->raw[14] - 
             in->raw[8]  * in->raw[2] * in->raw[15] + 
             in->raw[8]  * in->raw[3] * in->raw[14] + 
             in->raw[12] * in->raw[2] * in->raw[11] - 
             in->raw[12] * in->raw[3] * in->raw[10];
    
    inv[9] = -in->raw[0]  * in->raw[9] * in->raw[15] + 
              in->raw[0]  * in->raw[11] * in->raw[13] + 
              in->raw[8]  * in->raw[1] * in->raw[15] - 
              in->raw[8]  * in->raw[3] * in->raw[13] - 
              in->raw[12] * in->raw[1] * in->raw[11] + 
              in->raw[12] * in->raw[3] * in->raw[9];
    
    inv[13] = in->raw[0]  * in->raw[9] * in->raw[14] - 
              in->raw[0]  * in->raw[10] * in->raw[13] - 
              in->raw[8]  * in->raw[1] * in->raw[14] + 
              in->raw[8]  * in->raw[2] * in->raw[13] + 
              in->raw[12] * in->raw[1] * in->raw[10] - 
              in->raw[12] * in->raw[2] * in->raw[9];
    
    inv[2] = in->raw[1]  * in->raw[6] * in->raw[15] - 
             in->raw[1]  * in->raw[7] * in->raw[14] - 
             in->raw[5]  * in->raw[2] * in->raw[15] + 
             in->raw[5]  * in->raw[3] * in->raw[14] + 
             in->raw[13] * in->raw[2] * in->raw[7] - 
             in->raw[13] * in->raw[3] * in->raw[6];
    
    inv[6] = -in->raw[0]  * in->raw[6] * in->raw[15] + 
              in->raw[0]  * in->raw[7] * in->raw[14] + 
              in->raw[4]  * in->raw[2] * in->raw[15] - 
              in->raw[4]  * in->raw[3] * in->raw[14] - 
              in->raw[12] * in->raw[2] * in->raw[7] + 
              in->raw[12] * in->raw[3] * in->raw[6];
    
    inv[10] = in->raw[0]  * in->raw[5] * in->raw[15] - 
              in->raw[0]  * in->raw[7] * in->raw[13] - 
              in->raw[4]  * in->raw[1] * in->raw[15] + 
              in->raw[4]  * in->raw[3] * in->raw[13] + 
              in->raw[12] * in->raw[1] * in->raw[7] - 
              in->raw[12] * in->raw[3] * in->raw[5];
    
    inv[14] = -in->raw[0]  * in->raw[5] * in->raw[14] + 
               in->raw[0]  * in->raw[6] * in->raw[13] + 
               in->raw[4]  * in->raw[1] * in->raw[14] - 
               in->raw[4]  * in->raw[2] * in->raw[13] - 
               in->raw[12] * in->raw[1] * in->raw[6] + 
               in->raw[12] * in->raw[2] * in->raw[5];
    
    inv[3] = -in->raw[1] * in->raw[6] * in->raw[11] + 
              in->raw[1] * in->raw[7] * in->raw[10] + 
              in->raw[5] * in->raw[2] * in->raw[11] - 
              in->raw[5] * in->raw[3] * in->raw[10] - 
              in->raw[9] * in->raw[2] * in->raw[7] + 
              in->raw[9] * in->raw[3] * in->raw[6];
    
    inv[7] = in->raw[0] * in->raw[6] * in->raw[11] - 
             in->raw[0] * in->raw[7] * in->raw[10] - 
             in->raw[4] * in->raw[2] * in->raw[11] + 
             in->raw[4] * in->raw[3] * in->raw[10] + 
             in->raw[8] * in->raw[2] * in->raw[7] - 
             in->raw[8] * in->raw[3] * in->raw[6];
    
    inv[11] = -in->raw[0] * in->raw[5] * in->raw[11] + 
               in->raw[0] * in->raw[7] * in->raw[9] + 
               in->raw[4] * in->raw[1] * in->raw[11] - 
               in->raw[4] * in->raw[3] * in->raw[9] - 
               in->raw[8] * in->raw[1] * in->raw[7] + 
               in->raw[8] * in->raw[3] * in->raw[5];
    
    inv[15] = in->raw[0] * in->raw[5] * in->raw[10] - 
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

/* Algorithm from:  
 * http://www.euclideanspace.com/maths/geometry/rotations/conversions/quaternionToMatrix/ 
 */
void PFM_quat_from_rot_mat(mat4x4_t *mat, quat_t *out)
{
    GLfloat tr = mat->cols[0][0] + mat->cols[1][1] + mat->cols[2][2];

    if (tr > 0) {

        GLfloat S = sqrt(tr+1.0) * 2; // S=4*qw 
        out->w = 0.25 * S;
        out->x = (mat->cols[2][1] - mat->cols[1][2]) / S;
        out->y = (mat->cols[0][2] - mat->cols[2][0]) / S; 
        out->z = (mat->cols[1][0] - mat->cols[0][1]) / S; 

    } else if ((mat->cols[0][0] > mat->cols[1][1])&(mat->cols[0][0] > mat->cols[2][2])) {

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

