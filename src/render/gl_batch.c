/* 
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2020 Eduard Permyakov 
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

#include "gl_batch.h"
#include "gl_ringbuffer.h"
#include "gl_texture.h"
#include "gl_mesh.h"
#include "gl_assert.h"
#include "gl_material.h"
#include "gl_shader.h"
#include "gl_perf.h"
#include "gl_vertex.h"
#include "render_private.h"
#include "public/render.h"
#include "../entity.h"
#include "../lib/public/pf_malloc.h"
#include "../lib/public/khash.h"
#include "../map/public/tile.h"
#include "../game/public/game.h"

#include <inttypes.h>
#include <assert.h>


#define MESH_BUFF_SZ    (4*1024*1024)
#define TEX_ARR_SZ      (64)

#define MAX_TEX_ARRS    (4)
#define MAX_MESH_BUFFS  (16)

#define CMD_RING_SZ     (4 * 1024 * sizeof(struct GL_DAI_Cmd))
#define ATTR_RING_SZ    (2*1024*1024)

#define MAX_BATCHES     (256)
#define MAX_INSTS       (16384)
#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))

#define CMD_RING_TUNIT  (GL_TEXTURE5)
#define ATTR_RING_TUNIT (GL_TEXTURE6)

struct mesh_desc{
    int    vbo_idx;
    size_t offset;
};

struct tex_desc{
    int arr_idx;
    int tex_idx;
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

struct chunk_batch_desc{
    int chunk_r, chunk_c; 
    int start_idx;
    int end_idx;
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

KHASH_MAP_INIT_INT(mdesc, struct mesh_desc)
KHASH_MAP_INIT_INT(tdesc, struct tex_desc)

struct GL_DAI_Cmd{
	GLuint count;
	GLuint instance_count;
	GLuint first_index;
	GLuint base_instance;
};

struct gl_batch{
    /* Ringbuffer for draw commands for this batch */
    struct gl_ring     *cmd_ring;
    /* Ringbuffer for for per-instance attributes associated
     * with the draw commands */
    struct gl_ring     *attr_ring;
    /* A mapping of the mesh's VBO to its' position 
     * within the batch buffer list. */
    khash_t(mdesc)     *vbo_desc_map;
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
};

KHASH_MAP_INIT_INT(batch, struct gl_batch*)

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct gl_batch *s_anim_batch;
static khash_t(batch)  *s_chunk_batches;
static GLuint           s_draw_id_vbo;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

uint32_t batch_td_key(struct tile_desc td)
{
    return ((( ((uint32_t)td.chunk_r) & 0xffff) << 16)
         |  (( ((uint32_t)td.chunk_c) & 0xffff) <<  0));
}

uint32_t batch_chunk_key(int chunk_r, int chunk_c)
{
    return ((( ((uint32_t)chunk_r) & 0xffff) << 16)
         |  (( ((uint32_t)chunk_c) & 0xffff) <<  0));
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

static bool batch_alloc_texarray(struct gl_batch *batch)
{
    if(batch->ntexarrs == MAX_TEX_ARRS)
        return false;

    R_GL_Texture_ArrayAlloc(TEX_ARR_SZ, &batch->textures[batch->ntexarrs].arr, GL_TEXTURE0 + batch->ntexarrs);
    batch->textures[batch->ntexarrs].free = ~((uint32_t)0);
    batch->ntexarrs++;
    return true;
}

static void batch_init_stat_vao(GLuint *out, GLuint src_vbo)
{
    GLuint VAO;
    glGenVertexArrays(1, &VAO);

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, src_vbo);

    /* Attribute 0 - position */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void*)0);
    glEnableVertexAttribArray(0);

    /* Attribute 1 - texture coordinates */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, uv));
    glEnableVertexAttribArray(1);

    /* Attribute 2 - normal */
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, normal));
    glEnableVertexAttribArray(2);

    /* Attribute 3 - material index */
    glVertexAttribIPointer(3, 1, GL_INT, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, material_idx));
    glEnableVertexAttribArray(3);

    /* Attribute 4 - draw ID 
     * This is a per-instance attribute that is sourced from the draw ID buffer */
    glBindBuffer(GL_ARRAY_BUFFER, s_draw_id_vbo);

    glVertexAttribIPointer(4, 1, GL_INT, sizeof(GLint), 0);
    glEnableVertexAttribArray(4);
    glVertexAttribDivisor(4, 1); /* advance the ID once per instance */

    *out = VAO;
}

static bool batch_alloc_vbo(struct gl_batch *batch)
{
    if(batch->nvbos == MAX_MESH_BUFFS)
        return false;

    void *heap_meta = pf_metamalloc_init(MESH_BUFF_SZ);
    if(!heap_meta)
        return false;

    GLuint VBO;
    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, MESH_BUFF_SZ, NULL, GL_DYNAMIC_DRAW);

    GLuint VAO;
    batch_init_stat_vao(&VAO, VBO);

    batch->vbos[batch->nvbos] = (struct vbo_desc){heap_meta, VBO, VAO};
    batch->nvbos++;
    return true;
}

static bool batch_append_mesh(struct gl_batch *batch, GLuint VBO)
{
    khiter_t k = kh_get(mdesc, batch->vbo_desc_map, VBO);
    if(k != kh_end(batch->vbo_desc_map))
        return true; /* VBO already in the batch */

    GLint size;
    glBindBuffer(GL_COPY_READ_BUFFER, VBO);
    glGetBufferParameteriv(GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &size);

    if(size > MESH_BUFF_SZ)
        return false;

    int curr_vbo_idx = 0;
    int vbo_offset = -1;
    do{
        vbo_offset = pf_metamemalign(batch->vbos[curr_vbo_idx].heap_meta, sizeof(struct vertex), size);
        if(vbo_offset >= 0)
            break;
    }while(++curr_vbo_idx < batch->nvbos);

    if(vbo_offset < 0) {
        if(batch->nvbos == MAX_MESH_BUFFS)
            return false;
        if(!batch_alloc_vbo(batch))
            return false;
        curr_vbo_idx = batch->nvbos-1;
        vbo_offset = pf_metamemalign(batch->vbos[curr_vbo_idx].heap_meta, sizeof(struct vertex), size);
    }
    assert(curr_vbo_idx >= 0 && curr_vbo_idx < batch->nvbos);
    assert(vbo_offset >= 0);

    /* Perform VBO-to-VBO copy. The data should be copied without having 
     * to do a round-trip to the CPU.
     */
    glBindBuffer(GL_COPY_WRITE_BUFFER, batch->vbos[curr_vbo_idx].VBO);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, vbo_offset, size);

    int status;
    k = kh_put(mdesc, batch->vbo_desc_map, VBO, &status);
    if(status == -1) {
        pf_metafree(batch->vbos[curr_vbo_idx].heap_meta, vbo_offset);
        return false;
    }

    kh_value(batch->vbo_desc_map, k) = (struct mesh_desc){curr_vbo_idx, vbo_offset};
    return true;
}

static void batch_free_mesh(struct gl_batch *batch, GLuint VBO)
{
    khiter_t k = kh_get(mdesc, batch->vbo_desc_map, VBO);
    assert(k != kh_end(batch->vbo_desc_map));

    struct mesh_desc md = kh_value(batch->vbo_desc_map, k);
    pf_metafree(batch->vbos[md.vbo_idx].heap_meta, md.offset);

    kh_del(mdesc, batch->vbo_desc_map, k);
}

static bool batch_append_tex(struct gl_batch *batch, GLuint tid, int idx, struct texture_arr *arr)
{
    khiter_t k = kh_get(tdesc, batch->tid_desc_map, tid);
    if(k != kh_end(batch->tid_desc_map))
        return true; /* texture already in the batch */

    int curr_arr_idx = 0;
    int slice_idx = -1;
    do{
        if(batch->textures[curr_arr_idx].free == 0)
            continue;
        slice_idx = batch_first_free_idx(batch->textures[curr_arr_idx].free);
        break;
    }while(++curr_arr_idx < batch->ntexarrs);

    if(slice_idx == -1) {
        if(curr_arr_idx == batch->ntexarrs)
            return false;
        if(!batch_alloc_texarray(batch))
            return false;
        curr_arr_idx = batch->ntexarrs-1;
        slice_idx = batch_first_free_idx(batch->textures[curr_arr_idx].free);
    }
    assert(curr_arr_idx >= 0 && curr_arr_idx < batch->ntexarrs);
    assert(slice_idx >= 0 && slice_idx < 32);

    R_GL_Texture_BindArray(&batch->textures[curr_arr_idx].arr, R_GL_Shader_GetCurrActive());
    R_GL_Texture_ArrayCopyElem(&batch->textures[curr_arr_idx].arr, slice_idx, arr, idx);

    int status;
    k = kh_put(tdesc, batch->tid_desc_map, tid, &status);
    if(status == -1) {
        return false;
    }

    kh_value(batch->tid_desc_map, k) = (struct tex_desc){curr_arr_idx, slice_idx};
    batch->textures[curr_arr_idx].free &= ~(((uint32_t)0x1) << slice_idx);
    return true;
}

static void batch_free_tex(struct gl_batch *batch, GLuint id)
{
    khiter_t k = kh_get(tdesc, batch->tid_desc_map, id);
    assert(k != kh_end(batch->tid_desc_map));

    struct tex_desc td = kh_value(batch->tid_desc_map, k);
    batch->textures[td.arr_idx].free |= (0x1 << td.tex_idx);

    kh_del(tdesc, batch->tid_desc_map, k);
}

static bool batch_append(struct gl_batch *batch, struct render_private *priv)
{
    if(!batch_append_mesh(batch, priv->mesh.VBO))
        goto fail_append_mesh;

    int tex_idx = 0;
    for(; tex_idx < priv->num_materials; tex_idx++) {
        if(!batch_append_tex(batch, priv->materials[tex_idx].texture.id, tex_idx, &priv->material_arr))
            goto fail_append_tex;
    }

    return true;

fail_append_tex:
    do{
        --tex_idx;
        batch_free_tex(batch, priv->materials[tex_idx].texture.id);
    }while(tex_idx > 0);
    batch_free_mesh(batch, priv->mesh.VBO);
fail_append_mesh:
    return false;
}

static struct gl_batch *batch_init(void)
{
    struct gl_batch *batch = malloc(sizeof(struct gl_batch));
    if(!batch)
        goto fail_alloc;

    batch->cmd_ring = R_GL_RingbufferInit(CMD_RING_SZ, RING_UBYTE);
	if(!batch->cmd_ring)
        goto fail_cmd_ring;

    batch->attr_ring = R_GL_RingbufferInit(ATTR_RING_SZ, RING_FLOAT);
    if(!batch->attr_ring)
        goto fail_attr_ring;

    batch->vbo_desc_map = kh_init(mdesc);
    if(!batch->vbo_desc_map)
        goto fail_vbo_desc_map;
        
    batch->tid_desc_map = kh_init(tdesc);
    if(!batch->tid_desc_map)
        goto fail_tid_desc_map;

    batch->ntexarrs = 0;
    if(!batch_alloc_texarray(batch))
        goto fail_tex_array;

    batch->nvbos = 0;
    if(!batch_alloc_vbo(batch))
        goto fail_vbo;

    GL_ASSERT_OK();
    return batch;

fail_vbo:
    R_GL_Texture_ArrayFree(batch->textures[0].arr);
fail_tex_array:
    kh_destroy(tdesc, batch->tid_desc_map);
fail_tid_desc_map:
    kh_destroy(mdesc, batch->vbo_desc_map);
fail_vbo_desc_map:
    R_GL_RingbufferDestroy(batch->attr_ring);
fail_attr_ring:
    R_GL_RingbufferDestroy(batch->cmd_ring);
fail_cmd_ring:
	free(batch);
fail_alloc:
    return NULL;
}

static void batch_destroy(struct gl_batch *batch)
{
    for(int i = 0; i < batch->ntexarrs; i++) {
        R_GL_Texture_ArrayFree(batch->textures[i].arr);
    }
    for(int i = 0; i < batch->nvbos; i++) {
        glDeleteBuffers(1, &batch->vbos[i].VBO);
    }

    kh_destroy(tdesc, batch->tid_desc_map);
    kh_destroy(mdesc, batch->vbo_desc_map);

    R_GL_RingbufferDestroy(batch->attr_ring);
    R_GL_RingbufferDestroy(batch->cmd_ring);

	free(batch);
}

static struct mesh_desc batch_mdesc_for_vbo(struct gl_batch *batch, GLuint VBO)
{
    khiter_t k = kh_get(mdesc, batch->vbo_desc_map, VBO);
    assert(k != kh_end(batch->vbo_desc_map));
    return kh_value(batch->vbo_desc_map, k);
}

static struct tex_desc batch_tdesc_for_tid(struct gl_batch *batch, GLuint tid)
{
    khiter_t k = kh_get(tdesc, batch->tid_desc_map, tid);
    assert(k != kh_end(batch->tid_desc_map));
    return kh_value(batch->tid_desc_map, k);
}

static bool batch_chunk_compare(struct tile_desc a, struct tile_desc b)
{
    uint32_t ia = batch_td_key(a);
    uint32_t ib = batch_td_key(b);
    return ia > ib;
}

/* Sort the 'ents' array in-place by the chunk coordinate of the entities. Fill
 * 'out' with a list of descriptors about what subrange of the sorted array 
 * corresponds to which chunk */
static size_t batch_sort_by_chunk(vec_rstat_t *ents, struct chunk_batch_desc *out, size_t maxout)
{
    if(vec_size(ents) == 0)
        return 0;

    int i = 1;
    while(i < vec_size(ents)) {
        int j = i;
        while(j > 0 && batch_chunk_compare(vec_AT(ents, j - 1).td, vec_AT(ents, j).td)) {

            struct ent_stat_rstate tmp = vec_AT(ents, j - 1);
            vec_AT(ents, j - 1) = vec_AT(ents, j);
            vec_AT(ents, j) = tmp;
            j--;
        }
        i++;
    }

    size_t ret = 0;

    struct chunk_batch_desc curr = (struct chunk_batch_desc){
        .chunk_r = vec_AT(ents, 0).td.chunk_r,
        .chunk_c = vec_AT(ents, 0).td.chunk_c,
        .start_idx = 0,
    };
    for(int i = 1; i < vec_size(ents); i++) {
    
        if(batch_td_key(vec_AT(ents, i - 1).td) != batch_td_key(vec_AT(ents, i).td)) {
            curr.end_idx = i - 1;
            out[ret++] = curr;
            curr = (struct chunk_batch_desc){
                .chunk_r = vec_AT(ents, i).td.chunk_r,
                .chunk_c = vec_AT(ents, i).td.chunk_c,
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

static size_t batch_sort_by_inst(struct ent_stat_rstate *ents, size_t nents, 
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

static size_t batch_sort_by_vbo(struct gl_batch *batch, struct inst_group_desc *descs, 
                                size_t ndescs, struct draw_call_desc *out, size_t maxout)
{
    int i = 1;
    while(i < ndescs) {
        int j = i;

        GLuint VBO1 = ((struct render_private*)descs[j - 1].render_private)->mesh.VBO;
        GLuint VBO2 = ((struct render_private*)descs[j].render_private)->mesh.VBO;
        struct mesh_desc md1 = batch_mdesc_for_vbo(batch, VBO1);
        struct mesh_desc md2 = batch_mdesc_for_vbo(batch, VBO2);

        while(j > 0 && md1.vbo_idx > md2.vbo_idx) {

            struct inst_group_desc tmp = descs[j - 1];
            descs[j - 1] = descs[j];
            descs[j] = tmp;
            j--;
        }
        i++;
    }

    size_t ret = 0;

    GLuint VBO = ((struct render_private*)descs[0].render_private)->mesh.VBO;
    struct mesh_desc md = batch_mdesc_for_vbo(batch, VBO);

    struct draw_call_desc curr = (struct draw_call_desc){
        .vbo_idx = md.vbo_idx,
        .start_idx = 0,
    };
    for(int i = 1; i < ndescs; i++) {
    
        GLuint VBO1 = ((struct render_private*)descs[i - 1].render_private)->mesh.VBO;
        GLuint VBO2 = ((struct render_private*)descs[i].render_private)->mesh.VBO;
        struct mesh_desc md1 = batch_mdesc_for_vbo(batch, VBO1);
        struct mesh_desc md2 = batch_mdesc_for_vbo(batch, VBO2);

        if(md1.vbo_idx != md2.vbo_idx) {

            curr.end_idx = i - 1;
            out[ret++] = curr;
            curr = (struct draw_call_desc){
                .vbo_idx = md2.vbo_idx,
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

static void batch_push_stat_attrs(struct gl_batch *batch, const struct ent_stat_rstate *ents,
                                  struct draw_call_desc dcall, struct inst_group_desc *descs)
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
        
            /* Push the model matrix */
            if(i == dcall.start_idx && j == curr->start_idx) {
                R_GL_RingbufferPush(batch->attr_ring, &ents[j].model, sizeof(mat4x4_t));
            }else{
                R_GL_RingbufferAppendLast(batch->attr_ring, &ents[j].model, sizeof(mat4x4_t));
            }

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
        ninsts += curr->end_idx - curr->start_idx + 1;
    }
    size_t begin, end;
    R_GL_RingbufferGetLastRange(batch->attr_ring, &begin, &end);
    assert(end > begin ? (end - begin == 704 * ninsts)
                       : ((ATTR_RING_SZ - begin) + end == 704 * ninsts));
}

static void batch_push_cmds(struct gl_batch *batch, struct draw_call_desc dcall,
                            struct inst_group_desc *descs)
{
    size_t inst_idx = 0;
    for(int i = dcall.start_idx; i <= dcall.end_idx; i++) {

        struct render_private *priv = descs[i].render_private;
        struct mesh_desc mdesc = batch_mdesc_for_vbo(batch, priv->mesh.VBO);
        assert(mdesc.offset % sizeof(struct vertex) == 0);

        struct GL_DAI_Cmd cmd = (struct GL_DAI_Cmd){
            .count = priv->mesh.num_verts,
            .instance_count = descs[i].end_idx - descs[i].start_idx + 1,
            .first_index = mdesc.offset / sizeof(struct vertex),
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
    assert(end > begin ? (end - begin  == sizeof(struct GL_DAI_Cmd) * ncmds)
                       : ((CMD_RING_SZ - begin) + end == sizeof(struct GL_DAI_Cmd) * ncmds));
}

static void batch_do_drawcall(struct gl_batch *batch, const struct ent_stat_rstate *ents,
                              struct draw_call_desc dcall, struct inst_group_desc *descs)
{
    GLuint VAO = batch->vbos[dcall.vbo_idx].VAO;

    GLuint cmd_vbo = R_GL_RingbufferGetVBO(batch->cmd_ring);
    glBindVertexArray(VAO);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_vbo);

    batch_push_cmds(batch, dcall, descs);
    R_GL_RingbufferBindLast(batch->cmd_ring, CMD_RING_TUNIT, R_GL_Shader_GetCurrActive(), "cmdbuff");

    batch_push_stat_attrs(batch, ents, dcall, descs);
    R_GL_RingbufferBindLast(batch->attr_ring, ATTR_RING_TUNIT, R_GL_Shader_GetCurrActive(), "attrbuff");

    size_t cmd_begin, cmd_end;
    R_GL_RingbufferGetLastRange(batch->cmd_ring, &cmd_begin, &cmd_end);

    if(cmd_end < cmd_begin) {

        assert((CMD_RING_SZ - cmd_begin) % sizeof(struct GL_DAI_Cmd) == 0);
        size_t ncmds_end = (CMD_RING_SZ - cmd_begin) / sizeof(struct GL_DAI_Cmd);
        glMultiDrawArraysIndirect(GL_TRIANGLES, (void*)cmd_begin, ncmds_end, 0);

        assert(cmd_end % sizeof(struct GL_DAI_Cmd) == 0);
        size_t ncmds_begin = cmd_end / sizeof(struct GL_DAI_Cmd);
        glMultiDrawArraysIndirect(GL_TRIANGLES, (void*)0, ncmds_begin, 0);
    }else{
        size_t ncmds = dcall.end_idx - dcall.start_idx + 1;
        glMultiDrawArraysIndirect(GL_TRIANGLES, (void*)cmd_begin, ncmds, 0);
    }

    R_GL_RingbufferSyncLast(batch->cmd_ring);
    R_GL_RingbufferSyncLast(batch->attr_ring);
}

static void batch_render_static(struct gl_batch *batch, struct ent_stat_rstate *ents, size_t nents)
{
    GL_PERF_ENTER();

    struct inst_group_desc descs[MAX_BATCHES];
    size_t nbatches = batch_sort_by_inst(ents, nents, descs, ARR_SIZE(descs));

    struct draw_call_desc dcalls[MAX_BATCHES];
    size_t ndcalls = batch_sort_by_vbo(batch, descs, nbatches, dcalls, ARR_SIZE(dcalls));

    for(int i = 0; i < batch->ntexarrs; i++) {
        R_GL_Texture_BindArray(&batch->textures[i].arr, R_GL_Shader_GetCurrActive());
    }

    for(int i = 0; i < ndcalls; i++) {
        batch_do_drawcall(batch, ents, dcalls[i], descs);
    }

    GL_PERF_RETURN_VOID();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_Batch_Init(void)
{
    s_anim_batch = batch_init();
    if(!s_anim_batch)
        goto fail_anim_batch;
    s_chunk_batches = kh_init(batch);
    if(!s_chunk_batches)
        goto fail_chunk_batches;

    GLint draw_id_buff[MAX_INSTS];
    for(int i = 0; i < MAX_INSTS; i++)
        draw_id_buff[i] = i; 

    glGenBuffers(1, &s_draw_id_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_draw_id_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(draw_id_buff), draw_id_buff, GL_STATIC_DRAW);

    return true;

fail_chunk_batches:
    batch_destroy(s_anim_batch);
fail_anim_batch:
    return false;
}

void R_GL_Batch_Shutdown(void)
{
    batch_destroy(s_anim_batch);

    uint32_t key;
    struct gl_batch *curr;
    (void)key;

    kh_foreach(s_chunk_batches, key, curr, {
        batch_destroy(curr);
    });
    kh_destroy(batch, s_chunk_batches);
    glDeleteBuffers(1, &s_draw_id_vbo);
}

void R_GL_Batch_Draw(struct render_input *in)
{
    GL_PERF_ENTER();

    struct chunk_batch_desc descs[MAX_BATCHES];
    size_t nbatches = batch_sort_by_chunk(&in->cam_vis_stat, descs, ARR_SIZE(descs));

    if(nbatches == 0)
        GL_PERF_RETURN_VOID();

    //TODO: this can be shadowed or not shadowed
    R_GL_Shader_Install("mesh.static.textured-phong-shadowed-batched");

    for(int i = 0; i < nbatches; i++) {
    
        const struct chunk_batch_desc *curr = &descs[i];
        uint32_t key = batch_chunk_key(curr->chunk_r, curr->chunk_c);

        khiter_t k = kh_get(batch, s_chunk_batches, key);
        if(k == kh_end(s_chunk_batches)) {
            int status;
            k = kh_put(batch, s_chunk_batches, key, &status);
            assert(status != -1 && status != 0);
            kh_value(s_chunk_batches, k) = batch_init();
        }

        struct gl_batch *batch = kh_value(s_chunk_batches, k);
        size_t ndraw = curr->end_idx - curr->start_idx + 1;

        for(int i = 0; i < ndraw; i++) {
            batch_append(batch, vec_AT(&in->cam_vis_stat, curr->start_idx + i).render_private);
        }
        batch_render_static(batch, &vec_AT(&in->cam_vis_stat, curr->start_idx), ndraw);
    }

    GL_PERF_RETURN_VOID();
}

void R_GL_Batch_Reset(void)
{
    uint32_t key;
    struct gl_batch *curr;
    (void)key;

    kh_foreach(s_chunk_batches, key, curr, {
        batch_destroy(curr);
    });
    kh_clear(batch, s_chunk_batches);

    batch_destroy(s_anim_batch);
    s_anim_batch = batch_init();
}

void R_GL_Batch_AllocChunks(struct map_resolution *res)
{
    GL_PERF_ENTER();

    for(int r = 0; r < res->chunk_h; r++) {
    for(int c = 0; c < res->chunk_w; c++) {
    
        uint32_t key = batch_chunk_key(r, c);
        khiter_t k = kh_get(batch, s_chunk_batches, key);

        if(k == kh_end(s_chunk_batches)) {
            int status;
            k = kh_put(batch, s_chunk_batches, key, &status);
            assert(status != -1 && status != 0);
            kh_value(s_chunk_batches, k) = batch_init();
        }
    }}

    GL_PERF_RETURN_VOID();
}

