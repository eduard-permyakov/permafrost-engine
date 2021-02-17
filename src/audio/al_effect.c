/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2021 Eduard Permyakov 
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

#include "../pf_math.h"
#include "../event.h"
#include "../game/public/game.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/vec.h"
#include "../lib/public/quadtree.h"

#include <stdint.h>
#include <assert.h>

#include <AL/al.h>
#include <AL/alc.h>

struct al_effect{
    vec3_t   pos;
    uint32_t start_tick;
    ALuint   buffer;
};

VEC_TYPE(effect, struct al_effect)
VEC_IMPL(static inline, effect, struct al_effect)

QUADTREE_TYPE(effect, struct al_effect)
QUADTREE_PROTOTYPES(static, effect, struct al_effect)
QUADTREE_IMPL(static, effect, struct al_effect)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static vec_effect_t     s_effects;
static qt_effect_t      s_effect_tree;
static vec_effect_t     s_active;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void on_new_game(void *user, void *event)
{
    const struct map *map = event;
    if(!map)
        return;

    assert(vec_size(&s_active) == 0);
    assert(vec_size(&s_effects) == 0);
    assert(s_effect_tree.nrecs == 0);

    struct map_resolution res;
    M_GetResolution(map, &res);

    vec3_t center = M_GetCenterPos(map);

    float xmin = center.x - (res.tile_w * res.chunk_w * X_COORDS_PER_TILE) / 2.0f;
    float xmax = center.x + (res.tile_w * res.chunk_w * X_COORDS_PER_TILE) / 2.0f;
    float zmin = center.z - (res.tile_h * res.chunk_h * Z_COORDS_PER_TILE) / 2.0f;
    float zmax = center.z + (res.tile_h * res.chunk_h * Z_COORDS_PER_TILE) / 2.0f;

    qt_effect_destroy(&s_effect_tree);
    qt_effect_init(&s_effect_tree, xmin, xmax, zmin, zmax);
    qt_effect_reserve(&s_effect_tree, 4096);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Audio_Effect_Init(void)
{
    vec_effect_init(&s_effects);
    if(!vec_effect_resize(&s_effects, 4096))
        goto fail_vec;

    /* When no map is loaded, pick some arbitrary bounds for the 
     * quadtree. They will be updated for the map size when it is 
     * loaded. 
     */
    const float min = -1024.0;
    const float max = 1024.0;

    qt_effect_init(&s_effect_tree, min, max, min, max);
    if(!qt_effect_reserve(&s_effect_tree, 4096))
        goto fail_tree;

    vec_effect_init(&s_active);
    if(!vec_effect_resize(&s_effects, 64))
        goto fail_active;

    E_Global_Register(EVENT_NEW_GAME, on_new_game, NULL, G_ALL);
    return true;

fail_active:
    qt_effect_destroy(&s_effect_tree);
fail_tree:
    vec_effect_destroy(&s_effects);
fail_vec:
    return false;
}

void Audio_Effect_Shutdown(void)
{
    E_Global_Unregister(EVENT_NEW_GAME, on_new_game);
    vec_effect_destroy(&s_active);
    vec_effect_destroy(&s_effects);
    qt_effect_destroy(&s_effect_tree);
}

