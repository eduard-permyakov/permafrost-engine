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

#ifndef BITGRID_H
#define BITGRID_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* A 2D grid of bits: one bit of membership per (x, y) cell, packed into
 * uint64 words and stored row-major with each row aligned to a word boundary.
 * Use it when you need an O(1) "is cell (x, y) in the set" test over a fixed
 * grid (occupancy masks, dilated regions, dirty-cell tracking, ...).
 *
 * This is distinct from bitmap_grid.h, which is a spatial index for point
 * entities; here the bits themselves are the payload.
 */
struct bitgrid{
    int       width;      /* cells along x: valid x in [0, width)  */
    int       height;     /* cells along y: valid y in [0, height) */
    int       row_words;  /* uint64 words per row = ceil(width/64)  */
    uint64_t *bits;       /* row_words * height words, row-major     */
};

bool bitgrid_init(struct bitgrid *bg, int width, int height);
void bitgrid_destroy(struct bitgrid *bg);
void bitgrid_clear(struct bitgrid *bg);

/* Set every cell within Chebyshev (square) / Euclidean (disc) distance
 * 'radius' of (cx, cy). The region is clipped to the grid bounds.
 */
void bitgrid_stamp_square(struct bitgrid *bg, int cx, int cy, int radius);
void bitgrid_stamp_disc(struct bitgrid *bg, int cx, int cy, int radius);

static inline size_t bitgrid_word_idx(const struct bitgrid *bg, int x, int y)
{
    return (size_t)y * bg->row_words + ((unsigned)x >> 6);
}

static inline void bitgrid_set(struct bitgrid *bg, int x, int y)
{
    if(x < 0 || x >= bg->width || y < 0 || y >= bg->height)
        return;
    bg->bits[bitgrid_word_idx(bg, x, y)] |= ((uint64_t)1 << (x & 63));
}

static inline void bitgrid_clear_bit(struct bitgrid *bg, int x, int y)
{
    if(x < 0 || x >= bg->width || y < 0 || y >= bg->height)
        return;
    bg->bits[bitgrid_word_idx(bg, x, y)] &= ~((uint64_t)1 << (x & 63));
}

static inline bool bitgrid_test(const struct bitgrid *bg, int x, int y)
{
    if(x < 0 || x >= bg->width || y < 0 || y >= bg->height)
        return false;
    return (bg->bits[bitgrid_word_idx(bg, x, y)] >> (x & 63)) & 1;
}

#endif
