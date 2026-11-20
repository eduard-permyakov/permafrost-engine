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
#include "../camera/public/camera.h"
#include "../entity.h"
#include "../phys/public/collision.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/attr.h"
#include "../lib/public/vec.h"
#include "../lib/public/pf_string.h"
#include "../pf_math.h"

#include <assert.h>
#include <string.h>
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

#define ROAD_PRESENT        (1 << 7)
#define ROAD_DECAL_STALE    (1 << 6)
#define ROAD_PLANK          (1 << 4)
#define ROAD_NUM_MASKS      (ROAD_LINK_ALL + 1)
#define ROAD_TILE_X_DIM     (ROAD_TILE_SPAN * X_COORDS_PER_TILE)
#define ROAD_TILE_Z_DIM     (ROAD_TILE_SPAN * Z_COORDS_PER_TILE)
#define ROADS_PER_CHUNK_R   (TILES_PER_CHUNK_HEIGHT / ROAD_TILE_SPAN)
#define ROADS_PER_CHUNK_C   (TILES_PER_CHUNK_WIDTH / ROAD_TILE_SPAN)
#define OVERLAY_MAX_QUADS   (ROADS_PER_CHUNK_R * ROADS_PER_CHUNK_C * 5)
#define REFUSED_BATCH       (OVERLAY_MAX_QUADS / (ROAD_TILE_SPAN * ROAD_TILE_SPAN))
#define GRID_CELLS          (8)
#define GRID_LINES          (2 * GRID_CELLS + 2)
#define GRID_SAMPLES        ((2 * GRID_CELLS + 1) * ROAD_TILE_SPAN)
#define GRID_MAX_VERTS      (2 * GRID_LINES * GRID_SAMPLES * 2)
#define GRID_LIFT           (0.1f)
#define GRID_ALPHA          (0.5f)
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define ARR_SIZE(a)         (sizeof(a)/sizeof(a[0]))

_Static_assert(TILES_PER_CHUNK_WIDTH % ROAD_TILE_SPAN == 0, "Road tiles must not cross chunks");
_Static_assert(TILES_PER_CHUNK_HEIGHT % ROAD_TILE_SPAN == 0, "Road tiles must not cross chunks");

VEC_TYPE(road_idx, int)
VEC_IMPL(static inline, road_idx, int)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map    *s_map;
static struct map_resolution s_res;
static vec3_t               s_map_pos;
static int                  s_rows, s_cols;
static size_t               s_count;
/* The speed bonuses of dirt and plank roads, which the scripts may raise */
static float                s_dirt_bonus = ROAD_SPEED_BONUS;
static float                s_plank_bonus = ROAD_PLANK_SPEED_BONUS;

/* One byte per road tile: ROAD_PRESENT, ROAD_DECAL_STALE, ROAD_PLANK and the
 * road_link bits. A link is always set on both of the tiles it joins.
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
/* The tiles drawn in red, as no road can be laid on them */
static vec_road_idx_t       s_refused;

/* The lines of the road tile lattice about the cursor, or about a fixed
 * point, which the render thread keeps until they are laid out again
 */
static bool                 s_grid_on;
static bool                 s_grid_fixed;
static vec2_t               s_grid_fixed_xz;
static bool                 s_grid_built;
static struct road_desc     s_grid_cell;
static struct overlay_vert  s_grid_verts[GRID_MAX_VERTS];

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

static void tile_bounds(struct road_desc rd, vec2_t *out_min, vec2_t *out_max)
{
    out_max->x = s_map_pos.x - rd.c * ROAD_TILE_X_DIM;
    out_min->x = out_max->x - ROAD_TILE_X_DIM;
    out_min->z = s_map_pos.z + rd.r * ROAD_TILE_Z_DIM;
    out_max->z = out_min->z + ROAD_TILE_Z_DIM;
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
    tile_bounds(rd, &min, &max);

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
        tile_bounds(rd, &min, &max);

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

/* One square per map tile, as a single quad would not lie on the terrain 
 * over a cliff
 */
static void render_refused(int first, int ntiles)
{
    const vec3_t color = (vec3_t){1.0f, 0.0f, 0.0f};
    size_t count = ntiles * ROAD_TILE_SPAN * ROAD_TILE_SPAN;

    STALLOC(vec2_t, corners_buff, 4 * count);
    STALLOC(vec3_t, colors_buff, count);

    vec2_t *corners = corners_buff;
    vec3_t *colors = colors_buff;

    for(int i = first; i < first + ntiles; i++) {
        int idx = vec_AT(&s_refused, i);
        vec2_t min, max;
        tile_bounds((struct road_desc){idx / s_cols, idx % s_cols}, &min, &max);

        for(int r = 0; r < ROAD_TILE_SPAN; r++) {
        for(int c = 0; c < ROAD_TILE_SPAN; c++) {
            vec2_t tile_min = (vec2_t){min.x + c * X_COORDS_PER_TILE, min.z + r * Z_COORDS_PER_TILE};
            vec2_t tile_max = (vec2_t){tile_min.x + X_COORDS_PER_TILE, tile_min.z + Z_COORDS_PER_TILE};
            push_rect(&corners, &colors, tile_min, tile_max, color);
        }}
    }

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

/* The lines fade out toward the edge of the grid, so that it has none */
static struct overlay_vert grid_vert(float x, float z, vec2_t centre)
{
    const float radius = (GRID_CELLS + 0.5f) * ROAD_TILE_X_DIM;
    float dist = sqrtf((x - centre.x) * (x - centre.x) + (z - centre.z) * (z - centre.z));
    float alpha = GRID_ALPHA * MAX(1.0f - dist / radius, 0.0f);
    float y = MAX(M_HeightAtPoint(s_map, (vec2_t){x, z}), 0.0f) + GRID_LIFT;
    return (struct overlay_vert){(vec3_t){x, y, z}, (vec4_t){1.0f, 1.0f, 1.0f, alpha}};
}

/* Samples every map tile along the lines so they follow the ground */
static size_t build_grid(struct road_desc centre)
{
    vec2_t min, max;
    tile_bounds(centre, &min, &max);
    vec2_t middle = (vec2_t){(min.x + max.x) / 2.0f, (min.z + max.z) / 2.0f};

    struct overlay_vert *out = s_grid_verts;
    for(int line = 0; line < GRID_LINES; line++) {
    for(int dir = 0; dir < 2; dir++) {

        int edge = (dir ? centre.r : centre.c) - GRID_CELLS + line;
        if(edge < 0 || edge > (dir ? s_rows : s_cols))
            continue;
        int first = ((dir ? centre.c : centre.r) - GRID_CELLS) * ROAD_TILE_SPAN;

        for(int i = 0; i < GRID_SAMPLES; i++) {
            int from = first + i;
            if(from < 0 || from + 1 > (dir ? s_cols : s_rows) * ROAD_TILE_SPAN)
                continue;
            for(int end = 0; end < 2; end++) {
                int step = from + end;
                *out++ = dir ? grid_vert(s_map_pos.x - step * X_COORDS_PER_TILE,
                                         s_map_pos.z + edge * ROAD_TILE_Z_DIM, middle)
                             : grid_vert(s_map_pos.x - edge * ROAD_TILE_X_DIM,
                                         s_map_pos.z + step * Z_COORDS_PER_TILE, middle);
            }
        }
    }}
    return out - s_grid_verts;
}

static bool grid_centre_cell(struct road_desc *out)
{
    vec3_t pos;
    if(s_grid_fixed)
        return G_Roads_DescForPoint(s_grid_fixed_xz, out);
    return M_Raycast_MouseIntersecCoord(&pos) && G_Roads_DescForPoint((vec2_t){pos.x, pos.z}, out);
}

static void render_placement_grid(void)
{
    struct road_desc cell;
    if(grid_centre_cell(&cell)
    && (!s_grid_built || cell.r != s_grid_cell.r || cell.c != s_grid_cell.c)) {

        int slot = MAP_OVERLAY_MESH_GRID;
        size_t ntri_verts = 0;
        size_t nline_verts = build_grid(cell);
        R_PushCmd((struct rcmd){
            .func = R_GL_MapOverlayMeshSet,
            .nargs = 4,
            .args = {
                R_PushArg(&slot, sizeof(slot)),
                R_PushArg(s_grid_verts, nline_verts * sizeof(struct overlay_vert)),
                R_PushArg(&ntri_verts, sizeof(ntri_verts)),
                R_PushArg(&nline_verts, sizeof(nline_verts)),
            },
        });
        s_grid_cell = cell;
        s_grid_built = true;
    }
    if(!s_grid_built)
        return;

    int slot = MAP_OVERLAY_MESH_GRID;
    R_PushCmd((struct rcmd){
        .func = R_GL_MapOverlayMeshDraw,
        .nargs = 1,
        .args = { R_PushArg(&slot, sizeof(slot)) },
    });
}

static void on_render_3d(void *user, void *event)
{
    if(s_grid_on)
        render_placement_grid();

    if(s_hover_set)
        render_hover();

    for(int i = 0; i < vec_size(&s_refused); i += REFUSED_BATCH) {
        render_refused(i, MIN(REFUSED_BATCH, vec_size(&s_refused) - i));
    }

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
    tile_bounds(rd, &min, &max);
    M_NavSetRoad(s_map, min, max, on);
}

/* The footprint of a road site standing on the tile, kept just inside its
 * edges, as a cell on the edge would be taken from the neighbouring tile
 */
static void site_obb(struct road_desc rd, struct obb *out)
{
    const float inset = 0.05f;

    vec2_t min, max;
    tile_bounds(rd, &min, &max);
    vec2_t center = (vec2_t){(min.x + max.x) / 2.0f, (min.z + max.z) / 2.0f};
    vec3_t pos = (vec3_t){center.x, M_HeightAtPoint(s_map, center), center.z};

    struct aabb aabb = (struct aabb){
        .x_min = -ROAD_TILE_X_DIM / 2.0f + inset,
        .x_max =  ROAD_TILE_X_DIM / 2.0f - inset,
        .y_min = 0.0f,
        .y_max = 0.0f,
        .z_min = -ROAD_TILE_Z_DIM / 2.0f + inset,
        .z_max =  ROAD_TILE_Z_DIM / 2.0f - inset,
    };
    vec3_t scale = (vec3_t){1.0f, 1.0f, 1.0f};
    mat4x4_t model;
    Entity_ModelMatrixFrom(pos, (quat_t){0.0f, 0.0f, 0.0f, 1.0f}, scale, &model);
    Entity_CurrentOBBFrom(&aabb, model, scale, out);
}

static bool obb_overlaps_tile(const struct obb *obb, vec2_t min, vec2_t max)
{
    vec2_t centre = (vec2_t){obb->center.x, obb->center.z};
    vec2_t tile_centre = (vec2_t){(min.x + max.x) / 2.0f, (min.z + max.z) / 2.0f};
    vec2_t tile_half = (vec2_t){(max.x - min.x) / 2.0f, (max.z - min.z) / 2.0f};
    vec2_t rel = (vec2_t){centre.x - tile_centre.x, centre.z - tile_centre.z};

    vec2_t axes[5] = {{1.0f, 0.0f}, {0.0f, 1.0f}};
    int naxes = 2;
    for(int i = 0; i < 3; i++) {
        vec2_t axis = (vec2_t){obb->axes[i].x, obb->axes[i].z};
        float len = PFM_Vec2_Len(&axis);
        if(len < 1e-3f)
            continue;
        PFM_Vec2_Scale(&axis, 1.0f / len, &axes[naxes++]);
    }

    for(int i = 0; i < naxes; i++) {
        float tile_radius = tile_half.x * fabsf(axes[i].x) + tile_half.z * fabsf(axes[i].z);
        float obb_radius = 0.0f;
        for(int j = 0; j < 3; j++) {
            vec2_t axis = (vec2_t){obb->axes[j].x, obb->axes[j].z};
            obb_radius += obb->half_lengths[j] * fabsf(PFM_Vec2_Dot(&axis, &axes[i]));
        }
        if(fabsf(PFM_Vec2_Dot(&rel, &axes[i])) > tile_radius + obb_radius)
            return false;
    }
    return true;
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
    vec_road_idx_init(&s_refused);

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
    vec_road_idx_destroy(&s_refused);
    PF_FREE(s_decal_ids);
    PF_FREE(s_tiles);
    PF_FREE(s_chunk_counts);
    PF_FREE(s_chunk_dirty);

    s_map = NULL;
    s_rows = s_cols = 0;
    s_count = 0;
    s_hover_set = false;
    s_grid_built = false;
}

void G_Roads_Clear(void)
{
    s_dirt_bonus = ROAD_SPEED_BONUS;
    s_plank_bonus = ROAD_PLANK_SPEED_BONUS;
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
    vec_road_idx_reset(&s_refused);

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

    tile_bounds(rd, out_min, out_max);
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

bool G_Roads_SetKind(struct road_desc rd, enum road_kind kind)
{
    if(!present(rd))
        return false;

    if(kind == ROAD_KIND_PLANK) {
        *tile_at(rd) |= ROAD_PLANK;
    }else{
        *tile_at(rd) &= ~ROAD_PLANK;
    }
    return true;
}

bool G_Roads_GetKind(struct road_desc rd, enum road_kind *out)
{
    if(!present(rd))
        return false;

    *out = (*tile_at(rd) & ROAD_PLANK) ? ROAD_KIND_PLANK : ROAD_KIND_DIRT;
    return true;
}

float G_Roads_SpeedFactor(vec2_t xz)
{
    if(!s_map)
        return 1.0f;

    int c = (int)floorf((s_map_pos.x - xz.x) / ROAD_TILE_X_DIM);
    int r = (int)floorf((xz.z - s_map_pos.z) / ROAD_TILE_Z_DIM);
    struct road_desc rd = (struct road_desc){r, c};
    if(!present(rd))
        return 1.0f;
    return (*tile_at(rd) & ROAD_PLANK) ? s_plank_bonus : s_dirt_bonus;
}

float G_Roads_SpeedBonus(bool plank)
{
    return plank ? s_plank_bonus : s_dirt_bonus;
}

void G_Roads_SetSpeedBonus(float dirt, float plank)
{
    s_dirt_bonus = MAX(dirt, 1.0f);
    s_plank_bonus = MAX(plank, 1.0f);
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

void G_Roads_SetHover(const struct road_desc *rd)
{
    s_hover_set = (rd != NULL);
    if(rd) {
        s_hover = *rd;
    }
}

bool G_Roads_Buildable(struct road_desc rd, int faction_id)
{
    if(!s_map || !in_bounds(rd))
        return false;

    struct obb obb;
    site_obb(rd, &obb);
    return M_NavObjectPermanentlyBuildable(s_map, NAV_LAYER_GROUND_1X1, &obb);
}

size_t G_Roads_TilesUnder(uint32_t uid, size_t maxout, struct road_desc out[])
{
    if(!s_map || G_EntityIsZombie(uid) || !(G_FlagsGet(uid) & ENTITY_FLAG_BUILDING)
    || G_Building_GetPathable(uid))
        return 0;

    struct obb obb;
    Entity_CurrentOBB(uid, &obb, true);

    float extent_x = 0.0f, extent_z = 0.0f;
    for(int i = 0; i < 3; i++) {
        extent_x += fabsf(obb.axes[i].x) * obb.half_lengths[i];
        extent_z += fabsf(obb.axes[i].z) * obb.half_lengths[i];
    }
    int c_min = (int)floorf((s_map_pos.x - (obb.center.x + extent_x)) / ROAD_TILE_X_DIM);
    int c_max = (int)floorf((s_map_pos.x - (obb.center.x - extent_x)) / ROAD_TILE_X_DIM);
    int r_min = (int)floorf(((obb.center.z - extent_z) - s_map_pos.z) / ROAD_TILE_Z_DIM);
    int r_max = (int)floorf(((obb.center.z + extent_z) - s_map_pos.z) / ROAD_TILE_Z_DIM);

    size_t ret = 0;
    for(int r = r_min; r <= r_max; r++) {
    for(int c = c_min; c <= c_max; c++) {

        struct road_desc rd = (struct road_desc){r, c};
        vec2_t min, max;
        if(ret == maxout || !G_Roads_Bounds(rd, &min, &max))
            continue;
        if(obb_overlaps_tile(&obb, min, max))
            out[ret++] = rd;
    }}
    return ret;
}

void G_Roads_SetRefused(size_t ntiles, const struct road_desc tiles[])
{
    if(!s_map)
        return;

    vec_road_idx_reset(&s_refused);
    for(int i = 0; i < ntiles; i++) {
        if(!in_bounds(tiles[i]))
            continue;
        if(!vec_road_idx_push(&s_refused, tiles[i].r * s_cols + tiles[i].c))
            return;
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

void G_Roads_SetPlacementGrid(bool on, const vec2_t *centre)
{
    s_grid_on = on;
    s_grid_fixed = (on && centre);
    if(s_grid_fixed)
        s_grid_fixed_xz = *centre;
    s_grid_built = false;
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
            .val.as_int = *tile_at(rd) & (ROAD_LINK_ALL | ROAD_PLANK)
        };
        CHK_TRUE_RET(Attr_Write(stream, &links, "links"));
    }}

    struct attr dirt_bonus = (struct attr){
        .type = TYPE_FLOAT,
        .val.as_float = s_dirt_bonus
    };
    CHK_TRUE_RET(Attr_Write(stream, &dirt_bonus, "dirt_bonus"));

    struct attr plank_bonus = (struct attr){
        .type = TYPE_FLOAT,
        .val.as_float = s_plank_bonus
    };
    CHK_TRUE_RET(Attr_Write(stream, &plank_bonus, "plank_bonus"));
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
        CHK_TRUE_RET((links & ~(ROAD_LINK_ALL | ROAD_PLANK)) == 0);

        G_Roads_Add(rd);
        *tile_at(rd) |= links;
    }

    if(Attr_NextKeyIs(stream, "dirt_bonus")) {
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        s_dirt_bonus = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        s_plank_bonus = attr.val.as_float;
    }

    drop_one_sided_links();
    mark_all_stale();
    return true;
}
