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

struct pfchunk;
struct pfmap_hdr;
struct map;
struct camera;

/*###########################################################################*/
/* MAP GENERAL                                                               */
/*###########################################################################*/

/* ------------------------------------------------------------------------
 * This renders all the chunks at once, which is wasteful when there are 
 * many off-screen chunks.
 * TODO: Rendering of visible chunks only
 * ------------------------------------------------------------------------
 */
void   M_RenderEntireMap    (const struct map *map);

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

/*###########################################################################*/
/* MAP ASSET LOADING                                                         */
/*###########################################################################*/

bool   M_AL_InitMapFromStream(const struct pfmap_hdr *header, const char *basedir,
                              FILE *stream, void *outmap);
size_t M_AL_BuffSizeFromHeader(const struct pfmap_hdr *header);

/* ------------------------------------------------------------------------
 * Writes the map in PFMap format.
 * ------------------------------------------------------------------------
 */
void   M_AL_DumpMap(FILE *stream, const struct map *map);

#endif
