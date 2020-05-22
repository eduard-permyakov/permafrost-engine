/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#include "gl_state.h"
#include "../lib/public/khash.h"
#include "../lib/public/pf_string.h"

#include <assert.h>
#include <string.h>

/* private uval */
struct puval{
    struct uval v;
    bool dirty;
};

KHASH_MAP_INIT_STR(puval, struct puval)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(puval) *s_state_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static size_t uval_size(const struct uval *uv)
{
    switch(uv->type) {
    case UTYPE_FLOAT:
        return sizeof(float);
    case UTYPE_VEC2:
        return sizeof(vec2_t);
    case UTYPE_VEC3:
        return sizeof(vec3_t);
    case UTYPE_VEC4:
        return sizeof(vec4_t);
    case UTYPE_INT:
        return sizeof(GLint);
    case UTYPE_IVEC2:
        return sizeof(GLint[2]);
    case UTYPE_IVEC3:
        return sizeof(GLint[3]);
    case UTYPE_IVEC4:
        return sizeof(GLint[4]);
    case UTYPE_MAT3:
        return sizeof(mat3x3_t);
    case UTYPE_MAT4:
        return sizeof(mat4x4_t);
    default:
        return (assert(0), 0);
    }
}

static bool uval_equal(const struct uval *a, const struct uval *b)
{
    if(a->type != b->type)
        return false;
    return (0 == memcmp(&a->val, &b->val, uval_size(a)));
}

static void uval_install(GLuint shader_prog, const char *uname, const struct uval *uv)
{
    glUseProgram(shader_prog);
    GLuint loc = glGetUniformLocation(shader_prog, uname);

    switch(uv->type) {
    case UTYPE_FLOAT:
        glUniform1fv(loc,  1, &uv->val.as_float);
        break;
    case UTYPE_VEC2:
        glUniform1fv(loc,  2, uv->val.as_vec2.raw);
        break;
    case UTYPE_VEC3:
        glUniform1fv(loc,  3, uv->val.as_vec3.raw);
        break;
    case UTYPE_VEC4:
        glUniform1fv(loc,  4, uv->val.as_vec4.raw);
        break;
    case UTYPE_INT:
        glUniform1iv(loc,  1, &uv->val.as_int);
        break;
    case UTYPE_IVEC2:
        glUniform1iv(loc,  2, uv->val.as_ivec2);
        break;
    case UTYPE_IVEC3:
        glUniform1iv(loc,  3, uv->val.as_ivec3);
        break;
    case UTYPE_IVEC4:
        glUniform1iv(loc,  4, uv->val.as_ivec4);
        break;
    case UTYPE_MAT3:
        glUniform1fv(loc,  9, uv->val.as_mat3.raw);
        break;
    case UTYPE_MAT4:
        glUniform1fv(loc, 16, uv->val.as_mat4.raw);
        break;
    default:
        assert(0);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_StateInit(void)
{
    s_state_table = kh_init(puval);
    if(!s_state_table)
        return false;
    return true;
}

void R_GL_StateShutdown(void)
{
    const char *key;
    struct puval curr;
    (void)curr;

    kh_foreach(s_state_table, key, curr, {
        free((void*)key);
    });
    kh_destroy(puval, s_state_table);
}

void R_GL_StateSet(const char *uname, struct uval val)
{
    struct puval *p = NULL;
    khiter_t k = kh_get(puval, s_state_table, uname);

    if(k != kh_end(s_state_table)) {
    
        p = &kh_value(s_state_table, k);
        if(uval_equal(&p->v, &val))
            return;
    }else{
    
        int status;
        k = kh_put(puval, s_state_table, pf_strdup(uname), &status);
        assert(status != -1 && status != 0);
        p = &kh_value(s_state_table, k);
    }

    assert(p);
    *p = (struct puval){
        .v = val,
        .dirty = true,
    };
}

void R_GL_StateInstall(const char *uname, GLuint shader_prog)
{
    khiter_t k = kh_get(puval, s_state_table, uname);
    assert(k != kh_end(s_state_table));
    struct puval *p = &kh_value(s_state_table, k);

    if(!p->dirty)
        return;

    uval_install(shader_prog, uname, &p->v);
}

