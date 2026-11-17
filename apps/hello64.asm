; x86-64 Linux-style syscall для "Hello, world!"
global _start

section .data
    msg db "Hello, world!", 10
    len equ $ - msg

section .text
_start:
    ; write(1, msg, len)
    mov rax, 4          ; syscall number for write
    mov rdi, 1          ; fd = stdout
    mov rsi, msg        ; buffer
    mov rdx, len        ; count
    syscall

    ; exit(0)
    mov rax, 1         ; syscall number for exit
    xor rdi, rdi        ; status = 0
    syscall
