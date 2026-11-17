#include "../lib/common.h"
#include "isr.h"
#include "../multitask/task.h"
#include "../drivers/monitor.h"

isr_t interrupt_handlers[256];
extern void init_shell(void);

int in_program = 0;

void register_interrupt_handler(u8int n, isr_t handler)
{
    interrupt_handlers[n] = handler;
}

void isr_handler(registers_t *regs)
{
    u8int int_no = (u8int)(regs->int_no & 0xFF);

    if (int_no < 256 && interrupt_handlers[int_no] != 0)
    {
        isr_t handler = interrupt_handlers[int_no];
        handler(regs);
    }
}

void irq_handler(registers_t *regs)
{
    if (regs->int_no >= 40)
    {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);

    if (interrupt_handlers[regs->int_no] != 0)
    {
        isr_t handler = interrupt_handlers[regs->int_no];
        handler(regs);
    }
}

/* ============================================================
 * CPU exception handlers (vectors 0..31)
 * ============================================================
 *
 * Раньше обработчиков не было вообще: isr_handler() не находил
 * обработчик, выполнял iret обратно на упавшую инструкцию,
 * исключение повторялось -- и система выглядела зависшей
 * (типичный симптом: "программа запустилась и всё зависло").
 *
 * Теперь любое исключение диагностируется на serial-порт:
 *   - в контексте пользовательской задачи -- задача
 *     принудительно завершается (task_exit), shell продолжает
 *     работать;
 *   - в контексте корневой задачи ядра -- система останавливается
 *     (продолжать работу с разрушенным состоянием опаснее).
 *
 * debug_putc/puts/putnum are declared in lib/common.h and
 * defined in lib/syscalls.c; we just use them here.
 */

static const char *exception_names[32] = {
    "Divide-by-zero",
    "Debug",
    "NMI",
    "Breakpoint",
    "Overflow",
    "BOUND range exceeded",
    "Invalid opcode",
    "Device not available",
    "Double fault",
    "Coprocessor segment overrun",
    "Invalid TSS",
    "Segment not present",
    "Stack-segment fault",
    "General protection fault",
    "Page fault",
    "Reserved (15)",
    "x87 FPU error",
    "Alignment check",
    "Machine check",
    "SIMD FP exception",
    "Virtualization exception",
    "Control protection",
    "Reserved (22)",
    "Reserved (23)",
    "Reserved (24)",
    "Reserved (25)",
    "Reserved (26)",
    "Reserved (27)",
    "Reserved (28)",
    "Reserved (29)",
    "Reserved (30)",
    "Reserved (31)"
};

static void exception_print_hex(const char *label, u64int value)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[19];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++)
        buf[2 + i] = hex[(value >> (60 - i * 4)) & 0xF];
    buf[18] = 0;

    debug_puts(label);
    debug_puts(buf);
    debug_putc('\n');
}

static void exception_handler(registers_t *regs)
{
    u8int int_no = (u8int)(regs->int_no & 0xFF);
    u64int cr2 = 0;

    if (int_no == 14)
    {
        /* Page fault: CR2 содержит адрес, вызвавший отказ. */
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    }

    debug_puts("\n=== CPU EXCEPTION ===\n");
    debug_puts("EXC: ");
    debug_putnum((uint64_t)int_no);
    debug_putc(' ');
    debug_puts(int_no < 32 ? exception_names[int_no] : "Unknown");
    debug_putc('\n');

    exception_print_hex("RIP: ", regs->rip);
    exception_print_hex("RSP: ", regs->rsp);
    exception_print_hex("ERR: ", regs->err_code);
    exception_print_hex("CS : ", regs->cs);
    exception_print_hex("FLG: ", regs->rflags);
    if (int_no == 14)
        exception_print_hex("CR2: ", cr2);
    if (int_no == 13)
    {
        /* Дамп кадра, который читал упавший IRETQ. */
        u64int *f = (u64int *)(uintptr_t)regs->rsp;
        for (int k = 0; k < 6; k++)
            exception_print_hex("IFR: ", f[k]);
    }

    /*
     * Пользовательская задача -- завершаем её, shell выживет.
     * Корневая задача (ядро) -- останавливаемся: продолжение
     * работы с повреждённым состоянием только разрушит систему.
     */
    if (runningTask && runningTask->pid != TASK_ROOT_PID)
    {
        debug_puts("TASK: killed by exception, PID=");
        debug_putnum((uint64_t)runningTask->pid);
        debug_putc('\n');

        task_exit(139);
        /* task_exit не возвращается */
    }

    debug_puts("KERNEL: exception in root task -- halted\n");

    for (;;)
    {
        __asm__ volatile("cli; hlt");
    }
}

void initialise_exception_handlers(void)
{
    int i;

    for (i = 0; i < 32; i++)
        register_interrupt_handler((u8int)i, &exception_handler);
}
