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

#define MEM_FILE_SYS MEM_SYS_LIB
#define MEM_FILE_SUB MEM_SUB_LIB_BITGRID

#include "public/bitgrid.h"

#include <assert.h>
#include <string.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_LIB, MEM_SUB_LIB_BITGRID)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_LIB, MEM_SUB_LIB_BITGRID)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_LIB, MEM_SUB_LIB_BITGRID)

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool bitgrid_init(struct bitgrid *bg, int width, int height)
{
    assert(width > 0 && height > 0);

    bg->width = width;
    bg->height = height;
    bg->row_words = (width + 63) / 64;
    bg->bits = PF_CALLOC((size_t)bg->row_words * height, sizeof(uint64_t));

    return (bg->bits != NULL);
}

void bitgrid_destroy(struct bitgrid *bg)
{
    PF_FREE(bg->bits);
    bg->bits = NULL;
}

void bitgrid_clear(struct bitgrid *bg)
{
    memset(bg->bits, 0, (size_t)bg->row_words * bg->height * sizeof(uint64_t));
}

void bitgrid_stamp_square(struct bitgrid *bg, int cx, int cy, int radius)
{
    int x0 = cx - radius, x1 = cx + radius;
    int y0 = cy - radius, y1 = cy + radius;

    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x1 >= bg->width)  x1 = bg->width - 1;
    if(y1 >= bg->height) y1 = bg->height - 1;

    for(int y = y0; y <= y1; y++) {
        uint64_t *row = &bg->bits[(size_t)y * bg->row_words];
        for(int x = x0; x <= x1; x++) {
            row[(unsigned)x >> 6] |= ((uint64_t)1 << (x & 63));
        }
    }
}

void bitgrid_stamp_disc(struct bitgrid *bg, int cx, int cy, int radius)
{
    int r2 = radius * radius;
    int x0 = cx - radius, x1 = cx + radius;
    int y0 = cy - radius, y1 = cy + radius;

    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x1 >= bg->width)  x1 = bg->width - 1;
    if(y1 >= bg->height) y1 = bg->height - 1;

    for(int y = y0; y <= y1; y++) {
        uint64_t *row = &bg->bits[(size_t)y * bg->row_words];
        int dy = y - cy;
        for(int x = x0; x <= x1; x++) {
            int dx = x - cx;
            if(dx * dx + dy * dy <= r2)
                row[(unsigned)x >> 6] |= ((uint64_t)1 << (x & 63));
        }
    }
}

