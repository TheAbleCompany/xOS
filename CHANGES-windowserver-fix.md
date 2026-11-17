# CHANGES: фикс #GP(0x500) при старте wserver (окно сис-colлов vs TCC red zone)

Дата: 2025-10-20
Симптом: ядро падало сразу после "WSERVER_START (userspace GUI server)":

    === CPU EXCEPTION ===
    EXC: 13 General protection fault
    RIP: 0xFFFFFFFF802122CA      (IRETQ в isr_common_stub)
    RSP: 0xFFFF800000313F98      (стек wserver, kernel heap HHDM)
    ERR: 0x0000000000000500      (селектор 0x500 = индекс 160, GDT всего 9 записей)
    TASK: killed by exception, PID=2

Строка "WSERVER SCR:" при этом не печаталась -- падение происходило
на ПЕРВОМ сисколле wserver (FB_INFO, номер 189).

## Причина (подтверждена экспериментально в QEMU, дампом CPU-кадра)

1. Пользовательские бинарники собраны TCC, который кладёт мелкие
   локальные переменные В RED ZONE ([rsp-1 .. rsp-128]) и не
   резервирует под них стек. В gfx_init():

       lea -0x18(%rsp),%rdi    ; ax_fb_info_t fi -- в red zone
       mov $0xbd,%eax          ; 189 = FB_INFO
       int $0x80

2. При int $0x80 CPU вытолкивает кадр прерывания на СТЕК ЗАДАЧИ:
   RIP/CS/RFLAGS/RSP/SS (+pad), 48 байт -- ПРЯМО ПОВЕРХ fi.

3. Ядро (ipc_fb_info) честно записывает fb-инфо по указателю rdi --
   то есть В САМ КАДР: addr попадает в слот RSP, width=1280 (0x500)
   -- в слот SS (мл. половину).

4. IRETQ в long mode загружает SS из кадра даже при возврате в тот
   же ring 0: SS = 0x02D0_00000500 -> селектор вне GDT -> #GP(0x500).

То есть: iret-кадр и out-параметр сисколла физически лежали в одних
и тех же байтах стека.

## Исправление (3 части)

### 1. TSS + IST (kernel/descriptor_tables.c)

В GDT добавлен 64-битный TSS (селектор 0x48, дескриптор 16 байт,
слоты 9-10). В TSS заполнены IST1 (64 КиБ) и IST2 (16 КиБ для #DF).
Все IDT-гейты 0..47 (исключения и IRQ) переведены на IST1 -- кадры
аппаратных прерываний больше НИКОГДА не попадают на стек задачи
(раньше IRQ мыши тоже рвал red zone пользователя).

Сисколлы (128-130) оставлены на стеке задачи (их обработчики могут
уходить в yield/switchTask -- их контекст обязан жить на стеке
задачи), для них сделано отдельное решение (п.3).

### 2. Отдельный ядерно-стек задачи (multitask/task.c, task.h)

Task получил поля kstack_base/kstack_top (16 КиБ из kernel heap,
HHDM -- видны во всех адресных пространствах) и глобальную
ax_cur_kstack, которую планировщик обновляет при каждом
переключении. kstack НЕ освобождается в task_exit (на нём может
стоять сама задача) -- допустимая утечка 16 КиБ на задачу.

### 3. SYSCALL_STUB (kernel/asm/interrupt.asm)

Векторы 128/129/130 теперь: переключаются на ядерно-стек текущей
задачи, копируют туда кадр (6 слотов), обработчик выполняется на
ядерно-стеке, iretq -- тоже. Стек задачи ниже кадра не пишется
вовсе:

    mov r11, [rel ax_cur_kstack]
    xchg rsp, r11                  ; rsp = kstack_top, r11 = старый rsp
    push qword [r11+40]            ; pad
    push qword [r11+32]            ; SS
    push qword [r11+24]            ; RSP
    push qword [r11+16]            ; RFLAGS
    push qword [r11+8]             ; CS
    push qword [r11+0]             ; RIP
    push 0
    push 128
    jmp isr_common_stub

Итог: ядро пишет out-параметры в кадр на стеке задачи (пользователь
их читает), iretq восстанавливается из копии на ядерно-стеке --
конфликт исчез.

Побочные эффекты: портится r11 (как у syscall в Linux; аргументы
rax/rdi/rsi/rdx/r10/r8/r9 сохраняются). В kernel/isr.c при #GP
дополнительно печатаются CS/RFLAGS и дамп iret-кадра (IFR) --
упростит диагностику будущих проблем.

## Проверено в QEMU (qemu 10.0, -M pc -m 512M, cdrom+testdisk)

    WSERVER_START (userspace GUI server)
    WSERVER SCR:1280x800
    WSERVER port 'wserver' id=1
    ... 95 секунд работы, 0 исключений ...

## Изменённые файлы

    kernel/asm/interrupt.asm     -- SYSCALL_STUB для 128/129/130
    kernel/descriptor_tables.c   -- TSS + IST1/IST2, GDT 9 -> 11 слотов
    multitask/task.c             -- kstack, ax_cur_kstack
    multitask/task.h             -- поля Task, extern ax_cur_kstack
    kernel/isr.c                 -- расширенный дамп при #GP
