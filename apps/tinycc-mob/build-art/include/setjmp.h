/* setjmp.h — mini-libc (ArtyomX64) */
#ifndef _SETJMP_H
#define _SETJMP_H

#include <stddef.h>

#ifdef __x86_64__
typedef unsigned long long jmp_buf[8];
#elif defined(__i386__)
typedef unsigned long jmp_buf[6];
#elif defined(__aarch64__)
typedef unsigned long long jmp_buf[22];  /* x19-x30, sp, d8-d15 */
#elif defined(__arm__)
typedef unsigned long jmp_buf[10];
#endif

/*
 * setjmp: 0 при прямом вызове, значение val из longjmp (не 0) при возврате.
 * jmp_buf должен использоваться только volatile-безопасно (C standard).
 */
extern int setjmp(jmp_buf env);
extern int _setjmp(jmp_buf env);
extern int sigsetjmp(jmp_buf env, int savemask);
extern int __sigsetjmp(jmp_buf env, int savemask);

extern void longjmp(jmp_buf env, int val);
extern void _longjmp(jmp_buf env, int val);
extern void siglongjmp(jmp_buf env, int val);

#endif
