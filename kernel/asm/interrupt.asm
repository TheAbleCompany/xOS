[bits 64]

%macro ISR_NOERRCODE 1
  global isr%1
  isr%1:
    cli
    push 0
    push %1
    jmp isr_common_stub
%endmacro

; ============================================================
; ВАЖНО (фикс регрессии v2): r11 НЕЛЬЗЯ портить!
;
; Наши бинарники собираются GCC/TCC с inline-asm int 0x80,
; который НЕ объявляет r11 clobbered (в отличие от инструкции
; syscall в Linux, где r11/ rcx документированно губятся).
; Компилятор свободно держит в r11 живые переменные -- v2-стаб
; их затирал (r11 = старый rsp), из-за чего приложения не могли
; дорисовать содержимое окон (поломка циклов libax ax_poll и
; т.п.).  Теперь r11 сохраняется в слот ax_user_r11_save ДО
; переключения стека, а C-обработчики 128/129/130 восстанав-
; ливают его в регистровый слот кадра (pop r11 вернёт оригинал).
; ============================================================
extern ax_cur_kstack

section .data
global ax_user_r11_save
ax_user_r11_save: dq 0

section .text

%macro SYSCALL_STUB 1
  global isr%1
  isr%1:
    cli
    mov [rel ax_user_r11_save], r11   ; сохранить r11 ПОЛЬЗОВАТЕЛЯ (до порчи)
    mov r11, [rel ax_cur_kstack]
    xchg rsp, r11                 ; rsp = kstack_top; r11 = старый rsp
    push qword [r11+40]           ; pad (IRETQ его не читает)
    push qword [r11+32]           ; SS
    push qword [r11+24]           ; RSP задачи
    push qword [r11+16]           ; RFLAGS
    push qword [r11+8]            ; CS
    push qword [r11+0]            ; RIP
    push 0
    push %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
  global isr%1
  isr%1:
    cli
    push %1
    jmp isr_common_stub
%endmacro

%macro IRQ 2
  global irq%1
  irq%1:
    cli
    push 0
    push %2
    jmp irq_common_stub
%endmacro

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_NOERRCODE 17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31
; 128/129/130 -- сисколлы, идут через SYSCALL_STUB (реплика кадра)
SYSCALL_STUB 128
SYSCALL_STUB 129
SYSCALL_STUB 130
SYSCALL_STUB 131
IRQ   0,    32
IRQ   1,    33
IRQ   2,    34
IRQ   3,    35
IRQ   4,    36
IRQ   5,    37
IRQ   6,    38
IRQ   7,    39
IRQ   8,    40
IRQ   9,    41
IRQ  10,    42
IRQ  11,    43
IRQ  12,    44
IRQ  13,    45
IRQ  14,    46
IRQ  15,    47

extern isr_handler

isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov ax, 0x10
    mov ds, ax
    mov es, ax

    mov rdi, rsp
    call isr_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    sti
    iretq

extern irq_handler

irq_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov ax, 0x10
    mov ds, ax
    mov es, ax

    mov rdi, rsp
    call irq_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    sti
    iretq
