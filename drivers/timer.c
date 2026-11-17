// timer.c -- Initialises the PIT, and handles clock updates.
//            Written for JamesM's kernel development tutorials.

#include "timer.h"
#include "../kernel/isr.h"
#include "monitor.h"

u32int tick = 0;

void sleep(uint32_t timer_ticks) {
    /*
     * r12-bootfix: было `while (timer_ticks < end)` -- сравнение
     * константы с константой, вечный цикл при любом аргументе.
     * Правильное условие -- ждать, пока глобальный тик не дойдёт
     * до конца интервала.
     *
     * ВНИМАНИЕ: в текущей конфигурации ядра IRQ0 (PIT) замаскирован
     * и tick не инкрементируется, так что в ядре sleep() реально
     * не работает (и не используется).  Функция оставлена для
     * совместимости с туториальным кодом.
     */
    uint32_t end = tick + timer_ticks;
    while (tick < end) {
        __asm__ volatile("hlt");
    }
}
/*
 * Монотонные миллисекунды (PIT работает на 1000 Гц).
 * Сисколл 196 (TICKS_MS) отдаёт это значение в userspace.
 */
uint64_t timer_ticks_ms(void)
{
    return (uint64_t)tick;
}

extern void xhci_timer_poll(void);

static void timer_callback(registers_t regs)
{
    tick++;
    xhci_timer_poll();   /* USB (xHCI) event ring, 1 кГц */
    /*monitor_write("Tick: ");
    monitor_write_dec(tick);
    monitor_write("\n");
*/
    //switch_task(regs);
    //yield();
}

void init_timer(u32int frequency)
{
    // Firstly, register our timer callback.
    register_interrupt_handler(IRQ0, &timer_callback);

    // The value we send to the PIT is the value to divide it's input clock
    // (1193180 Hz) by, to get our required frequency. Important to note is
    // that the divisor must be small enough to fit into 16-bits.
    u32int divisor = 1193180 / frequency;

    // Send the command byte.
    outb(0x43, 0x36);

    // Divisor has to be sent byte-wise, so split here into upper/lower bytes.
    u8int l = (u8int)(divisor & 0xFF);
    u8int h = (u8int)( (divisor>>8) & 0xFF );

    // Send the frequency divisor.
    outb(0x40, l);
    outb(0x40, h);
}
