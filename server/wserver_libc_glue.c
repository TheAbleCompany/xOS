/*
 * ============================================================
 * wserver_libc_glue.c
 * ============================================================
 *
 * Микро-обвязка WindowServer'а: то, что в ядре давали lib/common.c
 * и lib/syscalls.c, но в userspace-сборке сервера:
 *
 *   - inb/outb/outw -- порты ввода-вывода (сервер, как и весь
 *     остальной код ArtyomX64, исполняется в ring 0, поэтому
 *     RTC/батарея/serial доступны напрямую);
 *   - debug_putc/puts/putnum -- COM1-диагностика (в ядре они
 *     жили в syscalls.c).
 */

#include "wserver_internal.h"

/* ---- Порты ввода-вывода ---- */

#ifdef HOST_HARNESS
/*
 * Host harness: пользовательский процесс не может трогать порты --
 * все операции no-op (inb читает 0xFF, как "устройство отсутствует").
 */
#include <stdio.h>

void outb(u16int port, u8int value) { (void)port; (void)value; }

u8int inb(u16int port) { (void)port; return 0xFF; }

void outw(u16int port, u16int value) { (void)port; (void)value; }

/* ---- COM1-диагностика -> stderr ---- */

void debug_putc(char c) { fputc(c, stderr); }
void debug_puts(const char *s) { if (s) fputs(s, stderr); }
void debug_putnum(uint64_t n) { fprintf(stderr, "%llu", (unsigned long long)n); }

#else /* HOST_HARNESS */

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

void outw(u16int port, u16int value)
{
    __asm__ volatile ("outw %1, %0" : : "dN" (port), "a" (value));
}

/* ---- COM1-диагностика ---- */

void debug_putc(char c)
{
    outb(0x3F8, (u8int)c);
}

void debug_puts(const char *s)
{
    if (!s)
        return;

    while (*s)
        debug_putc(*s++);
}

void debug_putnum(uint64_t n)
{
    char buf[32];
    int i = 0;

    if (n == 0) {
        debug_putc('0');
        return;
    }

    while (n) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }

    while (i)
        debug_putc(buf[--i]);
}

#endif /* HOST_HARNESS */
