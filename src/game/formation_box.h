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

#ifndef FORMATION_BOX_H
#define FORMATION_BOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The layout of a box formation: a set of concentric square shells, one per
 * unit group, with the highest-priority group forming the outer wall and the
 * lowest-priority one the solid core.
 *
 * A shell is described by the side of its bounding square and the side of the
 * unused square at its centre, which is the bounding square of the next shell
 * inwards. Both share a parity, so shells are concentric with an integer inset
 * of (outer_side - side) / 2 cells.
 *
 * Cell grids are row-major and follow the formation convention that row
 * (side - 1) is the front row and row 0 the back row.
 */

/* The box is abandoned in favour of a rank layout past this side, bounding a
 * selection made up of pathologically many distinct unit types.
 */
#define MAX_BOX_SIDE (47)

struct box_shell{
    int side;
    int hole;
};

/* Size one shell per group, from the innermost group outwards. 'counts' is
 * ordered from the outermost group to the innermost and must hold no zeroes.
 * Returns false if the box would grow past MAX_BOX_SIDE.
 */
bool G_FormationBox_Shells(const size_t *counts, size_t ngroups, struct box_shell *out);

/* Mark the 'nunits' cells of 'shell' which the group occupies in the
 * (side * side) row-major grid 'out'. Cells are taken ring by ring from the
 * outside in, and front-first within a ring, so a group too small to fill its
 * shell screens the front and leaves its gap at the back centre.
 */
void G_FormationBox_Mask(struct box_shell shell, size_t nunits, uint8_t *out);

#endif
