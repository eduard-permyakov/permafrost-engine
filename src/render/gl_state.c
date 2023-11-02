/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2023 Eduard Permyakov 
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
#include "gl_shader.h"
#include "gl_assert.h"
#include "../lib/public/khash.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/mpool.h"

#include <assert.h>
#include <string.h>


#define NINSTALLED_CACHE (32)

struct buff{
    char raw[16384];
};

struct compval{
    enum utype type;
    uint32_t   hash;
    size_t     itemsize;
    size_t     nitems;
    mp_ref_t   descs;
    mp_ref_t   data; 
};

struct arrval{
    enum utype type;
    enum utype itemtype;
    uint32_t   hash;
    size_t     nitems;
    mp_ref_t   data;
};

/* private uval */
struct puval{
    union{
        struct uval v;
        struct arrval av;
        struct compval cv;
    };
    size_t ninstalled;
    GLuint installed_progs[NINSTALLED_CACHE];
};

KHASH_MAP_INIT_STR(puval, struct puval)

MPOOL_TYPE(buff, struct buff)
MPOOL_IMPL(static inline, buff, struct buff)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(puval) *s_state_table;
static mp_buff_t       s_buff_pool;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static size_t uval_size(enum utype type)
{
    switch(type) {
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
    return (0 == memcmp(&a->val, &b->val, uval_size(a->type)));
}

static void uval_install(GLuint shader_prog, const char *uname, const struct uval *uv)
{
    GLuint loc = glGetUniformLocation(shader_prog, uname);
    if(loc == ((GLuint)-1))
        return;

    switch(uv->type) {
    case UTYPE_FLOAT:
        glUniform1fv(loc, 1, &uv->val.as_float);
        break;
    case UTYPE_VEC2:
        glUniform2fv(loc, 1, uv->val.as_vec2.raw);
        break;
    case UTYPE_VEC3:
        glUniform3fv(loc, 1, uv->val.as_vec3.raw);
        break;
    case UTYPE_VEC4:
        glUniform4fv(loc, 1, uv->val.as_vec4.raw);
        break;
    case UTYPE_INT:
        glUniform1iv(loc, 1, &uv->val.as_int);
        break;
    case UTYPE_IVEC2:
        glUniform2iv(loc, 1, uv->val.as_ivec2);
        break;
    case UTYPE_IVEC3:
        glUniform3iv(loc, 1, uv->val.as_ivec3);
        break;
    case UTYPE_IVEC4:
        glUniform4iv(loc, 1, uv->val.as_ivec4);
        break;
    case UTYPE_MAT3:
        glUniformMatrix3fv(loc, 1, GL_FALSE, uv->val.as_mat3.raw);
        break;
    case UTYPE_MAT4:
        glUniformMatrix4fv(loc, 1, GL_FALSE, uv->val.as_mat4.raw);
        break;
    default:
        assert(0);
    }
}

static void uval_array_install(GLuint shader_prog, const char *uname, const struct arrval *av)
{
    GLuint loc = glGetUniformLocation(shader_prog, uname);
    void *data = mp_buff_entry(&s_buff_pool, av->data)->raw;

    if(loc == ((GLuint)-1))
        return;

    switch(av->itemtype) {
    case UTYPE_FLOAT:
        glUniform1fv(loc, av->nitems, data);
        break;
    case UTYPE_VEC2:
        glUniform2fv(loc, av->nitems, data);
        break;
    case UTYPE_VEC3:
        glUniform3fv(loc, av->nitems, data);
        break;
    case UTYPE_VEC4:
        glUniform4fv(loc, av->nitems, data);
        break;
    case UTYPE_INT:
        glUniform1iv(loc, av->nitems, data);
        break;
    case UTYPE_IVEC2:
        glUniform2iv(loc, av->nitems, data);
        break;
    case UTYPE_IVEC3:
        glUniform3iv(loc, av->nitems, data);
        break;
    case UTYPE_IVEC4:
        glUniform4iv(loc, av->nitems, data);
        break;
    case UTYPE_MAT3:
        glUniformMatrix3fv(loc, av->nitems, GL_FALSE, data);
        break;
    case UTYPE_MAT4:
        glUniformMatrix4fv(loc, av->nitems, GL_FALSE, data);
        break;
    default:
        assert(0);
    }
}

static void uval_composite_install(GLuint shader_prog, const char *uname, const struct compval *cv)
{
    unsigned char *data = (unsigned char*)mp_buff_entry(&s_buff_pool, cv->data)->raw;
    const struct mdesc *descs = (const struct mdesc*)mp_buff_entry(&s_buff_pool, cv->descs)->raw;

    for(int i = 0; i < cv->nitems; i++) {
    
        const struct mdesc *curr = descs;
        while(curr->name) {

            char uname_full[256];
            pf_snprintf(uname_full, sizeof(uname_full), "%s[%d].%s", uname, i, curr->name);
            GLuint loc = glGetUniformLocation(shader_prog, uname_full);

            if(loc == ((GLuint)-1))
                continue;

            switch(curr->type) {
            case UTYPE_FLOAT:
                glUniform1fv(loc, 1, (void*)(data + curr->offset));
                break;
            case UTYPE_VEC2:
                glUniform2fv(loc, 1, (void*)(data + curr->offset));
                break;
            case UTYPE_VEC3:
                glUniform3fv(loc, 1, (void*)(data + curr->offset));
                break;
            case UTYPE_VEC4:
                glUniform4fv(loc, 1, (void*)(data + curr->offset));
                break;
            case UTYPE_INT:
                glUniform1iv(loc, 1, (void*)(data + curr->offset));
                break;
            case UTYPE_IVEC2:
                glUniform2iv(loc, 1, (void*)(data + curr->offset));
                break;
            case UTYPE_IVEC3:
                glUniform3iv(loc, 1, (void*)(data + curr->offset));
                break;
            case UTYPE_IVEC4:
                glUniform4iv(loc, 1, (void*)(data + curr->offset));
                break;
            case UTYPE_MAT3:
                glUniformMatrix3fv(loc, 1, GL_FALSE, (void*)(data + curr->offset));
                break;
            case UTYPE_MAT4:
                glUniformMatrix4fv(loc, 1, GL_FALSE, (void*)(data + curr->offset));
                break;
            default:
                assert(0);
            }
            curr++;
        }
        data += cv->itemsize;
    }
}

static uint32_t hash_adler32(const void *data, size_t len) 
{
     const uint8_t *buff = (const uint8_t*)data;

     uint32_t s1 = 1;
     uint32_t s2 = 0;

     for(size_t n = 0; n < len; n++) {
        s1 = (s1 + buff[n]) % 65521;
        s2 = (s2 + s1) % 65521;
     }     
     return (s2 << 16) | s1;
}

static bool uval_installed(const struct puval *p, GLuint prog)
{
    for(int i = 0; i < p->ninstalled; i++) {
        if(p->installed_progs[i] == prog)
            return true;
    }
    return false;
}

static void uval_installed_add(struct puval *p, GLuint prog)
{
    if(p->ninstalled == NINSTALLED_CACHE)
        return;
    p->installed_progs[p->ninstalled++] = prog;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_StateInit(void)
{
    s_state_table = kh_init(puval);
    if(!s_state_table)
        goto fail_table;
    mp_buff_init(&s_buff_pool, true);
    if(!mp_buff_reserve(&s_buff_pool, 512))
        goto fail_pool;
    return true;

fail_pool:
    kh_destroy(puval, s_state_table);
fail_table:
    return false;
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
    mp_buff_destroy(&s_buff_pool);
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
        .ninstalled = 0,
    };
}

bool R_GL_StateGet(const char *uname, struct uval *out)
{
    struct puval *p = NULL;
    khiter_t k = kh_get(puval, s_state_table, uname);

    if(k == kh_end(s_state_table))
        return false;

    p = &kh_value(s_state_table, k);
    if(p->v.type == UTYPE_COMPOSITE || p->v.type == UTYPE_ARRAY)
        return false;

    *out = p->v;
    return true;
}

void R_GL_StateInstall(const char *uname, GLuint shader_prog)
{
    khiter_t k = kh_get(puval, s_state_table, uname);
    if (k == kh_end(s_state_table))
        return;

    struct puval *p = &kh_value(s_state_table, k);

    if(p->v.type == UTYPE_ARRAY) {
        uval_array_install(shader_prog, uname, &p->av);
    }else if(p->v.type == UTYPE_COMPOSITE) {
        uval_composite_install(shader_prog, uname, &p->cv);
    }else{
        uval_install(shader_prog, uname, &p->v);
    }

    uval_installed_add(p, shader_prog);
}

void R_GL_StateSetArray(const char *uname, enum utype itemtype, size_t size, void *data)
{
    size_t len = uval_size(itemtype) * size;
    uint32_t hash = hash_adler32(data, len);

    struct puval *p = NULL;
    khiter_t k = kh_get(puval, s_state_table, uname);

    if(k != kh_end(s_state_table)) {
        p = &kh_value(s_state_table, k);
        if(p->av.hash == hash)
            return;
        mp_buff_free(&s_buff_pool, p->av.data);
    }else{
    
        int status;
        k = kh_put(puval, s_state_table, pf_strdup(uname), &status);
        assert(status != -1 && status != 0);
        p = &kh_value(s_state_table, k);
    }

    mp_ref_t data_ref = mp_buff_alloc(&s_buff_pool);
    assert(data_ref);
    assert(len <= sizeof(((struct buff*)NULL)->raw));

    memcpy(mp_buff_entry(&s_buff_pool, data_ref)->raw, data, len);
    *p = (struct puval){
        .av = (struct arrval){
            .type = UTYPE_ARRAY,
            .itemtype = itemtype,
            .hash = hash,
            .nitems = size,
            .data = data_ref
        },
        .ninstalled = true
    };
}

void R_GL_StateSetComposite(const char *uname, const struct mdesc *descs, 
                            size_t itemsize, size_t nitems, void *data)
{
    size_t len = nitems * itemsize;
    uint32_t hash = hash_adler32(data, len);

    struct puval *p = NULL;
    khiter_t k = kh_get(puval, s_state_table, uname);

    if(k != kh_end(s_state_table)) {
        p = &kh_value(s_state_table, k);
        if(p->cv.hash == hash)
            return;
        mp_buff_free(&s_buff_pool, p->cv.descs);
        mp_buff_free(&s_buff_pool, p->cv.data);
    }else{
    
        int status;
        k = kh_put(puval, s_state_table, pf_strdup(uname), &status);
        assert(status != -1 && status != 0);
        p = &kh_value(s_state_table, k);
    }

    mp_ref_t data_ref = mp_buff_alloc(&s_buff_pool);
    mp_ref_t desc_ref = mp_buff_alloc(&s_buff_pool);

    assert(data_ref && desc_ref);
    assert(len <= sizeof(((struct buff*)NULL)->raw));

    const struct mdesc *curr = descs;
    struct mdesc *base = (struct mdesc*)mp_buff_entry(&s_buff_pool, desc_ref)->raw;
    while(curr->name) {
        *base++ = *curr++;
    }
    *base = (struct mdesc){0};
    memcpy(mp_buff_entry(&s_buff_pool, data_ref)->raw, data, len);

    *p = (struct puval){
        .cv = (struct compval){
            .type = UTYPE_COMPOSITE,
            .hash = hash,
            .itemsize = itemsize,
            .nitems = nitems,
            .descs = desc_ref,
            .data = data_ref
        },
        .ninstalled = 0
    };
}

