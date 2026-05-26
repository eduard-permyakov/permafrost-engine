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
 */

/*
 *  Minimal cross-compiler SIMD helpers: runtime CPU feature detection and a
 *  per-function attribute that enables AVX2 in a single function while the
 *  rest of the TU stays on the baseline ISA. Lets a data structure ship a
 *  scalar fallback alongside an AVX2 specialisation and dispatch at runtime.
 *
 *  Usage:
 *      static int scan_scalar(...)  { ... }
 *      SIMD_TARGET_AVX2
 *      static int scan_avx2(...)    { ... }
 *      g_scan = simd_avx2_supported() ? scan_avx2 : scan_scalar;
 *
 *  Toolchains: GCC/Clang use target("avx2") + __builtin_cpu_supports. MSVC
 *  has no per-function target so we set SIMD_HAS_TARGET_AVX2 = 0 and the
 *  caller skips the AVX2 variant; CPU detection still works via __cpuid.
 *  Other compilers / non-x86: scalar only, simd_avx2_supported() returns 0.
 */

#ifndef SIMD_H
#define SIMD_H

#include <stdbool.h>
#include <stdint.h>

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

/* Intel intrinsics. <immintrin.h> declares __m256i etc. on both GCC/Clang and
 * MSVC. On GCC/Clang, the intrinsics emit AVX2 only inside functions decorated
 * with SIMD_TARGET_AVX2; on MSVC they require /arch:AVX2 for the TU and so the
 * AVX2 variants are not built when SIMD_HAS_TARGET_AVX2 is 0.
 */
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#  include <immintrin.h>
#endif

/* Portable count-trailing-zeros and popcount. CTZ result is undefined if
 * input is 0.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define SIMD_CTZ32(x)      ((unsigned)__builtin_ctz(x))
#  define SIMD_CTZ64(x)      ((unsigned)__builtin_ctzll(x))
#  define SIMD_POPCOUNT32(x) ((unsigned)__builtin_popcount(x))
#elif defined(_MSC_VER)
static inline unsigned simd_ctz32(unsigned x)
{
    unsigned long i;
    _BitScanForward(&i, x);
    return (unsigned)i;
}
static inline unsigned simd_ctz64(unsigned long long x)
{
    unsigned long i;
    _BitScanForward64(&i, x);
    return (unsigned)i;
}
#  define SIMD_CTZ32(x)      simd_ctz32(x)
#  define SIMD_CTZ64(x)      simd_ctz64(x)
#  define SIMD_POPCOUNT32(x) ((unsigned)__popcnt(x))
#else
static inline unsigned simd_ctz32(unsigned x)
{
    unsigned i = 0;
    while(!(x & 1u)) { x >>= 1; i++; }
    return i;
}
static inline unsigned simd_ctz64(unsigned long long x)
{
    unsigned i = 0;
    while(!(x & 1ull)) { x >>= 1; i++; }
    return i;
}
static inline unsigned simd_popcount32(unsigned x)
{
    unsigned c = 0;
    while(x) { x &= x - 1u; c++; }
    return c;
}
#  define SIMD_CTZ32(x)      simd_ctz32(x)
#  define SIMD_CTZ64(x)      simd_ctz64(x)
#  define SIMD_POPCOUNT32(x) simd_popcount32(x)
#endif

/* SIMD_TARGET_* : per-function attribute enabling a wider ISA in the
 * decorated function on toolchains that support it. When the toolchain has no
 * equivalent the macro is empty and SIMD_HAS_TARGET_* is 0; callers should
 * gate the wider variant accordingly.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define SIMD_TARGET_AVX2    __attribute__((target("avx2")))
#  define SIMD_HAS_TARGET_AVX2 1
#  define SIMD_TARGET_AVX512F __attribute__((target("avx512f,avx512vl,avx512bw,avx512dq")))
#  define SIMD_HAS_TARGET_AVX512F 1
#else
#  define SIMD_TARGET_AVX2
#  define SIMD_HAS_TARGET_AVX2 0
#  define SIMD_TARGET_AVX512F
#  define SIMD_HAS_TARGET_AVX512F 0
#endif

/* Returns non-zero if the current CPU supports AVX2. Cached after the first
 * call; the race between concurrent first-callers is harmless since they
 * all compute and write the same value.
 */
static inline int simd_avx2_supported(void)
{
    static int s_cached = -1;
    if(s_cached >= 0)
        return s_cached;
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    s_cached = __builtin_cpu_supports("avx2") ? 1 : 0;
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    {
        int regs[4];
        __cpuid(regs, 0);
        if(regs[0] >= 7) {
            __cpuidex(regs, 7, 0);
            /* CPUID.(EAX=7,ECX=0):EBX bit 5 == AVX2. */
            s_cached = (regs[1] & (1 << 5)) ? 1 : 0;
        } else {
            s_cached = 0;
        }
    }
#else
    s_cached = 0;
#endif
    return s_cached;
}

/* Returns non-zero if the current CPU supports the AVX-512 subset used by
 * the data structures in this codebase: F + BW + DQ + VL. Cached after the
 * first call.
 */
static inline int simd_avx512_supported(void)
{
    static int s_cached = -1;
    if(s_cached >= 0)
        return s_cached;
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    s_cached = (__builtin_cpu_supports("avx512f")
             && __builtin_cpu_supports("avx512bw")
             && __builtin_cpu_supports("avx512dq")
             && __builtin_cpu_supports("avx512vl")) ? 1 : 0;
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    {
        int regs[4];
        __cpuid(regs, 0);
        if(regs[0] >= 7) {
            __cpuidex(regs, 7, 0);
            /* CPUID.(EAX=7,ECX=0):EBX bits: 16=F, 17=DQ, 30=BW, 31=VL. */
            int ebx = regs[1];
            s_cached = ((ebx & (1 << 16))
                     && (ebx & (1 << 17))
                     && (ebx & (1 << 30))
                     && (ebx & (1 << 31))) ? 1 : 0;
        } else {
            s_cached = 0;
        }
    }
#else
    s_cached = 0;
#endif
    return s_cached;
}

#endif /* SIMD_H */
