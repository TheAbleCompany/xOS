#ifndef IXG_PLATFORM_H
#define IXG_PLATFORM_H

#if defined(__APPLE__) && defined(__MACH__)
#define IXG_PLATFORM_DARWIN 1
#else
#define IXG_PLATFORM_DARWIN 0
#endif

#if defined(__linux__)
#define IXG_PLATFORM_LINUX 1
#else
#define IXG_PLATFORM_LINUX 0
#endif

#if defined(_WIN32) || defined(_MSC_VER)
#define IXG_PLATFORM_WIN 1
#else
#define IXG_PLATFORM_WIN 0
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
#define IXG_ARCH_X64 1
#else
#define IXG_ARCH_X64 0
#endif

#if defined(__i386__) || defined(_M_IX86)
#define IXG_ARCH_X86 1
#else
#define IXG_ARCH_X86 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define IXG_ARCH_ARM64 1
#else
#define IXG_ARCH_ARM64 0
#endif

#if defined(__arm__) || defined(_M_ARM)
#define IXG_ARCH_ARM32 1
#else
#define IXG_ARCH_ARM32 0
#endif

#if IXG_ARCH_X64 || IXG_ARCH_X86
#define IXG_ARCH_X86_FAMILY 1
#else
#define IXG_ARCH_X86_FAMILY 0
#endif

#if IXG_ARCH_ARM64 || IXG_ARCH_ARM32
#define IXG_ARCH_ARM_FAMILY 1
#else
#define IXG_ARCH_ARM_FAMILY 0
#endif

#if defined(_MSC_VER)
#define IXG_COMPILER_MSVC 1
#else
#define IXG_COMPILER_MSVC 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#define IXG_COMPILER_GCC_LIKE 1
#else
#define IXG_COMPILER_GCC_LIKE 0
#endif

#if IXG_COMPILER_MSVC
#include <intrin.h>
#include <immintrin.h>
#endif

#if IXG_COMPILER_GCC_LIKE
#if IXG_ARCH_X86_FAMILY
#include <x86intrin.h>
#include <cpuid.h>
#endif
#include <stdint.h>
#endif

#if IXG_COMPILER_MSVC
#define IXG_ALIGNED(x) __declspec(align(x))
#else
#define IXG_ALIGNED(x) __attribute__((aligned(x)))
#endif

#if IXG_COMPILER_MSVC
#define IXG_INLINE __forceinline
#else
#define IXG_INLINE static inline __attribute__((always_inline))
#endif

#if IXG_COMPILER_MSVC
#define IXG_PACKED_PUSH __pragma(pack(push, 1))
#define IXG_PACKED_POP  __pragma(pack(pop))
#define IXG_PACKED
#else
#define IXG_PACKED_PUSH
#define IXG_PACKED_POP
#define IXG_PACKED __attribute__((packed))
#endif

#if IXG_COMPILER_MSVC
typedef __m128i ixg_m128i;
#else
typedef __m128i ixg_m128i;
#endif

#if IXG_ARCH_X86_FAMILY
#define IXG_INLINE_PAUSE() __asm__ volatile("pause" ::: "memory")
#define IXG_INLINE_MFENCE() __asm__ volatile("mfence" ::: "memory")
#define IXG_INLINE_SFENCE() __asm__ volatile("sfence" ::: "memory")
#define IXG_INLINE_LFENCE() __asm__ volatile("lfence" ::: "memory")
#else
#define IXG_INLINE_PAUSE() ((void)0)
#define IXG_INLINE_MFENCE() ((void)0)
#define IXG_INLINE_SFENCE() ((void)0)
#define IXG_INLINE_LFENCE() ((void)0)
#endif

#if IXG_ARCH_X86_FAMILY && !IXG_COMPILER_MSVC

#ifndef _InterlockedIncrement
IXG_INLINE long _InterlockedIncrement(volatile long* p) {
    return __sync_add_and_fetch(p, 1L);
}
#endif

#ifndef _InterlockedExchange
IXG_INLINE long _InterlockedExchange(volatile long* p, long val) {
    return __sync_lock_test_and_set(p, val);
}
#endif

#ifndef _InterlockedCompareExchange
IXG_INLINE long _InterlockedCompareExchange(volatile long* p, long val, long cmp) {
    return __sync_val_compare_and_swap(p, cmp, val);
}
#endif

#define ixg_lzcnt32(x) ((unsigned int)((x) == 0u ? 32u : (unsigned int)__builtin_clz((unsigned int)(x))))
#define ixg_lzcnt64(x) ((unsigned int)((x) == 0ull ? 64u : (unsigned int)__builtin_clzll((unsigned long long)(x))))

IXG_INLINE void ixg_cpuid(int info[4], int leaf) {
    unsigned int a, b, c, d;
    __cpuid(leaf, a, b, c, d);
    info[0] = (int)a;
    info[1] = (int)b;
    info[2] = (int)c;
    info[3] = (int)d;
}

IXG_INLINE void ixg_cpuidex(int info[4], int leaf, int subleaf) {
    unsigned int a, b, c, d;
    __cpuid_count(leaf, subleaf, a, b, c, d);
    info[0] = (int)a;
    info[1] = (int)b;
    info[2] = (int)c;
    info[3] = (int)d;
}

#if !IXG_COMPILER_GCC_LIKE
IXG_INLINE unsigned long long _xgetbv(unsigned int ext) {
    unsigned int eax, edx;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(ext));
    return ((unsigned long long)edx << 32) | (unsigned long long)eax;
}
#endif

#ifndef __writecr3
IXG_INLINE void __writecr3(unsigned long long val) {
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(val) : "memory");
}
#endif

#ifndef __readcr3
IXG_INLINE unsigned long long __readcr3(void) {
    unsigned long long val;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(val));
    return val;
}
#endif

#endif

#if IXG_ARCH_ARM_FAMILY && !IXG_COMPILER_MSVC

IXG_INLINE long _InterlockedIncrement(volatile long* p) {
    return __sync_add_and_fetch(p, 1L);
}

IXG_INLINE long _InterlockedExchange(volatile long* p, long val) {
    return __sync_lock_test_and_set(p, val);
}

IXG_INLINE long _InterlockedCompareExchange(volatile long* p, long val, long cmp) {
    return __sync_val_compare_and_swap(p, cmp, val);
}

#define __dmb(x)  __asm__ __volatile__("dmb " #x : : : "memory")
#define __dsb(x)  __asm__ __volatile__("dsb " #x : : : "memory")
#define __isb(x)  __asm__ __volatile__("isb " #x : : : "memory")

#endif

#if IXG_COMPILER_MSVC && IXG_ARCH_ARM_FAMILY

#define __dmb(x)  __dmb(x)
#define __dsb(x)  __dsb(x)
#define __isb(x)  __isb(x)

#endif

#ifndef _mm_pause
#if IXG_COMPILER_MSVC
#else
#endif
#endif

#if IXG_PLATFORM_LINUX || IXG_PLATFORM_DARWIN
#define IXG_HAS_POSIX 1
#else
#define IXG_HAS_POSIX 0
#endif

#if IXG_PLATFORM_DARWIN
#define IXG_OS_NAME "Darwin"
#elif IXG_PLATFORM_LINUX
#define IXG_OS_NAME "Linux"
#elif IXG_PLATFORM_WIN
#define IXG_OS_NAME "Windows"
#else
#define IXG_OS_NAME "Unknown"
#endif

#if IXG_ARCH_X64
#define IXG_ARCH_NAME "x86_64"
#elif IXG_ARCH_X86
#define IXG_ARCH_NAME "x86"
#elif IXG_ARCH_ARM64
#define IXG_ARCH_NAME "arm64"
#elif IXG_ARCH_ARM32
#define IXG_ARCH_NAME "arm32"
#else
#define IXG_ARCH_NAME "unknown"
#endif

#endif