#include <stddef.h>
#include "common.h"
#include "../drivers/monitor.h"

void outb(u16int port, u8int value)
{
    __asm__ volatile ("outb %1, %0" : : "dN" (port), "a" (value));
}

u8int inb(u16int port)
{
    u8int ret;
    __asm__ volatile("inb %1, %0" : "=a" (ret) : "dN" (port));
    return ret;
}

u16int inw(u16int port)
{
    u16int ret;
    __asm__ volatile ("inw %1, %0" : "=a" (ret) : "dN" (port));
    return ret;
}

void outw(u16int port, u16int value)
{
    __asm__ volatile ("outw %1, %0" : : "dN" (port), "a" (value));
}

void outl(u16int port, u32int value)
{
    __asm__ volatile ("outl %1, %0" : : "dN" (port), "a" (value));
}

u32int inl(u16int port)
{
    u32int ret;
    __asm__ volatile ("inl %1, %0" : "=a" (ret) : "dN" (port));
    return ret;
}

/*
 * ============================================================
 * Serial debug helpers (COM1).
 *
 * Раньше жили в kernel/axshell.c; после выноса GUI в отдельный
 * процесс wserver они переехали сюда -- serial-лог нужен
 * подсистемам ядра (multitask/loader.c и др.) независимо от GUI.
 * ============================================================
 */

void serial_putc_ax(char c)
{
    outb(0x3F8, (u8int)c);
}

void serial_puts_ax(const char *s)
{
    while (*s)
        serial_putc_ax(*s++);
}

void panic(const char *message, const char *file, u32int line)
{
    char num[24];
    u32int v;
    int i;

    __asm__ volatile("cli");

    serial_puts_ax("\n*** KERNEL PANIC: ");
    serial_puts_ax(message ? message : "(no message)");
    serial_puts_ax(" at ");
    serial_puts_ax(file ? file : "?");
    serial_puts_ax(":");

    v = line;
    i = 0;
    if (!v) num[i++] = '0';
    while (v) { num[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (i) serial_putc_ax(num[--i]);
    serial_puts_ax("\n");
    debug_puts("\n*** KERNEL PANIC: ");
    debug_puts(message ? message : "(no message)");
    debug_puts("\n");

    for (;;)
        __asm__ volatile("cli; hlt");
}

void panic_assert(const char *file, u32int line, const char *desc)
{
    panic(desc ? desc : "assertion failed", file, line);
}
