#include <pf_math.h>
#include <string.h>

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
            out->raw[r] += op1->cols[c][r] * op2->raw[r];
    }
}

void PFM_mat3x3_identity(mat3x3_t *out)
{
    out->m0 = 1; out->m3 = 0; out->m6 = 0;
    out->m1 = 0; out->m4 = 1; out->m7 = 0;
    out->m2 = 0; out->m5 = 0; out->m8 = 1;
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
    for(int r = 0; r < 3; r++) {
        out->raw[r] = 0.0f;
        for(int c = 0; c < 4; c++)
            out->raw[r] += op1->cols[c][r] * op2->raw[r];
    }
}

void PFM_mat4x4_identity(mat4x4_t *out)
{
    out->m0 = 1; out->m4 = 0; out->m8  = 0; out->m12 = 0;
    out->m1 = 0; out->m5 = 1; out->m9  = 0; out->m13 = 0; 
    out->m2 = 0; out->m6 = 0; out->m10 = 1; out->m14 = 0;
    out->m3 = 0; out->m7 = 0; out->m11 = 0; out->m15 = 1;
}

void PFM_mat4x4_make_scale(GLfloat s1, GLfloat s2, GLfloat s3, mat4x4_t *out)
{
    out->m0 = s1; out->m4 = 0;  out->m8  = 0;  out->m12 = 0;
    out->m1 = 0;  out->m5 = s2; out->m9  = 0;  out->m13 = 0; 
    out->m2 = 0;  out->m6 = 0;  out->m10 = s3; out->m14 = 0;
    out->m3 = 0;  out->m7 = 0;  out->m11 = 0;  out->m15 = 1;
}

void PFM_mat4x4_make_trans(GLfloat tx, GLfloat ty, GLfloat tz, mat4x4_t *out)
{
    out->m0 = 1;  out->m4 = 0;  out->m8  = 0;  out->m12 = tx;
    out->m1 = 0;  out->m5 = 1;  out->m9  = 0;  out->m13 = ty; 
    out->m2 = 0;  out->m6 = 0;  out->m10 = 1;  out->m14 = tz;
    out->m3 = 0;  out->m7 = 0;  out->m11 = 0;  out->m15 = 1;
}

void PFM_mat4x4_make_rot_x(GLfloat radians, mat4x4_t *out)
{
    out->m0 = 1;  out->m4 = 0;            out->m8  = 0;             out->m12 = 0;
    out->m1 = 0;  out->m5 = cos(radians); out->m9  = -sin(radians); out->m13 = 0; 
    out->m2 = 0;  out->m6 = sin(radians); out->m10 =  cos(radians); out->m14 = 0;
    out->m3 = 0;  out->m7 = 0;            out->m11 = 0;             out->m15 = 1;
}

void PFM_mat4x4_make_rot_y(GLfloat radians, mat4x4_t *out)
{
    out->m0 =  cos(radians); out->m4 = 0;  out->m8  = sin(radians); out->m12 = 0;
    out->m1 = 0;             out->m5 = 1;  out->m9  = 0;            out->m13 = 0; 
    out->m2 = -sin(radians); out->m6 = 0;  out->m10 = cos(radians); out->m14 = 0;
    out->m3 = 0;             out->m7 = 0;  out->m11 = 0;            out->m15 = 1;
}

void PFM_mat4x4_make_rot_z(GLfloat radians, mat4x4_t *out)
{
    out->m0 = cos(radians); out->m4 = -sin(radians); out->m8  = 0;  out->m12 = 0;
    out->m1 = sin(radians); out->m5 =  cos(radians); out->m9  = 0;  out->m13 = 0; 
    out->m2 = 0;            out->m6 = 0;             out->m10 = 1;  out->m14 = 0;
    out->m3 = 0;            out->m7 = 0;             out->m11 = 0;  out->m15 = 1;
}

void PFM_mat4x4_make_rot(vec3_t *axis, GLfloat radians, mat4x4_t *out)
{
    //TODO: http://paulbourke.net/geometry/rotate/
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

    PFM_mat4x4_identity(&trans);
    trans.cols[3][0] = -camera_pos->x;
    trans.cols[3][1] = -camera_pos->y;
    trans.cols[3][0] = -camera_pos->z;

    PFM_mat4x4_mult4x4(&axes, &trans, out);
}

