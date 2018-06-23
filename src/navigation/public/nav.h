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

#ifndef NAV_H
#define NAV_H

#include <stddef.h>

struct tile;

/*###########################################################################*/
/* NAV GENERAL                                                               */
/*###########################################################################*/

void *N_BuildForMapData(size_t w, size_t h, 
                        size_t chunk_w, size_t chunk_h,
                        struct tile **chunk_tiles);


void  N_FreePrivate(void *nav_private);

#endif

