/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * From Musl Libc: https://musl.libc.org/
 *
 * https://git.musl-libc.org/cgit/musl/tree/arch/x86_64/syscall_arch.h
 */

#ifndef __SYSCALL_ARCH_X86_64__
#define __SYSCALL_ARCH_X86_64__         1

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Хост-шим (для тестов на Linux-хосте, как в порту lua):
 * ARTYOM_HOST_SHIM=1 — syscall() реализуется в отдельном shim-файле
 * обычными вызовами Linux, номера аргументов остаются ArtyomX64.
 */
#ifdef ARTYOM_HOST_SHIM
extern long __artym_host_syscall(long n, long a1, long a2, long a3,
                                 long a4, long a5, long a6);
#endif

static inline long __syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
        unsigned long ret;

#ifdef ARTYOM_HOST_SHIM
        ret = (unsigned long)__artym_host_syscall(n, a1, a2, a3, a4, a5, a6);
        return (long)ret;
#else

        /*__asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2),
                                                  "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
        */
        // Надо (ваша ОС, int $0x80):
        __asm__ __volatile__ ("int $0x80" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2),
                      "d"(a3), "c"(a4), "r"(a5) : "memory");
        return ret;
#endif
}

#ifdef __cplusplus
}
#endif

#endif
