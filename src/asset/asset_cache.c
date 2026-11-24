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

#define MEM_FILE_SYS MEM_SYS_ASSET
#define MEM_FILE_SUB MEM_SUB_ASSET_CACHE

#include "public/asset_cache.h"
#include "../main.h"
#include "../config.h"
#include "../lib/public/windows.h"
#include "../lib/public/pf_string.h"
#include "../perf/public/mem.h"

#include <SDL.h>

#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <sys/utime.h>
#else
#include <dirent.h>
#include <utime.h>
#endif

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_ASSET, MEM_SUB_ASSET_CACHE)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_ASSET, MEM_SUB_ASSET_CACHE)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_ASSET, MEM_SUB_ASSET_CACHE)

#define TILESET_MAGIC       "PFTS"
#define TILESET_VERSION     (1)
#define OBJECT_MAGIC        "PFOM"
#define OBJECT_VERSION      (1)
#define TEXREF_MAGIC        "PFTR"
#define TEXBLOB_MAGIC       "PFTB"
#define TEXTURE_VERSION     (1)
#define IMPOSTOR_MAGIC      "PFIM"
#define IMPOSTOR_VERSION    (6)
#define IMPOSTOR_EXT        ".pfi"
#define MAX_PATH_LEN        (512)
#define MAX_REL_PATH_LEN    (256)

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

/* The model metadata, followed on disk by the 'verts', 'render_priv' and
 * 'anim_data' blobs in that order. 'rel_path' disambiguates the (flattened)
 * cache filename so two distinct sources cannot alias the same entry.
 */
struct object_hdr{
    char             magic[4];
    uint32_t         version;
    uint64_t         tag;
    char             rel_path[MAX_REL_PATH_LEN];
    struct pfobj_hdr pfobj;
    uint32_t         ent_flags;
    struct aabb      aabb;
    uint64_t         verts_size;
    uint64_t         render_priv_size;
    uint64_t         anim_size;
};

/* A texture ref points a source path at the content-addressed blob holding its
 * baked pixels; 'tag' invalidates the entry when the source image changes. */
struct texref_hdr{
    char     magic[4];
    uint32_t version;
    uint64_t tag;
    uint64_t content_hash;
};

struct texblob_hdr{
    char     magic[4];
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t channels;
};

/* The atlas metadata, followed on disk by the tightly-packed RGBA, depth,
 * surface and silhouette slices. 'rel_path' disambiguates the (flattened) cache filename like
 * the object cache's does.
 */
struct impostor_hdr{
    char     magic[4];
    uint32_t version;
    uint64_t tag;
    char     rel_path[MAX_REL_PATH_LEN];
    uint32_t cell_res;
    uint32_t nslices;
    uint32_t sil_res;
};

/* One file of the impostor cache, for keeping the cache within its bounds */
struct impostor_entry{
    char     file[MAX_REL_PATH_LEN + 8];
    uint64_t size;
    time_t   used;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

/* The impostor cache's files, listed on first use. Render thread only. */
static bool                   s_imp_indexed;
static struct impostor_entry *s_imp_entries;
static size_t                 s_imp_nentries;
static size_t                 s_imp_cap;
static uint64_t               s_imp_total;

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

/* A model is keyed by its relative path, which contains directory separators;
 * flatten them so the whole key becomes a single filename component. */
static bool object_path(char *out, size_t size, const char *name)
{
    if(name[0] == '\0')
        return false;

    char flat[MAX_REL_PATH_LEN];
    pf_strlcpy(flat, name, sizeof(flat));
    for(char *c = flat; *c; c++) {
        if(*c == '/' || *c == '\\')
            *c = '-';
    }

    pf_snprintf(out, size, "%s/cache/objects/%s.pfm", g_basepath, flat);
    return true;
}

static void hash_to_hex(char out[17], uint64_t hash)
{
    static const char digits[] = "0123456789abcdef";
    for(int i = 15; i >= 0; i--) {
        out[i] = digits[hash & 0xf];
        hash >>= 4;
    }
    out[16] = '\0';
}

static uint64_t content_hash(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint64_t h = 14695981039346656037ull; /* FNV-1a 64-bit */
    for(size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* The source-path-keyed ref (size+mtime tagged) that points at a shared blob. */
static bool texref_path(char *out, size_t size, const char *name)
{
    if(name[0] == '\0')
        return false;

    char flat[MAX_REL_PATH_LEN];
    pf_strlcpy(flat, name, sizeof(flat));
    for(char *c = flat; *c; c++) {
        if(*c == '/' || *c == '\\')
            *c = '-';
    }

    pf_snprintf(out, size, "%s/cache/textures/%s.ref", g_basepath, flat);
    return true;
}

/* The content-addressed blob shared by all sources that bake to it. */
static void texblob_path(char *out, size_t size, uint64_t hash)
{
    char hex[17];
    hash_to_hex(hex, hash);
    pf_snprintf(out, size, "%s/cache/textures/%s.tex", g_basepath, hex);
}

static void impostor_file_path(char *out, size_t size, const char *file)
{
    pf_snprintf(out, size, "%s/cache/impostors/%s", g_basepath, file);
}

/* The name of the file within the cache directory */
static bool impostor_file(char *out, size_t size, const char *name)
{
    if(name[0] == '\0')
        return false;

    char flat[MAX_REL_PATH_LEN];
    pf_strlcpy(flat, name, sizeof(flat));
    for(char *c = flat; *c; c++) {
        if(*c == '/' || *c == '\\')
            *c = '-';
    }

    pf_snprintf(out, size, "%s" IMPOSTOR_EXT, flat);
    return true;
}

static bool file_stat(const char *path, uint64_t *out_size, time_t *out_mtime)
{
#if defined(_WIN32)
    struct _stat64 st;
    if(_stat64(path, &st) != 0)
        return false;
#else
    struct stat st;
    if(stat(path, &st) != 0)
        return false;
#endif
    *out_size = (uint64_t)st.st_size;
    *out_mtime = st.st_mtime;
    return true;
}

/* Marks a file used now, so the least recently used order survives restarts */
static void file_touch(const char *path)
{
#if defined(_WIN32)
    _utime(path, NULL);
#else
    utime(path, NULL);
#endif
}

static void list_dir(const char *dir, void (*on_file)(const char *name))
{
#if defined(_WIN32)
    size_t wlen;
    wchar_t wdir[MAX_PATH_LEN + 2];
    mbstowcs_s(&wlen, wdir, MAX_PATH_LEN, dir, _TRUNCATE);
    wdir[wlen - 1] = '/';
    wdir[wlen + 0] = '*';
    wdir[wlen + 1] = '\0';

    WIN32_FIND_DATA entry = {0};
    HANDLE handle = FindFirstFile(wdir, &entry);
    if(handle == INVALID_HANDLE_VALUE)
        return;

    do{
        char cname[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, WC_NO_BEST_FIT_CHARS | WC_COMPOSITECHECK | WC_DEFAULTCHAR,
            entry.cFileName, -1, cname, sizeof(cname), NULL, NULL);
        if(!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            on_file(cname);
        }
    }while(FindNextFile(handle, &entry));
    FindClose(handle);
#else
    DIR *d = opendir(dir);
    if(!d)
        return;

    struct dirent *entry;
    while((entry = readdir(d)) != NULL) {
        if(entry->d_type != DT_DIR) {
            on_file(entry->d_name);
        }
    }
    closedir(d);
#endif
}

static bool has_suffix(const char *str, const char *suffix)
{
    size_t len = strlen(str), slen = strlen(suffix);
    return (len >= slen) && (strcmp(str + len - slen, suffix) == 0);
}

/* The files of one model's variants share the name up to the last '#' */
static size_t impostor_model_len(const char *file)
{
    const char *hash = strrchr(file, '#');
    return hash ? (size_t)(hash - file) : strlen(file);
}

static bool impostor_same_model(const char *a, const char *b)
{
    size_t len = impostor_model_len(a);
    return (len == impostor_model_len(b)) && (strncmp(a, b, len) == 0);
}

static bool impostor_header_current(const char *path)
{
    SDL_RWops *stream = SDL_RWFromFile(path, "rb");
    if(!stream)
        return false;

    struct impostor_hdr hdr;
    bool ret = (SDL_RWread(stream, &hdr, sizeof(hdr), 1) == 1)
            && (memcmp(hdr.magic, IMPOSTOR_MAGIC, sizeof(hdr.magic)) == 0)
            && (hdr.version == IMPOSTOR_VERSION);
    SDL_RWclose(stream);
    return ret;
}

static struct impostor_entry *impostor_find(const char *file)
{
    for(size_t i = 0; i < s_imp_nentries; i++) {
        if(strcmp(s_imp_entries[i].file, file) == 0)
            return &s_imp_entries[i];
    }
    return NULL;
}

static void impostor_track(const char *file, uint64_t size, time_t used)
{
    struct impostor_entry *entry = impostor_find(file);
    if(entry) {
        s_imp_total -= entry->size;
    }else{
        if(s_imp_nentries == s_imp_cap) {
            size_t cap = s_imp_cap ? s_imp_cap * 2 : 256;
            struct impostor_entry *entries = PF_REALLOC(s_imp_entries, cap * sizeof(*entries));
            if(!entries)
                return;
            s_imp_entries = entries;
            s_imp_cap = cap;
        }
        entry = &s_imp_entries[s_imp_nentries++];
        pf_strlcpy(entry->file, file, sizeof(entry->file));
    }
    entry->size = size;
    entry->used = used;
    s_imp_total += size;
}

static void impostor_evict(size_t idx)
{
    char path[MAX_PATH_LEN];
    impostor_file_path(path, sizeof(path), s_imp_entries[idx].file);
    remove(path);

    s_imp_total -= s_imp_entries[idx].size;
    s_imp_entries[idx] = s_imp_entries[--s_imp_nentries];
}

/* The least recently used file other than 'keep' (if any), optionally of
 * the same model as it; -1 if there is none.
 */
static ptrdiff_t impostor_oldest(const char *keep, bool same_model)
{
    ptrdiff_t ret = -1;
    for(size_t i = 0; i < s_imp_nentries; i++) {
        const struct impostor_entry *curr = &s_imp_entries[i];
        if(keep && strcmp(curr->file, keep) == 0)
            continue;
        if(same_model && !impostor_same_model(curr->file, keep))
            continue;
        if(ret < 0 || curr->used < s_imp_entries[ret].used) {
            ret = i;
        }
    }
    return ret;
}

static uint64_t impostor_cap_bytes(void)
{
    return (uint64_t)CONFIG_IMPOSTOR_CACHE_MAX_MB * 1024 * 1024;
}

/* Past the cap, the total is brought down to 90% of it so that eviction does
 * not run on every store
 */
static void impostor_trim_total(const char *keep)
{
    uint64_t cap = impostor_cap_bytes();
    if(s_imp_total <= cap)
        return;

    while(s_imp_total > cap / 10 * 9) {
        ptrdiff_t oldest = impostor_oldest(keep, false);
        if(oldest < 0)
            break;
        impostor_evict(oldest);
    }
}

/* Bounds the cache after 'file' was written: the variants of its model, then
 * the total size.
 */
static void impostor_enforce_bounds(const char *file)
{
    size_t nvariants = 0;
    for(size_t i = 0; i < s_imp_nentries; i++) {
        nvariants += impostor_same_model(s_imp_entries[i].file, file);
    }
    for(; nvariants > CONFIG_IMPOSTOR_CACHE_VARIANTS_PER_MODEL; nvariants--) {
        ptrdiff_t oldest = impostor_oldest(file, true);
        if(oldest < 0)
            break;
        impostor_evict(oldest);
    }
    impostor_trim_total(file);
}

static void impostor_index_file(const char *file)
{
    if(!has_suffix(file, IMPOSTOR_EXT))
        return;

    char path[MAX_PATH_LEN];
    impostor_file_path(path, sizeof(path), file);

    /* Files of an older format can never be loaded again */
    if(!impostor_header_current(path)) {
        remove(path);
        return;
    }

    uint64_t size;
    time_t mtime;
    if(file_stat(path, &size, &mtime)) {
        impostor_track(file, size, mtime);
    }
}

static void impostor_index_ensure(void)
{
    ASSERT_IN_RENDER_THREAD();

    if(s_imp_indexed)
        return;
    s_imp_indexed = true;

    char dir[MAX_PATH_LEN];
    pf_snprintf(dir, sizeof(dir), "%s/cache/impostors", g_basepath);
    list_dir(dir, impostor_index_file);
    impostor_trim_total(NULL);
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

    pf_snprintf(path, sizeof(path), "%s/cache/objects", g_basepath);
    if(!make_directory(path))
        return false;

    pf_snprintf(path, sizeof(path), "%s/cache/textures", g_basepath);
    if(!make_directory(path))
        return false;

    pf_snprintf(path, sizeof(path), "%s/cache/impostors", g_basepath);
    if(!make_directory(path))
        return false;

    return true;
}

void AssetCache_Shutdown(void)
{
    PF_FREE(s_imp_entries);
    s_imp_entries = NULL;
    s_imp_nentries = 0;
    s_imp_cap = 0;
    s_imp_total = 0;
    s_imp_indexed = false;
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

bool AssetCache_PFObjLoad(const char *name, uint64_t tag, struct pfobj_cache *out)
{
    char path[MAX_PATH_LEN];
    if(!object_path(path, sizeof(path), name))
        return false;

    SDL_RWops *stream = SDL_RWFromFile(path, "rb");
    if(!stream)
        return false;

    bool ret = false;
    void *verts = NULL, *render_priv = NULL, *anim_data = NULL;
    struct object_hdr hdr;

    if(SDL_RWread(stream, &hdr, sizeof(hdr), 1) != 1)
        goto out;
    if(memcmp(hdr.magic, OBJECT_MAGIC, sizeof(hdr.magic)) != 0)
        goto out;
    if(hdr.version != OBJECT_VERSION)
        goto out;
    if(hdr.tag != tag)
        goto out;

    /* A flattened name can collide; the stored relative path is authoritative. */
    hdr.rel_path[sizeof(hdr.rel_path) - 1] = '\0';
    if(strcmp(hdr.rel_path, name) != 0)
        goto out;

    if(hdr.verts_size == 0 || hdr.render_priv_size == 0 || hdr.anim_size == 0)
        goto out;

    verts       = PF_MALLOC(hdr.verts_size);
    render_priv = PF_MALLOC(hdr.render_priv_size);
    anim_data   = PF_MALLOC(hdr.anim_size);
    if(!verts || !render_priv || !anim_data)
        goto fail_blob;

    if(SDL_RWread(stream, verts, hdr.verts_size, 1) != 1)
        goto fail_blob;
    if(SDL_RWread(stream, render_priv, hdr.render_priv_size, 1) != 1)
        goto fail_blob;
    if(SDL_RWread(stream, anim_data, hdr.anim_size, 1) != 1)
        goto fail_blob;

    out->hdr = hdr.pfobj;
    out->ent_flags = hdr.ent_flags;
    out->aabb = hdr.aabb;
    out->verts = verts;
    out->verts_size = hdr.verts_size;
    out->render_priv = render_priv;
    out->render_priv_size = hdr.render_priv_size;
    out->anim_data = anim_data;
    out->anim_size = hdr.anim_size;
    ret = true;
    goto out;

fail_blob:
    if(verts)       PF_FREE(verts);
    if(render_priv) PF_FREE(render_priv);
    if(anim_data)   PF_FREE(anim_data);
out:
    SDL_RWclose(stream);
    return ret;
}

bool AssetCache_PFObjStore(const char *name, uint64_t tag, const struct pfobj_cache *in)
{
    char path[MAX_PATH_LEN];
    if(!object_path(path, sizeof(path), name))
        return false;

    /* Every blob must be present; a 0-sized entry would alias an empty read. */
    if(in->verts_size == 0 || !in->verts
    || in->render_priv_size == 0 || !in->render_priv
    || in->anim_size == 0 || !in->anim_data)
        return false;

    /* The relative path is the collision guard; it must round-trip untruncated. */
    if(strlen(name) >= MAX_REL_PATH_LEN)
        return false;

    SDL_RWops *stream = SDL_RWFromFile(path, "wb");
    if(!stream)
        return false;

    struct object_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, OBJECT_MAGIC, sizeof(hdr.magic));
    hdr.version = OBJECT_VERSION;
    hdr.tag = tag;
    pf_strlcpy(hdr.rel_path, name, sizeof(hdr.rel_path));
    hdr.pfobj = in->hdr;
    hdr.ent_flags = in->ent_flags;
    hdr.aabb = in->aabb;
    hdr.verts_size = in->verts_size;
    hdr.render_priv_size = in->render_priv_size;
    hdr.anim_size = in->anim_size;

    bool ret = (SDL_RWwrite(stream, &hdr, sizeof(hdr), 1) == 1)
            && (SDL_RWwrite(stream, in->verts, in->verts_size, 1) == 1)
            && (SDL_RWwrite(stream, in->render_priv, in->render_priv_size, 1) == 1)
            && (SDL_RWwrite(stream, in->anim_data, in->anim_size, 1) == 1);
    SDL_RWclose(stream);

    /* Don't leave a truncated file behind to be mistaken for a valid entry */
    if(!ret)
        remove(path);
    return ret;
}

void AssetCache_PFObjRelease(struct pfobj_cache *cache)
{
    if(cache->verts)
        PF_FREE(cache->verts);
    if(cache->render_priv)
        PF_FREE(cache->render_priv);
    if(cache->anim_data)
        PF_FREE(cache->anim_data);
}

bool AssetCache_TextureLoad(const char *src_name, uint64_t tag, struct texture_cache *out)
{
    char rpath[MAX_PATH_LEN], bpath[MAX_PATH_LEN];
    if(!texref_path(rpath, sizeof(rpath), src_name))
        return false;

    SDL_RWops *rs = SDL_RWFromFile(rpath, "rb");
    if(!rs)
        return false;

    struct texref_hdr rh;
    bool rok = (SDL_RWread(rs, &rh, sizeof(rh), 1) == 1);
    SDL_RWclose(rs);
    if(!rok)
        return false;
    if(memcmp(rh.magic, TEXREF_MAGIC, sizeof(rh.magic)) != 0)
        return false;
    if(rh.version != TEXTURE_VERSION)
        return false;
    if(rh.tag != tag)
        return false;

    texblob_path(bpath, sizeof(bpath), rh.content_hash);
    SDL_RWops *bs = SDL_RWFromFile(bpath, "rb");
    if(!bs)
        return false;

    bool ret = false;
    void *pixels = NULL;
    struct texblob_hdr bh;
    size_t nbytes;

    if(SDL_RWread(bs, &bh, sizeof(bh), 1) != 1)
        goto out;
    if(memcmp(bh.magic, TEXBLOB_MAGIC, sizeof(bh.magic)) != 0)
        goto out;
    if(bh.version != TEXTURE_VERSION)
        goto out;

    nbytes = (size_t)bh.width * bh.height * bh.channels;
    if(nbytes == 0)
        goto out;

    pixels = PF_MALLOC(nbytes);
    if(!pixels)
        goto out;
    if(SDL_RWread(bs, pixels, nbytes, 1) != 1) {
        PF_FREE(pixels);
        goto out;
    }

    out->width = bh.width;
    out->height = bh.height;
    out->channels = bh.channels;
    out->pixels = pixels;
    ret = true;

out:
    SDL_RWclose(bs);
    return ret;
}

bool AssetCache_TextureStore(const char *src_name, uint64_t tag, const struct texture_cache *in)
{
    if(in->width <= 0 || in->height <= 0 || in->channels <= 0 || !in->pixels)
        return false;
    if(strlen(src_name) >= MAX_REL_PATH_LEN)
        return false;

    char rpath[MAX_PATH_LEN], bpath[MAX_PATH_LEN];
    if(!texref_path(rpath, sizeof(rpath), src_name))
        return false;

    size_t nbytes = (size_t)in->width * in->height * in->channels;
    uint64_t hash = content_hash(in->pixels, nbytes);
    texblob_path(bpath, sizeof(bpath), hash);

    /* Write the shared blob only if a byte-identical one isn't already there. */
    SDL_RWops *probe = SDL_RWFromFile(bpath, "rb");
    if(probe) {
        SDL_RWclose(probe);
    }else{
        SDL_RWops *bs = SDL_RWFromFile(bpath, "wb");
        if(!bs)
            return false;

        struct texblob_hdr bh;
        memcpy(bh.magic, TEXBLOB_MAGIC, sizeof(bh.magic));
        bh.version = TEXTURE_VERSION;
        bh.width = in->width;
        bh.height = in->height;
        bh.channels = in->channels;

        bool bok = (SDL_RWwrite(bs, &bh, sizeof(bh), 1) == 1)
                && (SDL_RWwrite(bs, in->pixels, nbytes, 1) == 1);
        SDL_RWclose(bs);
        if(!bok) {
            remove(bpath);
            return false;
        }
    }

    SDL_RWops *rs = SDL_RWFromFile(rpath, "wb");
    if(!rs)
        return false;

    struct texref_hdr rh;
    memcpy(rh.magic, TEXREF_MAGIC, sizeof(rh.magic));
    rh.version = TEXTURE_VERSION;
    rh.tag = tag;
    rh.content_hash = hash;

    bool rok = (SDL_RWwrite(rs, &rh, sizeof(rh), 1) == 1);
    SDL_RWclose(rs);
    if(!rok) {
        remove(rpath);
        return false;
    }
    return true;
}

void AssetCache_TextureRelease(struct texture_cache *cache)
{
    if(cache->pixels)
        PF_FREE(cache->pixels);
}

bool AssetCache_ImpostorLoad(const char *name, uint64_t tag, struct impostor_cache *out)
{
    impostor_index_ensure();

    char file[MAX_REL_PATH_LEN + 8];
    if(!impostor_file(file, sizeof(file), name))
        return false;

    char path[MAX_PATH_LEN];
    impostor_file_path(path, sizeof(path), file);

    SDL_RWops *stream = SDL_RWFromFile(path, "rb");
    if(!stream)
        return false;

    bool ret = false;
    void *pixels = NULL;
    void *depth = NULL;
    void *surface = NULL;
    void *sil = NULL;
    struct impostor_hdr hdr;
    size_t ntexels, nsil;

    if(SDL_RWread(stream, &hdr, sizeof(hdr), 1) != 1)
        goto out;
    if(memcmp(hdr.magic, IMPOSTOR_MAGIC, sizeof(hdr.magic)) != 0)
        goto out;
    if(hdr.version != IMPOSTOR_VERSION)
        goto out;
    if(hdr.tag != tag)
        goto out;

    /* A flattened name can collide; the stored relative path is authoritative. */
    hdr.rel_path[sizeof(hdr.rel_path) - 1] = '\0';
    if(strcmp(hdr.rel_path, name) != 0)
        goto out;

    ntexels = (size_t)hdr.cell_res * hdr.cell_res * hdr.nslices;
    nsil = (size_t)hdr.sil_res * hdr.sil_res * hdr.nslices;
    if(ntexels == 0 || nsil == 0)
        goto out;

    pixels = PF_MALLOC(ntexels * 4);
    depth = PF_MALLOC(ntexels * sizeof(uint16_t));
    surface = PF_MALLOC(ntexels * 4);
    sil = PF_MALLOC(nsil * sizeof(uint16_t));
    if(!pixels || !depth || !surface || !sil)
        goto fail;
    if(SDL_RWread(stream, pixels, ntexels * 4, 1) != 1)
        goto fail;
    if(SDL_RWread(stream, depth, ntexels * sizeof(uint16_t), 1) != 1)
        goto fail;
    if(SDL_RWread(stream, surface, ntexels * 4, 1) != 1)
        goto fail;
    if(SDL_RWread(stream, sil, nsil * sizeof(uint16_t), 1) != 1)
        goto fail;

    {
        struct impostor_entry *entry = impostor_find(file);
        if(entry) {
            entry->used = time(NULL);
            file_touch(path);
        }
    }

    out->cell_res = hdr.cell_res;
    out->nslices = hdr.nslices;
    out->sil_res = hdr.sil_res;
    out->pixels = pixels;
    out->depth = depth;
    out->surface = surface;
    out->sil = sil;
    ret = true;
    goto out;

fail:
    if(pixels)
        PF_FREE(pixels);
    if(depth)
        PF_FREE(depth);
    if(surface)
        PF_FREE(surface);
    if(sil)
        PF_FREE(sil);

out:
    SDL_RWclose(stream);
    return ret;
}

bool AssetCache_ImpostorStore(const char *name, uint64_t tag, const struct impostor_cache *in)
{
    impostor_index_ensure();

    char file[MAX_REL_PATH_LEN + 8];
    if(!impostor_file(file, sizeof(file), name))
        return false;

    char path[MAX_PATH_LEN];
    impostor_file_path(path, sizeof(path), file);

    size_t ntexels = (size_t)in->cell_res * in->cell_res * in->nslices;
    size_t nsil = (size_t)in->sil_res * in->sil_res * in->nslices;
    if(ntexels == 0 || nsil == 0 || !in->pixels || !in->depth || !in->surface || !in->sil)
        return false;

    uint64_t fsize = sizeof(struct impostor_hdr) + ntexels * (4 + sizeof(uint16_t) + 4)
                   + nsil * sizeof(uint16_t);
    if(fsize > impostor_cap_bytes())
        return false;

    SDL_RWops *stream = SDL_RWFromFile(path, "wb");
    if(!stream)
        return false;

    struct impostor_hdr hdr = {0};
    memcpy(hdr.magic, IMPOSTOR_MAGIC, sizeof(hdr.magic));
    hdr.version = IMPOSTOR_VERSION;
    hdr.tag = tag;
    pf_strlcpy(hdr.rel_path, name, sizeof(hdr.rel_path));
    hdr.cell_res = in->cell_res;
    hdr.nslices = in->nslices;
    hdr.sil_res = in->sil_res;

    bool ret = (SDL_RWwrite(stream, &hdr, sizeof(hdr), 1) == 1)
            && (SDL_RWwrite(stream, in->pixels, ntexels * 4, 1) == 1)
            && (SDL_RWwrite(stream, in->depth, ntexels * sizeof(uint16_t), 1) == 1)
            && (SDL_RWwrite(stream, in->surface, ntexels * 4, 1) == 1)
            && (SDL_RWwrite(stream, in->sil, nsil * sizeof(uint16_t), 1) == 1);
    SDL_RWclose(stream);

    /* Don't leave a truncated file behind to be mistaken for a valid entry */
    if(!ret) {
        remove(path);
        return false;
    }

    impostor_track(file, fsize, time(NULL));
    impostor_enforce_bounds(file);
    return true;
}

void AssetCache_ImpostorRelease(struct impostor_cache *cache)
{
    if(cache->pixels)
        PF_FREE(cache->pixels);
    if(cache->depth)
        PF_FREE(cache->depth);
    if(cache->surface)
        PF_FREE(cache->surface);
    if(cache->sil)
        PF_FREE(cache->sil);
}

