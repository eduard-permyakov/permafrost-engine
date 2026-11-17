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

#ifndef LOCKFREE_H
#define LOCKFREE_H

#include <SDL_mutex.h>

#include <stdbool.h>
#include <stdint.h>

#if !defined(__GNUC__)
#error "lockfree.h needs the GCC/Clang __atomic builtins"
#endif

#define LF_RLX  __ATOMIC_RELAXED
#define LF_ACQ  __ATOMIC_ACQUIRE
#define LF_REL  __ATOMIC_RELEASE
#define LF_AR   __ATOMIC_ACQ_REL
#define LF_SC   __ATOMIC_SEQ_CST

#define LF_LOAD(_p, _mo)             __atomic_load_n((_p), (_mo))
#define LF_STORE(_p, _v, _mo)        __atomic_store_n((_p), (_v), (_mo))
#define LF_XCHG(_p, _v, _mo)         __atomic_exchange_n((_p), (_v), (_mo))
#define LF_CAS(_p, _e, _d, _s, _f)   __atomic_compare_exchange_n((_p), (_e), (_d), false, (_s), (_f))
#define LF_CASW(_p, _e, _d, _s, _f)  __atomic_compare_exchange_n((_p), (_e), (_d), true, (_s), (_f))
#define LF_FADD(_p, _v, _mo)         __atomic_fetch_add((_p), (_v), (_mo))
#define LF_FSUB(_p, _v, _mo)         __atomic_fetch_sub((_p), (_v), (_mo))
#define LF_FOR(_p, _v, _mo)          __atomic_fetch_or((_p), (_v), (_mo))
#define LF_FAND(_p, _v, _mo)         __atomic_fetch_and((_p), (_v), (_mo))
#define LF_FENCE(_mo)                __atomic_thread_fence(_mo)

#define LF_CACHELINE                 (64)
#define LF_ALIGNED                   __attribute__((aligned(LF_CACHELINE)))
#define LF_NIL                       (0xffffffffu)

static inline void lf_relax(void)
{
    __builtin_ia32_pause();
}

/*****************************************************************************/
/* Bounded multi-producer multi-consumer FIFO (Vyukov)                       */
/*****************************************************************************/

/* A cell's sequence number says whose turn it is: 'pos' for the producer of
 * lap 'pos', 'pos + 1' for its consumer and 'pos + capacity' for the producer
 * of the next lap. Its release store hands the value over; the acquire load
 * on the other side receives it.
 */
struct lf_cell{
    uint64_t seq;
    uint64_t val;
};

struct lf_ring{
    uint64_t        enq LF_ALIGNED;
    uint64_t        deq LF_ALIGNED;
    struct lf_cell *cells LF_ALIGNED;
    uint64_t        mask;
};

static inline void lf_ring_init(struct lf_ring *ring, struct lf_cell *cells, uint64_t capacity)
{
    ring->enq = 0;
    ring->deq = 0;
    ring->cells = cells;
    ring->mask = capacity - 1;
    for(uint64_t i = 0; i < capacity; i++) {
        cells[i].seq = i;
        cells[i].val = 0;
    }
}

static inline bool lf_ring_push(struct lf_ring *ring, uint64_t val)
{
    uint64_t pos = LF_LOAD(&ring->enq, LF_RLX);
    struct lf_cell *cell;
    for(;;) {
        cell = &ring->cells[pos & ring->mask];
        uint64_t seq = LF_LOAD(&cell->seq, LF_ACQ);
        int64_t dif = (int64_t)(seq - pos);
        if(dif == 0) {
            if(LF_CASW(&ring->enq, &pos, pos + 1, LF_RLX, LF_RLX))
                break;
        }else if(dif < 0) {
            return false;
        }else{
            pos = LF_LOAD(&ring->enq, LF_RLX);
        }
    }
    LF_STORE(&cell->val, val, LF_RLX);
    LF_STORE(&cell->seq, pos + 1, LF_REL);
    return true;
}

static inline bool lf_ring_pop(struct lf_ring *ring, uint64_t *out)
{
    uint64_t pos = LF_LOAD(&ring->deq, LF_RLX);
    struct lf_cell *cell;
    for(;;) {
        cell = &ring->cells[pos & ring->mask];
        uint64_t seq = LF_LOAD(&cell->seq, LF_ACQ);
        int64_t dif = (int64_t)(seq - (pos + 1));
        if(dif == 0) {
            if(LF_CASW(&ring->deq, &pos, pos + 1, LF_RLX, LF_RLX))
                break;
        }else if(dif < 0) {
            return false;
        }else{
            pos = LF_LOAD(&ring->deq, LF_RLX);
        }
    }
    *out = LF_LOAD(&cell->val, LF_RLX);
    LF_STORE(&cell->seq, pos + ring->mask + 1, LF_REL);
    return true;
}

/* True once a push has claimed its slot, before the value is readable: a
 * consumer that sees it must retry the pop rather than give up.
 */
static inline bool lf_ring_maybe_nonempty(struct lf_ring *ring)
{
    return LF_LOAD(&ring->enq, LF_RLX) != LF_LOAD(&ring->deq, LF_RLX);
}

static inline uint64_t lf_ring_size_hint(struct lf_ring *ring)
{
    uint64_t deq = LF_LOAD(&ring->deq, LF_RLX);
    uint64_t enq = LF_LOAD(&ring->enq, LF_RLX);
    return (int64_t)(enq - deq) > 0 ? enq - deq : 0;
}

/*****************************************************************************/
/* Stack of indices with a tagged head (Treiber)                             */
/*****************************************************************************/

/* The links live in a caller array indexed like the entries. The head packs
 * the top index with a tag bumped by every change, so that a pop which read a
 * link before the entry was taken and pushed back cannot succeed (ABA).
 */
struct lf_istack{
    uint64_t  head LF_ALIGNED;
    uint32_t *links;
};

static inline void lf_istack_init(struct lf_istack *stack, uint32_t *links)
{
    stack->head = LF_NIL;
    stack->links = links;
}

static inline void lf_istack_push(struct lf_istack *stack, uint32_t idx)
{
    uint64_t head = LF_LOAD(&stack->head, LF_RLX);
    uint64_t next;
    do{
        LF_STORE(&stack->links[idx], (uint32_t)head, LF_RLX);
        next = (((head >> 32) + 1) << 32) | idx;
    }while(!LF_CASW(&stack->head, &head, next, LF_REL, LF_RLX));
}

static inline uint32_t lf_istack_pop(struct lf_istack *stack)
{
    uint64_t head = LF_LOAD(&stack->head, LF_ACQ);
    uint64_t next;
    do{
        uint32_t idx = (uint32_t)head;
        if(idx == LF_NIL)
            return LF_NIL;
        uint32_t link = LF_LOAD(&stack->links[idx], LF_RLX);
        next = (((head >> 32) + 1) << 32) | link;
    }while(!LF_CASW(&stack->head, &head, next, LF_ACQ, LF_ACQ));
    return (uint32_t)head;
}

/*****************************************************************************/
/* Parking a thread                                                          */
/*****************************************************************************/

/* A thread announces it will sleep, re-checks its condition, then sleeps.
 * Whoever swaps the state from 1 to 0 owns the one wake that matches it: an
 * unparker posts the semaphore, a cancelling sleeper withdraws, and a sleeper
 * that loses the swap absorbs the post that is coming. The semaphore never
 * holds a stale count.
 */
struct lf_parker{
    int     state LF_ALIGNED;
    SDL_sem *sem;
};

static inline void lf_parker_prepare(struct lf_parker *parker)
{
    LF_STORE(&parker->state, 1, LF_SC);
}

/* Returns true when an unparker got there first and its wake was absorbed */
static inline bool lf_parker_cancel(struct lf_parker *parker)
{
    if(LF_XCHG(&parker->state, 0, LF_SC) == 1)
        return false;
    SDL_SemWait(parker->sem);
    return true;
}

/* Returns true when woken, false when the timeout (ms, < 0 for none) ran out */
static inline bool lf_parker_wait(struct lf_parker *parker, int timeout_ms)
{
    if(timeout_ms < 0) {
        SDL_SemWait(parker->sem);
        return true;
    }
    if(SDL_SemWaitTimeout(parker->sem, timeout_ms) == 0)
        return true;
    return lf_parker_cancel(parker);
}

/* A plain load decides 'not parked': a sleeper that prepares after it re-checks
 * the condition the caller published before calling.
 */
static inline bool lf_parker_unpark(struct lf_parker *parker)
{
    if(LF_LOAD(&parker->state, LF_SC) != 1)
        return false;
    if(LF_XCHG(&parker->state, 0, LF_SC) != 1)
        return false;
    SDL_SemPost(parker->sem);
    return true;
}

#endif

