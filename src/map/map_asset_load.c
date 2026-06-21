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

#define MEM_FILE_SYS MEM_SYS_MAP
#define MEM_FILE_SUB MEM_SUB_MAP_ASSET_LOAD

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
#include "../mem.h"
#include "../ui.h"
#include "../perf.h"
#include "../event.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_MAP, MEM_SUB_MAP_ASSET_LOAD)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_MAP, MEM_SUB_MAP_ASSET_LOAD)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_MAP, MEM_SUB_MAP_ASSET_LOAD)

/* ASCII to integer - argument must be an ascii digit */
#define A2I(_a) ((_a) - '0')
#define MINIMAP_DFLT_SZ (256)
#define PFMAP_VER       (1.2f)
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

/* Tile updates (from M_AL_UpdateTile) are not dispatched to the renderer
 * immediately. Instead, the affected tiles are accumulated in a dirty set and
 * flushed in a single per-chunk batch once per frame - see al_flush_dirty_tiles.
 * This amortizes the cost of mapping/unmapping the chunk vertex buffers when
 * many tiles are painted in the same frame. The minimap redraw for each dirty
 * chunk is coalesced into the same flush, so a chunk is redrawn at most once
 * per frame regardless of how many of its tiles changed. */
static const struct map *s_dirty_map     = NULL;
static bool              *s_tile_dirty    = NULL;  /* one flag per map tile    */
static bool              *s_chunk_dirty   = NULL;  /* one flag per map chunk   */
static struct tile_desc  *s_flush_scratch = NULL;  /* holds one chunk of descs */
static bool               s_dirty_pending = false;

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
    out->blend_normals = (bool)          A2I(str[14]);
    out->no_bump_map   = (bool)          A2I(str[15]);
    out->cover         = (enum tile_cover) A2I(str[16]);

    /* Per-side blend modes at positions 17-20 (top, right, bot, left). */
    uint8_t packed = 0;
    packed = TILE_BLEND_SET(packed, TILE_SIDE_TOP,   A2I(str[17]));
    packed = TILE_BLEND_SET(packed, TILE_SIDE_RIGHT, A2I(str[18]));
    packed = TILE_BLEND_SET(packed, TILE_SIDE_BOT,   A2I(str[19]));
    packed = TILE_BLEND_SET(packed, TILE_SIDE_LEFT,  A2I(str[20]));
    out->blend_mode = packed;

    return true;
}

static bool m_al_write_tile(const struct tile *tile, SDL_RWops *stream)
{
    char buff[MAX_LINE_LEN];
    /* Per-side blend modes (top, right, bot, left) at positions 17-20; position 13
     * is reserved. */
    pf_snprintf(buff, sizeof(buff), "%01X%c%02d%02d%03d%03d%01d0%01d%01d%01d%01d%01d%01d%01d000",
        tile->type,
        tile->base_height >= 0 ? '+' : '-',
        abs(tile->base_height),
        tile->ramp_height,
        tile->top_mat_idx,
        tile->sides_mat_idx,
        tile->pathable,
        tile->blend_normals,
        tile->no_bump_map,
        tile->cover,
        TILE_BLEND_GET(tile->blend_mode, TILE_SIDE_TOP),
        TILE_BLEND_GET(tile->blend_mode, TILE_SIDE_RIGHT),
        TILE_BLEND_GET(tile->blend_mode, TILE_SIDE_BOT),
        TILE_BLEND_GET(tile->blend_mode, TILE_SIDE_LEFT));

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

static void al_mark_tile_dirty(struct map_resolution res, struct tile_desc td)
{
    assert(s_tile_dirty && s_chunk_dirty);

    size_t chunk_idx = (size_t)td.chunk_r * res.chunk_w + td.chunk_c;
    size_t tile_idx  = chunk_idx * ((size_t)res.tile_w * res.tile_h)
                     + (size_t)td.tile_r * res.tile_w + td.tile_c;

    s_tile_dirty[tile_idx]   = true;
    s_chunk_dirty[chunk_idx] = true;
    s_dirty_pending          = true;
}

/* Dispatches a single batched R_GL_TileUpdate command for every chunk that has
 * dirty tiles, then clears the dirty set. Invoked once per frame. */
static void al_flush_dirty_tiles(void)
{
    if(!s_dirty_pending)
        return;

    struct map_resolution res;
    M_GetResolution(s_dirty_map, &res);

    size_t tiles_per_chunk = (size_t)res.tile_w * res.tile_h;
    const struct map *prev_map = G_GetPrevTickMap();

    for(int cr = 0; cr < res.chunk_h; cr++) {
    for(int cc = 0; cc < res.chunk_w; cc++) {

        size_t chunk_idx = (size_t)cr * res.chunk_w + cc;
        if(!s_chunk_dirty[chunk_idx])
            continue;
        s_chunk_dirty[chunk_idx] = false;

        bool *chunk_bits = &s_tile_dirty[chunk_idx * tiles_per_chunk];
        size_t ndescs = 0;

        for(int tr = 0; tr < res.tile_h; tr++) {
        for(int tc = 0; tc < res.tile_w; tc++) {

            size_t in_chunk = (size_t)tr * res.tile_w + tc;
            if(!chunk_bits[in_chunk])
                continue;
            chunk_bits[in_chunk] = false;
            s_flush_scratch[ndescs++] = (struct tile_desc){cr, cc, tr, tc};
        }}

        if(ndescs == 0)
            continue;

        const struct pfchunk *chunk = &s_dirty_map->chunks[chunk_idx];
        R_PushCmd((struct rcmd){
            .func = R_GL_TileUpdate,
            .nargs = 4,
            .args = {
                chunk->render_private,
                (void*)prev_map,
                R_PushArg(s_flush_scratch, ndescs * sizeof(struct tile_desc)),
                R_PushArg(&ndescs, sizeof(ndescs)),
            },
        });

        M_UpdateMinimapChunk(s_dirty_map, cr, cc);
    }}

    s_dirty_pending = false;
}

static void on_update_end(void *user, void *event)
{
    al_flush_dirty_tiles();
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

    for(int i = 0; i < num_chunks; i++) {

        struct pfchunk *chunk = &map->chunks[i];
        chunk->has_water = false;
        for(int j = 0; j < TILES_PER_CHUNK_HEIGHT * TILES_PER_CHUNK_WIDTH; j++) {
            if(chunk->tiles[j].base_height < 0) {
                chunk->has_water = true;
                break;
            }
        }
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

    chunk->has_water = false;
    for(int j = 0; j < TILES_PER_CHUNK_HEIGHT * TILES_PER_CHUNK_WIDTH; j++) {
        if(chunk->tiles[j].base_height < 0) {
            chunk->has_water = true;
            break;
        }
    }

    struct map_resolution res;
    M_GetResolution(map, &res);

    /* The painted tile and its 8 neighbors (which carry the adjacency/blend
     * information) need their render-side vertex data refreshed. We don't
     * dispatch the buffer updates here; instead the affected tiles are added
     * to a dirty set that is flushed as a single per-chunk batch once per
     * frame by al_flush_dirty_tiles. */
    for(int dr = -1; dr <= 1; dr++) {
    for(int dc = -1; dc <= 1; dc++) {

        struct tile_desc curr = *desc;
        if(M_Tile_RelativeDesc(res, &curr, dc, dr))
            al_mark_tile_dirty(res, curr);
    }}

    return true;
}

bool M_AL_InitTileUpdateBuffer(const struct map *map)
{
    struct map_resolution res;
    M_GetResolution(map, &res);

    size_t nchunks         = (size_t)res.chunk_w * res.chunk_h;
    size_t tiles_per_chunk = (size_t)res.tile_w * res.tile_h;
    size_t ntiles          = nchunks * tiles_per_chunk;

    s_tile_dirty    = PF_CALLOC(ntiles, sizeof(bool));
    s_chunk_dirty   = PF_CALLOC(nchunks, sizeof(bool));
    s_flush_scratch = PF_MALLOC(tiles_per_chunk * sizeof(struct tile_desc));
    if(!s_tile_dirty || !s_chunk_dirty || !s_flush_scratch)
        goto fail;

    s_dirty_map     = map;
    s_dirty_pending = false;

    /* Flush at EVENT_UPDATE_END - after this tick's tile edits but before the
     * per-frame fog buffer is pushed in G_Update. The minimap redraw pushes and
     * fences a transient 'fully visible' range onto the fog ringbuffer.
     */
    E_Global_Register(EVENT_UPDATE_END, on_update_end, NULL,
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    return true;

fail:
    PF_FREE(s_tile_dirty);
    PF_FREE(s_chunk_dirty);
    PF_FREE(s_flush_scratch);
    s_tile_dirty    = NULL;
    s_chunk_dirty   = NULL;
    s_flush_scratch = NULL;
    return false;
}

void M_AL_DestroyTileUpdateBuffer(void)
{
    E_Global_Unregister(EVENT_UPDATE_END, on_update_end);

    PF_FREE(s_tile_dirty);
    PF_FREE(s_chunk_dirty);
    PF_FREE(s_flush_scratch);

    s_tile_dirty    = NULL;
    s_chunk_dirty   = NULL;
    s_flush_scratch = NULL;
    s_dirty_map     = NULL;
    s_dirty_pending = false;
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
    struct map *ret = PF_MALLOC(M_AL_ShallowCopySize(src->width, src->height));
    if(!ret)
        return NULL;

    void *nav = PF_MALLOC(N_DeepCopySize(src->nav_private));
    if(!nav) {
        PF_FREE(ret);
        return NULL;
    }

    M_AL_ShallowCopy(ret, src);

    N_CloneCtx(src->nav_private, nav);
    ret->nav_private = nav;

    return ret;
}

void M_AL_FreeCopyWithFields(struct map *map)
{
    N_DestroyCtx(map->nav_private);
    PF_FREE(map->nav_private);
    PF_FREE(map);
}

struct map *M_AL_SnapshotShared(const struct map *src)
{
    struct map *ret = PF_MALLOC(M_AL_ShallowCopySize(src->width, src->height));
    if(!ret)
        return NULL;

    M_AL_ShallowCopy(ret, src);
    ret->nav_private = N_NewReaderCtx(src->nav_private);
    if(!ret->nav_private) {
        PF_FREE(ret);
        return NULL;
    }
    return ret;
}

void M_AL_FreeSnapshotShared(struct map *map)
{
    N_FreeReaderCtx(map->nav_private);
    PF_FREE(map);
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

