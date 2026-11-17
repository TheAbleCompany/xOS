/* host_start.S — Linux ELF entry (argc/argv НА СТЕКЕ) ->
   exec_args_t* в RDI — точная имитация task_spawn() ArtyomX64 */
    .text
    .globl  _start
_start:
    movq    (%rsp), %r12         /* argc  */
    leaq    8(%rsp), %rbx        /* argv  */

    leaq    ax_ea(%rip), %rdi
    movl    %r12d, (%rdi)        /* argc */
    movq    %rbx, 8(%rdi)        /* argv */

    jmp     artyom_start

    .section .bss
    .align  8
ax_ea:
    .long   0
    .long   0
    .quad   0
