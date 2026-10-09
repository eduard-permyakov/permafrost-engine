/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
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

#define MEM_FILE_SYS MEM_SYS_GAME
#define MEM_FILE_SUB MEM_SUB_GAME_ROADS

#include "roads.h"
#include "public/game.h"
#include "game_private.h"
#include "../event.h"
#include "../camera.h"
#include "../phys/public/collision.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/attr.h"
#include "../lib/public/vec.h"
#include "../lib/public/pqueue.h"
#include "../lib/public/pf_string.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <SDL.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_ROADS)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_ROADS)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_ROADS)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define CLAMP(a, min, max)  (MIN(MAX((a), (min)), (max)))

#define ROAD_PRESENT        (1 << 7)
#define ROAD_DECAL_STALE    (1 << 6)
#define ROAD_NUM_MASKS      (ROAD_LINK_ALL + 1)
#define ROAD_TILE_X_DIM     (ROAD_TILE_SPAN * X_COORDS_PER_TILE)
#define ROAD_TILE_Z_DIM     (ROAD_TILE_SPAN * Z_COORDS_PER_TILE)
#define ROADS_PER_CHUNK_R   (TILES_PER_CHUNK_HEIGHT / ROAD_TILE_SPAN)
#define ROADS_PER_CHUNK_C   (TILES_PER_CHUNK_WIDTH / ROAD_TILE_SPAN)
#define OVERLAY_MAX_QUADS   (ROADS_PER_CHUNK_R * ROADS_PER_CHUNK_C * 5)

_Static_assert(TILES_PER_CHUNK_WIDTH % ROAD_TILE_SPAN == 0, "Road tiles must not cross chunks");
_Static_assert(TILES_PER_CHUNK_HEIGHT % ROAD_TILE_SPAN == 0, "Road tiles must not cross chunks");

VEC_TYPE(road_idx, int)
VEC_IMPL(static inline, road_idx, int)

PQUEUE_TYPE(road, int)
PQUEUE_IMPL(static, road, int)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map    *s_map;
static struct map_resolution s_res;
static vec3_t               s_map_pos;
static int                  s_rows, s_cols;
static size_t               s_count;

/* One byte per road tile: ROAD_PRESENT, ROAD_DECAL_STALE and the road_link 
 * bits. A link is always set on both of the tiles it joins. 
 */
static uint8_t             *s_tiles;
/* The decal drawn on each road tile, if any, and the tiles whose decal no 
 * longer matches their links. 
 */
static uint32_t            *s_decal_ids;
static vec_road_idx_t       s_stale;

/* Kept across maps, as it is set up by the scripts */
static struct road_tileset  s_tileset;
static bool                 s_has_tileset;
/* Whether the map's decals keep the tileset's images loaded, so that a tile 
 * taking on a new shape never has to wait for its image */
static bool                 s_tileset_retained;

/* Per chunk: the number of road tiles and whether any of them changed. 
 * The dirty flags are left for the consumers of the road layout to drain. 
 */
static int                 *s_chunk_counts;
static bool                *s_chunk_dirty;

static bool                 s_render;
static bool                 s_hover_set;
static struct road_desc     s_hover;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool in_bounds(struct road_desc rd)
{
    return (rd.r >= 0 && rd.r < s_rows && rd.c >= 0 && rd.c < s_cols);
}

static uint8_t *tile_at(struct road_desc rd)
{
    return &s_tiles[rd.r * s_cols + rd.c];
}

static bool present(struct road_desc rd)
{
    return in_bounds(rd) && (*tile_at(rd) & ROAD_PRESENT);
}

static int chunk_index(struct road_desc rd)
{
    return (rd.r / ROADS_PER_CHUNK_R) * s_res.chunk_w + (rd.c / ROADS_PER_CHUNK_C);
}

static uint8_t opposite(uint8_t link)
{
    return ((link << 2) | (link >> 2)) & ROAD_LINK_ALL;
}

static struct road_desc neighbour(struct road_desc rd, uint8_t link)
{
    switch(link) {
    case ROAD_LINK_UP:      return (struct road_desc){rd.r - 1, rd.c};
    case ROAD_LINK_RIGHT:   return (struct road_desc){rd.r, rd.c + 1};
    case ROAD_LINK_DOWN:    return (struct road_desc){rd.r + 1, rd.c};
    case ROAD_LINK_LEFT:    return (struct road_desc){rd.r, rd.c - 1};
    default: assert(0);     return rd;
    }
}

static void mark_stale(struct road_desc rd)
{
    uint8_t *tile = tile_at(rd);
    if(*tile & ROAD_DECAL_STALE)
        return;
    if(!vec_road_idx_push(&s_stale, rd.r * s_cols + rd.c))
        return;
    *tile |= ROAD_DECAL_STALE;
}

static void mark_all_stale(void)
{
    for(int r = 0; r < s_rows; r++) {
    for(int c = 0; c < s_cols; c++) {
        struct road_desc rd = (struct road_desc){r, c};
        if(present(rd) || s_decal_ids[r * s_cols + c] != NULL_DECAL_ID)
            mark_stale(rd);
    }}
}

/* A stable pick among the variants, so a tile keeps its look across saves */
static const char *tile_image(struct road_desc rd, uint8_t links)
{
    int nvariants = s_tileset.nvariants[links];
    uint32_t hash = ((uint32_t)rd.r * 73856093u) ^ ((uint32_t)rd.c * 19349663u);
    return s_tileset.paths[links][(hash >> 4) % nvariants];
}

static void refresh_decal(int idx)
{
    struct road_desc rd = (struct road_desc){idx / s_cols, idx % s_cols};
    uint32_t *id = &s_decal_ids[idx];

    if(!present(rd) || !s_has_tileset) {
        if(*id != NULL_DECAL_ID)
            M_Decal_RemoveInternal(*id);
        *id = NULL_DECAL_ID;
        return;
    }

    vec2_t min, max;
    G_Roads_Bounds(rd, &min, &max);

    struct decal_desc desc = (struct decal_desc){
        .pos = (vec2_t){(min.x + max.x) / 2.0f, (min.z + max.z) / 2.0f},
        .dims = (vec2_t){ROAD_TILE_X_DIM, ROAD_TILE_Z_DIM},
        .rotation = 0.0f,
        .alpha = 1.0f,
        .hide_foliage = true,
    };
    pf_strlcpy(desc.path, tile_image(rd, *tile_at(rd) & ROAD_LINK_ALL), sizeof(desc.path));

    if(*id != NULL_DECAL_ID && M_Decal_UpdateInternal(*id, &desc))
        return;
    if(*id != NULL_DECAL_ID)
        M_Decal_RemoveInternal(*id);
    *id = M_Decal_AddInternal(&desc);
}

static void on_update_start(void *user, void *event)
{
    for(int i = 0; i < vec_size(&s_stale); i++) {
        int idx = vec_AT(&s_stale, i);
        s_tiles[idx] &= ~ROAD_DECAL_STALE;
        refresh_decal(idx);
    }
    vec_road_idx_reset(&s_stale);
}

/* The link from 'a' that leads to 'b', or 0 when they are not orthogonal neighbours */
static uint8_t link_between(struct road_desc a, struct road_desc b)
{
    int dr = b.r - a.r;
    int dc = b.c - a.c;

    if(dr == -1 && dc == 0) return ROAD_LINK_UP;
    if(dr ==  0 && dc == 1) return ROAD_LINK_RIGHT;
    if(dr ==  1 && dc == 0) return ROAD_LINK_DOWN;
    if(dr ==  0 && dc == -1) return ROAD_LINK_LEFT;
    return 0;
}

static void set_link(struct road_desc a, struct road_desc b, bool on)
{
    uint8_t link = link_between(a, b);
    assert(link);

    if(on) {
        *tile_at(a) |= link;
        *tile_at(b) |= opposite(link);
    }else{
        *tile_at(a) &= ~link;
        *tile_at(b) &= ~opposite(link);
    }
    s_chunk_dirty[chunk_index(a)] = true;
    s_chunk_dirty[chunk_index(b)] = true;
    mark_stale(a);
    mark_stale(b);
}

/* Drop every link that is not set on both of the tiles it joins */
static void drop_one_sided_links(void)
{
    for(int r = 0; r < s_rows; r++) {
    for(int c = 0; c < s_cols; c++) {

        struct road_desc rd = (struct road_desc){r, c};
        if(!present(rd))
            continue;

        for(uint8_t link = ROAD_LINK_UP; link & ROAD_LINK_ALL; link <<= 1) {
            if(!(*tile_at(rd) & link))
                continue;
            struct road_desc nb = neighbour(rd, link);
            if(!present(nb) || !(*tile_at(nb) & opposite(link)))
                *tile_at(rd) &= ~link;
        }
    }}
}

static void push_rect(vec2_t **corners, vec3_t **colors, vec2_t min, vec2_t max, vec3_t color)
{
    *(*corners)++ = (vec2_t){max.x, min.z};
    *(*corners)++ = (vec2_t){max.x, max.z};
    *(*corners)++ = (vec2_t){min.x, max.z};
    *(*corners)++ = (vec2_t){min.x, min.z};
    *(*colors)++ = color;
}

/* A square in the middle of each road tile and an arm toward each linked 
 * neighbour, in world space.
 */
static void render_chunk(int chunk_r, int chunk_c)
{
    const vec3_t color = (vec3_t){1.0f, 0.85f, 0.15f};
    const float inset = ROAD_TILE_X_DIM * 0.3f;

    STALLOC(vec2_t, corners_buff, 4 * OVERLAY_MAX_QUADS);
    STALLOC(vec3_t, colors_buff, OVERLAY_MAX_QUADS);

    vec2_t *corners = corners_buff;
    vec3_t *colors = colors_buff;

    for(int r = 0; r < ROADS_PER_CHUNK_R; r++) {
    for(int c = 0; c < ROADS_PER_CHUNK_C; c++) {

        struct road_desc rd = (struct road_desc){
            chunk_r * ROADS_PER_CHUNK_R + r,
            chunk_c * ROADS_PER_CHUNK_C + c
        };
        uint8_t links = *tile_at(rd);
        if(!(links & ROAD_PRESENT))
            continue;

        vec2_t min, max;
        G_Roads_Bounds(rd, &min, &max);

        vec2_t inner_min = (vec2_t){min.x + inset, min.z + inset};
        vec2_t inner_max = (vec2_t){max.x - inset, max.z - inset};
        push_rect(&corners, &colors, inner_min, inner_max, color);

        /* Columns grow toward -X, rows toward +Z */
        if(links & ROAD_LINK_UP)
            push_rect(&corners, &colors, (vec2_t){inner_min.x, min.z}, (vec2_t){inner_max.x, inner_min.z}, color);
        if(links & ROAD_LINK_DOWN)
            push_rect(&corners, &colors, (vec2_t){inner_min.x, inner_max.z}, (vec2_t){inner_max.x, max.z}, color);
        if(links & ROAD_LINK_RIGHT)
            push_rect(&corners, &colors, (vec2_t){min.x, inner_min.z}, (vec2_t){inner_min.x, inner_max.z}, color);
        if(links & ROAD_LINK_LEFT)
            push_rect(&corners, &colors, (vec2_t){inner_max.x, inner_min.z}, (vec2_t){max.x, inner_max.z}, color);
    }}

    size_t count = colors - colors_buff;
    assert(count <= OVERLAY_MAX_QUADS);

    mat4x4_t model;
    PFM_Mat4x4_Identity(&model);
    bool on_water_surface = true;

    R_PushCmd((struct rcmd){
        .func = R_GL_DrawMapOverlayQuads,
        .nargs = 6,
        .args = {
            R_PushArg(corners_buff, sizeof(vec2_t) * 4 * count),
            R_PushArg(colors_buff, sizeof(vec3_t) * count),
            R_PushArg(&count, sizeof(count)),
            R_PushArg(&model, sizeof(model)),
            R_PushArg(&on_water_surface, sizeof(bool)),
            (void*)G_GetPrevTickMap(),
        },
    });

    STFREE(corners_buff);
    STFREE(colors_buff);
}

static void render_hover(void)
{
    const float width = 0.5f;
    const vec3_t color = (vec3_t){1.0f, 1.0f, 1.0f};

    vec2_t min, max;
    if(!G_Roads_Bounds(s_hover, &min, &max))
        return;

    vec2_t corners[4] = {
        (vec2_t){max.x, min.z},
        (vec2_t){min.x, min.z},
        (vec2_t){min.x, max.z},
        (vec2_t){max.x, max.z},
    };
    R_PushCmd((struct rcmd){
        .func = R_GL_DrawQuad,
        .nargs = 4,
        .args = {
            R_PushArg(corners, sizeof(corners)),
            R_PushArg(&width, sizeof(width)),
            R_PushArg(&color, sizeof(color)),
            (void*)G_GetPrevTickMap(),
        },
    });
}

static void on_render_3d(void *user, void *event)
{
    if(s_hover_set)
        render_hover();

    if(!s_render || s_count == 0)
        return;

    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    struct frustum frustum;
    Camera_MakeFrustum(G_GetActiveCamera(), &frustum);

    for(int r = 0; r < s_res.chunk_h; r++) {
    for(int c = 0; c < s_res.chunk_w; c++) {

        if(s_chunk_counts[r * s_res.chunk_w + c] == 0)
            continue;

        struct aabb chunk_aabb = (struct aabb){
            .x_max = s_map_pos.x - c * chunk_x_dim,
            .x_min = s_map_pos.x - (c + 1) * chunk_x_dim,
            .y_min = 0.0f,
            .y_max = MAX_HEIGHT_LEVEL * Y_COORDS_PER_TILE,
            .z_min = s_map_pos.z + r * chunk_z_dim,
            .z_max = s_map_pos.z + (r + 1) * chunk_z_dim,
        };
        if(!C_FrustumAABBIntersectionExact(&frustum, &chunk_aabb))
            continue;

        render_chunk(r, c);
    }}
}

static void retain_tileset(bool retain)
{
    if(s_tileset_retained == retain)
        return;
    for(int m = 0; s_has_tileset && m < ROAD_NUM_MASKS; m++) {
        for(int v = 0; v < s_tileset.nvariants[m]; v++) {
            if(retain) {
                M_Decal_RetainImage(s_tileset.paths[m][v]);
            }else{
                M_Decal_ReleaseImage(s_tileset.paths[m][v]);
            }
        }
    }
    s_tileset_retained = retain;
}

static void set_nav_road(struct road_desc rd, bool on)
{
    vec2_t min, max;
    G_Roads_Bounds(rd, &min, &max);
    M_NavSetRoad(s_map, min, max, on);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Roads_Init(const struct map *map)
{
    struct map_resolution res;
    M_GetResolution(map, &res);

    const int rows = res.chunk_h * ROADS_PER_CHUNK_R;
    const int cols = res.chunk_w * ROADS_PER_CHUNK_C;
    const size_t nchunks = res.chunk_w * res.chunk_h;

    s_tiles = PF_CALLOC(rows * cols, sizeof(s_tiles[0]));
    s_decal_ids = PF_CALLOC(rows * cols, sizeof(s_decal_ids[0]));
    s_chunk_counts = PF_CALLOC(nchunks, sizeof(s_chunk_counts[0]));
    s_chunk_dirty = PF_CALLOC(nchunks, sizeof(s_chunk_dirty[0]));
    if(!s_tiles || !s_decal_ids || !s_chunk_counts || !s_chunk_dirty)
        goto fail;
    vec_road_idx_init(&s_stale);

    s_map = map;
    s_res = res;
    s_map_pos = M_GetPos(map);
    s_rows = rows;
    s_cols = cols;
    s_count = 0;
    s_hover_set = false;

    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, G_ALL);
    E_Global_Register(EVENT_UPDATE_START, on_update_start, NULL, G_ALL);
    if(s_has_tileset)
        retain_tileset(true);
    return true;

fail:
    PF_FREE(s_tiles);
    PF_FREE(s_decal_ids);
    PF_FREE(s_chunk_counts);
    PF_FREE(s_chunk_dirty);
    return false;
}

void G_Roads_Shutdown(void)
{
    if(!s_map)
        return;

    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    E_Global_Unregister(EVENT_UPDATE_START, on_update_start);
    retain_tileset(false);

    vec_road_idx_destroy(&s_stale);
    PF_FREE(s_decal_ids);
    PF_FREE(s_tiles);
    PF_FREE(s_chunk_counts);
    PF_FREE(s_chunk_dirty);

    s_map = NULL;
    s_rows = s_cols = 0;
    s_count = 0;
    s_hover_set = false;
}

void G_Roads_Clear(void)
{
    if(!s_map)
        return;

    const size_t nchunks = s_res.chunk_w * s_res.chunk_h;
    for(int i = 0; i < nchunks; i++) {
        if(s_chunk_counts[i] > 0)
            s_chunk_dirty[i] = true;
    }

    for(int i = 0; i < s_rows * s_cols; i++) {
        if(s_decal_ids[i] != NULL_DECAL_ID)
            M_Decal_RemoveInternal(s_decal_ids[i]);
        if(s_tiles[i] & ROAD_PRESENT)
            set_nav_road((struct road_desc){i / s_cols, i % s_cols}, false);
    }
    memset(s_decal_ids, 0, s_rows * s_cols * sizeof(s_decal_ids[0]));
    vec_road_idx_reset(&s_stale);

    memset(s_tiles, 0, s_rows * s_cols * sizeof(s_tiles[0]));
    memset(s_chunk_counts, 0, nchunks * sizeof(s_chunk_counts[0]));
    s_count = 0;
}

bool G_Roads_DescForPoint(vec2_t xz, struct road_desc *out)
{
    if(!s_map)
        return false;

    struct tile_desc td;
    if(!M_Tile_DescForPoint2D(s_res, s_map_pos, xz, &td))
        return false;
    return G_Roads_DescForTile(&td, out);
}

bool G_Roads_DescForTile(const struct tile_desc *td, struct road_desc *out)
{
    if(!s_map)
        return false;

    struct road_desc rd = (struct road_desc){
        (td->chunk_r * TILES_PER_CHUNK_HEIGHT + td->tile_r) / ROAD_TILE_SPAN,
        (td->chunk_c * TILES_PER_CHUNK_WIDTH  + td->tile_c) / ROAD_TILE_SPAN,
    };
    if(td->tile_r < 0 || td->tile_c < 0 || !in_bounds(rd))
        return false;

    *out = rd;
    return true;
}

bool G_Roads_Bounds(struct road_desc rd, vec2_t *out_min, vec2_t *out_max)
{
    if(!s_map || !in_bounds(rd))
        return false;

    out_max->x = s_map_pos.x - rd.c * ROAD_TILE_X_DIM;
    out_min->x = out_max->x - ROAD_TILE_X_DIM;
    out_min->z = s_map_pos.z + rd.r * ROAD_TILE_Z_DIM;
    out_max->z = out_min->z + ROAD_TILE_Z_DIM;
    return true;
}

bool G_Roads_Add(struct road_desc rd)
{
    if(!s_map || !in_bounds(rd))
        return false;
    if(present(rd))
        return true;

    *tile_at(rd) = (*tile_at(rd) & ROAD_DECAL_STALE) | ROAD_PRESENT;
    set_nav_road(rd, true);
    s_chunk_counts[chunk_index(rd)]++;
    s_chunk_dirty[chunk_index(rd)] = true;
    s_count++;
    mark_stale(rd);
    return true;
}

bool G_Roads_Remove(struct road_desc rd)
{
    if(!present(rd))
        return false;

    uint8_t links = *tile_at(rd);
    for(uint8_t link = ROAD_LINK_UP; link & ROAD_LINK_ALL; link <<= 1) {
        if(links & link)
            set_link(rd, neighbour(rd, link), false);
    }

    *tile_at(rd) &= ROAD_DECAL_STALE;
    set_nav_road(rd, false);
    s_chunk_counts[chunk_index(rd)]--;
    s_chunk_dirty[chunk_index(rd)] = true;
    s_count--;
    mark_stale(rd);
    return true;
}

bool G_Roads_Link(struct road_desc a, struct road_desc b)
{
    if(!present(a) || !present(b) || !link_between(a, b))
        return false;

    set_link(a, b, true);
    return true;
}

bool G_Roads_Unlink(struct road_desc a, struct road_desc b)
{
    if(!present(a) || !present(b) || !link_between(a, b))
        return false;

    set_link(a, b, false);
    return true;
}

bool G_Roads_Get(struct road_desc rd, uint8_t *out_links)
{
    if(!present(rd))
        return false;

    *out_links = *tile_at(rd) & ROAD_LINK_ALL;
    return true;
}

size_t G_Roads_Count(void)
{
    return s_count;
}

size_t G_Roads_GetAll(size_t maxout, struct road_desc out[], uint8_t out_links[])
{
    size_t ret = 0;
    for(int r = 0; r < s_rows; r++) {
    for(int c = 0; c < s_cols; c++) {

        if(ret == maxout)
            return ret;

        struct road_desc rd = (struct road_desc){r, c};
        if(!present(rd))
            continue;

        out[ret] = rd;
        out_links[ret] = *tile_at(rd) & ROAD_LINK_ALL;
        ret++;
    }}
    return ret;
}

bool G_Roads_Nearest(vec2_t xz, float max_dist, struct road_desc *out)
{
    struct road_desc centre;
    if(!G_Roads_DescForPoint(xz, &centre)) {
        /* Off the map's edge: start from the nearest tile on it */
        vec2_t clamped = xz;
        clamped.x = CLAMP(clamped.x, s_map_pos.x - s_cols * ROAD_TILE_X_DIM + 1.0f, s_map_pos.x - 1.0f);
        clamped.z = CLAMP(clamped.z, s_map_pos.z + 1.0f, s_map_pos.z + s_rows * ROAD_TILE_Z_DIM - 1.0f);
        if(!G_Roads_DescForPoint(clamped, &centre))
            return false;
    }

    const int max_ring = (int)(max_dist / MIN(ROAD_TILE_X_DIM, ROAD_TILE_Z_DIM)) + 1;
    float best = INFINITY;
    for(int ring = 0; ring <= max_ring; ring++) {
        /* A tile one ring further out can not be nearer than this */
        if(ring > 0 && best < (ring - 1) * MIN(ROAD_TILE_X_DIM, ROAD_TILE_Z_DIM))
            break;
        for(int dr = -ring; dr <= ring; dr++) {
        for(int dc = -ring; dc <= ring; dc++) {

            if(abs(dr) != ring && abs(dc) != ring)
                continue;
            struct road_desc rd = (struct road_desc){centre.r + dr, centre.c + dc};
            if(!present(rd))
                continue;
            vec2_t min, max;
            G_Roads_Bounds(rd, &min, &max);
            float dx = MAX(MAX(min.x - xz.x, 0.0f), xz.x - max.x);
            float dz = MAX(MAX(min.z - xz.z, 0.0f), xz.z - max.z);
            float dist = sqrtf(dx * dx + dz * dz);
            if(dist < best && dist <= max_dist) {
                best = dist;
                *out = rd;
            }
        }}
    }
    return isfinite(best);
}

vec2_t G_Roads_Centre(struct road_desc rd)
{
    vec2_t min, max;
    G_Roads_Bounds(rd, &min, &max);
    return (vec2_t){(min.x + max.x) / 2.0f, (min.z + max.z) / 2.0f};
}

size_t G_Roads_Route(struct road_desc from, struct road_desc to, size_t maxout, 
                     struct road_desc out[])
{
    if(!present(from) || !present(to) || maxout == 0)
        return 0;

    const int ntiles = s_rows * s_cols;
    int *came_from = PF_MALLOC(ntiles * sizeof(int));
    float *cost = PF_MALLOC(ntiles * sizeof(float));
    if(!came_from || !cost) {
        PF_FREE(came_from);
        PF_FREE(cost);
        return 0;
    }
    for(int i = 0; i < ntiles; i++) {
        came_from[i] = -1;
        cost[i] = INFINITY;
    }

    pq(road) frontier;
    pq_road_init(&frontier);
    const int start = from.r * s_cols + from.c;
    const int goal = to.r * s_cols + to.c;
    cost[start] = 0.0f;
    came_from[start] = start;
    pq_road_push(&frontier, 0.0f, start);

    while(pq_size(&frontier) > 0) {

        int curr;
        pq_road_pop(&frontier, &curr);
        if(curr == goal)
            break;

        struct road_desc rd = (struct road_desc){curr / s_cols, curr % s_cols};
        uint8_t links = s_tiles[curr] & ROAD_LINK_ALL;

        for(uint8_t link = ROAD_LINK_UP; link & ROAD_LINK_ALL; link <<= 1) {

            if(!(links & link))
                continue;
            struct road_desc nb = neighbour(rd, link);
            int next = nb.r * s_cols + nb.c;
            float next_cost = cost[curr] + 1.0f;
            if(next_cost >= cost[next])
                continue;
            cost[next] = next_cost;
            came_from[next] = curr;
            float h = abs(nb.r - to.r) + abs(nb.c - to.c);
            pq_road_push(&frontier, next_cost + h, next);
        }
    }
    pq_road_destroy(&frontier);

    size_t ret = 0;
    if(came_from[goal] >= 0) {
        size_t len = 1;
        for(int i = goal; i != start; i = came_from[i])
            len++;
        if(len <= maxout) {
            ret = len;
            int i = goal;
            for(size_t j = len; j-- > 0; i = came_from[i])
                out[j] = (struct road_desc){i / s_cols, i % s_cols};
        }
    }

    PF_FREE(came_from);
    PF_FREE(cost);
    return ret;
}

void G_Roads_SetHover(const struct road_desc *rd)
{
    s_hover_set = (rd != NULL);
    if(rd) {
        s_hover = *rd;
    }
}

void G_Roads_SetRender(bool on)
{
    s_render = on;
}

bool G_Roads_GetRender(void)
{
    return s_render;
}

bool G_Roads_SetTileset(const struct road_tileset *tileset)
{
    if(tileset) {
        for(int m = 0; m < ROAD_NUM_MASKS; m++) {
            if(tileset->nvariants[m] < 1 || tileset->nvariants[m] > MAX_ROAD_VARIANTS)
                return false;
            for(int v = 0; v < tileset->nvariants[m]; v++) {
                int w, h;
                if(!M_Decal_ImageSize(tileset->paths[m][v], &w, &h))
                    return false;
            }
        }
    }
    if(s_map)
        retain_tileset(false);
    if(tileset)
        s_tileset = *tileset;
    s_has_tileset = (tileset != NULL);

    if(s_map) {
        if(s_has_tileset)
            retain_tileset(true);
        mark_all_stale();
    }
    return true;
}

bool G_Roads_GetTileset(struct road_tileset *out)
{
    if(!s_has_tileset)
        return false;
    *out = s_tileset;
    return true;
}

bool G_Roads_SaveState(struct SDL_RWops *stream)
{
    struct attr has_tileset = (struct attr){
        .type = TYPE_BOOL,
        .val.as_bool = s_has_tileset
    };
    CHK_TRUE_RET(Attr_Write(stream, &has_tileset, "has_tileset"));

    for(int m = 0; s_has_tileset && m < ROAD_NUM_MASKS; m++) {

        struct attr nvariants = (struct attr){
            .type = TYPE_INT,
            .val.as_int = s_tileset.nvariants[m]
        };
        CHK_TRUE_RET(Attr_Write(stream, &nvariants, "nvariants"));

        for(int v = 0; v < s_tileset.nvariants[m]; v++) {
            struct attr path = (struct attr){ .type = TYPE_STRING };
            pf_strlcpy(path.val.as_string, s_tileset.paths[m][v], sizeof(path.val.as_string));
            CHK_TRUE_RET(Attr_Write(stream, &path, "path"));
        }
    }

    struct attr num_roads = (struct attr){
        .type = TYPE_INT,
        .val.as_int = s_count
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_roads, "num_roads"));

    for(int r = 0; r < s_rows; r++) {
    for(int c = 0; c < s_cols; c++) {

        struct road_desc rd = (struct road_desc){r, c};
        if(!present(rd))
            continue;

        struct attr row = (struct attr){
            .type = TYPE_INT,
            .val.as_int = r
        };
        CHK_TRUE_RET(Attr_Write(stream, &row, "row"));

        struct attr col = (struct attr){
            .type = TYPE_INT,
            .val.as_int = c
        };
        CHK_TRUE_RET(Attr_Write(stream, &col, "col"));

        struct attr links = (struct attr){
            .type = TYPE_INT,
            .val.as_int = *tile_at(rd) & ROAD_LINK_ALL
        };
        CHK_TRUE_RET(Attr_Write(stream, &links, "links"));
    }}
    return true;
}

bool G_Roads_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;
    G_Roads_Clear();

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_BOOL);
    const bool has_tileset = attr.val.as_bool;

    static struct road_tileset s_loaded;
    struct road_tileset *tileset = &s_loaded;
    for(int m = 0; has_tileset && m < ROAD_NUM_MASKS; m++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        CHK_TRUE_RET(attr.val.as_int >= 1 && attr.val.as_int <= MAX_ROAD_VARIANTS);
        tileset->nvariants[m] = attr.val.as_int;

        for(int v = 0; v < tileset->nvariants[m]; v++) {
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_STRING);
            pf_strlcpy(tileset->paths[m][v], attr.val.as_string, sizeof(tileset->paths[m][v]));
        }
    }

    /* Images missing since the save leave the roads undrawn */
    if(!G_Roads_SetTileset(has_tileset ? tileset : NULL)) {
        G_Roads_SetTileset(NULL);
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_roads = attr.val.as_int;

    for(int i = 0; i < num_roads; i++) {

        struct road_desc rd;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        rd.r = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        rd.c = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int links = attr.val.as_int;

        CHK_TRUE_RET(in_bounds(rd));
        CHK_TRUE_RET((links & ~ROAD_LINK_ALL) == 0);

        G_Roads_Add(rd);
        *tile_at(rd) |= links;
    }

    drop_one_sided_links();
    mark_all_stale();
    return true;
}
