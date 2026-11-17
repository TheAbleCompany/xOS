    .text

    .globl setjmp
    .globl _setjmp
    .type setjmp,@function
    .type _setjmp,@function

setjmp:
_setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret

    .globl longjmp
    .globl _longjmp
    .type longjmp,@function
    .type _longjmp,@function

longjmp:
_longjmp:
    movl %esi, %eax
    testl %eax, %eax
    jne 1f
    movl $1, %eax
1:
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %rsp
    movq 24(%rdi), %r12
    movq 32(%rdi), %r13
    movq 40(%rdi), %r14
    movq 48(%rdi), %r15
    jmp *56(%rdi)

    .section .note.GNU-stack,"",@progbits
