/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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

#ifndef GAMESTATE_H
#define GAMESTATE_H

#include "public/game.h"
#include "../lib/public/vec.h"
#include "faction.h"
#include "selection.h"

#include <stdint.h>

#define NUM_CAMERAS  2

struct gamestate{
    enum simstate           ss;
    /*-------------------------------------------------------------------------
     * The SDL tick during which we last changed simulation states.
     *-------------------------------------------------------------------------
     */
    uint32_t                ss_change_tick;
    struct map             *map;
    int                     active_cam_idx;
    struct camera          *cameras[NUM_CAMERAS];
    /*-------------------------------------------------------------------------
     * The set of all game entities currently taking part in the game simulation.
     *-------------------------------------------------------------------------
     */
    khash_t(entity)        *active;
    /*-------------------------------------------------------------------------
     * The set of entities potentially visible by the active camera.
     *-------------------------------------------------------------------------
     */
    vec_pentity_t           visible;
    /*-------------------------------------------------------------------------
     * Cache of current-frame OBBs for visible entities.
     *-------------------------------------------------------------------------
     */
    vec_obb_t               visible_obbs;
    /*-------------------------------------------------------------------------
     * Up-to-date set of all non-static entities. (Subset of 'active' set). 
     * Used for collision avoidance force computations.
     *-------------------------------------------------------------------------
     */
    khash_t(entity)        *dynamic;
    size_t                  num_factions;
    struct faction          factions[MAX_FACTIONS];
    /*-------------------------------------------------------------------------
     * Holds the relationships between every 2 factions. Note that diplomatic
     * relations are always symmetric (i.e always 'mutually' at war or peace).
     *-------------------------------------------------------------------------
     */
    enum diplomacy_state    diplomacy_table[MAX_FACTIONS][MAX_FACTIONS];
};

#endif

