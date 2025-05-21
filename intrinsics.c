/* See LICENSE for license details. */
#include "compiler.h"

#if COMPILER_CLANG || COMPILER_GCC
  #define force_inline inline __attribute__((always_inline))
#elif COMPILER_MSVC
  #define force_inline __forceinline
#endif

#if COMPILER_MSVC || (COMPILER_CLANG && OS_WINDOWS)
  #pragma section(".rdata$", read)
  #define read_only __declspec(allocate(".rdata$"))
#elif COMPILER_CLANG
  #define read_only __attribute__((section(".rodata")))
#elif COMPILER_GCC
  /* TODO(rnp): how do we do this with gcc, putting it in rodata causes warnings and writing to
   * it doesn't cause a fault */
  #define read_only
#endif

/* TODO(rnp): msvc probably won't build this but there are other things preventing that as well */
#define sqrt_f32(a)     __builtin_sqrtf(a)
#define atan2_f32(y, x) __builtin_atan2f(y, x)

#define atomic_store(ptr, n)     __atomic_store_n(ptr,    n, __ATOMIC_RELEASE)
#define atomic_load(ptr)         __atomic_load_n(ptr,        __ATOMIC_ACQUIRE)
#define atomic_swap(ptr, n)      __atomic_exchange_n(ptr, n, __ATOMIC_RELEASE)
#define atomic_and(ptr, n)       __atomic_and_fetch(ptr,  n, __ATOMIC_RELEASE)
#define atomic_add(ptr, n)       __atomic_add_fetch(ptr,  n, __ATOMIC_RELEASE)
#define atomic_inc(ptr, n)       __atomic_fetch_add(ptr,  n, __ATOMIC_ACQ_REL)
#define atomic_cas(ptr, cptr, n) __atomic_compare_exchange_n(ptr, cptr, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

function force_inline u32
clz_u32(u32 a)
{
	u32 result = 32;
	if (a) result = __builtin_clz(a);
	return result;
}

function force_inline u32
ctz_u32(u32 a)
{
	u32 result = 32;
	if (a) result = __builtin_ctz(a);
	return result;
}

#if ARCH_ARM64
/* TODO? debuggers just loop here forever and need a manual PC increment (step over) */
#define debugbreak() asm volatile ("brk 0xf000")

/* NOTE(rnp): we are only doing a handful of f32x4 operations so we will just use NEON and do
 * the macro renaming thing. If you are implementing a serious wide vector operation you should
 * use SVE(2) instead. The semantics are different however and the code will be written for an
 * arbitrary vector bit width. In that case you will also need x86_64 code for determining
 * the supported vector width (ideally at runtime though that may not be possible).
 */
#include <arm_neon.h>
typedef float32x4_t f32x4;
typedef int32x4_t   i32x4;

#define cvt_i32x4_f32x4(a)    vcvtq_f32_s32(a)
#define cvt_f32x4_i32x4(a)    vcvtq_s32_f32(a)
#define dup_f32x4(f)          vdupq_n_f32(f)
#define load_f32x4(a)         vld1q_f32(a)
#define load_i32x4(a)         vld1q_s32(a)
#define mul_f32x4(a, b)       vmulq_f32(a, b)
#define set_f32x4(a, b, c, d) vld1q_f32((f32 []){d, c, b, a})
#define sqrt_f32x4(a)         vsqrtq_f32(a)
#define store_f32x4(a, o)     vst1q_f32(o, a)
#define store_i32x4(a, o)     vst1q_s32(o, a)

#elif ARCH_X64
#include <immintrin.h>
typedef __m128  f32x4;
typedef __m128i i32x4;

#define cvt_i32x4_f32x4(a)    _mm_cvtepi32_ps(a)
#define cvt_f32x4_i32x4(a)    _mm_cvtps_epi32(a)
#define dup_f32x4(f)          _mm_set1_ps(f)
#define load_f32x4(a)         _mm_loadu_ps(a)
#define load_i32x4(a)         _mm_loadu_si128((i32x4 *)a)
#define mul_f32x4(a, b)       _mm_mul_ps(a, b)
#define set_f32x4(a, b, c, d) _mm_set_ps(a, b, c, d)
#define sqrt_f32x4(a)         _mm_sqrt_ps(a)
#define store_f32x4(a, o)     _mm_storeu_ps(o, a)
#define store_i32x4(a, o)     _mm_storeu_si128((i32x4 *)o, a)

#define debugbreak() asm volatile ("int3; nop")

#endif
