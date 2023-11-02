/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2023 Eduard Permyakov 
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

#include "public/SDL_vec_rwops.h"
#include "public/vec.h"

#include <stdlib.h>
#include <assert.h>

VEC_TYPE(uchar, unsigned char)
VEC_IMPL(static inline, uchar, unsigned char)

#define VEC(rwops)          ((vec_uchar_t*)((rwops)->hidden.unknown.data1))
#define SEEK_IDX(rwops)     ((uintptr_t)((rwops)->hidden.unknown.data2))
#define SDL_RWOPS_VEC       (0xffff)

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static Sint64 rw_vec_size(SDL_RWops *ctx)
{
    assert(ctx->type == SDL_RWOPS_VEC);
    return vec_size(VEC(ctx));
}

static Sint64 rw_vec_seek(SDL_RWops *ctx, Sint64 offset, int whence)
{
    assert(ctx->type == SDL_RWOPS_VEC);
    switch (whence) {
    case RW_SEEK_SET:
        ctx->hidden.unknown.data2 = (void*)offset;
        break;
    case RW_SEEK_CUR:
        ctx->hidden.unknown.data2 = (void*)(SEEK_IDX(ctx) + offset);
        break;
    case RW_SEEK_END:
        ctx->hidden.unknown.data2 = (void*)(rw_vec_size(ctx)-1 + offset);
        break;
    default:
        return SDL_SetError("rw_vec_seek: Unknown value for 'whence'");
    }
    return SEEK_IDX(ctx);
}

static size_t rw_vec_write(SDL_RWops *ctx, const void *ptr, size_t size, size_t num)
{
    assert(ctx->type == SDL_RWOPS_VEC);

    if(rw_vec_size(ctx) <= SEEK_IDX(ctx) + size * num
    && !vec_uchar_resize(VEC(ctx), SEEK_IDX(ctx) + size * num)) {
    
        SDL_Error(SDL_EFWRITE);
        return 0;
    }

    for(int i = 0; i < size * num; i++) {
        vec_uchar_push(VEC(ctx), *((unsigned char*)ptr));
        ptr = ((unsigned char*)ptr) + 1;
    }

    ctx->hidden.unknown.data2 = (void*)(SEEK_IDX(ctx) + size * num);
    return num;
}

static size_t rw_vec_read(SDL_RWops *ctx, void *ptr, size_t size, size_t num)
{
    assert(ctx->type == SDL_RWOPS_VEC);

    if(rw_vec_size(ctx) < SEEK_IDX(ctx) + size * num) {

        SDL_Error(SDL_EFREAD);
        return 0;
    }

    memcpy(ptr, &vec_AT(VEC(ctx), SEEK_IDX(ctx)), size * num);
    ctx->hidden.unknown.data2 = (void*)(SEEK_IDX(ctx) + size * num);
    return num;
}

static int rw_vec_close(SDL_RWops *ctx)
{
    assert(ctx->type == SDL_RWOPS_VEC);
    vec_uchar_destroy(ctx->hidden.unknown.data1);
    free(ctx);
    return 0;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

SDL_RWops *PFSDL_VectorRWOps(void)
{
    SDL_RWops *ret = malloc(sizeof(SDL_RWops) + sizeof(vec_uchar_t));
    if(!ret)
        return ret;

    ret->size = rw_vec_size;
    ret->seek = rw_vec_seek;
    ret->read = rw_vec_read;
    ret->write = rw_vec_write;
    ret->close = rw_vec_close;
    ret->type = SDL_RWOPS_VEC;

    ret->hidden.unknown.data1 = ret + 1;
    vec_uchar_init(VEC(ret));
    ret->hidden.unknown.data2 = (void*)0; /* This is the seek index */

    return ret;
}

const char *PFSDL_VectorRWOpsRaw(SDL_RWops *ctx)
{
    return (const char*)VEC(ctx)->array;
}

bool PFSDL_VectorRWOpsReserve(SDL_RWops* ctx, size_t size)
{
    return vec_uchar_resize(VEC(ctx), size);
}
