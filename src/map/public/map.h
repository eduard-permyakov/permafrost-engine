/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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

#ifndef MAP_H
#define MAP_H

#include "../../pf_math.h"

#include <stdio.h>
#include <stdbool.h>

#include <SDL.h> /* for SDL_RWops */


#define MATERIALS_PER_CHUNK  (8)
#define MINIMAP_SIZE         (256)

struct pfchunk;
struct pfmap_hdr;
struct map;
struct camera;
struct tile;

struct tile_desc{
    int chunk_r, chunk_c;
    int tile_r, tile_c;
};

enum chunk_render_mode{

    /* The first option for rendering a terrain chunk is using a shader-based 
     * approach to draw individual tiles. Each tile gets a texture and the 
     * texture is blended with adjacent tiles' textures. */
    CHUNK_RENDER_MODE_REALTIME_BLEND,

    /* The second option is to initially render the entire top face of the 
     * chunk to a single texture and use that. As well, during the 'baking'
     * process we also strip away any non-visible tile faces. This makes rendering
     * much faster but it is not suitable for real-time terrain updates.*/
    CHUNK_RENDER_MODE_PREBAKED,
};

/*###########################################################################*/
/* MAP GENERAL                                                               */
/*###########################################################################*/

/* ------------------------------------------------------------------------
 * This renders all the chunks at once, which is wasteful when there are 
 * many off-screen chunks.
 * ------------------------------------------------------------------------
 */
void   M_RenderEntireMap    (const struct map *map);

/* ------------------------------------------------------------------------
 * Renders the chunks of the map that are currently visible by the specified
 * camera using a frustrum-chunk intersection test.
 * ------------------------------------------------------------------------
 */
void   M_RenderVisibleMap   (const struct map *map, const struct camera *cam);

/* ------------------------------------------------------------------------
 * Centers the map at the worldspace origin.
 * ------------------------------------------------------------------------
 */
void   M_CenterAtOrigin     (struct map *map);

/* ------------------------------------------------------------------------
 * Sets an XZ bounding box for the camera such that the XZ coordinate of 
 * the intersection of the camera ray with the ground plane is always 
 * within the map area.
 * ------------------------------------------------------------------------
 */
void   M_RestrictRTSCamToMap(const struct map *map, struct camera *cam);

/* ------------------------------------------------------------------------
 * Install event handlers which will keep up-to-date state of the currently
 * hovered over tile.
 * ------------------------------------------------------------------------
 */
int    M_Raycast_Install(struct map *map, struct camera *cam);

/* ------------------------------------------------------------------------
 * Uninstall handlers installed by 'M_Raycast_Install'
 * ------------------------------------------------------------------------
 */
void   M_Raycast_Uninstall(void);

/* ------------------------------------------------------------------------
 * Determines how many tiles around the selected tile are highlighted during
 * rendering. 0 (default) means no tile is highlighted; 1 = single tile is 
 * highlighted; 3 = 3x3 grid is highlighted; etc.
 * ------------------------------------------------------------------------
 */
void   M_Raycast_SetHighlightSize(size_t size);

/* ------------------------------------------------------------------------
 * Sets the rendering mode for a particular chunk. In the case that the 
 * mode is 'CHUNK_RENDER_MODE_PREBAKED', the baking will be performed in
 * this call.
 * ------------------------------------------------------------------------
 */
void   M_SetChunkRenderMode(struct map *map, int chunk_r, int chunk_c, 
                            enum chunk_render_mode mode);

/* ------------------------------------------------------------------------
 * Sets the render mode for every chunk in the map.
 * ------------------------------------------------------------------------
 */
void   M_SetMapRenderMode(struct map *map, enum chunk_render_mode mode);

/* ------------------------------------------------------------------------
 * Utility function to convert an XZ worldspace coordinate to one in the 
 * range (-1, -1) in the 'top left' corner to (1, 1) in the 'bottom right' 
 * corner for square maps, with (0, 0) being in the exact center of the map. 
 * Coordinates outside the map bounds will be ouside this range. Note that 
 * for non-square maps, the proportion of the width to the height is kept 
 * the same, meaning that the shorter dimension will not span the entire 
 * range of [-1, 1]
 * ------------------------------------------------------------------------
 */
vec2_t M_WorldCoordsToNormMapCoords(const struct map *map, vec2_t xz);


/*###########################################################################*/
/* MINIMAP                                                                   */
/*###########################################################################*/

/* ------------------------------------------------------------------------
 * Creates a minimap texture from the map to be rendered later.
 * ------------------------------------------------------------------------
 */
bool   M_InitMinimap     (struct map *map, vec2_t center_pos);

/* ------------------------------------------------------------------------
 * Sets the position of the minimap center, in screen coordinates.
 * ------------------------------------------------------------------------
 */
void   M_SetMinimapPos   (struct map *map, vec2_t center_pos);

/* ------------------------------------------------------------------------
 * Render the minimap at the location specified by 'M_SetMinimapPos' and 
 * draw a box around the area visible by the specified camera.
 * ------------------------------------------------------------------------
 */
void   M_RenderMinimap   (const struct map *map, const struct camera *cam);

/* ------------------------------------------------------------------------
 * Render the minimap at the location specified by 'M_SetMinimapPos'.
 * ------------------------------------------------------------------------
 */
bool   M_MouseOverMinimap(const struct map *map);

/*###########################################################################*/
/* MAP ASSET LOADING                                                         */
/*###########################################################################*/

bool   M_AL_InitMapFromStream(const struct pfmap_hdr *header, const char *basedir,
                              SDL_RWops *stream, void *outmap);
size_t M_AL_BuffSizeFromHeader(const struct pfmap_hdr *header);

/* ------------------------------------------------------------------------
 * Writes the map in PFMap format.
 * ------------------------------------------------------------------------
 */
void   M_AL_DumpMap(FILE *stream, const struct map *map);

/* ------------------------------------------------------------------------
 * Updates the material list for the map, parsed from a PFMAP materials 
 * section string.
 * ------------------------------------------------------------------------
 */
bool   M_AL_UpdateChunkMats(const struct map *map, int chunk_r, int chunk_c, 
                            const char *mats_string);

bool   M_AL_UpdateTile(struct map *map, const struct tile_desc *desc, 
                       const struct tile *tile);


#endif
