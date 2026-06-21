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

#include "public/mem.h"

#include <SDL.h>
#include <mimalloc.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#define ARR_SIZE(a)         (sizeof(a)/sizeof((a)[0]))

#ifndef NDEBUG

#define MEM_HDR_MAGIC       (0x4D454D31u) /* 'MEM1' */
#define MEM_SCOPE_DEPTH_MAX (16)

/* Prepended to every PF_MALLOC allocation. 16-byte struct keeps the user
 * pointer at 16-byte alignment, matching mimalloc's natural alignment. */
struct mem_hdr{
    uint32_t magic;
    uint32_t size;
    uint16_t sys;
    uint16_t sub;
    uint32_t reserved;
};

struct mem_scope_frame{
    uint16_t sys;
    uint16_t sub;
};

struct mem_scope_stack{
    int                    depth;
    struct mem_scope_frame frames[MEM_SCOPE_DEPTH_MAX];
};

static struct mem_scope_stack *scope_get_or_create(void);
static void                    scope_peek(uint16_t *out_sys, uint16_t *out_sub);
static void                    counter_add(uint16_t sys, uint16_t sub,
                                           int64_t bytes_delta, int64_t count_delta);
static void                   *alloc_with_tag(size_t n, uint16_t sys, uint16_t sub);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static SDL_TLSID s_scope_tls;

static int64_t   s_sys_bytes[MEM_SYS_COUNT];
static int64_t   s_sys_count[MEM_SYS_COUNT];
static int64_t   s_sub_bytes[MEM_SYS_COUNT][MEM_SUB_MAX_PER_SYS];
static int64_t   s_sub_count[MEM_SYS_COUNT][MEM_SUB_MAX_PER_SYS];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/* SDL2 only exposes a 32-bit atomic_add; for our 64-bit counters we CAS on a
 * pointer-sized slot, which is 64 bits on every target we support. 
 */
static int64_t atomic_add64(int64_t *p, int64_t delta)
{
    void **slot = (void**)p;
    void  *old, *next;
    do {
        old  = SDL_AtomicGetPtr(slot);
        next = (void*)((intptr_t)old + (intptr_t)delta);
    } while(!SDL_AtomicCASPtr(slot, old, next));
    return (int64_t)(intptr_t)next;
}

static int64_t atomic_load64(int64_t *p)
{
    return (int64_t)(intptr_t)SDL_AtomicGetPtr((void**)p);
}

static void atomic_store64(int64_t *p, int64_t val)
{
    SDL_AtomicSetPtr((void**)p, (void*)(intptr_t)val);
}

static struct mem_scope_stack *scope_get_or_create(void)
{
    struct mem_scope_stack *s = SDL_TLSGet(s_scope_tls);
    if(s)
        return s;

    s = mi_calloc(1, sizeof *s);
    if(!s)
        return NULL;
    if(SDL_TLSSet(s_scope_tls, s, mi_free) != 0) {
        mi_free(s);
        return NULL;
    }
    return s;
}

static void scope_peek(uint16_t *out_sys, uint16_t *out_sub)
{
    struct mem_scope_stack *s = SDL_TLSGet(s_scope_tls);
    if(!s || s->depth == 0) {
        *out_sys = MEM_SYS_UNKNOWN;
        *out_sub = 0;
        return;
    }
    *out_sys = s->frames[s->depth - 1].sys;
    *out_sub = s->frames[s->depth - 1].sub;
}

static void counter_add(uint16_t sys, uint16_t sub,
                        int64_t bytes_delta, int64_t count_delta)
{
    if(sys >= MEM_SYS_COUNT)
        sys = MEM_SYS_UNKNOWN;
    atomic_add64(&s_sys_bytes[sys], bytes_delta);
    atomic_add64(&s_sys_count[sys], count_delta);
    if(sub < MEM_SUB_MAX_PER_SYS) {
        atomic_add64(&s_sub_bytes[sys][sub], bytes_delta);
        atomic_add64(&s_sub_count[sys][sub], count_delta);
    }
}

static void *alloc_with_tag(size_t n, uint16_t sys, uint16_t sub)
{
    void *raw = mi_malloc(sizeof(struct mem_hdr) + n);
    if(!raw)
        return NULL;

    struct mem_hdr *h = raw;
    h->magic    = MEM_HDR_MAGIC;
    h->size     = (uint32_t)n;
    h->sys      = sys;
    h->sub      = sub;
    h->reserved = 0;

    counter_add(sys, sub, (int64_t)n, 1);
    return (char*)raw + sizeof *h;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Mem_Init(void)
{
    int ret = 1;
    ret &= ((s_scope_tls = SDL_TLSCreate()) != 0);
    return (bool)ret;
}

void Mem_Shutdown(void)
{
    /* SDL_TLS slots are not reclaimable. Per-thread stacks are freed by the
     * destructor passed to SDL_TLSSet when each thread exits. */
}

void Mem_PushScope(uint16_t sys, uint16_t sub)
{
    struct mem_scope_stack *s = scope_get_or_create();
    if(!s || s->depth >= MEM_SCOPE_DEPTH_MAX) {
        assert(0 && "mem scope stack overflow");
        return;
    }
    s->frames[s->depth].sys = sys;
    s->frames[s->depth].sub = sub;
    s->depth++;
}

void Mem_PopScope(void)
{
    struct mem_scope_stack *s = SDL_TLSGet(s_scope_tls);
    if(!s || s->depth == 0) {
        assert(0 && "mem scope stack underflow");
        return;
    }
    s->depth--;
}

void *Mem_Malloc(size_t n)
{
    uint16_t sys, sub;
    scope_peek(&sys, &sub);
    return alloc_with_tag(n, sys, sub);
}

void *Mem_Calloc(size_t c, size_t n)
{
    size_t bytes = c * n;
    void *p = Mem_Malloc(bytes);
    if(p)
        memset(p, 0, bytes);
    return p;
}

void *Mem_Realloc(void *p, size_t n)
{
    if(!p) {
        uint16_t sys, sub;
        scope_peek(&sys, &sub);
        return alloc_with_tag(n, sys, sub);
    }
    if(n == 0) {
        Mem_Free(p);
        return NULL;
    }

    struct mem_hdr *old_h = (struct mem_hdr*)((char*)p - sizeof(struct mem_hdr));
    assert(old_h->magic == MEM_HDR_MAGIC);

    uint16_t sys      = old_h->sys;
    uint16_t sub      = old_h->sub;
    uint32_t old_size = old_h->size;

    void *new_raw = mi_realloc(old_h, sizeof(struct mem_hdr) + n);
    if(!new_raw)
        return NULL;

    struct mem_hdr *new_h = new_raw;
    new_h->magic    = MEM_HDR_MAGIC;
    new_h->size     = (uint32_t)n;
    new_h->sys      = sys;
    new_h->sub      = sub;
    new_h->reserved = 0;

    counter_add(sys, sub, (int64_t)n - (int64_t)old_size, 0);
    return (char*)new_raw + sizeof(struct mem_hdr);
}

void *Mem_MallocTagged(size_t n, uint16_t sys, uint16_t sub)
{
    return alloc_with_tag(n, sys, sub);
}

void *Mem_CallocTagged(size_t c, size_t n, uint16_t sys, uint16_t sub)
{
    size_t bytes = c * n;
    void *p = alloc_with_tag(bytes, sys, sub);
    if(p)
        memset(p, 0, bytes);
    return p;
}

void *Mem_ReallocTagged(void *p, size_t n, uint16_t sys, uint16_t sub)
{
    if(!p)
        return alloc_with_tag(n, sys, sub);
    return Mem_Realloc(p, n);
}

void Mem_Free(void *p)
{
    if(!p)
        return;
    struct mem_hdr *h = (struct mem_hdr*)((char*)p - sizeof(struct mem_hdr));
    assert(h->magic == MEM_HDR_MAGIC);

    counter_add(h->sys, h->sub, -(int64_t)h->size, -1);
    h->magic = 0;
    mi_free(h);
}

static bool audit_block_visitor(const mi_heap_t *heap, const mi_heap_area_t *area,
                                void *block, size_t block_size, void *arg)
{
    (void)heap; (void)area;
    if(block == NULL || block_size < sizeof(struct mem_hdr))
        return true;
    struct mem_hdr *h = block;
    if(h->magic != MEM_HDR_MAGIC)
        return true;
    struct mem_accounting *out = arg;
    uint16_t sys = h->sys < MEM_SYS_COUNT ? h->sys : MEM_SYS_UNKNOWN;
    uint16_t sub = h->sub < MEM_SUB_MAX_PER_SYS ? h->sub : 0;
    out->sys_bytes[sys] += h->size;
    out->sys_count[sys] += 1;
    out->sub_bytes[sys][sub] += h->size;
    out->sub_count[sys][sub] += 1;
    return true;
}

void Mem_AuditTaggedBytes(struct mem_accounting *out)
{
    memset(out, 0, sizeof(*out));
    mi_heap_visit_blocks(mi_heap_get_default(), true, audit_block_visitor, out);
}

void Mem_GetAccounting(struct mem_accounting *out)
{
    size_t i, j;
    for(i = 0; i < MEM_SYS_COUNT; i++) {
        out->sys_bytes[i] = atomic_load64(&s_sys_bytes[i]);
        out->sys_count[i] = atomic_load64(&s_sys_count[i]);
        for(j = 0; j < MEM_SUB_MAX_PER_SYS; j++) {
            out->sub_bytes[i][j] = atomic_load64(&s_sub_bytes[i][j]);
            out->sub_count[i][j] = atomic_load64(&s_sub_count[i][j]);
        }
    }
}

void Mem_SetPythonStats(int64_t arena_bytes, int64_t arena_count,
                        int64_t raw_bytes, int64_t raw_count)
{
    atomic_store64(&s_sys_bytes[MEM_SYS_PYTHON], arena_bytes + raw_bytes);
    atomic_store64(&s_sys_count[MEM_SYS_PYTHON], arena_count + raw_count);
    atomic_store64(&s_sub_bytes[MEM_SYS_PYTHON][MEM_SUB_PYTHON_ARENAS], arena_bytes);
    atomic_store64(&s_sub_count[MEM_SYS_PYTHON][MEM_SUB_PYTHON_ARENAS], arena_count);
    atomic_store64(&s_sub_bytes[MEM_SYS_PYTHON][MEM_SUB_PYTHON_LARGE_ALLOCS], raw_bytes);
    atomic_store64(&s_sub_count[MEM_SYS_PYTHON][MEM_SUB_PYTHON_LARGE_ALLOCS], raw_count);
}

#else /* NDEBUG */

bool Mem_Init(void)
{
    return true;
}

void Mem_Shutdown(void)
{
    /* no-op */
}

void *Mem_Malloc(size_t n)
{
    return mi_malloc(n);
}

void *Mem_Calloc(size_t c, size_t n)
{
    return mi_calloc(c, n);
}

void *Mem_Realloc(void *p, size_t n)
{
    return mi_realloc(p, n);
}

void *Mem_MallocTagged(size_t n, uint16_t sys, uint16_t sub)
{
    (void)sys; (void)sub;
    return mi_malloc(n);
}

void *Mem_CallocTagged(size_t c, size_t n, uint16_t sys, uint16_t sub)
{
    (void)sys; (void)sub;
    return mi_calloc(c, n);
}

void *Mem_ReallocTagged(void *p, size_t n, uint16_t sys, uint16_t sub)
{
    (void)sys; (void)sub;
    return mi_realloc(p, n);
}

void Mem_Free(void *p)
{
    mi_free(p);
}

void Mem_GetAccounting(struct mem_accounting *out)
{
    memset(out, 0, sizeof *out);
}

void Mem_AuditTaggedBytes(struct mem_accounting *out)
{
    memset(out, 0, sizeof *out);
}

void Mem_SetPythonStats(int64_t arena_bytes, int64_t arena_count,
                        int64_t raw_bytes, int64_t raw_count)
{
    (void)arena_bytes;
    (void)arena_count;
    (void)raw_bytes;
    (void)raw_count;
}

#endif /* NDEBUG */

const char *Mem_SysName(uint16_t sys)
{
    static const char *names[MEM_SYS_COUNT] = {
        [MEM_SYS_UNKNOWN]        = "unknown",
        [MEM_SYS_ANIM]           = "anim",
        [MEM_SYS_AUDIO]          = "audio",
        [MEM_SYS_GAME]           = "game",
        [MEM_SYS_LIB]            = "lib",
        [MEM_SYS_MAP]            = "map",
        [MEM_SYS_NAV]            = "nav",
        [MEM_SYS_PHYS]           = "phys",
        [MEM_SYS_RENDER]         = "render",
        [MEM_SYS_SCRIPT]         = "script",
        [MEM_SYS_ASSET_CACHE]    = "asset_cache",
        [MEM_SYS_ASSET_LOAD]     = "asset_load",
        [MEM_SYS_CAM_CONTROL]    = "cam_control",
        [MEM_SYS_CAMERA]         = "camera",
        [MEM_SYS_CURSOR]         = "cursor",
        [MEM_SYS_ENTITY]         = "entity",
        [MEM_SYS_EVENT]          = "event",
        [MEM_SYS_LOADING_SCREEN] = "loading_screen",
        [MEM_SYS_MAIN]           = "main",
        [MEM_SYS_PERF]           = "perf",
        [MEM_SYS_PF_MATH]        = "pf_math",
        [MEM_SYS_SCENE]          = "scene",
        [MEM_SYS_SCHED]          = "sched",
        [MEM_SYS_SESSION]        = "session",
        [MEM_SYS_SETTINGS]       = "settings",
        [MEM_SYS_SPRITE]         = "sprite",
        [MEM_SYS_TASK]           = "task",
        [MEM_SYS_UI]             = "ui",
        [MEM_SYS_PYTHON]         = "python",
    };
    if(sys >= MEM_SYS_COUNT)
        return NULL;
    return names[sys];
}

const char *Mem_SubName(uint16_t sys, uint16_t sub)
{
    static const char *anim_subs[] = {
        [MEM_SUB_ANIM_DISPATCH]   = "dispatch",
        [MEM_SUB_ANIM_ASSET_LOAD] = "asset_load",
        [MEM_SUB_ANIM_TEXTURE]    = "texture",
    };
    static const char *audio_subs[] = {
        [MEM_SUB_AUDIO_DISPATCH] = "dispatch",
        [MEM_SUB_AUDIO_EFFECT]   = "effect",
    };
    static const char *game_subs[] = {
        [MEM_SUB_GAME_AUTOMATION]   = "automation",
        [MEM_SUB_GAME_BUILDER]      = "builder",
        [MEM_SUB_GAME_BUILDING]     = "building",
        [MEM_SUB_GAME_CLEARPATH]    = "clearpath",
        [MEM_SUB_GAME_COMBAT]       = "combat",
        [MEM_SUB_GAME_DISPATCH]     = "dispatch",
        [MEM_SUB_GAME_FOG_OF_WAR]   = "fog_of_war",
        [MEM_SUB_GAME_FORMATION]    = "formation",
        [MEM_SUB_GAME_GARRISON]     = "garrison",
        [MEM_SUB_GAME_HARVESTER]    = "harvester",
        [MEM_SUB_GAME_MOVEMENT]     = "movement",
        [MEM_SUB_GAME_POPULATION]   = "population",
        [MEM_SUB_GAME_POSITION]     = "position",
        [MEM_SUB_GAME_REGION]       = "region",
        [MEM_SUB_GAME_RESOURCE]     = "resource",
        [MEM_SUB_GAME_SELECTION]    = "selection",
        [MEM_SUB_GAME_STORAGE_SITE] = "storage_site",
        [MEM_SUB_GAME_TIMER_EVENTS] = "timer_events",
    };
    static const char *lib_subs[] = {
        [MEM_SUB_LIB_ATTR]            = "attr",
        [MEM_SUB_LIB_NK_FILE_BROWSER] = "nk_file_browser",
        [MEM_SUB_LIB_NOISE]           = "noise",
        [MEM_SUB_LIB_PF_MALLOC]       = "pf_malloc",
        [MEM_SUB_LIB_PF_STRING]       = "pf_string",
        [MEM_SUB_LIB_SDL_VEC_RWOPS]   = "sdl_vec_rwops",
        [MEM_SUB_LIB_STALLOC]         = "stalloc",
        [MEM_SUB_LIB_STRING_INTERN]   = "string_intern",
    };
    static const char *map_subs[] = {
        [MEM_SUB_MAP_FOLIAGE]    = "foliage",
        [MEM_SUB_MAP_ASSET_LOAD] = "asset_load",
        [MEM_SUB_MAP_DISPATCH]   = "dispatch",
        [MEM_SUB_MAP_MINIMAP]    = "minimap",
        [MEM_SUB_MAP_RAYCAST]    = "raycast",
        [MEM_SUB_MAP_TILE]       = "tile",
    };
    static const char *nav_subs[] = {
        [MEM_SUB_NAV_A_STAR]     = "a_star",
        [MEM_SUB_NAV_FIELD]      = "field",
        [MEM_SUB_NAV_FIELDCACHE] = "fieldcache",
        [MEM_SUB_NAV_DISPATCH]   = "dispatch",
    };
    static const char *phys_subs[] = {
        [MEM_SUB_PHYS_COLLISION]  = "collision",
        [MEM_SUB_PHYS_PROJECTILE] = "projectile",
    };
    static const char *render_subs[] = {
        [MEM_SUB_RENDER_DISPATCH]       = "dispatch",
        [MEM_SUB_RENDER_ASSET_LOAD]     = "asset_load",
        [MEM_SUB_RENDER_GL_ANIM]        = "anim",
        [MEM_SUB_RENDER_GL_BATCH]       = "batch",
        [MEM_SUB_RENDER_GL_FOLIAGE]     = "foliage",
        [MEM_SUB_RENDER_GL_IMAGE_QUILT] = "image_quilt",
        [MEM_SUB_RENDER_GL_MEM]         = "mem",
        [MEM_SUB_RENDER_GL_MINIMAP]     = "minimap",
        [MEM_SUB_RENDER_GL_MOVEMENT]    = "movement",
        [MEM_SUB_RENDER_GL_PERF]        = "perf",
        [MEM_SUB_RENDER_GL_POSITION]    = "position",
        [MEM_SUB_RENDER_GL_RENDER]      = "renderer",
        [MEM_SUB_RENDER_GL_RINGBUFFER]  = "ringbuffer",
        [MEM_SUB_RENDER_GL_SHADER]      = "shader",
        [MEM_SUB_RENDER_GL_SHADOWS]     = "shadows",
        [MEM_SUB_RENDER_GL_SKYBOX]      = "skybox",
        [MEM_SUB_RENDER_GL_SPRITE]      = "sprite",
        [MEM_SUB_RENDER_GL_STATE]       = "state",
        [MEM_SUB_RENDER_GL_STATUSBAR]   = "statusbar",
        [MEM_SUB_RENDER_GL_SWAPCHAIN]   = "swapchain",
        [MEM_SUB_RENDER_GL_TERRAIN]     = "terrain",
        [MEM_SUB_RENDER_GL_TEXTURE]     = "texture",
        [MEM_SUB_RENDER_GL_TILE]        = "tile",
        [MEM_SUB_RENDER_GL_UI]          = "ui",
        [MEM_SUB_RENDER_GL_WATER]       = "water",
    };
    static const char *script_subs[] = {
        [MEM_SUB_SCRIPT_CAMERA]    = "camera",
        [MEM_SUB_SCRIPT_CONSOLE]   = "console",
        [MEM_SUB_SCRIPT_CONSTANTS] = "constants",
        [MEM_SUB_SCRIPT_DISPATCH]  = "dispatch",
        [MEM_SUB_SCRIPT_ENTITY]    = "entity",
        [MEM_SUB_SCRIPT_ERROR]     = "error",
        [MEM_SUB_SCRIPT_PERF]      = "perf",
        [MEM_SUB_SCRIPT_PICKLE]    = "pickle",
        [MEM_SUB_SCRIPT_REGION]    = "region",
        [MEM_SUB_SCRIPT_TASK]      = "task",
        [MEM_SUB_SCRIPT_TILE]      = "tile",
        [MEM_SUB_SCRIPT_TRAVERSE]  = "traverse",
        [MEM_SUB_SCRIPT_UI]        = "ui",
        [MEM_SUB_SCRIPT_UI_STYLE]  = "ui_style",
    };
    static const char *python_subs[] = {
        [MEM_SUB_PYTHON_ARENAS]   = "arenas",
        [MEM_SUB_PYTHON_LARGE_ALLOCS] = "large allocations",
    };

    const char **table;
    size_t       n;
    switch(sys) {
    case MEM_SYS_ANIM:   table = anim_subs;   n = ARR_SIZE(anim_subs);   break;
    case MEM_SYS_AUDIO:  table = audio_subs;  n = ARR_SIZE(audio_subs);  break;
    case MEM_SYS_GAME:   table = game_subs;   n = ARR_SIZE(game_subs);   break;
    case MEM_SYS_LIB:    table = lib_subs;    n = ARR_SIZE(lib_subs);    break;
    case MEM_SYS_MAP:    table = map_subs;    n = ARR_SIZE(map_subs);    break;
    case MEM_SYS_NAV:    table = nav_subs;    n = ARR_SIZE(nav_subs);    break;
    case MEM_SYS_PHYS:   table = phys_subs;   n = ARR_SIZE(phys_subs);   break;
    case MEM_SYS_RENDER: table = render_subs; n = ARR_SIZE(render_subs); break;
    case MEM_SYS_SCRIPT: table = script_subs; n = ARR_SIZE(script_subs); break;
    case MEM_SYS_PYTHON: table = python_subs; n = ARR_SIZE(python_subs); break;
    default: return NULL;
    }
    if(sub >= n)
        return NULL;
    return table[sub];
}

