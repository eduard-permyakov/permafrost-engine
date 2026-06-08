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

#include "asset_load.h"
#include "phys/public/collision.h"

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

/* A baked PFOBJ model: the engine-ready blocks that loading a model from its
 * ASCII source would otherwise have to parse and assemble. The three blobs are
 * contiguous and restored with a plain memcpy:
 *
 *   'verts'       - the interleaved vertex buffer, ready to upload to the batch
 *   'render_priv' - the 'struct render_private' + trailing 'struct material[]'
 *   'anim_data'   - the 'struct anim_data' buffer (skeleton, clips, samples)
 *
 * The pointer-bearing blobs carry stale, run-local pointers; the consumer
 * re-bases them from 'hdr' after the copy.
 */
struct pfobj_cache{
    struct pfobj_hdr hdr;
    uint32_t         ent_flags;
    struct aabb      aabb;
    void            *verts;
    size_t           verts_size;
    void            *render_priv;
    size_t           render_priv_size;
    void            *anim_data;
    size_t           anim_size;
};

/* A baked texture: 'pixels' is a tightly-packed 'width' x 'height'
 * image at 'channels' bytes per texel, already downscaled to the batch's slice
 * resolution so it can be uploaded with a single glTexSubImage3D. The store is
 * content-addressed: byte-identical results across different source paths share
 * one on-disk blob, so the expensive resize is performed at most once per
 * distinct image.
 */
struct texture_cache{
    int   width;
    int   height;
    int   channels;
    void *pixels;
};

bool AssetCache_Init(void);
void AssetCache_Shutdown(void);

/* Fingerprint a source asset (size + mtime) for cache validation; 0 if absent. */
uint64_t AssetCache_SourceTag(const char *path);

bool AssetCache_TilesetExists(const char *name);
bool AssetCache_TilesetLoad(const char *name, uint64_t tag, struct tileset_cache *out);
bool AssetCache_TilesetStore(const char *name, uint64_t tag, const struct tileset_cache *in);

/* 'name' is the model's base-path-relative path (e.g. 'assets/foo/bar.pfobj'),
 * which also identifies the source for tag validation. A successful Load fills
 * 'out' with freshly-allocated blobs that the caller must hand to PFObjRelease
 * once they have been copied into engine-owned storage.
 */
bool AssetCache_PFObjLoad(const char *name, uint64_t tag, struct pfobj_cache *out);
bool AssetCache_PFObjStore(const char *name, uint64_t tag, const struct pfobj_cache *in);
void AssetCache_PFObjRelease(struct pfobj_cache *cache);

/* 'src_name' is the base-path-relative path of the source image, used both as
 * the lookup key and (with 'tag') for invalidation. A successful Load fills
 * 'out' with a freshly-allocated pixel buffer the caller must hand to
 * TextureRelease. Store content-addresses 'in->pixels' so identical results are
 * not duplicated on disk.
 */
bool AssetCache_TextureLoad(const char *src_name, uint64_t tag, struct texture_cache *out);
bool AssetCache_TextureStore(const char *src_name, uint64_t tag, const struct texture_cache *in);
void AssetCache_TextureRelease(struct texture_cache *cache);

#endif
