/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
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

#ifndef ASSET_CACHE_H
#define ASSET_CACHE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Pre-baked, engine-ready assets are cached under the 'cache' directory of the
 * base path so that expensive generation can be skipped on later sessions. The
 * blobs store data in the exact layout the engine consumes and are machine-local
 * (not portable across differing endianness or word size).
 */

/* A baked Wang tileset: 'num_tiles' square layers, each 'width' x 'height' texels
 * tightly packed at 'nr_channels' bytes per texel, ready for upload as a single
 * GL_TEXTURE_2D_ARRAY.
 */
struct tileset_cache{
    int   width;
    int   height;
    int   num_tiles;
    int   nr_channels;
    void *pixels;
};

bool AssetCache_Init(void);
void AssetCache_Shutdown(void);

/* Fingerprint a source asset (size + mtime) for cache validation; 0 if absent. */
uint64_t AssetCache_SourceTag(const char *path);

bool AssetCache_TilesetExists(const char *name);
bool AssetCache_TilesetLoad(const char *name, uint64_t tag, struct tileset_cache *out);
bool AssetCache_TilesetStore(const char *name, uint64_t tag, const struct tileset_cache *in);

#endif
