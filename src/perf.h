/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2023 Eduard Permyakov 
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
        Perf_Pop(NULL);         \
        return (__VA_ARGS__);   \
    }while(0)

#define PERF_RETURN_VOID()      \
    do{                         \
        Perf_Pop(NULL);         \
        return;                 \
    }while(0)

#define PERF_PUSH(name)         \
    Perf_Push(name)

#define PERF_POP()              \
    Perf_Pop(NULL)

#define PERF_POP_NAME(_ptr)     \
    Perf_Pop(_ptr)

#else

#define PERF_ENTER()
#define PERF_RETURN(...) do {return (__VA_ARGS__); } while(0)
#define PERF_RETURN_VOID(...) do { return; } while(0)
#define PERF_PUSH(name)
#define PERF_POP()
#define PERF_POP_NAME(_ptr)

#endif


#define NFRAMES_LOGGED  (5)


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

void     Perf_Push(const char *name);
void     Perf_Pop(const char **out);

void     Perf_PushGPU(const char *name, uint32_t cookie);
void     Perf_PopGPU(uint32_t cookie);

bool     Perf_IsRoot(void);

/* Note that due to buffering of the frame timing data, the statistics
 * reported will be from NFRAMES_LOGGED ago. The reason for this is that
 * the GPU may be lagging a couple of frames behind the CPU. We want to get
 * far enough ahead so that the GPU is finished with the frame we're 
 * getting the statistics for. This way querying the GPU timestamps doesn't 
 * cause a CPU<->GPU synch, which would negatively impact performance.
 */

/* This returns an array of perf_info structs (one for each thread). They
 * must be 'free'd by the caller. */
size_t   Perf_Report(size_t maxout, struct perf_info **out);
uint32_t Perf_LastFrameMS(void);
uint32_t Perf_CurrFrameMS(void);

/* The following can only be called from the main thread, making sure that 
 * none of the other threads are touching the Perf_ API concurrently */
bool     Perf_Init(void);
void     Perf_Shutdown(void);
bool     Perf_RegisterThread(SDL_threadID tid, const char *name);
void     Perf_BeginTick(void);
void     Perf_FinishTick(void);

#endif

