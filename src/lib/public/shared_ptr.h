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

#ifndef SHARED_PTR_H
#define SHARED_PTR_H

#include <SDL_atomic.h>
#include <stddef.h>

/***********************************************************************************************/

/* An intrusive atomic reference count. Embed 'struct sharedptr_header' as the first member of
 * any heap-allocated object shared between threads, then manage its lifetime with sp_init /
 * sp_retain / sp_release. The final sp_release runs the type-erased destructor, which owns
 * tearing down and freeing the object itself.
 */

struct sharedptr_header{
    SDL_atomic_t   refcount;
    void         (*destroy)(void *owner);
};

#define SHARED_PTR_HEADER  struct sharedptr_header sp

#define SHARED_PTR_GLUE_(a, b)  a##b
#define SHARED_PTR_GLUE(a, b)   SHARED_PTR_GLUE_(a, b)

/* Compile-time guard that 'member' sits at offset zero of 'type', the contract that lets the
 * sp_* operations work through a void*. Place beside the struct definition at file scope.
 */
#define SHARED_PTR_ASSERT_LAYOUT(type, member)                            \
    typedef char SHARED_PTR_GLUE(sharedptr_layout_check_, __LINE__)        \
        [offsetof(type, member) == 0 ? 1 : -1]

/***********************************************************************************************/

/* Initialise a freshly allocated object with a single owning reference. */
static inline void *sp_init(void *owner, void (*destroy)(void *owner))
{
    struct sharedptr_header *sp = owner;
    sp->destroy = destroy;
    SDL_AtomicSet(&sp->refcount, 1);
    return owner;
}

/* Acquire another owning reference. Returns 'owner' for convenient assignment. */
static inline void *sp_retain(void *owner)
{
    struct sharedptr_header *sp = owner;
    if(!owner)
        return NULL;

    int old;
    do {
        old = SDL_AtomicGet(&sp->refcount);
    } while(!SDL_AtomicCAS(&sp->refcount, old, old + 1));
    return owner;
}

/* Release an owning reference; the final one destroys the object. */
static inline void sp_release(void *owner)
{
    struct sharedptr_header *sp = owner;
    if(!owner)
        return;

    /* Publish this thread's writes before the count drops, so the thread that
     * observes the final reference is guaranteed to see them.
     */
    SDL_MemoryBarrierRelease();

    int old;
    do {
        old = SDL_AtomicGet(&sp->refcount);
    } while(!SDL_AtomicCAS(&sp->refcount, old, old - 1));

    if(old == 1) {
        /* Acquire the release sequence of every prior sp_release so that the
         * destructor happens-after all of them.
         */
        SDL_MemoryBarrierAcquire();
        sp->destroy(owner);
    }
}

/* Snapshot the live reference count. Advisory only; for eviction checks on the owning thread. */
static inline int sp_refcount(const void *owner)
{
    const struct sharedptr_header *sp = owner;
    if(!owner)
        return 0;
    return SDL_AtomicGet((SDL_atomic_t*)&sp->refcount);
}

#endif /* SHARED_PTR_H */
