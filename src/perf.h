/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#ifndef PERF_H
#define PERF_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL_thread.h>

#ifndef NDEBUG

#define PERF_ENTER()            \
    do{                         \
        Perf_Push(__func__);    \
    }while(0)

#define PERF_RETURN(...)        \
    do{                         \
        Perf_Pop();             \
        return (__VA_ARGS__);   \
    }while(0)

#define PERF_RETURN_VOID()      \
    do{                         \
        Perf_Pop();             \
        return;                 \
    }while(0)

#else

#define PERF_ENTER()
#define PERF_RETURN(...) do {return (__VA_ARGS__); } while(0)
#define PERF_RETURN_VOID(...) do { return; } while(0)

#endif


void   Perf_Push(const char *name);
void   Perf_Pop(void);

struct perf_info{
    char threadname[64];
    size_t nentries;
    struct{
        const char *funcname; /* borrowed */
        uint64_t    pc_delta;
        double      ms_delta;
        int         parent_idx;
    }entries[];
};

/* This returns an array of perf_info structs (one for each thread). They
 * must be 'free'd by the caller. */
size_t Perf_Report(size_t maxout, struct perf_info **out);

/* The following can only be called from the main thread, making sure that 
 * none of the other threads are touching the Perf_ API concurrently */
bool   Perf_Init(void);
void   Perf_Shutdown(void);
bool   Perf_RegisterThread(SDL_threadID tid, const char *name);
void   Perf_FinishTick(void);

#endif

