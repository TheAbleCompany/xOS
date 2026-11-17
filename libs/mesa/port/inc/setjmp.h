#ifndef MESA_PORT_SETJMP_H
#define MESA_PORT_SETJMP_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { long rbx, rbp, rsp, r12, r13, r14, r15, rip; } jmp_buf[1];
int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif
