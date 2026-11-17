/*
 * crt/start.S — точка входа приложений ArtyomX64 (mini-libc).
 *
 * GNU-ассемблер вместо nasm: одна кодовая база для сборки
 * и на хосте, и в ОС; никаких внешних инструментов.
 *
 * При входе:
 *
 *     RDI = exec_args_t *
 *
 *     struct exec_args {
 *         int      argc;    offset 0
 *         pad               offset 4
 *         char    **argv;   offset 8
 *     };
 *
 * Всё машинное кодирование PIC-безопасно (RIP-относительное),
 * чтобы объект можно было линковать в ET_DYN/PIE-образ.
 */

    .text
    .globl  _start
    .type   _start, @function

_start:
    /* Стабильный фрейм для отладчиков/бэктрейсов. */
    xorl    %ebp, %ebp

    /*
     * Выравниваем стек: по x86-64 ABI в точке call
     * RSP должно быть кратно 16.
     * task_spawn() кладёт адрес возврата на 16-выровненный
     * стек, поэтому на входе в _start RSP % 16 == 8.
     * and выравнивает в 0, после call у callee будет 8 — как
     * требует ABI (иначе movaps в SSE-коде может упасть).
     */
    andq    $-16, %rsp

    /* RDX = exec_args_t * */
    movq    %rdi, %rdx

    /* argc */
    movl    (%rdx), %esi

    /* argv */
    movq    8(%rdx), %rdx

    /*
     * __libc_start_main(main, argc, argv)
     *
     * lea rel — без релокаций; (mov rdi, main дал бы
     * абсолютный imm64 и TEXTREL в PIE-образе).
     */
    leaq    main(%rip), %rdi
    call    __libc_start_main

    /* exit(код возврата main) */
    movq    %rax, %rdi
    call    exit

    /* сюда попасть нельзя: exit не возвращает */
    hlt

    .size   _start, . - _start

    /* Стековые скобки для отладчика (не обязательны). */
    .section .note.GNU-stack,"",@progbits
