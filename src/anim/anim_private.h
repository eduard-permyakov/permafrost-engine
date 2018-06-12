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

#ifndef ANIM_PRIVATE_H
#define ANIM_PRIVATE_H

struct skeleton;

/* Computes the inverse bind matrix for each joint based on the 
 * joint's bind SQT. The inverse bind matrix will be used by the vertex
 * shader to transform a vertex to the coordinate space of a joint
 * it is bound to (i.e. give a position of the vertex relative to 
 * a joint in bind pose). The matrices will be written to the memory
 * pointed to by 'skel->inv_bind_poses' which is expected to be 
 * allocated already.
 */
void A_PrepareInvBindMatrices(const struct skeleton *skel);

#endif
