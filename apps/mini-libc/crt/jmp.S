/*
 * crt/jmp.S — setjmp/longjmp для mini-libc на ArtyomX64 (x86_64, SysV ABI).
 *
 * jmp_buf (unsigned long long [8]):
 *
 *      [0]  rbx
 *      [1]  rbp
 *      [2]  r12
 *      [3]  r13
 *      [4]  r14
 *      [5]  r15
 *      [6]  rsp  (БЕЗ адреса возврата)
 *      [7]  rip  (адрес возврата setjmp)
 *
 * Это ровно набор callee-saved регистров SysV x86-64, поэтому буфер
 * совместим и с кодом gcc, и с кодом tcc.
 *
 * sigsetjmp/siglongjmp — сигналов в ядре ArtyomX64 нет, поэтому это
 * просто алиасы setjmp/longjmp (совместимость по именам).
 */

    .text

/* ------------------------------------------------------------------ */
/* setjmp / _setjmp                                                    */
/* ------------------------------------------------------------------ */

    .globl  setjmp
    .globl  _setjmp
    .globl  sigsetjmp
    .globl  __sigsetjmp
    .type   setjmp, @function

setjmp:
_setjmp:
sigsetjmp:
__sigsetjmp:
    /* rdi = jmp_buf env */
    movq    %rbx,  0(%rdi)
    movq    %rbp,  8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)

    /* call setjmp уже положил ret addr на стек: rsp без него */
    leaq    8(%rsp), %rax
    movq    %rax, 48(%rdi)

    /* адрес возврата — куда вернётся и setjmp, и longjmp */
    movq    (%rsp), %rax
    movq    %rax, 56(%rdi)

    /* первый возврат: всегда 0 */
    xorl    %eax, %eax
    ret

    .size   setjmp, . - setjmp


/* ------------------------------------------------------------------ */
/* longjmp / _longjmp                                                  */
/* ------------------------------------------------------------------ */

    .globl  longjmp
    .globl  _longjmp
    .globl  siglongjmp
    .globl  __longjmp_chk
    .type   longjmp, @function

longjmp:
_longjmp:
siglongjmp:
__longjmp_chk:
    /* rdi = jmp_buf env, esi = val */
    movl    %esi, %eax          /* val */
    testl   %eax, %eax
    jnz     1f
    movl    $1, %eax            /* longjmp(env, 0) => setjmp вернёт 1 */
1:
    movq     0(%rdi), %rbx
    movq     8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    48(%rdi), %rsp
    movq    56(%rdi), %rdx
    jmp     *%rdx

    .size   longjmp, . - longjmp

/* Стековые скобки: объект линкуется и в ET_EXEC, и в PIE-образы. */
    .section .note.GNU-stack,"",@progbits
