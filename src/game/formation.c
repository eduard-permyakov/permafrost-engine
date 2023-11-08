/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2023 Eduard Permyakov 
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

#include "formation.h"
#include "position.h"
#include "../main.h"
#include "../event.h"
#include "../settings.h"
#include "../perf.h"
#include "../camera.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/vec.h"
#include "../lib/public/khash.h"
#include "../lib/public/queue.h"
#include "../lib/public/pf_string.h"
#include "../navigation/public/nav.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"

#include <assert.h>

#define COLUMN_WIDTH_RATIO       (0.25f)
#define RANK_WIDTH_RATIO         (4.0f)
#define OCCUPIED_FIELD_RES       (95) /* Must be odd */
#define CELL_IDX(_r, _c, _ncols) ((_r) * (_ncols) + (_c))
#define ARR_SIZE(a)              (sizeof(a)/sizeof(a[0]))
#define MIN(a, b)                ((a) < (b) ? (a) : (b))
#define MAX(a, b)                ((a) > (b) ? (a) : (b))
#define CLAMP(a, min, max)       (MIN(MAX((a), (min)), (max)))

enum cell_state
{
    CELL_NOT_PLACED,
    CELL_VISITED,
    CELL_OCCUPIED,
    CELL_NOT_OCCUPIED
};

enum tile_state
{
    TILE_FREE,
    TILE_BLOCKED,
    TILE_ALLOCATED
};

enum direction{
    DIR_TOP = (1 << 0),
    DIR_BOT = (1 << 1),
    DIR_LEFT = (1 << 2),
    DIR_RIGHT = (1 << 3)
};

struct coord
{
    int r, c;
};

struct cell
{
    enum cell_state state;
    vec2_t          pos;
};

struct range2d{
    int min_r, max_r;
    int min_c, max_c;
};

VEC_TYPE(cell, struct cell)
VEC_IMPL(static inline, cell, struct cell)

KHASH_MAP_INIT_INT(assignment, struct coord)

QUEUE_TYPE(coord, struct coord)
QUEUE_IMPL(static, coord, struct coord)

enum formation_type
{
    FORMATION_RANK,
    FORMATION_COLUMN
};

struct subformation{
    vec2_t     anchor;
    vec2_t     orientation;
    size_t     nrows;
    size_t     ncols;
    /* Each cell holds a single unit from the subformation
     */
    vec_cell_t cells;
    /* A mapping between entities and a cell within the formation 
     */
    khash_t(assignment) *assignment;
};

VEC_TYPE(subformation, struct subformation)
VEC_IMPL(static inline, subformation, struct subformation)

struct formation
{
    enum formation_type  type;
    vec2_t               target;
    vec2_t               orientation;
    khash_t(entity)     *ents;
    /* A mapping between entities and subformations 
     */
    khash_t(assignment) *sub_assignment;
    vec_subformation_t   subformations;
    /* The map tiles which have already been allocated to cells.
     * Centered at the target position.
     */
    uint8_t              occupied[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES];
    /* A copy of the navigation 'island' field for the area specified
     * by the 'occupied' field.
     */
    uint16_t             islands[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES];
};

KHASH_MAP_INIT_INT64(formation, struct formation)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map   *s_map;
static khash_t(formation) *s_formations;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static size_t ncols(enum formation_type type, size_t nunits)
{
    switch(type) {
    case FORMATION_RANK:
        return ceilf(sqrtf(nunits / RANK_WIDTH_RATIO));
    case FORMATION_COLUMN:
        return ceilf(sqrtf(nunits / COLUMN_WIDTH_RATIO));
    default: assert(0);
    }
}

static size_t nrows(enum formation_type type, size_t nunits)
{
    return ceilf(nunits / ncols(type, nunits));
}

static vec2_t compute_orientation(vec2_t target, khash_t(entity) *ents)
{
    uint32_t curr;
    vec2_t COM = (vec2_t){0.0f, 0.0f};
    kh_foreach_key(ents, curr, {
        vec2_t curr_pos = G_Pos_GetXZ(curr);
        PFM_Vec2_Add(&COM, &curr_pos, &COM);
    });
    size_t nents = kh_size(ents);
    PFM_Vec2_Scale(&COM, 1.0f / nents, &COM);

    vec2_t orientation;
    PFM_Vec2_Sub(&target, &COM, &orientation);
    PFM_Vec2_Normal(&orientation, &orientation);
    return orientation;
}

static bool try_allocate(struct coord *curr, float radius)
{
    return true;
}

static bool nearest_free_cell(struct coord *curr, struct coord *out, int direction_mask,
                              uint8_t occupied[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES],
                              uint16_t islands[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    return true;
}

static void place_cell(struct cell *curr, struct coord pos,
                       const struct cell *left, const struct cell *right,
                       const struct cell *top,  const struct cell *bot,
                       uint8_t occupied[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES],
                       uint16_t islands[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    curr->state = CELL_NOT_OCCUPIED;

    enum direction dir = 0;
    if(left)
        dir |= DIR_LEFT;
    if(right)
        dir |= DIR_RIGHT;
    if(top)
        dir |= DIR_TOP;
    if(bot)
        dir |= DIR_BOT;

    /* First find a "target" tile based on direction and the positions of existing cells */

    // TODO: do breadth-first traversal of the 'occupied' field
    // greedily place each cell
    // if we are not able to place a cell, mark that tile

    struct coord nearest;
    bool exists = nearest_free_cell(&pos, &nearest, dir, occupied, islands);
}

static void init_occupied_field(vec2_t target, 
                                uint8_t occupied[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    PERF_ENTER();

    struct map_resolution res;
    M_NavGetResolution(s_map, &res);
    vec3_t map_pos = M_GetPos(s_map);

    struct tile_desc center_tile;
    M_Tile_DescForPoint2D(res, map_pos, target, &center_tile);

    struct coord center_coord = (struct coord){
        OCCUPIED_FIELD_RES / 2,
        OCCUPIED_FIELD_RES / 2
    };

    memset(occupied, 0, OCCUPIED_FIELD_RES * OCCUPIED_FIELD_RES);
    for(int r = 0; r < OCCUPIED_FIELD_RES; r++) {
    for(int c = 0; c < OCCUPIED_FIELD_RES; c++) {

        int dr = center_coord.r - r;
        int dc = center_coord.c - c;
        struct tile_desc curr = center_tile;
        bool exists = M_Tile_RelativeDesc(res, &curr, dc, dr);
        if(!exists) {
            occupied[r][c] = TILE_BLOCKED;
            continue;
        }

        struct box bounds = M_Tile_Bounds(res, map_pos, curr);
        vec2_t center = (vec2_t){
            bounds.x - bounds.width / 2.0f,
            bounds.z + bounds.height / 2.0f
        };
        if(!M_NavPositionPathable(s_map, NAV_LAYER_GROUND_1X1, center)
        ||  M_NavPositionBlocked(s_map, NAV_LAYER_GROUND_1X1, center)) {
            occupied[r][c] = TILE_BLOCKED;
            continue;
        }
    }}

    PERF_RETURN_VOID();
}

static void init_islands_field(vec2_t target,
                               uint16_t islands[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    M_NavCopyIslandsFieldView(s_map, target, OCCUPIED_FIELD_RES, OCCUPIED_FIELD_RES,
        NAV_LAYER_GROUND_1X1, (uint16_t*)islands);
}

static void place_subformation(struct subformation *formation,
                               uint8_t occupied[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES],
                               uint16_t islands[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    PERF_ENTER();

    int nrows = formation->nrows;
    int ncols = formation->ncols;

    size_t total = formation->nrows * formation->ncols;
    vec_cell_init(&formation->cells);
    vec_cell_resize(&formation->cells, total);
    formation->cells.size = total;
    for(int r = 0; r < nrows; r++) {
    for(int c = 0; c < ncols; c++) {
        size_t idx = r * ncols + c;
        vec_AT(&formation->cells, idx) = (struct cell){CELL_NOT_PLACED};
    }}

    /* Position the cells on pathable and unobstructed terrain.
     */
    struct coord center = (struct coord){
        .r = nrows / 2,
        .c = ncols / 2
    };

    /* Start by placing the center-most cell and traverse the cell 
     * grid outwards in a breadth-first manner.
     */
    queue_coord_t frontier;
    queue_coord_init(&frontier, nrows * ncols);
    queue_coord_push(&frontier, &center);

    struct coord deltas[] = {
        { 0, -1},
        { 0, +1},
        {-1,  0},
        {+1,  0},
    };

    while(queue_size(frontier) > 0) {

        struct coord curr;
        queue_coord_pop(&frontier, &curr);
        struct cell *curr_cell = &vec_AT(&formation->cells, CELL_IDX(curr.r, curr.c, ncols));

        struct coord top = (struct coord){curr.r - 1, curr.c};
        struct coord bot = (struct coord){curr.r + 1, curr.c};
        struct coord left = (struct coord){curr.r, curr.c - 1};
        struct coord right = (struct coord){curr.r, curr.c + 1};

        struct cell *top_cell = (top.r >= 0) 
                              ? &vec_AT(&formation->cells, CELL_IDX(top.r, top.c, ncols)) 
                              : NULL;
        struct cell *bot_cell = (bot.r < ncols) 
                              ? &vec_AT(&formation->cells, CELL_IDX(bot.r, bot.c, ncols)) 
                              : NULL;
        struct cell *left_cell = (left.c >= 0) 
                               ? &vec_AT(&formation->cells, CELL_IDX(left.r, left.c, ncols))
                               : NULL;
        struct cell *right_cell = (right.c < nrows) 
                                ? &vec_AT(&formation->cells, CELL_IDX(right.r, right.c, ncols))
                                : NULL;

        place_cell(curr_cell, curr, left_cell, right_cell, top_cell, bot_cell, occupied, islands);

        if(left_cell && left_cell->state == CELL_NOT_PLACED)
            queue_coord_push(&frontier, &left);
        if(right_cell && right_cell->state == CELL_NOT_PLACED)
            queue_coord_push(&frontier, &right);
        if(top_cell && top_cell->state == CELL_NOT_PLACED)
            queue_coord_push(&frontier, &top);
        if(bot_cell && bot_cell->state == CELL_NOT_PLACED)
            queue_coord_push(&frontier, &bot);
    }

    queue_coord_destroy(&frontier);
    PERF_RETURN_VOID();
}

static void init_subformations(struct formation *formation)
{
    vec_subformation_init(&formation->subformations);
}

static void render_formations(void)
{
    struct formation formation;
    kh_foreach_value(s_formations, formation, {
        const float length = 15.0f;
        const float width = 1.5f;
        const vec3_t green = (vec3_t){0.0, 1.0, 0.0};
        vec2_t origin = formation.target;
        vec2_t delta, end;
        PFM_Vec2_Scale(&formation.orientation, length, &delta);
        PFM_Vec2_Add(&origin, &delta, &end);

        vec2_t endpoints[] = {origin, end};
        R_PushCmd((struct rcmd){
            .func = R_GL_DrawLine,
            .nargs = 4,
            .args = {
                R_PushArg(endpoints, sizeof(endpoints)),
                R_PushArg(&width, sizeof(width)),
                R_PushArg(&green, sizeof(green)),
                (void*)G_GetPrevTickMap()
            }
        });
    });
}

static bool chunks_compare(struct coord *a, struct coord *b)
{
    if(a->r > b->r)
        return true;
    if(a->c > b->c)
        return true;
    return false;
}

static void swap_corners(vec2_t *corners_buff, size_t a, size_t b)
{
    vec2_t tmp[4];
    memcpy(tmp, corners_buff + (a * 4), sizeof(tmp));
    memcpy(corners_buff + (a * 4), corners_buff + (b * 4), sizeof(tmp));
    memcpy(corners_buff + (b * 4), tmp, sizeof(tmp));
}

static void swap_colors(vec3_t *colors_buff, size_t a, size_t b)
{
    vec3_t tmp = colors_buff[a];
    colors_buff[a] = colors_buff[b];
    colors_buff[b] = tmp;
}

static void swap_chunks(struct coord *chunk_buff, size_t a, size_t b)
{
    struct coord tmp = chunk_buff[a];
    chunk_buff[a] = chunk_buff[b];
    chunk_buff[b] = tmp;
}

static size_t sort_by_chunk(size_t size, vec2_t *corners_buff, 
                            vec3_t *colors_buff, struct coord *chunk_buff)
{
    if(size == 0)
        return 0;

    int i = 1;
    while(i < size) {
        int j = i;
        while(j > 0 && chunks_compare(chunk_buff + j - 1, chunk_buff + j)) {

            swap_corners(corners_buff, j, j-1);
            swap_colors(colors_buff, j, j-1);
            swap_chunks(chunk_buff, j, j-1);

            j--;
        }
        i++;
    }

    size_t ret = 1;
    for(int i = 1; i < size; i++) {
        struct coord *a = chunk_buff + i;
        struct coord *b = chunk_buff + i - 1;
        if(a->r != b->r || a->c != b->c)
            ret++;
    }
    return ret;
}

static size_t next_chunk_range(size_t begin, size_t size, 
                               struct coord *chunk_buff, size_t *out_count)
{
    size_t count = 0;
    int i = begin;
    for(; i < size; i++) {
        struct coord *a = chunk_buff + i;
        struct coord *b = chunk_buff + i + 1;
        if(a->r != b->r || a->c != b->c)
            break;
        count++;
    }
    *out_count = count + 1;
    return i + 1;
}

static size_t chunks_for_field(vec2_t center, size_t maxout, 
                               struct coord *out_chunks, struct range2d *out_ranges)
{
    struct map_resolution res;
    M_NavGetResolution(s_map, &res);
    vec3_t map_pos = M_GetPos(s_map);

    struct tile_desc center_tile;
    M_Tile_DescForPoint2D(res, map_pos, center, &center_tile);

    int min_dr = -OCCUPIED_FIELD_RES / 2;
    int min_dc = -OCCUPIED_FIELD_RES / 2;
    struct tile_desc min_tile = center_tile;
    bool exists = M_Tile_RelativeDesc(res, &min_tile, min_dc, min_dr);
    if(!exists) {
        bool unclipped_r = M_Tile_RelativeDesc(res, &min_tile, 0, min_dr);
        if(unclipped_r) {
            min_tile = (struct tile_desc){
                min_tile.chunk_r, 0, 
                min_tile.tile_r,  0
            };
            goto done_min;
        }
        bool unclipped_c = M_Tile_RelativeDesc(res, &min_tile, min_dc, 0);
        if(unclipped_c) {
            min_tile = (struct tile_desc){
                0, min_tile.chunk_c,
                0, min_tile.tile_c
            };
            goto done_min;
        }
        min_tile = (struct tile_desc){
            0, 0,
            0, 0
        };
    }
done_min:;

    int max_dr = OCCUPIED_FIELD_RES / 2;
    int max_dc = OCCUPIED_FIELD_RES / 2;
    struct tile_desc max_tile = center_tile;
    exists = M_Tile_RelativeDesc(res, &max_tile, max_dr, max_dc);
    if(!exists) {
        bool unclipped_r = M_Tile_RelativeDesc(res, &max_tile, 0, max_dr);
        if(unclipped_r) {
            max_tile = (struct tile_desc){
                max_tile.chunk_r, res.chunk_w-1, 
                max_tile.tile_r,  res.tile_w-1 
            };
            goto done_max;
        }
        bool unclipped_c = M_Tile_RelativeDesc(res, &max_tile, max_dc, 0);
        if(unclipped_c) {
            max_tile = (struct tile_desc){
                res.chunk_h-1, max_tile.chunk_c,
                res.tile_h-1,  max_tile.tile_c
            };
            goto done_max;
        }
        max_tile = (struct tile_desc){
            res.chunk_h-1, res.chunk_w-1,
            res.tile_h-1,  res.tile_h-1
        };
    }
done_max:;

    size_t ret = 0;
    for(int r = min_tile.chunk_r; r <= max_tile.chunk_r; r++) {
    for(int c = min_tile.chunk_c; c <= max_tile.chunk_c; c++) {

        if(ret == maxout)
            break;

        struct coord curr = (struct coord){r, c};
        out_chunks[ret] = curr;

        struct range2d curr_range = (struct range2d){
            0, res.tile_w-1,
            0, res.tile_h-1
        };

        if(r == min_tile.chunk_r) {
            curr_range.min_r = min_tile.tile_r;
        }
        if(r == max_tile.chunk_r) {
            curr_range.max_r = max_tile.tile_r;
        }
        if(c == min_tile.chunk_c) {
            curr_range.min_c = min_tile.tile_c;
        }
        if(c == max_tile.chunk_c) {
            curr_range.max_c = max_tile.tile_c;
        }
        out_ranges[ret] = curr_range;
        ret++;
    }}
    return ret;
}

static void render_islands_field(void)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    struct map_resolution res;
    M_NavGetResolution(s_map, &res);
    struct camera *cam = G_GetActiveCamera();
    vec3_t map_pos = M_GetPos(s_map);

    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view); 
    Camera_MakeProjMat(cam, &proj);

    struct formation *formation;
    kh_foreach_ptr(s_formations, formation, {

        struct coord chunks[32];
        struct range2d ranges[32];
        size_t nchunks = chunks_for_field(formation->target, 32, chunks, ranges);

        struct tile_desc center_tile;
        M_Tile_DescForPoint2D(res, map_pos, formation->target, &center_tile);

        for(int i = 0; i < nchunks; i++) {
            struct coord *chunk = &chunks[i];
            struct range2d *range = &ranges[i];

            mat4x4_t chunk_model;
            M_ModelMatrixForChunk(s_map, (struct chunkpos){chunk->r, chunk->c}, &chunk_model);

            for(int r = range->min_r; r <= range->max_r; r++) {
            for(int c = range->min_c; c <= range->max_c; c++) {

                float square_x_len = (1.0f / res.tile_w) * chunk_x_dim;
                float square_z_len = (1.0f / res.tile_h) * chunk_z_dim;
                float square_x = CLAMP(-(((float)c) / res.tile_w) * chunk_x_dim, 
                                       -chunk_x_dim, chunk_x_dim);
                float square_z = CLAMP((((float)r) / res.tile_h) * chunk_z_dim, 
                                       -chunk_z_dim, chunk_z_dim);

                vec4_t center_homo = (vec4_t) {
                    square_x - square_x_len / 2.0,
                    0.0,
                    square_z + square_z_len / 2.0,
                    1.0
                };

                struct tile_desc curr = (struct tile_desc){
                    chunk->r, chunk->c,
                    r, c 
                };
                int dr, dc;
                M_Tile_Distance(res, &curr, &center_tile, &dr, &dc);

                int offset_r = (OCCUPIED_FIELD_RES / 2) + dr;
                int offset_c = (OCCUPIED_FIELD_RES / 2) + dc;
                assert(offset_r >= 0 && offset_r < OCCUPIED_FIELD_RES);
                assert(offset_c >= 0 && offset_c < OCCUPIED_FIELD_RES);
                uint16_t island_id = formation->islands[offset_r][offset_c];

                char text[8];
                pf_snprintf(text, sizeof(text), "%u", island_id);
                N_RenderOverlayText(text, center_homo, &chunk_model, &view, &proj);
            }}
        }
    });
}

static void render_formations_occupied_field(void)
{
    struct map_resolution res;
    M_NavGetResolution(s_map, &res);
    vec3_t map_pos = M_GetPos(s_map);

    struct formation *formation;
    kh_foreach_ptr(s_formations, formation, {

        struct tile_desc center_tile;
        M_Tile_DescForPoint2D(res, map_pos, formation->target, &center_tile);

        struct box center_bounds = M_Tile_Bounds(res, map_pos, center_tile);
        vec2_t center = (vec2_t){
            center_bounds.x - center_bounds.width / 2.0f,
            center_bounds.z + center_bounds.height / 2.0f
        };

        const float field_width = center_bounds.width * OCCUPIED_FIELD_RES;
        const float line_width = 1.0f;
        const vec3_t blue = (vec3_t){0.0f, 0.0f, 1.0f};

        vec2_t field_corners[4] = {
            (vec2_t){center.x + field_width/2.0f, center.z - field_width/2.0f},
            (vec2_t){center.x - field_width/2.0f, center.z - field_width/2.0f},
            (vec2_t){center.x - field_width/2.0f, center.z + field_width/2.0f},
            (vec2_t){center.x + field_width/2.0f, center.z + field_width/2.0f},
        };
        R_PushCmd((struct rcmd){
            .func = R_GL_DrawQuad,
            .nargs = 4,
            .args = {
                R_PushArg(field_corners, sizeof(field_corners)),
                R_PushArg(&line_width, sizeof(line_width)),
                R_PushArg(&blue, sizeof(blue)),
                (void*)G_GetPrevTickMap(),
            },
        });

        struct coord center_coord = (struct coord){
            OCCUPIED_FIELD_RES / 2,
            OCCUPIED_FIELD_RES / 2
        };

        const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
        const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

        vec2_t corners_buff[4 * OCCUPIED_FIELD_RES * OCCUPIED_FIELD_RES];
        vec3_t colors_buff[OCCUPIED_FIELD_RES * OCCUPIED_FIELD_RES];
        struct coord chunk_buff[OCCUPIED_FIELD_RES * OCCUPIED_FIELD_RES];

        vec2_t *corners_base = corners_buff;
        vec3_t *colors_base = colors_buff; 
        struct coord *chunk_base = chunk_buff;
        size_t count = 0;

        for(int r = 0; r < OCCUPIED_FIELD_RES; r++) {
        for(int c = 0; c < OCCUPIED_FIELD_RES; c++) {

            int dr = center_coord.r - r;
            int dc = center_coord.c - c;
            struct tile_desc curr = center_tile;
            bool exists = M_Tile_RelativeDesc(res, &curr, dc, dr);
            if(!exists)
                continue;

            float square_x_len = center_bounds.width;
            float square_z_len = center_bounds.height;

            float square_x = CLAMP(-(((float)curr.tile_c) / res.tile_w) * chunk_x_dim, 
                                   -chunk_x_dim, chunk_x_dim);
            float square_z = CLAMP((((float)curr.tile_r) / res.tile_h) * chunk_z_dim, 
                                   -chunk_z_dim, chunk_z_dim);

            *corners_base++ = (vec2_t){square_x, square_z};
            *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
            *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
            *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

            if(formation->occupied[r][c] == TILE_BLOCKED) {
                *colors_base++ = (vec3_t){1.0f, 0.0f, 0.0f};
            }else if(formation->occupied[r][c] == TILE_ALLOCATED) {
                *colors_base++ = (vec3_t){0.0f, 0.0f, 1.0f};
            }else{
                *colors_base++ = (vec3_t){0.0f, 1.0f, 0.0f};
            }
            *chunk_base++ = (struct coord){curr.chunk_r, curr.chunk_c};
            count++;
        }}

        size_t nchunks = sort_by_chunk(count, corners_buff, colors_buff, chunk_buff);
        size_t offset = 0;
        for(int i = 0; i < nchunks; i++) {

            mat4x4_t chunk_model;
            M_ModelMatrixForChunk(s_map, 
                (struct chunkpos){chunk_buff[offset].r, chunk_buff[offset].c}, &chunk_model);

            size_t num_tiles;
            size_t next_offset = next_chunk_range(offset, count, chunk_buff, &num_tiles);
            R_PushCmd((struct rcmd){
                .func = R_GL_DrawMapOverlayQuads,
                .nargs = 5,
                .args = {
                    R_PushArg(corners_buff + 4 * offset, sizeof(vec2_t) * 4 * num_tiles),
                    R_PushArg(colors_buff + offset, sizeof(vec3_t) * num_tiles),
                    R_PushArg(&num_tiles, sizeof(num_tiles)),
                    R_PushArg(&chunk_model, sizeof(chunk_model)),
                    (void*)G_GetPrevTickMap(),
                },
            });
            offset = next_offset;
        }
    });
}

static void on_render_3d(void *user, void *event)
{
    struct sval setting;
    ss_e status;
    (void)status;

    status = Settings_Get("pf.debug.show_formations", &setting);
    assert(status == SS_OKAY);
    bool enabled = setting.as_bool;

    if(enabled) {
        render_formations();
    }

    status = Settings_Get("pf.debug.show_formations_occupied_field", &setting);
    assert(status == SS_OKAY);
    enabled = setting.as_bool;

    if(enabled) {
        render_formations_occupied_field();
        render_islands_field();
    }
}

static void destroy_subformation(struct subformation *formation)
{
    vec_cell_destroy(&formation->cells);
    kh_destroy(assignment, formation->assignment);
}

static void destroy_formation(struct formation *formation)
{
    for(int i = 0; i < vec_size(&formation->subformations); i++) {
        struct subformation *sub = &vec_AT(&formation->subformations, i);
        destroy_subformation(sub);
    }
    kh_destroy(entity, formation->ents);
    vec_subformation_destroy(&formation->subformations);
    kh_destroy(assignment, formation->sub_assignment);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Formation_Init(const struct map *map)
{
    if(NULL == (s_formations = kh_init(formation)))
        return false;

    s_map = map;
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, 
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    return true;
}

void G_Formation_Shutdown(void)
{
    s_map = NULL;

    struct formation *formation;
    kh_foreach_ptr(s_formations, formation, {
        destroy_formation(formation);
    });

    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    kh_destroy(formation, s_formations);
}

void G_Formation_Create(dest_id_t id, vec2_t target, khash_t(entity) *ents)
{
    ASSERT_IN_MAIN_THREAD();

    int ret;
    khiter_t k = kh_put(formation, s_formations, id, &ret);
    assert(ret != -1);
    struct formation *new = &kh_val(s_formations, k);

    *new = (struct formation){
        .type = FORMATION_RANK,
        .target = target,
        .orientation = compute_orientation(target, ents),
        .ents = kh_copy_entity(ents),
        .sub_assignment = kh_init(assignment)
    };
    init_subformations(new);
    init_occupied_field(target, new->occupied);
    init_islands_field(target, new->islands);

    for(int i = 0; i < vec_size(&new->subformations); i++) {
        struct subformation *sub = &vec_AT(&new->subformations, i);
        place_subformation(sub, new->occupied, new->islands);
    }
}

void G_Formation_Destroy(dest_id_t id)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(formation, s_formations, id);
    assert(k != kh_end(s_formations));

    struct formation *formation = &kh_val(s_formations, k);
    destroy_formation(formation);

    kh_del(formation, s_formations, k);
}

void G_Formation_AddUnits(dest_id_t id, khash_t(entity) *ents)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(formation, s_formations, id);
    assert(k != kh_end(s_formations));
    struct formation *formation = &kh_val(s_formations, k);

    uint32_t uid;
    kh_foreach_key(ents, uid, {
        int ret;
        k = kh_put(entity, formation->ents, uid, &ret);
        assert(ret != -1 && ret != 0);
    });
    /* Re-assign the entities */
}

void G_Formation_RemoveUnit(dest_id_t id, uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(formation, s_formations, id);
    assert(k != kh_end(s_formations));
    struct formation *formation = &kh_val(s_formations, k);

    k = kh_get(entity, formation->ents, uid);
    assert(k != kh_end(formation->ents));
    kh_del(entity, formation->ents, k);

    /* Remove the entity assignment */
}

