/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2024 Eduard Permyakov 
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

#ifndef NOISE_H
#define NOISE_H

#include <stddef.h>

void Noise_Init(void);
void Noise_GeneratePerlin1D(size_t x, float frequency, float *outbuff);
void Noise_GeneratePerlin2D(size_t x, size_t y, float frequency, float *outbuff);
void Noise_GenerateOctavePerlin2D(size_t x, size_t y, float frequency, int octaves,
                                  float persistence, float *outbuff);
/* Generate a noise image that is seamlessly tilable in all directions. 
 * For perfect tiling, frequency should be a power of 2. */
void Noise_GenerateOctavePerlinTile2D(size_t x, size_t y, float frequency, int octaves,
                                      float persistence, float *outbuff);
void Noise_GeneratePerlin3D(size_t x, size_t y, size_t z, float frequency, float *outbuff);
void Noise_Normalize2D(size_t x, size_t y, float *inout);
void Noise_DumpPPM(const char *path, size_t width, size_t height, float *buffer);

#endif

