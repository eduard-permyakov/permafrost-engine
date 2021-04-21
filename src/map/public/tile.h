/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2020 Eduard Permyakov 
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

#ifndef TILE_H
#define TILE_H

#include "../../phys/public/collision.h"
#include <stdbool.h>

#define X_COORDS_PER_TILE 8 
#define Y_COORDS_PER_TILE 4 
#define Z_COORDS_PER_TILE 8 

#define TILES_PER_CHUNK_HEIGHT 32
#define TILES_PER_CHUNK_WIDTH  32

enum tiletype{
    /* TILETYPE_FLAT:
     *                     +----------+
     *                    /          /|
     *                -  +----------+ +
     * base_height -> |  |          |/
     *                -  +----------+
     */
    TILETYPE_FLAT              = 0x0,
    /* By convention, the second letter (ex. 'N' in 'SN') is the raised end */
    TILETYPE_RAMP_SN           = 0x1,
    TILETYPE_RAMP_NS           = 0x2,
    TILETYPE_RAMP_EW           = 0x3,
    TILETYPE_RAMP_WE           = 0x4,
    /* For corners, the direction in the name is that of the central lowered corner */
    TILETYPE_CORNER_CONCAVE_SW = 0x5,
    TILETYPE_CORNER_CONVEX_SW  = 0x6,
    TILETYPE_CORNER_CONCAVE_SE = 0x7,
    TILETYPE_CORNER_CONVEX_SE  = 0x8,
    TILETYPE_CORNER_CONCAVE_NW = 0x9,
    TILETYPE_CORNER_CONVEX_NW  = 0xa,
    TILETYPE_CORNER_CONCAVE_NE = 0xb,
    TILETYPE_CORNER_CONVEX_NE  = 0xc,
};

enum blend_mode{
    BLEND_MODE_NOBLEND = 0,
    BLEND_MODE_BLUR,
};

struct tile{
    bool            pathable;    
    enum tiletype   type;
    int             base_height;
    /* ------------------------------------------------------------------------
     * Only valid when 'type' is a ramp or corner tile.
     * ------------------------------------------------------------------------
     */
    int             ramp_height;
    /* ------------------------------------------------------------------------
     * Render-specific tile attributes. Only used for populating private render
     * data.
     * ------------------------------------------------------------------------
     */
    int             top_mat_idx;
    int             sides_mat_idx;
    enum blend_mode blend_mode;
    bool            blend_normals;
};

struct tile_desc{
    int chunk_r, chunk_c;
    int tile_r, tile_c;
};

struct map_resolution{
    int chunk_w, chunk_h;
    int tile_w, tile_h;
};

#define TILETYPE_IS_RAMP(t) \
    (  ((t) == TILETYPE_RAMP_SN ) \
    || ((t) == TILETYPE_RAMP_NS ) \
    || ((t) == TILETYPE_RAMP_EW ) \
    || ((t) == TILETYPE_RAMP_WE))


#define TILETYPE_IS_CORNER_CONCAVE(t) \
    (  ((t) == TILETYPE_CORNER_CONCAVE_SW ) \
    || ((t) == TILETYPE_CORNER_CONCAVE_SE ) \
    || ((t) == TILETYPE_CORNER_CONCAVE_NW ) \
    || ((t) == TILETYPE_CORNER_CONCAVE_NE))

#define TILETYPE_IS_CORNER_CONVEX(t) \
    (  ((t) == TILETYPE_CORNER_CONVEX_SW ) \
    || ((t) == TILETYPE_CORNER_CONVEX_SE ) \
    || ((t) == TILETYPE_CORNER_CONVEX_NW ) \
    || ((t) == TILETYPE_CORNER_CONVEX_NE))


int   M_Tile_NWHeight(const struct tile *tile);
int   M_Tile_NEHeight(const struct tile *tile);
int   M_Tile_SWHeight(const struct tile *tile);
int   M_Tile_SEHeight(const struct tile *tile);
int   M_Tile_BaseHeight(const struct tile *tile);

bool  M_Tile_FrontFaceVisible(const struct tile *tiles, int r, int c);
bool  M_Tile_BackFaceVisible (const struct tile *tiles, int r, int c);
bool  M_Tile_LeftFaceVisible (const struct tile *tiles, int r, int c);
bool  M_Tile_RightFaceVisible(const struct tile *tiles, int r, int c);

/* 'frac_with' and 'frac_height' are given in screen cooridnates (width increases to the
 * right and height increases downwards. 
 */
float      M_Tile_HeightAtPos(const struct tile *tile, float frac_width, float frac_height);

struct box M_Tile_Bounds(struct map_resolution res, vec3_t map_pos, struct tile_desc desc);
struct box M_Tile_ChunkBounds(struct map_resolution res, vec3_t map_pos, int chunk_r, int chunk_c);
bool       M_Tile_RelativeDesc(struct map_resolution res, struct tile_desc *inout, 
                               int tile_dc, int tile_dr);
void       M_Tile_Distance(struct map_resolution res, struct tile_desc *a, struct tile_desc *b, 
                           int *out_r, int *out_c);

/* Fills 'out' with a list of tile descriptors which are intersected by the 2D line 
 * segment. The descriptors will be in the order they are intersected by the line
 * segment, starting at the first point and ending at the second. 'res' specifies 
 * the map resolution in the number of chunks per map and the number of tiles per chunk. 
 */
int        M_Tile_LineSupercoverTilesSorted(struct map_resolution res, vec3_t map_pos, 
                                            struct line_seg_2d line, struct tile_desc out[], size_t maxout);

bool       M_Tile_DescForPoint2D(struct map_resolution res, vec3_t map_pos, 
                                 vec2_t point, struct tile_desc *out);

/* Fills a buffer with all tiles intersecting an object's OBB. Will never give false
 * negatives. It is possible for there to be duplicate tile descriptors in the buffer. */
size_t     M_Tile_AllUnderObj(vec3_t map_pos, struct map_resolution res, const struct obb *obb,
                              struct tile_desc *out, size_t maxout);

size_t     M_Tile_AllUnderCircle(struct map_resolution res, vec2_t xz_center, float radius,
                                 vec3_t map_pos, struct tile_desc *out, size_t maxout);

size_t     M_Tile_AllUnderAABB(struct map_resolution res, vec2_t xz_center, float halfx, float halfz,
                               vec3_t map_pos, struct tile_desc *out, size_t maxout);

size_t     M_Tile_Contour(size_t ntds, const struct tile_desc tds[static ntds],
                          struct map_resolution res, struct tile_desc *out, size_t maxout);

#endif
