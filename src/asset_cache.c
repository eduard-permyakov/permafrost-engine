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

#define MEM_FILE_SYS MEM_SYS_ASSET_CACHE
#define MEM_FILE_SUB 0

#include "asset_cache.h"
#include "main.h"
#include "lib/public/windows.h"
#include "lib/public/pf_string.h"
#include "lib/public/mem.h"

#include <SDL.h>

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_ASSET_CACHE, 0)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_ASSET_CACHE, 0)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_ASSET_CACHE, 0)

#define TILESET_MAGIC       "PFTS"
#define TILESET_VERSION     (1)
#define MAX_PATH_LEN        (512)

/* 'tag' fingerprints the source asset (size + mtime); a mismatch on load
 * invalidates the entry so a changed source is re-baked.
 */
struct tileset_hdr{
    char     magic[4];
    uint32_t version;
    uint64_t tag;
    uint32_t width;
    uint32_t height;
    uint32_t num_tiles;
    uint32_t nr_channels;
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool valid_name(const char *name)
{
    return (name[0] != '\0')
        && (strchr(name, '/') == NULL)
        && (strchr(name, '\\') == NULL);
}

static bool make_directory(const char *path)
{
#if defined(_WIN32)
    size_t wlen;
    wchar_t wpath[MAX_PATH_LEN];
    mbstowcs_s(&wlen, wpath, sizeof(wpath) / sizeof(wpath[0]), path, _TRUNCATE);
    return CreateDirectory(wpath, NULL) || (ERROR_ALREADY_EXISTS == GetLastError());
#else
    return (mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == 0) || (errno == EEXIST);
#endif
}

static bool tileset_path(char *out, size_t size, const char *name)
{
    if(!valid_name(name))
        return false;
    pf_snprintf(out, size, "%s/cache/tilesets/%s.pfts", g_basepath, name);
    return true;
}

/*****************************************************************************/
/* PUBLIC FUNCTIONS                                                          */
/*****************************************************************************/

bool AssetCache_Init(void)
{
    char path[MAX_PATH_LEN];

    pf_snprintf(path, sizeof(path), "%s/cache", g_basepath);
    if(!make_directory(path))
        return false;

    pf_snprintf(path, sizeof(path), "%s/cache/tilesets", g_basepath);
    if(!make_directory(path))
        return false;

    return true;
}

void AssetCache_Shutdown(void)
{
}

uint64_t AssetCache_SourceTag(const char *path)
{
#if defined(_WIN32)
    struct _stat64 st;
    if(_stat64(path, &st) != 0)
        return 0;
#else
    struct stat st;
    if(stat(path, &st) != 0)
        return 0;
#endif
    uint64_t tag = (uint64_t)st.st_size;
    tag = tag * 1099511628211ull + (uint64_t)st.st_mtime;
    return tag;
}

bool AssetCache_TilesetExists(const char *name)
{
    char path[MAX_PATH_LEN];
    if(!tileset_path(path, sizeof(path), name))
        return false;

    SDL_RWops *stream = SDL_RWFromFile(path, "rb");
    if(!stream)
        return false;

    SDL_RWclose(stream);
    return true;
}

bool AssetCache_TilesetLoad(const char *name, uint64_t tag, struct tileset_cache *out)
{
    char path[MAX_PATH_LEN];
    if(!tileset_path(path, sizeof(path), name))
        return false;

    SDL_RWops *stream = SDL_RWFromFile(path, "rb");
    if(!stream)
        return false;

    bool ret = false;
    void *pixels = NULL;
    struct tileset_hdr hdr;
    size_t nbytes;

    if(SDL_RWread(stream, &hdr, sizeof(hdr), 1) != 1)
        goto out;
    if(memcmp(hdr.magic, TILESET_MAGIC, sizeof(hdr.magic)) != 0)
        goto out;
    if(hdr.version != TILESET_VERSION)
        goto out;
    if(hdr.tag != tag)
        goto out;

    nbytes = (size_t)hdr.width * hdr.height * hdr.num_tiles * hdr.nr_channels;
    if(nbytes == 0)
        goto out;

    pixels = PF_MALLOC(nbytes);
    if(!pixels)
        goto out;
    if(SDL_RWread(stream, pixels, nbytes, 1) != 1) {
        PF_FREE(pixels);
        goto out;
    }

    out->width = hdr.width;
    out->height = hdr.height;
    out->num_tiles = hdr.num_tiles;
    out->nr_channels = hdr.nr_channels;
    out->pixels = pixels;
    ret = true;

out:
    SDL_RWclose(stream);
    return ret;
}

bool AssetCache_TilesetStore(const char *name, uint64_t tag, const struct tileset_cache *in)
{
    char path[MAX_PATH_LEN];
    if(!tileset_path(path, sizeof(path), name))
        return false;

    size_t nbytes = (size_t)in->width * in->height * in->num_tiles * in->nr_channels;
    if(nbytes == 0 || !in->pixels)
        return false;

    SDL_RWops *stream = SDL_RWFromFile(path, "wb");
    if(!stream)
        return false;

    struct tileset_hdr hdr;
    memcpy(hdr.magic, TILESET_MAGIC, sizeof(hdr.magic));
    hdr.version = TILESET_VERSION;
    hdr.tag = tag;
    hdr.width = in->width;
    hdr.height = in->height;
    hdr.num_tiles = in->num_tiles;
    hdr.nr_channels = in->nr_channels;

    bool ret = (SDL_RWwrite(stream, &hdr, sizeof(hdr), 1) == 1)
            && (SDL_RWwrite(stream, in->pixels, nbytes, 1) == 1);
    SDL_RWclose(stream);

    /* Don't leave a truncated file behind to be mistaken for a valid entry */
    if(!ret)
        remove(path);
    return ret;
}

