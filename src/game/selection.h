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

#ifndef SELECTION_H
#define SELECTION_H

#include "../lib/public/kvec.h"
#include "../entity.h"

#include <stdbool.h>

struct obb;
struct camera;

typedef kvec_t(struct entity*) pentity_kvec_t;
typedef kvec_t(struct obb) obb_kvec_t;


void G_Sel_Clear(void);
/* Returns true if the current selection has changed. If returning false, 'out_selection'
 * parameter is unmodified. */
bool G_Sel_GetSelection(struct camera *cam, const pentity_kvec_t *visible, 
                        const obb_kvec_t *visible_obbs, pentity_kvec_t *inout);

#endif
