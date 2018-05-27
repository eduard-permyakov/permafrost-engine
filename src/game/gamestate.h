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
 */

#ifndef GAMESTATE_H
#define GAMESTATE_H

#include "../lib/public/khash.h"
#include "../lib/public/kvec.h"

KHASH_MAP_INIT_INT(entity, struct entity *)

#define NUM_CAMERAS 2

struct gamestate{
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
    kvec_t(struct entity*)  visible;
    /*-------------------------------------------------------------------------
     * Cache of current-frame OBBs for visible entities.
     *-------------------------------------------------------------------------
     */
    kvec_t(struct obb)      visible_obbs;
    /*-------------------------------------------------------------------------
     * The player's current unit selection.
     *-------------------------------------------------------------------------
     */
    kvec_t(struct entity*)  selected;
};

#endif

