/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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

#ifndef MAP_PRIVATE_H
#define MAP_PRIVATE_H

#include "pfchunk.h"
#include "../pf_math.h"

#define MAX_NUM_MATS (256)


struct map{
    /* ------------------------------------------------------------------------
     * Map dimensions in numbers of chunks.
     * ------------------------------------------------------------------------
     */
    size_t width, height;
    /* ------------------------------------------------------------------------
     * World-space location of the top left corner of the map.
     * ------------------------------------------------------------------------
     */
    vec3_t pos;
    /* ------------------------------------------------------------------------
     * Virtual resolution used to draw the minimap.Other parameters
     * assume that this is the screen resolution. The minimap is then scaled as 
     * necessary for the current window resolution at the rendering stage.
     * ------------------------------------------------------------------------
     */
    vec2_t minimap_vres;
    /* ------------------------------------------------------------------------
     * Minimap center location, in virtual screen coordinates.      
     * ------------------------------------------------------------------------
     */
    vec2_t minimap_center_pos;
    /* ------------------------------------------------------------------------
     * Minimap side length, in virtual screen coordinates.
     * ------------------------------------------------------------------------
     */
    int minimap_sz;
    /* ------------------------------------------------------------------------
     * Controls the minimap bounds as the screen aspect ratio changes. (see ui.h)
     * ------------------------------------------------------------------------
     */
    int minimap_resize_mask;
    /* ------------------------------------------------------------------------
     * Navigation private data for the map.
     * ------------------------------------------------------------------------
     */
    void *nav_private;
    /* ------------------------------------------------------------------------
     * Save the materials information read from the source PFMap file. This is 
     * used when saving to a new PFMAp file.
     * ------------------------------------------------------------------------
     */
    size_t num_mats;
    char texnames[MAX_NUM_MATS][256];
    /* ------------------------------------------------------------------------
     * The map chunks stored in row-major order. In total, there must be 
     * (width * height) number of chunks.
     * ------------------------------------------------------------------------
     */
    struct pfchunk chunks[];
};

#endif
