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

        /*
         * Родной ABI ArtyomX64 (int $0x80): номер в rax, аргументы
         * rdi, rsi, rdx, r10, r8, r9 -- как в Linux x86-64.  Раньше
         * 4-й аргумент клали в rcx: ядро читает r10, поэтому
         * syscall'ы с указателем в 4-м аргументе (213 ping) получали
         * мусор и сразу возвращали -1.
         */
        register long r10 __asm__("r10") = a4;
        register long r8  __asm__("r8")  = a5;
        register long r9  __asm__("r9")  = a6;

        __asm__ __volatile__ ("int $0x80" : "=a"(ret)
                      : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                        "r"(r10), "r"(r8), "r"(r9) : "memory");
        return ret;
#endif
}

#ifdef __cplusplus
}
#endif

#endif
