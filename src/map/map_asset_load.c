/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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

#include "public/map.h"
#include "pfchunk.h"
#include "map_private.h"
#include "../asset_load.h"
#include "../render/public/render.h"
#include "../render/public/render_al.h"
#include "../render/public/render_ctrl.h"
#include "../navigation/public/nav.h"
#include "../game/public/game.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/mem.h"
#include "../lib/public/block_allocator.h"
#include "../ui.h"
#include "../perf.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* ASCII to integer - argument must be an ascii digit */
#define A2I(_a) ((_a) - '0')
#define MINIMAP_DFLT_SZ (256)
#define PFMAP_VER       (1.1f)
#define CHK_TRUE(_pred, _label) do{ if(!(_pred)) goto _label; }while(0)

/* The Wang tiling algorithm is based on the paper "Wang Tiles for Image and Texture Generation"
 * by Cohen, Shade, Hiller, and Deussen.
 */
enum wang_tile_color{
    BLUE,
    RED,
    YELLOW,
    GREEN,
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct block_allocator s_map_block_alloc;
static struct block_allocator s_nav_block_alloc;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool m_al_parse_tile(const char *str, struct tile *out)
{
    if(strlen(str) != 24)
        return false;

    char type_hexstr[2] = {str[0], '\0'};

    memset(out, 0, sizeof(struct tile));
    out->type          = (enum tiletype) strtol(type_hexstr, NULL, 16);
    out->base_height   = (int)           (str[1] == '-' ? -1 : 1) * (10  * A2I(str[2]) + A2I(str[3]));
    out->ramp_height   = (int)           (10  * A2I(str[4]) + A2I(str[5]));
    out->top_mat_idx   = (int)           (100 * A2I(str[6]) + 10 * A2I(str[7 ]) + A2I(str[8 ]));
    out->sides_mat_idx = (int)           (100 * A2I(str[9]) + 10 * A2I(str[10]) + A2I(str[11]));
    out->pathable      = (bool)          A2I(str[12]);
    out->blend_mode    = (int)           A2I(str[13]);
    out->blend_normals = (bool)          A2I(str[14]);
    out->no_bump_map   = (bool)          A2I(str[15]);

    return true;
}

static bool m_al_write_tile(const struct tile *tile, SDL_RWops *stream)
{
    char buff[MAX_LINE_LEN];
    pf_snprintf(buff, sizeof(buff), "%01X%c%02d%02d%03d%03d%01d%01d%01d%01d00000000", 
        tile->type,
        tile->base_height >= 0 ? '+' : '-',
        abs(tile->base_height),
        tile->ramp_height,
        tile->top_mat_idx, 
        tile->sides_mat_idx,
        tile->pathable,
        tile->blend_mode,
        tile->blend_normals,
        tile->no_bump_map);

    return SDL_RWwrite(stream, buff, strlen(buff), 1);
}

static bool m_al_read_row(SDL_RWops *stream, struct tile *out, size_t *out_nread)
{
    char line[MAX_LINE_LEN];
    READ_LINE(stream, line, fail); 

    char *saveptr;
    /* String points to the first token - the first tile of this row */
    char *string = pf_strtok_r(line, " \t\n", &saveptr);
    assert(out_nread);
    *out_nread = 0;

    while(string) {

        if(!m_al_parse_tile(string, out + *out_nread))
            goto fail;
        (*out_nread)++;
        string = pf_strtok_r(NULL, " \t\n", &saveptr);
    }

    return true;

fail:
    return false;
}

static bool m_al_read_pfchunk(SDL_RWops *stream, struct pfchunk *out)
{
    size_t tiles_read = 0;
    while(tiles_read < TILES_PER_CHUNK_WIDTH * TILES_PER_CHUNK_HEIGHT) {
        
        size_t tiles_in_row;
        if(!m_al_read_row(stream, out->tiles + tiles_read, &tiles_in_row))
            return false;
        tiles_read += tiles_in_row;
    }

    return true;
}

static bool m_al_read_material(SDL_RWops *stream, char *out_texname)
{
    char line[MAX_LINE_LEN];
    READ_LINE(stream, line, fail); 

    char *saveptr;
    char *string = pf_strtok_r(line, " \t\n", &saveptr);

    if(strcmp(string, "material") != 0)
        goto fail;

    string = pf_strtok_r(NULL, " \t\n", &saveptr); /* skip name */
    string = pf_strtok_r(NULL, " \t\n", &saveptr);

    strncpy(out_texname, string, MAX_LINE_LEN);
    return true;

fail:
    return false;
}

static bool m_al_read_splat(SDL_RWops *stream, struct splat *out_splat)
{
    char line[MAX_LINE_LEN];
    READ_LINE(stream, line, fail); 

    char *saveptr;
    char *string = pf_strtok_r(line, " \t\n", &saveptr);

    if(strcmp(string, "splat") != 0)
        goto fail;

    string = pf_strtok_r(NULL, " \t\n", &saveptr);
    if(!sscanf(string, "%u", &out_splat->base_mat_idx))
        goto fail;

    string = pf_strtok_r(NULL, " \t\n", &saveptr);
    if(!sscanf(string, "%u", &out_splat->accent_mat_idx))
        goto fail;

    return true;

fail:
    return false;
}

static void m_al_patch_adjacency_info(struct map *map)
{
    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width;  c++) {

        void *chunk_rprivate = map->chunks[r * map->width + c].render_private;
        for(int tile_r = 0; tile_r < TILES_PER_CHUNK_HEIGHT; tile_r++) {
        for(int tile_c = 0; tile_c < TILES_PER_CHUNK_HEIGHT; tile_c++) {
        
            struct tile_desc desc = (struct tile_desc){r, c, tile_r, tile_c};
            const struct tile *tile = &map->chunks[r * map->width + c].tiles[tile_r * TILES_PER_CHUNK_WIDTH + tile_c];

            R_PushCmd((struct rcmd){
                .func = R_GL_TilePatchVertsBlend,
                .nargs = 3,
                .args = {
                    chunk_rprivate,
                    (void*)G_GetPrevTickMap(),
                    R_PushArg(&desc, sizeof(desc)),
                },
            });

            if(!tile->blend_normals)
                continue;

            R_PushCmd((struct rcmd){
                .func = R_GL_TilePatchVertsSmooth,
                .nargs = 3,
                .args = {
                    chunk_rprivate,
                    (void*)G_GetPrevTickMap(),
                    R_PushArg(&desc, sizeof(desc)),
                },
            });
        }}
    }}
}

static enum wang_tile_color bot_edge_color(int idx)
{
    assert(idx >= 0 && idx < 8);
    enum wang_tile_color map[] = {
        [0] = GREEN,
        [1] = GREEN,
        [2] = RED,
        [3] = RED,
        [4] = GREEN,
        [5] = GREEN,
        [6] = RED,
        [7] = RED
    };
    return map[idx];
}

static enum wang_tile_color top_edge_color(int idx)
{
    assert(idx >= 0 && idx < 8);
    enum wang_tile_color map[] = {
        [0] = RED,
        [1] = GREEN,
        [2] = RED,
        [3] = GREEN,
        [4] = RED,
        [5] = GREEN,
        [6] = RED,
        [7] = GREEN
    };
    return map[idx];
}

static enum wang_tile_color right_edge_color(int idx)
{
    assert(idx >= 0 && idx < 8);
    enum wang_tile_color map[] = {
        [0] = YELLOW,
        [1] = BLUE,
        [2] = YELLOW,
        [3] = BLUE,
        [4] = BLUE,
        [5] = YELLOW,
        [6] = BLUE,
        [7] = YELLOW
    };
    return map[idx];
}

static enum wang_tile_color left_edge_color(int idx)
{
    assert(idx >= 0 && idx < 8);
    enum wang_tile_color map[] = {
        [0] = BLUE,
        [1] = BLUE,
        [2] = YELLOW,
        [3] = YELLOW,
        [4] = YELLOW,
        [5] = YELLOW,
        [6] = BLUE,
        [7] = BLUE
    };
    return map[idx];
}

static int random_wang_idx(void)
{
    return rand() % 8;
}

size_t indices_with_top_color(enum wang_tile_color top_color, int *out)
{
    size_t ret = 0;
    for(int i = 0; i < 8; i++) {
        if(top_edge_color(i) == top_color)
            out[ret++] = i;
    }
    assert(ret > 0 && ret < 8);
    return ret;
}

size_t indices_with_left_color(enum wang_tile_color left_color, int *out)
{
    size_t ret = 0;
    for(int i = 0; i < 8; i++) {
        if(left_edge_color(i) == left_color)
            out[ret++] = i;
    }
    assert(ret > 0 && ret < 8);
    return ret;
}

size_t indices_with_top_left_colors(enum wang_tile_color top_color,
                                    enum wang_tile_color left_color, int *out)
{
    size_t ret = 0;
    for(int i = 0; i < 8; i++) {
        if(top_edge_color(i) == top_color && left_edge_color(i) == left_color)
            out[ret++] = i;
    }
    assert(ret > 0 && ret < 8);
    return ret;
}

static int random_wang_idx_constrain_top(int top_idx)
{
    enum wang_tile_color top_color = bot_edge_color(top_idx);
    int indices[8];
    size_t ncompat = indices_with_top_color(top_color, indices);
    assert(ncompat > 0);
    size_t roll = rand() % ncompat;
    return indices[roll];
}

static int random_wang_idx_constrain_left(int left_idx)
{
    enum wang_tile_color left_color = right_edge_color(left_idx);
    int indices[8];
    size_t ncompat = indices_with_left_color(left_color, indices);
    assert(ncompat > 0);
    size_t roll = rand() % ncompat;
    return indices[roll];
}

static int random_wang_idx_constrain_top_left(int top_idx, int left_idx)
{
    enum wang_tile_color top_color = bot_edge_color(top_idx);
    enum wang_tile_color left_color = right_edge_color(left_idx);
    int indices[8];
    size_t ncompat = indices_with_top_left_colors(top_color, left_color, indices);
    assert(ncompat > 0);
    size_t roll = rand() % ncompat;
    return indices[roll];
}

static void generate_wang_indices(struct map *map)
{
    struct map_resolution res;
    M_GetResolution(map, &res);

    for(int chunk_r = 0; chunk_r < res.chunk_h; chunk_r++) {
    for(int chunk_c = 0; chunk_c < res.chunk_w; chunk_c++) {

        struct pfchunk *chunk = &map->chunks[chunk_r * res.chunk_w + chunk_c];

        for(int tile_r = 0; tile_r < res.tile_h; tile_r++) {
        for(int tile_c = 0; tile_c < res.tile_w; tile_c++) {

            int global_r = (chunk_r * res.tile_w) + tile_r;
            int global_c = (chunk_c * res.tile_h) + tile_c;
            struct tile *tile = &chunk->tiles[tile_r * res.tile_w + tile_c];

            int wang_idx;
            if(global_r == 0 && global_c == 0) {

                wang_idx = random_wang_idx();

            }else if(global_r == 0 && global_c > 0) {

                struct tile_desc td = (struct tile_desc){chunk_r, chunk_c, tile_r, tile_c};
                M_Tile_RelativeDesc(res, &td, -1, 0);
                struct tile *left_tile;
                M_TileForDesc(map, td, &left_tile);
                int left_idx = left_tile->wang_idx;
                wang_idx = random_wang_idx_constrain_left(left_idx);

            }else if(global_r > 0 && global_c == 0) {

                struct tile_desc td = (struct tile_desc){chunk_r, chunk_c, tile_r, tile_c};
                M_Tile_RelativeDesc(res, &td, 0, -1);
                struct tile *top_tile;
                M_TileForDesc(map, td, &top_tile);
                int top_idx = top_tile->wang_idx;
                wang_idx = random_wang_idx_constrain_top(top_idx);

            }else{

                struct tile_desc td = (struct tile_desc){chunk_r, chunk_c, tile_r, tile_c};
                M_Tile_RelativeDesc(res, &td, -1, 0);
                struct tile *left_tile;
                M_TileForDesc(map, td, &left_tile);
                int left_idx = left_tile->wang_idx;

                td = (struct tile_desc){chunk_r, chunk_c, tile_r, tile_c};
                M_Tile_RelativeDesc(res, &td, 0, -1);
                struct tile *top_tile;
                M_TileForDesc(map, td, &top_tile);
                int top_idx = top_tile->wang_idx;

                wang_idx = random_wang_idx_constrain_top_left(top_idx, left_idx);
                assert(bot_edge_color(top_idx) == top_edge_color(wang_idx));
                assert(right_edge_color(left_idx) == left_edge_color(wang_idx));
            }
            tile->wang_idx = wang_idx;
        }}
    }}
}

static void set_minimap_defaults(struct map *map)
{
    map->minimap_vres = (vec2_t){1920, 1080};
    map->minimap_sz = MINIMAP_DFLT_SZ;
    map->minimap_center_pos = (vec2_t){
        MINIMAP_DFLT_SZ * cos(M_PI/4.0) + 10, 
        1080 - (MINIMAP_DFLT_SZ * cos(M_PI/4.0) + 10)};
    map->minimap_resize_mask = ANCHOR_X_LEFT | ANCHOR_Y_BOT;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/
 
bool M_AL_InitMapFromStream(const struct pfmap_hdr *header, const char *basedir,
                            SDL_RWops *stream, void *outmap, bool update_navgrid)
{
    struct map *map = outmap;

    map->width = header->num_cols;
    map->height = header->num_rows;
    map->pos = (vec3_t) {0.0f, 0.0f, 0.0f};
    set_minimap_defaults(map);

    /* Read materials */
    STALLOC(char, texnames, header->num_materials * 256);
    for(int i = 0; i < header->num_materials; i++) {
        if(i >= MAX_NUM_MATS)
            return false;
        if(!m_al_read_material(stream, &texnames[i * 256]))
            return false;
        strcpy(map->texnames[i], &texnames[i * 256]);
    }
    map->num_mats = header->num_materials;

    /* Read splats */
    for(int i = 0; i < header->num_splats; i++) {
        if(i >= MAX_NUM_SPLATS)
            return false;
        if(!m_al_read_splat(stream, &map->splatmap.splats[i]))
            return false;
    }
    map->num_splats = header->num_splats;

    struct map_resolution res = (struct map_resolution){
        header->num_cols,
        header->num_rows,
        TILES_PER_CHUNK_WIDTH,
        TILES_PER_CHUNK_HEIGHT,
		TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE,
		TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE
    };

    R_PushCmd((struct rcmd){
        .func = R_GL_MapInit,
        .nargs = 3,
        .args = {
            R_PushArg(texnames, header->num_materials * 256),
            R_PushArg(&header->num_materials, sizeof(header->num_materials)),
            R_PushArg(&res, sizeof(res)),
        },
    });
    STFREE(texnames);

    /* Read chunks */
    size_t num_chunks = header->num_rows * header->num_cols;
    char *unused_base = (char*)(map + 1);
    unused_base += num_chunks * sizeof(struct pfchunk);

    for(int i = 0; i < num_chunks; i++) {

        if(!m_al_read_pfchunk(stream, map->chunks + i))
            return false;
    }

    generate_wang_indices(map);

    for(int i = 0; i < num_chunks; i++) {
    
        map->chunks[i].render_private = (void*)unused_base;
        size_t renderbuff_sz = R_AL_PrivBuffSizeForChunk(
                               TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT, 0);
        unused_base += renderbuff_sz;

        if(!R_AL_InitPrivFromTiles(map, i / header->num_cols, i % header->num_cols,
                                   map->chunks[i].tiles, TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT,
                                   map->chunks[i].render_private, basedir)) {
            return false;
        }
    }

    m_al_patch_adjacency_info(map);

    /* Build navigation grid */
    STALLOC(const struct tile*, chunk_tiles, map->width * map->height);

    for(int r = 0; r < map->height; r++) {
    for(int c = 0; c < map->width; c++) {
        chunk_tiles[r * map->width + c] = map->chunks[r * map->width + c].tiles;
    }}

    map->nav_private = N_NewCtxForMapData(map->width, map->height, 
        TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT, chunk_tiles, update_navgrid);
    STFREE(chunk_tiles);

    if(!map->nav_private)
        return false;

    return true;
}

size_t M_AL_BuffSizeFromHeader(const struct pfmap_hdr *header)
{
    size_t num_chunks = header->num_rows * header->num_cols;

    return sizeof(struct map) + num_chunks * 
           (sizeof(struct pfchunk) + R_AL_PrivBuffSizeForChunk(
                                     TILES_PER_CHUNK_WIDTH, TILES_PER_CHUNK_HEIGHT, 0));
}

bool M_AL_UpdateTile(struct map *map, const struct tile_desc *desc, const struct tile *tile)
{
    if(desc->chunk_r >= map->height || desc->chunk_c >= map->width)
        return false;

    struct pfchunk *chunk = &map->chunks[desc->chunk_r * map->width + desc->chunk_c];
    int old_wang_idx = chunk->tiles[desc->tile_r * TILES_PER_CHUNK_WIDTH + desc->tile_c].wang_idx;
    chunk->tiles[desc->tile_r * TILES_PER_CHUNK_WIDTH + desc->tile_c] = *tile;
    chunk->tiles[desc->tile_r * TILES_PER_CHUNK_WIDTH + desc->tile_c].wang_idx = old_wang_idx;

    struct map_resolution res;
    M_GetResolution(map, &res);

    for(int dr = -1; dr <= 1; dr++) {
    for(int dc = -1; dc <= 1; dc++) {
    
        struct tile_desc curr = *desc;
        int ret = M_Tile_RelativeDesc(res, &curr, dc, dr);
        if(ret) {
        
            struct pfchunk *chunk = &map->chunks[curr.chunk_r * map->width + curr.chunk_c];
            R_PushCmd((struct rcmd){
                .func = R_GL_TileUpdate,
                .nargs = 3,
                .args = {
                    chunk->render_private,
                    (void*)G_GetPrevTickMap(),
                    R_PushArg(&curr, sizeof(curr)),
                },
            });
        }
    }}

    return true;
}

void M_AL_FreePrivate(struct map *map)
{
    R_PushCmd((struct rcmd){ .func = R_GL_MapShutdown });
    assert(map->nav_private);
    N_FreeCtx(map->nav_private);
}

size_t M_AL_ShallowCopySize(size_t nrows, size_t ncols)
{
	size_t nchunks = nrows * ncols;
    return sizeof(struct map) + nchunks * sizeof(struct pfchunk);
}

void M_AL_ShallowCopy(struct map *dst, const struct map *src)
{
    memcpy(dst, src, M_AL_ShallowCopySize(src->width, src->height));
}

struct map *M_AL_CopyWithFields(const struct map *src)
{
    PERF_PUSH("alloc map block");
    struct map *ret = block_alloc(&s_map_block_alloc);
    PERF_POP();
    if(!ret)
        return NULL;

    PERF_PUSH("alloc nav block");
    void *nav = block_alloc(&s_nav_block_alloc);
    PERF_POP();
    if(!nav)
        return NULL;

    M_AL_ShallowCopy(ret, src);

    N_CloneCtx(src->nav_private, nav);
    ret->nav_private = nav;

    return ret;
}

void M_AL_FreeCopyWithFields(struct map *map)
{
    N_DestroyCtx(map->nav_private);
    block_free(&s_nav_block_alloc, map->nav_private);
    block_free(&s_map_block_alloc, map);
}

bool M_AL_WritePFMap(const struct map *map, SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];

    pf_snprintf(line, sizeof(line), "version %.01f\n", PFMAP_VER);
    CHK_TRUE(SDL_RWwrite(stream, line, strlen(line), 1), fail);

    pf_snprintf(line, sizeof(line), "num_materials %d\n", map->num_mats);
    CHK_TRUE(SDL_RWwrite(stream, line, strlen(line), 1), fail);

    pf_snprintf(line, sizeof(line), "num_splats %d\n", map->num_splats);
    CHK_TRUE(SDL_RWwrite(stream, line, strlen(line), 1), fail);

    pf_snprintf(line, sizeof(line), "num_rows %d\n", map->height);
    CHK_TRUE(SDL_RWwrite(stream, line, strlen(line), 1), fail);

    pf_snprintf(line, sizeof(line), "num_cols %d\n", map->width);
    CHK_TRUE(SDL_RWwrite(stream, line, strlen(line), 1), fail);

    for(int i = 0; i < map->num_mats; i++) {
    
        pf_snprintf(line, sizeof(line), "material __anonymous__ %s\n", map->texnames[i]);
        CHK_TRUE(SDL_RWwrite(stream, line, strlen(line), 1), fail);
    }

    for(int i = 0; i < map->num_splats; i++) {

        pf_snprintf(line, sizeof(line), "splat %d %d\n", 
            map->splatmap.splats[i].base_mat_idx, map->splatmap.splats[i].accent_mat_idx);
        CHK_TRUE(SDL_RWwrite(stream, line, strlen(line), 1), fail);
    }

    for(int chunk_r = 0; chunk_r < map->height; chunk_r++) {
    for(int chunk_c = 0; chunk_c < map->width;  chunk_c++) {

        const struct pfchunk *curr_chunk = &map->chunks[chunk_r * map->width + chunk_c];
        for(int tile_r = 0; tile_r < TILES_PER_CHUNK_HEIGHT; tile_r++) {
        for(int tile_c = 0; tile_c < TILES_PER_CHUNK_WIDTH;  tile_c++) {

            const struct tile *curr_tile = &curr_chunk->tiles[tile_r * TILES_PER_CHUNK_WIDTH + tile_c];
            CHK_TRUE(m_al_write_tile(curr_tile, stream), fail);

            if((tile_c+1) % 4 == 0) {
                CHK_TRUE(SDL_RWwrite(stream, "\n", 1, 1), fail);
            }else {
                CHK_TRUE(SDL_RWwrite(stream, " ", 1, 1), fail);
            }
        }}
    }}

    return true;

fail:
    return false;
}

void M_InitCopyPools(const struct map *map)
{
    size_t map_size = M_AL_ShallowCopySize(map->width, map->height);
    size_t nav_size = N_DeepCopySize(map->nav_private);

    block_alloc_init(&s_map_block_alloc, map_size, 8);
    block_alloc_init(&s_nav_block_alloc, nav_size, 8);
}

void M_DestroyCopyPools(void)
{
    block_alloc_destroy(&s_nav_block_alloc);
    block_alloc_destroy(&s_map_block_alloc);
}

