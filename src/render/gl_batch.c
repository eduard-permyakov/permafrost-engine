/* 
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2026 Eduard Permyakov 
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

#define MEM_FILE_SYS MEM_SYS_RENDER
#define MEM_FILE_SUB MEM_SUB_RENDER_GL_BATCH

#include "gl_batch.h"
#include "gl_ringbuffer.h"
#include "gl_texture.h"
#include "gl_mesh.h"
#include "gl_assert.h"
#include "gl_material.h"
#include "gl_shader.h"
#include "gl_perf.h"
#include "gl_vertex.h"
#include "gl_state.h"
#include "gl_anim.h"
#include "gl_render.h"
#include "render_private.h"
#include "public/render.h"
#include "../entity.h"
#include "../config.h"
#include "../lib/public/pf_malloc.h"
#include "../lib/public/khash.h"
#include "../lib/public/mem.h"
#include "../map/public/tile.h"
#include "../game/public/game.h"

#include <inttypes.h>
#include <assert.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_BATCH)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_BATCH)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_BATCH)


#define MESH_BUFF_SZ        (16*1024*1024)
#define TEX_ARR_SZ          (64)

#define MAX_TEX_ARRS        (4)
#define MAX_MESH_BUFFS      (16)

#define CMD_RING_SZ         (4 * 1024 * sizeof(struct GL_DAI_Cmd))
#define STAT_ATTR_RING_SZ   (32*1024*1024)
#define ANIM_ATTR_RING_SZ   (32*1024*1024)

#define MAX_BATCHES         (256)
#define MAX_INSTS           (65536)
#define ARR_SIZE(a)         (sizeof(a)/sizeof(a[0]))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))

#define CMD_RING_TUNIT      (GL_TEXTURE5)
#define ATTR_RING_TUNIT     (GL_TEXTURE6)

#define GL_PERF_CALL(name, ...)     \
    do{                             \
        GL_GPU_PERF_PUSH(name);     \
        __VA_ARGS__;                \
        GL_GPU_PERF_POP();          \
    }while(0)


struct tex_desc{
    struct gl_batch *batch;
    int              arr_idx;
    int              tex_idx;
};

struct tex_arr_desc{
    struct texture_arr arr;
    /* bitfield of free slots */
    uint32_t           free;
};

struct vbo_desc{
    void   *heap_meta;
    GLuint  VBO;
    GLuint  VAO;
};

struct inst_group_desc{
    void *render_private;
    int   start_idx;
    int   end_idx;
};

struct draw_call_desc{
    int vbo_idx; 
    int start_idx;
    int end_idx;
};

struct batch_draw_desc{
    struct gl_batch *batch;
    int start_idx;
    int end_idx;
};

KHASH_MAP_INIT_INT(tdesc, struct tex_desc)

struct GL_DAI_Cmd{
    GLuint count;
    GLuint instance_count;
    GLuint first_index;
    GLuint base_instance;
};

enum batch_type{
    BATCH_TYPE_ANIM,
    BATCH_TYPE_STAT,
};

struct gl_batch{
    enum batch_type     type;
    /* Ringbuffer for draw commands for this batch */
    struct gl_ring     *cmd_ring;
    /* Ringbuffer for for per-instance attributes associated
     * with the draw commands */
    struct gl_ring     *attr_ring;
    /* A mapping of onne of the mesh's texture IDs to its
     * position within the texture array list. */
    khash_t(tdesc)     *tid_desc_map;
    /* The number of texture arrays in this batch */
    size_t              ntexarrs;
    /* The number of buffers storing all the meshes for
     * this batch. Meshes are packed together in a buffer 
     * of MESH_BUFF_SZ, until it is full, in which case 
     * an extra buffer is allocated. */
    size_t              nvbos;
    /* The textues for all the meshes in this batch. All 
     * the textures are packed into a single texture array 
     * with a fixed number of entries. If the array fills 
     * up, the textures overflow into the next array. */
    struct tex_arr_desc textures[MAX_TEX_ARRS];
    /* The VBOs holding the combiend meshes for this batch. */
    struct vbo_desc     vbos[MAX_MESH_BUFFS];
    /* To allow further dynamic growing and shrinking of the batches,
     * an additional batch can be appended to this one, when there
     * is no more space for storing additional mesh or texture data.
     */
    struct gl_batch    *next;
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct gl_batch *s_anim_batch;
static struct gl_batch *s_stat_batch;
static GLuint           s_draw_id_vbo;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static size_t batch_vert_alignment(enum batch_type type)
{
    return type == BATCH_TYPE_STAT ? sizeof(struct vertex)
         : type == BATCH_TYPE_ANIM ? sizeof(struct anim_vert)
                                   : (assert(0), 0);
}

static int batch_first_free_idx(uint32_t mask)
{
    int ret = 0;
    while(mask) {
        if(mask & 0x1)    
            return ret;
        ret++, mask >>= 1;
    }
    return -1;
}

static void batch_init_vao(enum batch_type type, GLuint *out, GLuint src_vbo)
{
    GLuint VAO;
    glGenVertexArrays(1, &VAO);
    size_t stride = batch_vert_alignment(type);

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, src_vbo);

    /* Attribute 0 - position */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(0);

    /* Attribute 1 - texture coordinates */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, 
        (void*)offsetof(struct vertex, uv));
    glEnableVertexAttribArray(1);

    /* Attribute 2 - normal */
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, 
        (void*)offsetof(struct vertex, normal));
    glEnableVertexAttribArray(2);

    /* Attribute 3 - material index */
    glVertexAttribIPointer(3, 1, GL_INT, stride, 
        (void*)offsetof(struct vertex, material_idx));
    glEnableVertexAttribArray(3);

    if(type == BATCH_TYPE_STAT) {
    
        /* Attribute 4 - draw ID 
         * This is a per-instance attribute that is batchd from the draw ID buffer */
        glBindBuffer(GL_ARRAY_BUFFER, s_draw_id_vbo);

        glVertexAttribIPointer(4, 1, GL_INT, sizeof(GLint), 0);
        glEnableVertexAttribArray(4);
        glVertexAttribDivisor(4, 1); /* advance the ID once per instance */

    }else if(type == BATCH_TYPE_ANIM) {
    
        /* Attribute 4/5 - joint indices */
        glVertexAttribIPointer(4, 3, GL_UNSIGNED_BYTE, stride,
            (void*)offsetof(struct anim_vert, joint_indices));
        glEnableVertexAttribArray(4);  
        glVertexAttribIPointer(5, 3, GL_UNSIGNED_BYTE, stride,
            (void*)(offsetof(struct anim_vert, joint_indices) + 3*sizeof(GLubyte)));
        glEnableVertexAttribArray(5);

        /* Attribute 6/7 - joint weights */
        glVertexAttribPointer(6, 3, GL_FLOAT, GL_FALSE, stride,
            (void*)offsetof(struct anim_vert, weights));
        glEnableVertexAttribArray(6);  
        glVertexAttribPointer(7, 3, GL_FLOAT, GL_FALSE, stride,
            (void*)(offsetof(struct anim_vert, weights) + 3*sizeof(GLfloat)));
        glEnableVertexAttribArray(7);  

        /* Attribute 8 - draw ID 
         * This is a per-instance attribute that is batchd from the draw ID buffer */
        glBindBuffer(GL_ARRAY_BUFFER, s_draw_id_vbo);

        glVertexAttribIPointer(8, 1, GL_INT, sizeof(GLint), 0);
        glEnableVertexAttribArray(8);
        glVertexAttribDivisor(8, 1); /* advance the ID once per instance */
    }

    *out = VAO;
    glBindVertexArray(0);
}

static bool batch_alloc_texarray(struct gl_batch *batch)
{
    if(batch->ntexarrs == MAX_TEX_ARRS)
        return false;

    R_GL_Texture_ArrayAlloc(TEX_ARR_SZ, &batch->textures[batch->ntexarrs].arr, GL_TEXTURE0 + batch->ntexarrs);
    batch->textures[batch->ntexarrs].free = ~((uint32_t)0);
    batch->ntexarrs++;
    return true;
}

static bool batch_alloc_vbo(struct gl_batch *batch, size_t size)
{
    if(batch->nvbos == MAX_MESH_BUFFS)
        return false;

    size = MAX(size, MESH_BUFF_SZ);
    void *heap_meta = pf_metamalloc_init(size);
    if(!heap_meta)
        return false;

    GLuint VBO;
    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, size, NULL, GL_DYNAMIC_DRAW);

    GLuint VAO;
    batch_init_vao(batch->type, &VAO, VBO);

    batch->vbos[batch->nvbos] = (struct vbo_desc){heap_meta, VBO, VAO};
    batch->nvbos++;
    return true;
}

static struct gl_batch *batch_init(enum batch_type type)
{
    struct gl_batch *batch = PF_MALLOC(sizeof(struct gl_batch));
    if(!batch)
        goto fail_alloc;

    batch->type = type;
    batch->ntexarrs = 0;
    batch->nvbos = 0;
    batch->next = NULL;

    batch->cmd_ring = R_GL_RingbufferInit(CMD_RING_SZ, RING_UBYTE);
    if(!batch->cmd_ring)
        goto fail_cmd_ring;

    size_t attr_sz = (type == BATCH_TYPE_ANIM) ? ANIM_ATTR_RING_SZ : STAT_ATTR_RING_SZ;
    batch->attr_ring = R_GL_RingbufferInit(attr_sz, RING_FLOAT);
    if(!batch->attr_ring)
        goto fail_attr_ring;
        
    batch->tid_desc_map = kh_init(tdesc);
    if(!batch->tid_desc_map)
        goto fail_tid_desc_map;

    if(!batch_alloc_texarray(batch))
        goto fail_tex_array;

    if(!batch_alloc_vbo(batch, MESH_BUFF_SZ))
        goto fail_vbo;

    GL_ASSERT_OK();
    return batch;

fail_vbo:
    R_GL_Texture_ArrayFree(batch->textures[0].arr);
fail_tex_array:
    kh_destroy(tdesc, batch->tid_desc_map);
fail_tid_desc_map:
    R_GL_RingbufferDestroy(batch->attr_ring);
fail_attr_ring:
    R_GL_RingbufferDestroy(batch->cmd_ring);
fail_cmd_ring:
    PF_FREE(batch);
fail_alloc:
    return NULL;
}

static void batch_destroy(struct gl_batch *batch)
{
    if(batch->next) {
        batch_destroy(batch->next);
    }
    for(int i = 0; i < batch->ntexarrs; i++) {
        R_GL_Texture_ArrayFree(batch->textures[i].arr);
    }
    for(int i = 0; i < batch->nvbos; i++) {
        glDeleteVertexArrays(1, &batch->vbos[i].VAO);
        glDeleteBuffers(1, &batch->vbos[i].VBO);
        pf_metamalloc_destroy(batch->vbos[i].heap_meta);
    }
    kh_destroy(tdesc, batch->tid_desc_map);
    R_GL_RingbufferDestroy(batch->attr_ring);
    R_GL_RingbufferDestroy(batch->cmd_ring);
    PF_FREE(batch);
}

static bool batch_grow(struct gl_batch *batch)
{
    struct gl_batch *next = batch_init(batch->type);
    if(!next)
        return false;

    while(batch->next) {
        batch = batch->next;
    }
    batch->next = next;
    return true;
}

static bool batch_append_mesh(struct gl_batch *batch, const void *vbuff, size_t size,
                              size_t alignment, int *out_vbo_idx, int *out_offset)
{
    int vbo_idx = 0;
    int offset = -1;
    do{
        offset = pf_metamemalign(batch->vbos[vbo_idx].heap_meta, alignment, size);
        if(offset >= 0)
            break;
    }while(++vbo_idx < batch->nvbos);

    if(offset < 0) {
        if(!batch_alloc_vbo(batch, size))
            return false;
        vbo_idx = batch->nvbos - 1;
        offset = pf_metamemalign(batch->vbos[vbo_idx].heap_meta, alignment, size);
        if(offset < 0)
            return false;
    }
    assert(vbo_idx >= 0 && vbo_idx < batch->nvbos);
    assert(offset >= 0);

    glBindBuffer(GL_ARRAY_BUFFER, batch->vbos[vbo_idx].VBO);
    glBufferSubData(GL_ARRAY_BUFFER, offset, size, vbuff);

    *out_vbo_idx = vbo_idx;
    *out_offset = offset;

    GL_ASSERT_OK();
    return true;
}

static void batch_free_mesh(struct gl_batch *batch, int vbo_idx, int offset)
{
    pf_metafree(batch->vbos[vbo_idx].heap_meta, offset);
}

static bool batch_append_tex(struct gl_batch *batch, GLuint tid, struct texture_arr *src,
                             int src_idx, bool *out_new)
{
    *out_new = false;

    khiter_t k = kh_get(tdesc, batch->tid_desc_map, tid);
    if(k != kh_end(batch->tid_desc_map))
        return true; /* texture already in the batch */

    int arr_idx = 0;
    int slice_idx = -1;
    do{
        if(batch->textures[arr_idx].free == 0)
            continue;
        slice_idx = batch_first_free_idx(batch->textures[arr_idx].free);
        break;
    }while(++arr_idx < batch->ntexarrs);

    if(slice_idx == -1) {
        if(!batch_alloc_texarray(batch))
            return false;
        arr_idx = batch->ntexarrs - 1;
        slice_idx = batch_first_free_idx(batch->textures[arr_idx].free);
    }
    assert(arr_idx >= 0 && arr_idx < batch->ntexarrs);
    assert(slice_idx >= 0 && slice_idx < 32);

    assert(glIsTexture(batch->textures[arr_idx].arr.id));
    assert(glIsTexture(src->id));
    R_GL_Texture_ArrayCopyElem(&batch->textures[arr_idx].arr, slice_idx, src, src_idx,
        CONFIG_ARR_TEX_RES);

    int status;
    k = kh_put(tdesc, batch->tid_desc_map, tid, &status);
    if(status == -1)
        return false;

    kh_value(batch->tid_desc_map, k) = (struct tex_desc){batch, arr_idx, slice_idx};
    batch->textures[arr_idx].free &= ~(((uint32_t)0x1) << slice_idx);

    *out_new = true;
    GL_ASSERT_OK();
    return true;
}

static void batch_free_tex(struct gl_batch *batch, GLuint tid)
{
    khiter_t k = kh_get(tdesc, batch->tid_desc_map, tid);
    if(k != kh_end(batch->tid_desc_map)) {
        struct tex_desc td = kh_value(batch->tid_desc_map, k);
        batch->textures[td.arr_idx].free |= (0x1 << td.tex_idx);
        kh_del(tdesc, batch->tid_desc_map, k);
    }
}

static struct tex_desc batch_tdesc_for_tid(struct gl_batch *batch, GLuint tid)
{
    khiter_t k = kh_get(tdesc, batch->tid_desc_map, tid);
    if(k != kh_end(batch->tid_desc_map)) {
        return kh_value(batch->tid_desc_map, k);
    }
    assert(0);
    return (struct tex_desc){0};
}

/* Try to place a mesh and all of its textures into a single batch node. On
 * success the mesh's descriptor is filled in and 'true' is returned; on failure
 * any partial allocation is rolled back. */
static bool batch_try_place(struct gl_batch *batch, struct render_private *priv,
                            const void *vbuff, size_t size, struct texture_arr *src_tex)
{
    size_t alignment = batch_vert_alignment(batch->type);

    int vbo_idx, offset;
    if(!batch_append_mesh(batch, vbuff, size, alignment, &vbo_idx, &offset))
        return false;

    GLuint new_tids[MAX_MATERIALS];
    int nnew = 0;
    int i = 0;
    for(; i < priv->num_materials; i++) {
        bool isnew;
        if(!batch_append_tex(batch, priv->materials[i].texture.id, src_tex, i, &isnew))
            goto fail_tex;
        if(isnew)
            new_tids[nnew++] = priv->materials[i].texture.id;
    }

    /* Record where each material's texture landed so the non-batched draw path
     * can sample the correct array/slot. */
    for(int k = 0; k < priv->num_materials; k++) {
        struct tex_desc td = batch_tdesc_for_tid(batch, priv->materials[k].texture.id);
        priv->materials[k].tex_arr_idx = td.arr_idx;
        priv->materials[k].tex_slot = td.tex_idx;
    }

    priv->mesh.batch = batch;
    priv->mesh.vbo_idx = vbo_idx;
    priv->mesh.offset = offset;
    GL_ASSERT_OK();
    return true;

fail_tex:
    /* Only roll back textures that this call actually added; shared textures
     * placed by earlier meshes must remain. */
    for(int j = 0; j < nnew; j++) {
        batch_free_tex(batch, new_tids[j]);
    }
    batch_free_mesh(batch, vbo_idx, offset);
    return false;
}

static size_t batch_sort_by_batch_stat(struct ent_stat_rstate *ents, size_t nents,
                                       struct batch_draw_desc *out, size_t maxout)
{
    if(nents == 0)
        return 0;

    int i = 1;
    while(i < nents) {
        int j = i;
        while(j > 0) {
            struct gl_batch *b1 = ((struct render_private*)ents[j - 1].render_private)->mesh.batch;
            struct gl_batch *b2 = ((struct render_private*)ents[j].render_private)->mesh.batch;
            if((uintptr_t)b1 <= (uintptr_t)b2)
                break;
            struct ent_stat_rstate tmp = ents[j - 1];
            ents[j - 1] = ents[j];
            ents[j] = tmp;
            j--;
        }
        i++;
    }

    size_t ret = 0;
    struct batch_draw_desc curr = (struct batch_draw_desc){
        .batch = ((struct render_private*)ents[0].render_private)->mesh.batch,
        .start_idx = 0
    };
    for(i = 0; i < nents; i++) {
        struct gl_batch *b = ((struct render_private*)ents[i].render_private)->mesh.batch;
        if(b != curr.batch) {
            curr.end_idx = i - 1;
            out[ret++] = curr;
            curr = (struct batch_draw_desc){ .batch = b, .start_idx = i };
        }
        if(ret == maxout)
            break;
    }

    curr.end_idx = i - 1;
    out[ret++] = curr;
    return ret;
}

static size_t batch_sort_by_batch_anim(struct ent_anim_rstate *ents, size_t nents,
                                       struct batch_draw_desc *out, size_t maxout)
{
    if(nents == 0)
        return 0;

    int i = 1;
    while(i < nents) {
        int j = i;
        while(j > 0) {
            struct gl_batch *b1 = ((struct render_private*)ents[j - 1].render_private)->mesh.batch;
            struct gl_batch *b2 = ((struct render_private*)ents[j].render_private)->mesh.batch;
            if((uintptr_t)b1 <= (uintptr_t)b2)
                break;
            struct ent_anim_rstate tmp = ents[j - 1];
            ents[j - 1] = ents[j];
            ents[j] = tmp;
            j--;
        }
        i++;
    }

    size_t ret = 0;
    struct batch_draw_desc curr = (struct batch_draw_desc){
        .batch = ((struct render_private*)ents[0].render_private)->mesh.batch,
        .start_idx = 0
    };
    for(i = 0; i < nents; i++) {
        struct gl_batch *b = ((struct render_private*)ents[i].render_private)->mesh.batch;
        if(b != curr.batch) {
            curr.end_idx = i - 1;
            out[ret++] = curr;
            curr = (struct batch_draw_desc){ .batch = b, .start_idx = i };
        }
        if(ret == maxout)
            break;
    }

    curr.end_idx = i - 1;
    out[ret++] = curr;
    return ret;
}

static size_t batch_sort_by_transparency(vec_rstat_t *ents, size_t nents)
{
    if(vec_size(ents) == 0)
        return 0;

    int i = 0;
    while(i < nents) {
        int j = i;
        while(j > 0 && (vec_AT(ents, j - 1).translucent && !vec_AT(ents, j).translucent)) {

            struct ent_stat_rstate tmp = vec_AT(ents, j - 1);
            vec_AT(ents, j - 1) = vec_AT(ents, j);
            vec_AT(ents, j) = tmp;
            j--;
        }
        i++;
    }

    size_t ret = 0;
    for(int i = 0; i < nents; i++) {
        struct ent_stat_rstate *curr = &vec_AT(ents, i);
        if(curr->translucent)
            ret++;
    }
    return ret;
}

static size_t batch_anim_sort_by_transparency(vec_ranim_t *ents, size_t nents)
{
    if(vec_size(ents) == 0)
        return 0;

    int i = 0;
    while(i < nents) {
        int j = i;
        while(j > 0 && (vec_AT(ents, j - 1).translucent && !vec_AT(ents, j).translucent)) {

            struct ent_anim_rstate tmp = vec_AT(ents, j - 1);
            vec_AT(ents, j - 1) = vec_AT(ents, j);
            vec_AT(ents, j) = tmp;
            j--;
        }
        i++;
    }

    size_t ret = 0;
    for(int i = 0; i < nents; i++) {
        struct ent_anim_rstate *curr = &vec_AT(ents, i);
        if(curr->translucent)
            ret++;
    }
    return ret;
}

static size_t batch_sort_by_inst_stat(struct ent_stat_rstate *ents, size_t nents, 
                                      struct inst_group_desc *out, size_t maxout)
{
    int i = 1;
    while(i < nents) {
        int j = i;
        while(j > 0 && ((uintptr_t)ents[j - 1].render_private) > ((uintptr_t)ents[j].render_private)) {

            struct ent_stat_rstate tmp = ents[j - 1];
            ents[j - 1] = ents[j];
            ents[j] = tmp;
            j--;
        }
        i++;
    }

    size_t ret = 0;

    struct inst_group_desc curr = (struct inst_group_desc){
        .render_private = ents[0].render_private,
        .start_idx = 0,
    };
    for(int i = 1; i < nents; i++) {
    
        if(((uintptr_t)ents[i - 1].render_private) != ((uintptr_t)ents[i].render_private)) {

            curr.end_idx = i - 1;
            out[ret++] = curr;
            curr = (struct inst_group_desc){
                .render_private = ents[i].render_private,
                .start_idx = i,
            };
        }
        if(ret == maxout)
            break;
    }

    curr.end_idx = i - 1;
    out[ret++] = curr;

    return ret;
}

static size_t batch_sort_by_inst_anim(struct ent_anim_rstate *ents, size_t nents, 
                                      struct inst_group_desc *out, size_t maxout)
{
    int i = 1;
    while(i < nents) {
        int j = i;
        while(j > 0 && ((uintptr_t)ents[j - 1].render_private) > ((uintptr_t)ents[j].render_private)) {

            struct ent_anim_rstate tmp = ents[j - 1];
            ents[j - 1] = ents[j];
            ents[j] = tmp;
            j--;
        }
        i++;
    }

    size_t ret = 0;

    struct inst_group_desc curr = (struct inst_group_desc){
        .render_private = ents[0].render_private,
        .start_idx = 0,
    };
    for(int i = 1; i < nents; i++) {
    
        if(((uintptr_t)ents[i - 1].render_private) != ((uintptr_t)ents[i].render_private)) {

            curr.end_idx = i - 1;
            out[ret++] = curr;
            curr = (struct inst_group_desc){
                .render_private = ents[i].render_private,
                .start_idx = i,
            };
        }
        if(ret == maxout)
            break;
    }

    curr.end_idx = i - 1;
    out[ret++] = curr;

    return ret;
}

static size_t batch_sort_by_vbo(struct inst_group_desc *descs, size_t ndescs,
                                struct draw_call_desc *out, size_t maxout)
{
    int i = 1;
    while(i < ndescs) {
        int j = i;
        while(j > 0) {
            int v1 = ((struct render_private*)descs[j - 1].render_private)->mesh.vbo_idx;
            int v2 = ((struct render_private*)descs[j].render_private)->mesh.vbo_idx;
            if(v1 <= v2)
                break;
            struct inst_group_desc tmp = descs[j - 1];
            descs[j - 1] = descs[j];
            descs[j] = tmp;
            j--;
        }
        i++;
    }

    size_t ret = 0;
    struct draw_call_desc curr = (struct draw_call_desc){
        .vbo_idx = ((struct render_private*)descs[0].render_private)->mesh.vbo_idx,
        .start_idx = 0,
    };
    for(int i = 1; i < ndescs; i++) {
        int v = ((struct render_private*)descs[i].render_private)->mesh.vbo_idx;
        if(v != curr.vbo_idx) {
            curr.end_idx = i - 1;
            out[ret++] = curr;
            curr = (struct draw_call_desc){ .vbo_idx = v, .start_idx = i };
        }
        if(ret == maxout)
            break;
    }

    curr.end_idx = i - 1;
    out[ret++] = curr;

    return ret;
}

static void batch_ring_append_mats(struct gl_batch *batch, struct render_private *priv)
{
    /* Push a lookup table mapping the per-vertex material index to 
     * a texture slot inside the list of texture arrays */
    for(int k = 0; k < MAX_MATERIALS; k++) {
        if(k < priv->num_materials) {
            struct tex_desc td = batch_tdesc_for_tid(batch, priv->materials[k].texture.id);
            vec2_t tex_arr_coord = (vec2_t){td.arr_idx, td.tex_idx};
            R_GL_RingbufferAppendLast(batch->attr_ring, &tex_arr_coord, sizeof(vec2_t));
        }else{
            vec2_t tex_arr_coord = (vec2_t){0.0f, 0.0f};
            R_GL_RingbufferAppendLast(batch->attr_ring, &tex_arr_coord, sizeof(vec2_t));
        }
    }

    /* Push the material attributes */
    for(int k = 0; k < MAX_MATERIALS; k++) {
        if(k < priv->num_materials) {
            struct material *mat = &priv->materials[k];
            float zero = 0.0f;
            R_GL_RingbufferAppendLast(batch->attr_ring, &mat->ambient_intensity, sizeof(float));
            R_GL_RingbufferAppendLast(batch->attr_ring, &zero, sizeof(float));
            R_GL_RingbufferAppendLast(batch->attr_ring, &mat->diffuse_clr, sizeof(vec3_t));
            R_GL_RingbufferAppendLast(batch->attr_ring, &mat->specular_clr, sizeof(vec3_t));
        }else{
            float zero[8] = {0};
            R_GL_RingbufferAppendLast(batch->attr_ring, &zero, sizeof(zero));
        }
    }
}

static void batch_push_stat_attrs(struct gl_batch *batch, const struct ent_stat_rstate *ents,
                                  struct draw_call_desc dcall, struct inst_group_desc *descs,
                                  size_t offset)
{
    /* The per-instance static attributes have the follwing layout in the buffer:
     *
     *  +--------------------------------------------------+ <-- base
     *  | mat4x4_t (16 floats)                             | (model matrix)
     *  +--------------------------------------------------+
     *  | vec2_t[16] (32 floats)                           | (material:texture mapping)
     *  +--------------------------------------------------+
     *  | {float, float, vec3_t, vec3_t}[16] (128 floats)  | (material properties)
     *  +--------------------------------------------------+
     *
     * In total, 176 floats (704 bytes) are pushed per instance.
     */
    size_t ninsts = 0;
    for(int i = dcall.start_idx; i <= dcall.end_idx; i++) {

        const struct inst_group_desc *curr = descs + i;
        struct render_private *priv = curr->render_private;

        for(int j = curr->start_idx; j <= curr->end_idx; j++) {
        
            if(i == dcall.start_idx && j == curr->start_idx) {
                R_GL_RingbufferPush(batch->attr_ring, &ents[offset + j].model, sizeof(mat4x4_t));
            }else{
                R_GL_RingbufferAppendLast(batch->attr_ring, &ents[offset + j].model, sizeof(mat4x4_t));
            }
            batch_ring_append_mats(batch, priv);
        }
        ninsts += curr->end_idx - curr->start_idx + 1;
    }

    size_t begin, end;
    R_GL_RingbufferGetLastRange(batch->attr_ring, &begin, &end);
    assert(end > begin ? (end - begin == 704 * ninsts)
                       : ((STAT_ATTR_RING_SZ - begin) + end == 704 * ninsts));

    R_GL_StateSet(GL_U_ATTR_STRIDE, (struct uval){ 
        .type = UTYPE_INT, 
        .val.as_int = 176 
    });
    R_GL_StateInstall(GL_U_ATTR_STRIDE, R_GL_Shader_GetCurrActive());
}

static void batch_push_stat_attrs_depth(struct gl_batch *batch, const struct ent_stat_rstate *ents,
                                        struct draw_call_desc dcall, struct inst_group_desc *descs,
                                        size_t offset)
{
    /* The per-instance static attributes have the following layout in the buffer:
     *
     *  +--------------------------------------------------+ <-- base
     *  | mat4x4_t (16 floats)                             | (model matrix)
     *  +--------------------------------------------------+
     *
     * In total, 16 floats (64 bytes) are pushed per instance.
     */
    size_t ninsts = 0;
    for(int i = dcall.start_idx; i <= dcall.end_idx; i++) {

        const struct inst_group_desc *curr = descs + i;

        for(int j = curr->start_idx; j <= curr->end_idx; j++) {
     
            if(i == dcall.start_idx && j == curr->start_idx) {
                R_GL_RingbufferPush(batch->attr_ring, &ents[offset + j].model, sizeof(mat4x4_t));
            }else{
                R_GL_RingbufferAppendLast(batch->attr_ring, &ents[offset + j].model, sizeof(mat4x4_t));
            }
        }

        ninsts += curr->end_idx - curr->start_idx + 1;
    }
    size_t begin, end;
    R_GL_RingbufferGetLastRange(batch->attr_ring, &begin, &end);
    assert(end > begin ? (end - begin == 64 * ninsts)
                       : ((STAT_ATTR_RING_SZ - begin) + end == 64 * ninsts));

    R_GL_StateSet(GL_U_ATTR_STRIDE, (struct uval){ 
        .type = UTYPE_INT, 
        .val.as_int = 16
    });
    R_GL_StateInstall(GL_U_ATTR_STRIDE, R_GL_Shader_GetCurrActive());
}

static void batch_push_anim_attrs(struct gl_batch *batch, const struct ent_anim_rstate *ents,
                                  struct draw_call_desc dcall, struct inst_group_desc *descs,
                                  size_t offset)
{
    /* The per-instance static attributes have the follwing layout in the buffer:
     *
     *  +--------------------------------------------------+ <-- base
     *  | mat4x4_t (16 floats)                             | (model matrix)
     *  +--------------------------------------------------+
     *  | vec2_t[16] (32 floats)                           | (material:texture mapping)
     *  +--------------------------------------------------+
     *  | {float, float, vec3_t, vec3_t}[16] (128 floats)  | (material properties)
     *  +--------------------------------------------------+
     *  | mat4x4_t (16 floats)                             | (normal matrix)
     *  +--------------------------------------------------+
     *  | int (1 float)                                    | (inv. bind pose offset)
     *  +--------------------------------------------------+
     *  | int (1 float)                                    | (curr. pose offset)
     *  +--------------------------------------------------+
     *
     * In total, 194 floats (776 bytes) are pushed per instance.
     */
    size_t ninsts = 0;
    for(int i = dcall.start_idx; i <= dcall.end_idx; i++) {

        const struct inst_group_desc *curr = descs + i;
        struct render_private *priv = curr->render_private;

        for(int j = curr->start_idx; j <= curr->end_idx; j++) {

            if(i == dcall.start_idx && j == curr->start_idx) {
                R_GL_RingbufferPush(batch->attr_ring, &ents[offset + j].model, sizeof(mat4x4_t));
            }else{
                R_GL_RingbufferAppendLast(batch->attr_ring, &ents[offset + j].model, sizeof(mat4x4_t));
            }
            batch_ring_append_mats(batch, priv);

            mat4x4_t model, normal;
            PFM_Mat4x4_Inverse((mat4x4_t*)&ents[offset + j].model, &model);
            PFM_Mat4x4_Transpose(&model, &normal);

            GLfloat inv_idx = ents[offset + j].desc.inv_bind_pose_offset;
            GLfloat curr_idx = ents[offset + j].desc.curr_pose_offset;

            R_GL_RingbufferAppendLast(batch->attr_ring, &ents[offset + j].model, sizeof(mat4x4_t));
            R_GL_RingbufferAppendLast(batch->attr_ring, &inv_idx, sizeof(GLfloat));
            R_GL_RingbufferAppendLast(batch->attr_ring, &curr_idx, sizeof(GLfloat));
        }
        ninsts += curr->end_idx - curr->start_idx + 1;
    }

    R_GL_StateSet(GL_U_ATTR_STRIDE, (struct uval){ 
        .type = UTYPE_INT, 
        .val.as_int = 194
    });
    R_GL_StateInstall(GL_U_ATTR_STRIDE, R_GL_Shader_GetCurrActive());

    R_GL_AnimBindPoseBuff();
    R_GL_StateSet(GL_U_POSEBUFF, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = POSE_BUFF_TUNINT - GL_TEXTURE0
    });
    R_GL_StateInstall(GL_U_POSEBUFF, R_GL_Shader_GetCurrActive());
}

static void batch_push_cmds(struct gl_batch *batch, struct draw_call_desc dcall,
                            struct inst_group_desc *descs)
{
    size_t alignment = batch_vert_alignment(batch->type);
    size_t inst_idx = 0;
    for(int i = dcall.start_idx; i <= dcall.end_idx; i++) {

        struct render_private *priv = descs[i].render_private;
        assert(priv->mesh.offset % alignment == 0);

        struct GL_DAI_Cmd cmd = (struct GL_DAI_Cmd){
            .count = priv->mesh.num_verts,
            .instance_count = descs[i].end_idx - descs[i].start_idx + 1,
            .first_index = priv->mesh.offset / alignment,
            .base_instance = inst_idx,
        };

        if(i == dcall.start_idx) {
            R_GL_RingbufferPush(batch->cmd_ring, &cmd, sizeof(struct GL_DAI_Cmd));
        }else{
            R_GL_RingbufferAppendLast(batch->cmd_ring, &cmd, sizeof(struct GL_DAI_Cmd));
        }
        inst_idx += cmd.instance_count;
    }

    size_t ncmds = dcall.end_idx - dcall.start_idx + 1;
    size_t begin, end;
    R_GL_RingbufferGetLastRange(batch->cmd_ring, &begin, &end);
    assert(end > begin ? (end - begin == sizeof(struct GL_DAI_Cmd) * ncmds)
                       : ((CMD_RING_SZ - begin) + end  == sizeof(struct GL_DAI_Cmd) * ncmds));
}

static void batch_multidraw_legacy(struct gl_batch *batch, struct draw_call_desc dcall,
                                   struct inst_group_desc *descs)
{
    size_t alignment = batch_vert_alignment(batch->type);
    size_t inst_idx = 0;
    for(int i = dcall.start_idx; i <= dcall.end_idx; i++) {

        const struct inst_group_desc *curr = descs + i;
        struct render_private *priv = curr->render_private;

        R_GL_StateSet(GL_U_ATTR_OFFSET, (struct uval){ 
            .type = UTYPE_INT, 
            .val.as_int = inst_idx
        });
        R_GL_StateInstall(GL_U_ATTR_OFFSET, R_GL_Shader_GetCurrActive());

        GLint first = priv->mesh.offset / alignment;
        GLint count = priv->mesh.num_verts;
        size_t instcount = curr->end_idx - curr->start_idx + 1;

        glDrawArraysInstanced(GL_TRIANGLES, first, count, instcount);
        inst_idx += instcount;
    }
}

static void batch_multidraw(struct gl_batch *batch, struct draw_call_desc dcall,
                            struct inst_group_desc *descs)
{
    R_GL_StateSet(GL_U_ATTR_OFFSET, (struct uval){ 
        .type = UTYPE_INT, 
        .val.as_int = 0
    });
    R_GL_StateInstall(GL_U_ATTR_OFFSET, R_GL_Shader_GetCurrActive());

    batch_push_cmds(batch, dcall, descs);

    GLuint cmd_vbo = R_GL_RingbufferGetVBO(batch->cmd_ring);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_vbo);

    size_t cmd_begin, cmd_end;
    R_GL_RingbufferGetLastRange(batch->cmd_ring, &cmd_begin, &cmd_end);

    if(cmd_end < cmd_begin) {

        assert((CMD_RING_SZ - cmd_begin) % sizeof(struct GL_DAI_Cmd) == 0);
        size_t ncmds_end = (CMD_RING_SZ - cmd_begin) / sizeof(struct GL_DAI_Cmd);
        GL_PERF_CALL("multidraw", glMultiDrawArraysIndirect(GL_TRIANGLES, (void*)cmd_begin, ncmds_end, 0));

        assert(cmd_end % sizeof(struct GL_DAI_Cmd) == 0);
        size_t ncmds_begin = cmd_end / sizeof(struct GL_DAI_Cmd);
        GL_PERF_CALL("multidraw", glMultiDrawArraysIndirect(GL_TRIANGLES, (void*)0, ncmds_begin, 0));
    }else{
        size_t ncmds = dcall.end_idx - dcall.start_idx + 1;
        GL_PERF_CALL("multidraw", glMultiDrawArraysIndirect(GL_TRIANGLES, (void*)cmd_begin, ncmds, 0));
    }

    R_GL_RingbufferSyncLast(batch->cmd_ring);
    GL_ASSERT_OK();
}

static void batch_do_drawcall_stat(struct gl_batch *batch, const struct ent_stat_rstate *ents,
                                   struct draw_call_desc dcall, struct inst_group_desc *descs,
                                   enum render_pass pass, size_t offset)
{
    switch(pass) {
    case RENDER_PASS_DEPTH:
        batch_push_stat_attrs_depth(batch, ents, dcall, descs, offset);
        break;
    case RENDER_PASS_REGULAR:
        batch_push_stat_attrs(batch, ents, dcall, descs, offset);
        break;
    default: assert(0);
    }

    R_GL_RingbufferBindLast(batch->attr_ring, ATTR_RING_TUNIT, R_GL_Shader_GetCurrActive(), "attrbuff");

    GLuint VAO = batch->vbos[dcall.vbo_idx].VAO;
    glBindVertexArray(VAO);

    if(!GL_ARB_multi_draw_indirect) {
        batch_multidraw_legacy(batch, dcall, descs);
    }else{
        batch_multidraw(batch, dcall, descs);
    }

    glBindVertexArray(0);
    R_GL_RingbufferSyncLast(batch->attr_ring);
}

static void batch_do_drawcall_anim(struct gl_batch *batch, const struct ent_anim_rstate *ents,
                                   struct draw_call_desc dcall, struct inst_group_desc *descs,
                                   size_t offset)
{
    batch_push_anim_attrs(batch, ents, dcall, descs, offset);
    R_GL_RingbufferBindLast(batch->attr_ring, ATTR_RING_TUNIT, R_GL_Shader_GetCurrActive(), "attrbuff");

    GLuint VAO = batch->vbos[dcall.vbo_idx].VAO;
    glBindVertexArray(VAO);

    if(!GL_ARB_multi_draw_indirect) {
        batch_multidraw_legacy(batch, dcall, descs);
    }else{
        batch_multidraw(batch, dcall, descs);
    }

    glBindVertexArray(0);
    R_GL_RingbufferSyncLast(batch->attr_ring);
    GL_ASSERT_OK();
}

static void batch_render_stat(struct ent_stat_rstate *ents, size_t nents, enum render_pass pass)
{
    GL_PERF_ENTER();

    struct batch_draw_desc bdescs[MAX_BATCHES];
    size_t nbatches = batch_sort_by_batch_stat(ents, nents, bdescs, ARR_SIZE(bdescs));

    for(int bi = 0; bi < nbatches; bi++) {

        size_t offset = bdescs[bi].start_idx;
        size_t len = bdescs[bi].end_idx - bdescs[bi].start_idx + 1;
        struct gl_batch *batch = bdescs[bi].batch;

        STALLOC(struct inst_group_desc, descs, len);
        size_t ninsts = batch_sort_by_inst_stat(ents + offset, len, descs, len);

        STALLOC(struct draw_call_desc, dcalls, len);
        size_t ndcalls = batch_sort_by_vbo(descs, ninsts, dcalls, len);

        for(int i = 0; i < batch->ntexarrs; i++) {
            assert(batch->textures[i].arr.tunit == GL_TEXTURE0 + i);
            R_GL_Texture_BindArray(&batch->textures[i].arr, R_GL_Shader_GetCurrActive());
        }

        for(int i = 0; i < ndcalls; i++) {
            batch_do_drawcall_stat(batch, ents, dcalls[i], descs, pass, offset);
        }

        STFREE(dcalls);
        STFREE(descs);
    }
    GL_PERF_RETURN_VOID();
}

static void batch_render_anim(struct ent_anim_rstate *ents, size_t nents, enum render_pass pass)
{
    GL_PERF_ENTER();

    struct batch_draw_desc bdescs[MAX_BATCHES];
    size_t nbatches = batch_sort_by_batch_anim(ents, nents, bdescs, ARR_SIZE(bdescs));

    for(int bi = 0; bi < nbatches; bi++) {

        size_t offset = bdescs[bi].start_idx;
        size_t len = bdescs[bi].end_idx - bdescs[bi].start_idx + 1;
        struct gl_batch *batch = bdescs[bi].batch;

        STALLOC(struct inst_group_desc, descs, len);
        size_t ninsts = batch_sort_by_inst_anim(ents + offset, len, descs, len);

        STALLOC(struct draw_call_desc, dcalls, len);
        size_t ndcalls = batch_sort_by_vbo(descs, ninsts, dcalls, len);

        for(int i = 0; i < batch->ntexarrs; i++) {
            R_GL_Texture_BindArray(&batch->textures[i].arr, R_GL_Shader_GetCurrActive());
        }

        for(int i = 0; i < ndcalls; i++) {
            batch_do_drawcall_anim(batch, ents, dcalls[i], descs, offset);
        }

        STFREE(dcalls);
        STFREE(descs);
    }
    GL_PERF_RETURN_VOID();
}

static void batch_render_anim_all(vec_ranim_t *ents, bool shadows, enum render_pass pass)
{
    size_t nanim = vec_size(ents);
    if(nanim == 0)
        return;

    switch(pass) {
    case RENDER_PASS_DEPTH:
        R_GL_Shader_Install("batched.mesh.animated.depth");
        break;
    case RENDER_PASS_REGULAR:
        R_GL_Shader_Install("batched.mesh.animated.textured-phong-shadowed");
        break;
    default: assert(0);
    }

    size_t ntranslucent = batch_anim_sort_by_transparency(ents, nanim);
    size_t nopaque = nanim - ntranslucent;

    if(nopaque > 0) {
        batch_render_anim(&vec_AT(ents, 0), nopaque, pass);
    }
    if(ntranslucent > 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR);
        batch_render_anim(&vec_AT(ents, nopaque), ntranslucent, pass);
        glDisable(GL_BLEND);
    }
    GL_ASSERT_OK();
}

static void batch_render_stat_all(vec_rstat_t *ents, bool shadows, enum render_pass pass)
{
    size_t nents = vec_size(ents);
    if(nents == 0)
        return;

    switch(pass) {
    case RENDER_PASS_DEPTH:
        R_GL_Shader_Install("batched.mesh.static.depth");
        break;
    case RENDER_PASS_REGULAR:
        R_GL_Shader_Install("batched.mesh.static.textured-phong-shadowed");
        break;
    default: assert(0);
    }

    /* Partition the whole static set into opaque|translucent and draw each
     * group separately - translucency requires pipeline state changes. */
    size_t ntranslucent = batch_sort_by_transparency(ents, nents);
    size_t nopaque = nents - ntranslucent;

    if(nopaque > 0) {
        batch_render_stat(&vec_AT(ents, 0), nopaque, pass);
    }
    if(ntranslucent > 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR);
        batch_render_stat(&vec_AT(ents, nopaque), ntranslucent, pass);
        glDisable(GL_BLEND);
    }
    GL_ASSERT_OK();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_Batch_Init(void)
{
    GLint draw_id_buff[MAX_INSTS];
    for(int i = 0; i < MAX_INSTS; i++)
        draw_id_buff[i] = i;

    glGenBuffers(1, &s_draw_id_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_draw_id_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(draw_id_buff), draw_id_buff, GL_STATIC_DRAW);

    /* Allocated unconditionally up front: every entity mesh's vertex and texture
     * data lives here for its lifetime, regardless of the 'use_batch_rendering'
     * setting. */
    s_stat_batch = batch_init(BATCH_TYPE_STAT);
    if(!s_stat_batch)
        goto fail_stat_batch;

    s_anim_batch = batch_init(BATCH_TYPE_ANIM);
    if(!s_anim_batch)
        goto fail_anim_batch;

    GL_ASSERT_OK();
    return true;

fail_anim_batch:
    batch_destroy(s_stat_batch);
    s_stat_batch = NULL;
fail_stat_batch:
    glDeleteBuffers(1, &s_draw_id_vbo);
    return false;
}

void R_GL_Batch_Shutdown(void)
{
    batch_destroy(s_anim_batch);
    batch_destroy(s_stat_batch);
    s_anim_batch = NULL;
    s_stat_batch = NULL;

    glDeleteBuffers(1, &s_draw_id_vbo);
}

bool R_GL_Batch_AppendMesh(struct render_private *priv, const void *vbuff)
{
    GL_PERF_ENTER();

    enum batch_type type;
    if(priv->vertex_stride == sizeof(struct vertex)) {
        type = BATCH_TYPE_STAT;
    }else if(priv->vertex_stride == sizeof(struct anim_vert)) {
        type = BATCH_TYPE_ANIM;
    }else{
        assert(0);
        GL_PERF_RETURN(false);
    }

    struct gl_batch *batch = (type == BATCH_TYPE_STAT) ? s_stat_batch : s_anim_batch;
    size_t size = priv->mesh.num_verts * priv->vertex_stride;

    /* Gather the materials into a transient array so the not-yet-resident
     * slices can be copied into the shared storage, then free it. */
    struct texture_arr src_tex = {0};
    bool have_tex = (priv->num_materials > 0);
    if(have_tex) {
        R_GL_Texture_ArrayMake(priv->materials, priv->num_materials, &src_tex, GL_TEXTURE0);
    }

    bool ok = false;
    for(struct gl_batch *node = batch; node != NULL; node = node->next) {
        if(batch_try_place(node, priv, vbuff, size, have_tex ? &src_tex : NULL)) {
            ok = true;
            break;
        }
        if(node->next == NULL && !batch_grow(node))
            break;
    }

    if(have_tex) {
        R_GL_Texture_ArrayFree(src_tex);
    }

    priv->mesh.VBO = 0;
    priv->mesh.VAO = 0;
    if(!ok) {
        priv->mesh.batch = NULL;
    }

    GL_ASSERT_OK();
    GL_PERF_RETURN(ok);
}

const void *R_GL_Batch_MeshVertsMap(const struct render_private *priv)
{
    struct gl_batch *batch = priv->mesh.batch;
    assert(batch);

    glBindBuffer(GL_ARRAY_BUFFER, batch->vbos[priv->mesh.vbo_idx].VBO);
    char *base = glMapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY);
    if(!base)
        return NULL;
    return base + priv->mesh.offset;
}

void R_GL_Batch_MeshVertsUnmap(const struct render_private *priv)
{
    struct gl_batch *batch = priv->mesh.batch;
    assert(batch);

    glBindBuffer(GL_ARRAY_BUFFER, batch->vbos[priv->mesh.vbo_idx].VBO);
    glUnmapBuffer(GL_ARRAY_BUFFER);
}

GLint R_GL_Batch_MeshBindVAO(const struct render_private *priv)
{
    struct gl_batch *batch = priv->mesh.batch;
    assert(batch);

    glBindVertexArray(batch->vbos[priv->mesh.vbo_idx].VAO);
    return priv->mesh.offset / batch_vert_alignment(batch->type);
}

void R_GL_Batch_MeshGetStorage(const struct render_private *priv, GLuint *out_vbo, size_t *out_offset)
{
    if(priv->mesh.type == MESH_TYPE_BATCHED_INDIRECT) {
        *out_vbo = priv->mesh.batch->vbos[priv->mesh.vbo_idx].VBO;
        *out_offset = priv->mesh.offset;
    }else{
        *out_vbo = priv->mesh.VBO;
        *out_offset = 0;
    }
}

void R_GL_Batch_MeshBindTextures(const struct render_private *priv, GLuint shader_prog)
{
    struct gl_batch *batch = priv->mesh.batch;
    assert(batch);
    for(int i = 0; i < batch->ntexarrs; i++) {
        R_GL_Texture_BindArray(&batch->textures[i].arr, shader_prog);
    }
}

void R_GL_Batch_Draw(struct render_input *in)
{
    GL_PERF_ENTER();
    GL_PERF_PUSH_GROUP(0, "batch::Draw");

    R_GL_ShadowMapBind();
    batch_render_anim_all(&in->cam_vis_anim, true, RENDER_PASS_REGULAR);
    batch_render_stat_all(&in->cam_vis_stat, true, RENDER_PASS_REGULAR);

    GL_PERF_POP_GROUP();
    GL_PERF_RETURN_VOID();
}

void R_GL_Batch_RenderDepthMap(struct render_input *in)
{
    GL_PERF_ENTER();
    GL_PERF_PUSH_GROUP(0, "batch::RenderDepthMap");

    batch_render_anim_all(&in->light_vis_anim, true, RENDER_PASS_DEPTH);
    batch_render_stat_all(&in->light_vis_stat, true, RENDER_PASS_DEPTH);

    GL_PERF_POP_GROUP();
    GL_PERF_RETURN_VOID();
}
