extern main
extern exit
extern __libc_start_main

section .text

global _start

_start:

    ; При входе:
    ;
    ; RDI = exec_args_t *
    ;
    ; struct exec_args {
    ;     int argc;      ; offset 0
    ;     padding        ; offset 4
    ;     char **argv;   ; offset 8
    ; }

    mov rdx, rdi

    ; argc
    mov esi, dword [rdx]

    ; argv
    mov rdx, [rdx + 8]

    ; __libc_start_main(
    ;     main,
    ;     argc,
    ;     argv
    ; )

    mov rdi, main

    call __libc_start_main

    ; exit(return_value)

    mov rdi, rax
    call exit

    hlt