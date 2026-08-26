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

#include "formation_box.h"

#include <string.h>

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static int box_side(size_t nunits)
{
    int side = 1;
    while((size_t)side * side < nunits)
        side++;
    return side;
}

/* The i-th column of a width-'w' row, walking outwards from the centre. Mirrors
 * the order in which the leader scan sweeps a row.
 */
static int centre_out(int w, int i)
{
    int centre = w / 2;
    if(i % 2)
        return centre - (i + 1) / 2;
    return centre + i / 2;
}

/* The i-th of 'm' cells, walking inwards from both ends. */
static int edge_in(int m, int i)
{
    if(i % 2)
        return m - 1 - (i - 1) / 2;
    return i / 2;
}

static void mark_cell(uint8_t *out, int side, int r, int c, size_t *inout_placed)
{
    out[r * side + c] = 1;
    (*inout_placed)++;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_FormationBox_Shells(const size_t *counts, size_t ngroups, struct box_shell *out)
{
    if(ngroups == 0)
        return false;

    int inner = box_side(counts[ngroups - 1]);
    if(inner > MAX_BOX_SIDE)
        return false;
    out[ngroups - 1] = (struct box_shell){inner, 0};

    /* Grow each shell outwards in steps of 2 until it holds its group. Keeping
     * the parity of the core makes every shell concentric with it.
     */
    for(int i = (int)ngroups - 2; i >= 0; i--) {

        int side = inner + 2;
        while((side <= MAX_BOX_SIDE)
           && ((size_t)side * side - (size_t)inner * inner < counts[i])) {
            side += 2;
        }
        if(side > MAX_BOX_SIDE)
            return false;

        out[i] = (struct box_shell){side, inner};
        inner = side;
    }
    return true;
}

void G_FormationBox_Mask(struct box_shell shell, size_t nunits, uint8_t *out)
{
    memset(out, 0, (size_t)shell.side * shell.side);

    size_t placed = 0;
    int nrings = (shell.side - shell.hole + 1) / 2;

    for(int ring = 0; (ring < nrings) && (placed < nunits); ring++) {

        int lo = ring;
        int hi = shell.side - 1 - ring;
        int w = hi - lo + 1;

        for(int i = 0; (i < w) && (placed < nunits); i++)
            mark_cell(out, shell.side, hi, lo + centre_out(w, i), &placed);

        for(int r = hi - 1; (r >= lo) && (placed < nunits); r--) {
            mark_cell(out, shell.side, r, hi, &placed);
            if((w > 1) && (placed < nunits))
                mark_cell(out, shell.side, r, lo, &placed);
        }

        for(int i = 0; (i < w - 2) && (placed < nunits); i++)
            mark_cell(out, shell.side, lo, lo + 1 + edge_in(w - 2, i), &placed);
    }
}
