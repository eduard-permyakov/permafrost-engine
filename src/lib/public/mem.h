/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2021-2026 Eduard Permyakov
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

#ifndef MEM_H
#define MEM_H

#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef _MSC_VER
#include <malloc.h>
#endif

/*****************************************************************************/
/* MEMORY ACCOUNTING                                                         */
/*****************************************************************************/

/* Engine-level system identifiers. Subsystem ids are private to each system
 * (a plain uint16_t in the range [0, MEM_SUB_MAX_PER_SYS)). Accounting is
 * compiled out entirely under NDEBUG. */

enum mem_system{
    MEM_SYS_UNKNOWN = 0,

    /* Systems with subsystems */
    MEM_SYS_ANIM,
    MEM_SYS_AUDIO,
    MEM_SYS_GAME,
    MEM_SYS_LIB,
    MEM_SYS_MAP,
    MEM_SYS_NAV,
    MEM_SYS_PHYS,
    MEM_SYS_RENDER,
    MEM_SYS_SCRIPT,

    /* Single-file systems */
    MEM_SYS_ASSET_LOAD,
    MEM_SYS_CAM_CONTROL,
    MEM_SYS_CAMERA,
    MEM_SYS_CURSOR,
    MEM_SYS_ENTITY,
    MEM_SYS_EVENT,
    MEM_SYS_LOADING_SCREEN,
    MEM_SYS_MAIN,
    MEM_SYS_PERF,
    MEM_SYS_PF_MATH,
    MEM_SYS_SCENE,
    MEM_SYS_SCHED,
    MEM_SYS_SESSION,
    MEM_SYS_SETTINGS,
    MEM_SYS_SPRITE,
    MEM_SYS_TASK,
    MEM_SYS_UI,

    /* Embedded Python runtime (pymalloc arena footprint) */
    MEM_SYS_PYTHON,

    MEM_SYS_COUNT
};

#define MEM_SUB_MAX_PER_SYS 32

enum mem_sub_anim{
    MEM_SUB_ANIM_DISPATCH = 0,
    MEM_SUB_ANIM_ASSET_LOAD,
    MEM_SUB_ANIM_TEXTURE
};

enum mem_sub_audio{
    MEM_SUB_AUDIO_DISPATCH = 0,
    MEM_SUB_AUDIO_EFFECT
};

enum mem_sub_game{
    MEM_SUB_GAME_AUTOMATION = 0,
    MEM_SUB_GAME_BUILDER,
    MEM_SUB_GAME_BUILDING,
    MEM_SUB_GAME_CLEARPATH,
    MEM_SUB_GAME_COMBAT,
    MEM_SUB_GAME_DISPATCH,
    MEM_SUB_GAME_FOG_OF_WAR,
    MEM_SUB_GAME_FORMATION,
    MEM_SUB_GAME_GARRISON,
    MEM_SUB_GAME_HARVESTER,
    MEM_SUB_GAME_MOVEMENT,
    MEM_SUB_GAME_POPULATION,
    MEM_SUB_GAME_POSITION,
    MEM_SUB_GAME_REGION,
    MEM_SUB_GAME_RESOURCE,
    MEM_SUB_GAME_SELECTION,
    MEM_SUB_GAME_STORAGE_SITE,
    MEM_SUB_GAME_TIMER_EVENTS
};

enum mem_sub_lib{
    MEM_SUB_LIB_ATTR = 0,
    MEM_SUB_LIB_NK_FILE_BROWSER,
    MEM_SUB_LIB_NOISE,
    MEM_SUB_LIB_PF_MALLOC,
    MEM_SUB_LIB_PF_STRING,
    MEM_SUB_LIB_SDL_VEC_RWOPS,
    MEM_SUB_LIB_STALLOC,
    MEM_SUB_LIB_STRING_INTERN
};

enum mem_sub_map{
    MEM_SUB_MAP_FOLIAGE = 0,
    MEM_SUB_MAP_ASSET_LOAD,
    MEM_SUB_MAP_DISPATCH,
    MEM_SUB_MAP_MINIMAP,
    MEM_SUB_MAP_RAYCAST,
    MEM_SUB_MAP_TILE
};

enum mem_sub_nav{
    MEM_SUB_NAV_A_STAR = 0,
    MEM_SUB_NAV_FIELD,
    MEM_SUB_NAV_FIELDCACHE,
    MEM_SUB_NAV_DISPATCH
};

enum mem_sub_phys{
    MEM_SUB_PHYS_COLLISION = 0,
    MEM_SUB_PHYS_PROJECTILE
};

enum mem_sub_render{
    MEM_SUB_RENDER_DISPATCH = 0,
    MEM_SUB_RENDER_ASSET_LOAD,
    MEM_SUB_RENDER_GL_ANIM,
    MEM_SUB_RENDER_GL_BATCH,
    MEM_SUB_RENDER_GL_FOLIAGE,
    MEM_SUB_RENDER_GL_IMAGE_QUILT,
    MEM_SUB_RENDER_GL_MINIMAP,
    MEM_SUB_RENDER_GL_MOVEMENT,
    MEM_SUB_RENDER_GL_PERF,
    MEM_SUB_RENDER_GL_POSITION,
    MEM_SUB_RENDER_GL_RENDER,
    MEM_SUB_RENDER_GL_RINGBUFFER,
    MEM_SUB_RENDER_GL_SHADER,
    MEM_SUB_RENDER_GL_SHADOWS,
    MEM_SUB_RENDER_GL_SKYBOX,
    MEM_SUB_RENDER_GL_SPRITE,
    MEM_SUB_RENDER_GL_STATE,
    MEM_SUB_RENDER_GL_STATUSBAR,
    MEM_SUB_RENDER_GL_SWAPCHAIN,
    MEM_SUB_RENDER_GL_TERRAIN,
    MEM_SUB_RENDER_GL_TEXTURE,
    MEM_SUB_RENDER_GL_TILE,
    MEM_SUB_RENDER_GL_UI,
    MEM_SUB_RENDER_GL_WATER
};

enum mem_sub_script{
    MEM_SUB_SCRIPT_CAMERA = 0,
    MEM_SUB_SCRIPT_CONSOLE,
    MEM_SUB_SCRIPT_CONSTANTS,
    MEM_SUB_SCRIPT_DISPATCH,
    MEM_SUB_SCRIPT_ENTITY,
    MEM_SUB_SCRIPT_ERROR,
    MEM_SUB_SCRIPT_PERF,
    MEM_SUB_SCRIPT_PICKLE,
    MEM_SUB_SCRIPT_REGION,
    MEM_SUB_SCRIPT_TASK,
    MEM_SUB_SCRIPT_TILE,
    MEM_SUB_SCRIPT_TRAVERSE,
    MEM_SUB_SCRIPT_UI,
    MEM_SUB_SCRIPT_UI_STYLE
};

enum mem_sub_python{
    MEM_SUB_PYTHON_ARENAS = 0,
    MEM_SUB_PYTHON_LARGE_ALLOCS
};

struct mem_accounting{
    int64_t sys_bytes[MEM_SYS_COUNT];
    int64_t sys_count[MEM_SYS_COUNT];
    int64_t sub_bytes[MEM_SYS_COUNT][MEM_SUB_MAX_PER_SYS];
    int64_t sub_count[MEM_SYS_COUNT][MEM_SUB_MAX_PER_SYS];
};

#ifndef MEM_FILE_SYS
#define MEM_FILE_SYS MEM_SYS_UNKNOWN
#endif
#ifndef MEM_FILE_SUB
#define MEM_FILE_SUB 0
#endif

bool        Mem_Init(void);
void        Mem_Shutdown(void);
void        Mem_GetAccounting(struct mem_accounting *out);
const char *Mem_SysName(uint16_t sys);
const char *Mem_SubName(uint16_t sys, uint16_t sub);

void        Mem_AuditTaggedBytes(struct mem_accounting *out);
void        Mem_SetPythonStats(int64_t arena_bytes, int64_t arena_count,
                               int64_t raw_bytes, int64_t raw_count);

#ifndef NDEBUG

void *Mem_Malloc        (size_t n);
void *Mem_Calloc        (size_t c, size_t n);
void *Mem_Realloc       (void *p, size_t n);
void *Mem_MallocTagged  (size_t n, uint16_t sys, uint16_t sub);
void *Mem_CallocTagged  (size_t c, size_t n, uint16_t sys, uint16_t sub);
void *Mem_ReallocTagged (void *p, size_t n, uint16_t sys, uint16_t sub);
void  Mem_Free          (void *p);

void  Mem_PushScope    (uint16_t sys, uint16_t sub);
void  Mem_PopScope     (void);

#define PF_MALLOC(_n)                          Mem_Malloc(_n)
#define PF_CALLOC(_c, _n)                      Mem_Calloc((_c), (_n))
#define PF_REALLOC(_p, _n)                     Mem_Realloc((_p), (_n))
#define PF_MALLOC_TAGGED(_n, _sys, _sub)       Mem_MallocTagged((_n), (_sys), (_sub))
#define PF_CALLOC_TAGGED(_c, _n, _sys, _sub)   Mem_CallocTagged((_c), (_n), (_sys), (_sub))
#define PF_REALLOC_TAGGED(_p, _n, _sys, _sub)  Mem_ReallocTagged((_p), (_n), (_sys), (_sub))

#define MEM_PUSH_SCOPE(_sys, _sub)       Mem_PushScope((_sys), (_sub))
#define MEM_POP_SCOPE()                  Mem_PopScope()

#define PF_FREE(...)                                    \
    do{                                                 \
        Mem_Free((void*)__VA_ARGS__);                   \
        __VA_ARGS__ = (void*)((uintptr_t)0xDEADBEEF);   \
    }while(0)

#else /* NDEBUG */

#define PF_MALLOC(_n)                          malloc(_n)
#define PF_CALLOC(_c, _n)                      calloc((_c), (_n))
#define PF_REALLOC(_p, _n)                     realloc((_p), (_n))
#define PF_MALLOC_TAGGED(_n, _sys, _sub)       (((void)(_sys), (void)(_sub), malloc(_n)))
#define PF_CALLOC_TAGGED(_c, _n, _sys, _sub)   (((void)(_sys), (void)(_sub), calloc((_c), (_n))))
#define PF_REALLOC_TAGGED(_p, _n, _sys, _sub)  (((void)(_sys), (void)(_sub), realloc((_p), (_n))))

#define MEM_PUSH_SCOPE(_sys, _sub)       ((void)0)
#define MEM_POP_SCOPE()                  ((void)0)

#define PF_FREE(...)                                    \
    do{                                                 \
        free((void*)__VA_ARGS__);                       \
        __VA_ARGS__ = (void*)((uintptr_t)0xDEADBEEF);   \
    }while(0)

#endif /* NDEBUG */

#endif /* MEM_H */

#ifdef _MSC_VER
#define STALLOC(_type, _name, _size)                    \
    _type *_name = _malloca(sizeof(_type) * (_size))
#else
#define STALLOC(_type, _name, _size)                    \
    _type _name[_size]
#endif

#ifdef _MSC_VER
#define STFREE(_ptr) _freea(_ptr)
#else
#define STFREE(_ptr) /* no-op */
#endif
