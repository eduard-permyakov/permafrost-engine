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

#define LF_CACHELINE                 (64)
#define LF_NIL                       (0xffffffffu)

#if defined(__GNUC__)

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

/* Goes ahead of a member's or a variable's type, or after the 'struct' keyword */
#define LF_ALIGNED                   __attribute__((aligned(LF_CACHELINE)))

static inline void lf_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

#elif defined(_MSC_VER)

/* Visual Studio has no atomics that take an ordering for a plain object, so
 * each operation is built from the Interlocked intrinsics, one family per
 * type, picked by _Generic (which needs /std:c11 or later). An Interlocked
 * operation is sequentially consistent: it serves the read-modify-write
 * operations and the sequentially consistent store, and plain loads and
 * stores serve the rest.
 */
#include <intrin.h>

#if !defined(_M_X64) && !defined(_M_ARM64)
#error "lockfree.h supports Visual Studio on x64 and ARM64 only"
#endif

enum{ LF_RLX, LF_ACQ, LF_REL, LF_AR, LF_SC };

#define LF_ALIGNED                   __declspec(align(LF_CACHELINE))

#if defined(_M_ARM64)
#define LF_PLAIN_LOAD(_w, _p)        __iso_volatile_load##_w(_p)
#define LF_PLAIN_STORE(_w, _p, _v)   __iso_volatile_store##_w((_p), (_v))
#define LF_BARRIER()                 __dmb(_ARM64_BARRIER_ISH)
#else
/* On x86 an aligned load or store is atomic, and the processor keeps every
 * ordering but a load running ahead of an earlier store
 */
#define LF_PLAIN_LOAD(_w, _p)        (*(_p))
#define LF_PLAIN_STORE(_w, _p, _v)   (*(_p) = (_v))
#define LF_BARRIER()                 ((void)0)
#endif

static __forceinline void lf_relax(void)
{
#if defined(_M_ARM64)
    __yield();
#else
    _mm_pause();
#endif
}

static __forceinline void lf_fence(int mo)
{
#if defined(_M_ARM64)
    if(mo != LF_RLX)
        __dmb(_ARM64_BARRIER_ISH);
#else
    if(mo == LF_SC)
        _mm_mfence();
#endif
}

/* _s: the family's suffix, _t: its type, _w: its width in bits, _it: the
 * Interlocked type of that width, _x: the suffix of its Interlocked names
 */
#define LF_DEFINE(_s, _t, _w, _it, _x)                                                          \
    static __forceinline _t lf_load_##_s(const volatile _t *p, int mo)                          \
    {                                                                                           \
        _t ret = (_t)LF_PLAIN_LOAD(_w, (const volatile _it*)p);                                 \
        if(mo != LF_RLX)                                                                        \
            LF_BARRIER();                                                                       \
        return ret;                                                                             \
    }                                                                                           \
    static __forceinline _t lf_xchg_##_s(volatile _t *p, _t v, int mo)                          \
    {                                                                                           \
        (void)mo;                                                                               \
        return (_t)_InterlockedExchange##_x((volatile _it*)p, (_it)v);                          \
    }                                                                                           \
    static __forceinline void lf_store_##_s(volatile _t *p, _t v, int mo)                       \
    {                                                                                           \
        if(mo == LF_SC) {                                                                       \
            _InterlockedExchange##_x((volatile _it*)p, (_it)v);                                 \
            return;                                                                             \
        }                                                                                       \
        if(mo != LF_RLX)                                                                        \
            LF_BARRIER();                                                                       \
        LF_PLAIN_STORE(_w, (volatile _it*)p, (_it)v);                                           \
    }                                                                                           \
    static __forceinline bool lf_cas_##_s(volatile _t *p, _t *expected, _t desired)             \
    {                                                                                           \
        _it want = (_it)*expected;                                                              \
        _it had = _InterlockedCompareExchange##_x((volatile _it*)p, (_it)desired, want);        \
        if(had == want)                                                                         \
            return true;                                                                        \
        *expected = (_t)had;                                                                    \
        return false;                                                                           \
    }                                                                                           \
    static __forceinline _t lf_fadd_##_s(volatile _t *p, _t v, int mo)                          \
    {                                                                                           \
        (void)mo;                                                                               \
        return (_t)_InterlockedExchangeAdd##_x((volatile _it*)p, (_it)v);                       \
    }                                                                                           \
    static __forceinline _t lf_fsub_##_s(volatile _t *p, _t v, int mo)                          \
    {                                                                                           \
        (void)mo;                                                                               \
        return (_t)_InterlockedExchangeAdd##_x((volatile _it*)p, (_it)(0 - (_it)v));            \
    }                                                                                           \
    static __forceinline _t lf_for_##_s(volatile _t *p, _t v, int mo)                           \
    {                                                                                           \
        (void)mo;                                                                               \
        return (_t)_InterlockedOr##_x((volatile _it*)p, (_it)v);                                \
    }                                                                                           \
    static __forceinline _t lf_fand_##_s(volatile _t *p, _t v, int mo)                          \
    {                                                                                           \
        (void)mo;                                                                               \
        return (_t)_InterlockedAnd##_x((volatile _it*)p, (_it)v);                               \
    }

LF_DEFINE(b8,   bool,               8,  char,    8)
LF_DEFINE(c8,   char,               8,  char,    8)
LF_DEFINE(i8,   signed char,        8,  char,    8)
LF_DEFINE(u8,   unsigned char,      8,  char,    8)
LF_DEFINE(i16,  short,              16, short,   16)
LF_DEFINE(u16,  unsigned short,     16, short,   16)
LF_DEFINE(i32,  int,                32, long,    )
LF_DEFINE(u32,  unsigned int,       32, long,    )
LF_DEFINE(l32,  long,               32, long,    )
LF_DEFINE(ul32, unsigned long,      32, long,    )
LF_DEFINE(i64,  long long,          64, __int64, 64)
LF_DEFINE(u64,  unsigned long long, 64, __int64, 64)

/* Pointers of every type share one family; they have no arithmetic */
static __forceinline void *lf_load_ptr(void *const volatile *p, int mo)
{
    return (void*)lf_load_u64((const volatile unsigned long long*)p, mo);
}

static __forceinline void *lf_xchg_ptr(void *volatile *p, void *v, int mo)
{
    (void)mo;
    return _InterlockedExchangePointer(p, v);
}

static __forceinline void lf_store_ptr(void *volatile *p, void *v, int mo)
{
    lf_store_u64((volatile unsigned long long*)p, (unsigned long long)v, mo);
}

static __forceinline bool lf_cas_ptr(void *volatile *p, void **expected, void *desired)
{
    void *want = *expected;
    void *had = _InterlockedCompareExchangePointer(p, desired, want);
    if(had == want)
        return true;
    *expected = had;
    return false;
}

#define LF_FAMILY(_p, _op) _Generic(*(_p),                                                      \
    bool: lf_##_op##_b8, char: lf_##_op##_c8,                                                   \
    signed char: lf_##_op##_i8, unsigned char: lf_##_op##_u8,                                   \
    short: lf_##_op##_i16, unsigned short: lf_##_op##_u16,                                      \
    int: lf_##_op##_i32, unsigned int: lf_##_op##_u32,                                          \
    long: lf_##_op##_l32, unsigned long: lf_##_op##_ul32,                                       \
    long long: lf_##_op##_i64, unsigned long long: lf_##_op##_u64,                              \
    default: lf_##_op##_ptr)

/* Arithmetic has no pointer family: a pointer argument fails to compile */
#define LF_FAMILY_INT(_p, _op) _Generic(*(_p),                                                  \
    bool: lf_##_op##_b8, char: lf_##_op##_c8,                                                   \
    signed char: lf_##_op##_i8, unsigned char: lf_##_op##_u8,                                   \
    short: lf_##_op##_i16, unsigned short: lf_##_op##_u16,                                      \
    int: lf_##_op##_i32, unsigned int: lf_##_op##_u32,                                          \
    long: lf_##_op##_l32, unsigned long: lf_##_op##_ul32,                                       \
    long long: lf_##_op##_i64, unsigned long long: lf_##_op##_u64)

#define LF_LOAD(_p, _mo)             LF_FAMILY(_p, load)((void*)(_p), (_mo))
#define LF_STORE(_p, _v, _mo)        LF_FAMILY(_p, store)((void*)(_p), (_v), (_mo))
#define LF_XCHG(_p, _v, _mo)         LF_FAMILY(_p, xchg)((void*)(_p), (_v), (_mo))
#define LF_CAS(_p, _e, _d, _s, _f)   ((void)(_s), (void)(_f), LF_FAMILY(_p, cas)((void*)(_p), (void*)(_e), (_d)))
#define LF_CASW(_p, _e, _d, _s, _f)  LF_CAS(_p, _e, _d, _s, _f)
#define LF_FADD(_p, _v, _mo)         LF_FAMILY_INT(_p, fadd)((void*)(_p), (_v), (_mo))
#define LF_FSUB(_p, _v, _mo)         LF_FAMILY_INT(_p, fsub)((void*)(_p), (_v), (_mo))
#define LF_FOR(_p, _v, _mo)          LF_FAMILY_INT(_p, for)((void*)(_p), (_v), (_mo))
#define LF_FAND(_p, _v, _mo)         LF_FAMILY_INT(_p, fand)((void*)(_p), (_v), (_mo))
#define LF_FENCE(_mo)                lf_fence(_mo)

#else
#error "lockfree.h needs the GCC/Clang __atomic builtins or the Visual Studio Interlocked intrinsics"
#endif

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
    LF_ALIGNED uint64_t        enq;
    LF_ALIGNED uint64_t        deq;
    LF_ALIGNED struct lf_cell *cells;
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
    LF_ALIGNED uint64_t  head;
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
    LF_ALIGNED int     state;
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

