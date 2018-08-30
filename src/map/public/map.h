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

#ifndef MAP_H
#define MAP_H

#include "../../pf_math.h"
#include "../../navigation/public/nav.h" /* dest_id_t */

#include <stdio.h>
#include <stdbool.h>

#include <SDL.h> /* for SDL_RWops */


#define MATERIALS_PER_CHUNK  (8)
#define MINIMAP_SIZE         (256)
#define MAX_HEIGHT_LEVEL     (9)

struct pfchunk;
struct pfmap_hdr;
struct map;
struct camera;
struct tile;
struct tile_desc;
struct obb;

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
 * Render a layer over the visible map surface showing which regions are 
 * pathable and which are not.
 * ------------------------------------------------------------------------
 */
void   M_RenderVisiblePathableLayer(const struct map *map, const struct camera *cam);

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
 * If returning true, the height of the map under the mouse cursor will be
 * written to 'out'. Otherwise, the mouse cursor is not over the map surface.
 * ------------------------------------------------------------------------
 */
bool   M_Raycast_IntersecCoordinate(vec3_t *out);

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

/* ------------------------------------------------------------------------
 * Returns true if the XZ coordinate is within the map bounds.
 * ------------------------------------------------------------------------
 */
bool   M_PointInsideMap(const struct map *map, vec2_t xz);

/* ------------------------------------------------------------------------
 * Returns an XZ coordinate that is contained within the map bounds. The 
 * 'xz' arg is truncated if it's outside the map range, or unchanged otherwise.
 * ------------------------------------------------------------------------
 */
vec2_t M_ClampedMapCoordinate(const struct map *map, vec2_t xz);

/* ------------------------------------------------------------------------
 * Returns the Y coordinate for an XZ point on the map's surface.
 * ------------------------------------------------------------------------
 */
float  M_HeightAtPoint(const struct map *map, vec2_t xz);

/* ------------------------------------------------------------------------
 * Make an impassable region in the navigation data, making it not possible 
 * for pathable units to pass through the region underneath the OBB.
 * ------------------------------------------------------------------------
 */
void   M_NavCutoutStaticObject(const struct map *map, const struct obb *obb);

/* ------------------------------------------------------------------------
 * Update navigation private data after calls to 'M_NavCutoutStaticObject'.
 * (ex. to remove a path in case it was blocked off by a placed object)
 * ------------------------------------------------------------------------
 */
void   M_NavUpdatePortals(const struct map *map);

/* ------------------------------------------------------------------------
 * Makes a path request to the navigation subsystem, causing the required
 * flowfields to be generated and cached. Returns true if a successful path
 * has been made, false otherwise.
 * ------------------------------------------------------------------------
 */
bool   M_NavRequestPath(const struct map *map, vec2_t xz_src, vec2_t xz_dest, 
                        dest_id_t *out_dest_id);

/* ------------------------------------------------------------------------
 * Render the flow field that will steer entities towards a particular 
 * destination over the map surface.
 * Also render the LOS field of tiles directly visible from the destination.
 * ------------------------------------------------------------------------
 */
void   M_NavRenderVisiblePathFlowField(const struct map *map, const struct camera *cam, 
                                       dest_id_t id);

/* ------------------------------------------------------------------------
 * Returns the desired velocity vector for moving with the flow field 
 * to the specified destination.
 * ------------------------------------------------------------------------
 */
vec2_t M_NavDesiredVelocity(const struct map *map, dest_id_t id, 
                            vec2_t curr_pos, vec2_t xz_dest);

/* ------------------------------------------------------------------------
 * Returns true if the specified coordinate is in direct line of sight of 
 * the specified destination.
 * ------------------------------------------------------------------------
 */
bool   M_NavHasDestLOS(const struct map *map, dest_id_t id, vec2_t curr_pos);

/* ------------------------------------------------------------------------
 * Returns true if the specified positions is pathable (i.e. a unit is 
 * allowed to stand on this region of the map)
 * ------------------------------------------------------------------------
 */
bool   M_NavPositionPathable(const struct map *map, vec2_t xz_pos);

/*###########################################################################*/
/* MINIMAP                                                                   */
/*###########################################################################*/

/* ------------------------------------------------------------------------
 * Creates a minimap texture from the map to be rendered later.
 * ------------------------------------------------------------------------
 */
bool   M_InitMinimap     (struct map *map, vec2_t center_pos);

/* ------------------------------------------------------------------------
 * Update a chunk-sized region of the minimap texture with the most 
 * up-to-date vertex data.
 * ------------------------------------------------------------------------
 */
bool   M_UpdateMinimapChunk(const struct map *map, int chunk_r, int chunk_c);

/* ------------------------------------------------------------------------
 * Frees the resources allocated by 'M_InitMinimap'.
 * ------------------------------------------------------------------------
 */
void   M_FreeMinimap     (struct map *map);

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

/* ------------------------------------------------------------------------
 * Initialize private map data ('outmap', which is allocated by the calleer) 
 * from PFMAP stream.
 * ------------------------------------------------------------------------
 */
bool   M_AL_InitMapFromStream(const struct pfmap_hdr *header, const char *basedir,
                              SDL_RWops *stream, void *outmap);

/* ------------------------------------------------------------------------
 * Returns the size, in bytes, needed to store the private map data
 * based on the header contents.
 * ------------------------------------------------------------------------
 */
size_t M_AL_BuffSizeFromHeader(const struct pfmap_hdr *header);

/* ------------------------------------------------------------------------
 * Writes the map in PFMap format.
 * ------------------------------------------------------------------------
 */
void   M_AL_DumpMap(FILE *stream, const struct map *map);

/* ------------------------------------------------------------------------
 * Cleans up resource allocations done during map initialization.
 * ------------------------------------------------------------------------
 */
void   M_AL_FreePrivate(struct map *map);

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
