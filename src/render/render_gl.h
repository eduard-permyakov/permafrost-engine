/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017 Eduard Permyakov 
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

#ifndef RENDER_GL_H
#define RENDER_GL_H

#include <stddef.h>
#include <stdbool.h>

/* Each face is made of 2 independent triangles. The top face is an exception, and is made up of 4 
 * triangles. This is to give each triangle a vertex which lies at the center of the tile in the XZ
 * dimensions.
 * This center vertex will have its' own texture coordinate (used for blending edges between tiles).
 * As well, the center vertex can have its' own normal for potentially "smooth" corner and ramp tiles. 
 */
#define VERTS_PER_FACE 6
#define VERTS_PER_TILE ((5 * VERTS_PER_FACE) + (4 * 3))

struct render_private;
struct vertex;
struct tile;

void R_GL_Init(struct render_private *priv, const char *shader);
void R_GL_VerticesFromTile(const struct tile *tile, struct vertex *out, size_t r, size_t c);
void R_GL_BufferSubData(const void *chunk_rprivate, size_t offset, size_t size);

#endif
