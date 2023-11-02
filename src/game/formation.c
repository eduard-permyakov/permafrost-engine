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
#include "../map/public/map.h"
#include "../lib/public/vec.h"
#include "../lib/public/khash.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"

#include <assert.h>

#define COLUMN_WIDTH_RATIO  (0.25f)
#define RANK_WIDTH_RATIO    (4.0f)

enum cell_state
{
    CELL_OCCUPIED,
    CELL_NOT_OCCUPIED
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

VEC_TYPE(cell, struct cell)
VEC_IMPL(static inline, cell, struct cell)

KHASH_MAP_INIT_INT(assignment, struct coord)

enum formation_type
{
    FORMATION_RANK,
    FORMATION_COLUMN
};

struct formation
{
    enum formation_type  type;
    vec2_t               target;
    vec2_t               orientation;
    khash_t(entity)     *ents;
    size_t               nrows;
    size_t               ncols;
    vec_cell_t           cells;
    khash_t(assignment) *assignment;
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

static void init_cells(size_t nrows, size_t ncols, vec_cell_t *cells)
{
    size_t total = nrows * ncols;
    vec_cell_init(cells);
    vec_cell_resize(cells, total);
    for(int r = 0; r < nrows; r++) {
    for(int c = 0; c < ncols; c++) {
        size_t idx = r * ncols + c;
        vec_AT(cells, idx) = (struct cell){CELL_NOT_OCCUPIED};
    }}

    /* Position the cells on pathable and unobstructed terrain */
}

static void on_render_3d(void *user, void *event)
{
    struct sval setting;
    ss_e status;
    (void)status;

    status = Settings_Get("pf.debug.show_formations", &setting);
    assert(status == SS_OKAY);
    bool enabled = setting.as_bool;

    if(!enabled)
        return;

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

    struct formation formation;
    kh_foreach_value(s_formations, formation, {
        kh_destroy(entity, formation.ents);
        vec_cell_destroy(&formation.cells);
        kh_destroy(assignment, formation.assignment);
    });

    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    kh_destroy(formation, s_formations);
}

void G_Formation_Create(dest_id_t id, vec2_t target, khash_t(entity) *ents)
{
    ASSERT_IN_MAIN_THREAD();

    struct formation new = (struct formation){
        .type = FORMATION_RANK,
        .target = target,
        .orientation = compute_orientation(target, ents),
        .ents = kh_copy_entity(ents),
        .nrows = nrows(FORMATION_RANK, kh_size(ents)),
        .ncols = ncols(FORMATION_RANK, kh_size(ents)),
        .assignment = kh_init(assignment)
    };
    init_cells(new.nrows, new.ncols, &new.cells);

    int ret;
    khiter_t k = kh_put(formation, s_formations, id, &ret);
    assert(ret != -1);
    kh_val(s_formations, k) = new;
}

void G_Formation_Destroy(dest_id_t id)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(formation, s_formations, id);
    assert(k != kh_end(s_formations));

    struct formation *formation = &kh_val(s_formations, k);
    kh_destroy(entity, formation->ents);
    vec_cell_destroy(&formation->cells);
    kh_destroy(assignment, formation->assignment);

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

