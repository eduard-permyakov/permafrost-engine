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

#ifndef MOVE_PRIVATE_H
#define MOVE_PRIVATE_H

#include "public/movement.h"
#include "../lib/public/shared_ptr.h"

/* Reference-counted nav snapshot; borrowers must treat the grid as read-only. */
struct refcounted_map{
    SHARED_PTR_HEADER;
    struct map *snapshot;
};

/* Borrow movement's current nav snapshot (NULL if not running); release with sp_release. */
struct refcounted_map *Move_NavSnapshotAcquire(void);
/* Advances every time the snapshot is retaken from the live map. */
uint32_t Move_NavSnapshotGeneration(void);

/* The tid of the navigation tick task, or NULL_TID when none is in flight. */
uint32_t Move_GetNavTID(void);

/* Drain the navigation tick task to a quiescent state so the main thread may
 * safely read the task-owned field cache (e.g. for debug rendering). Returns
 * false if the task is GPU-blocked and could not be completed this call. */
bool Move_NavQuiesce(void);

int  Move_GetTickHz(void);

bool Move_SeekingFiringPosition(uint32_t uid);
/* Whether the unit is chasing enemies, away from the order it keeps */
bool Move_Seeking(uint32_t uid);
/* Whether the unit has stopped for good (not merely waiting) */
bool Move_Arrived(uint32_t uid);

#endif
