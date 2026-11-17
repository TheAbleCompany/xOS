/*
 * sys_artyom.c -- системный слой TyrQuake для ArtyomX64.
 *
 * Заменяет sys_unix.c: время берётся из сисколла 196 (PIT, мс),
 * sleep/подача процесса -- через yield (151), вывод отладки -- в
 * COM1 (виден в QEMU -serial file:...).  Файлы читаются через
 * mini-libc (initrd плоский, поэтому basedir == "").
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "common.h"
#include "console.h"
#include "host.h"
#include "quakedef.h"
#include "sys.h"
#include "zone.h"

#include "artyom.h"
#include "axglue.h"

/* ------------------------------------------------------------------ */
/* COM1                                                               */
/* ------------------------------------------------------------------ */

static inline void ax_outb(unsigned short port, unsigned char val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char ax_inb(unsigned short port)
{
    unsigned char r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

void AX_InitDebug(void)
{
    static int done;

    if (done)
        return;
    done = 1;

    ax_outb(0x3F9, 0x00);
    ax_outb(0x3FB, 0x80);
    ax_outb(0x3F8, 0x03);
    ax_outb(0x3F9, 0x00);
    ax_outb(0x3FB, 0x03);
    ax_outb(0x3FA, 0xC7);
    ax_outb(0x3FC, 0x0B);
}

void AX_DebugPuts(const char *s)
{
    if (!s)
        return;

    while (*s) {
        while ((ax_inb(0x3FD) & 0x20) == 0)
            ;
        ax_outb(0x3F8, (unsigned char)*s++);
    }
}

void AX_DebugHex(uint32_t v)
{
    char buf[11];

    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        unsigned d = (v >> ((7 - i) * 4)) & 0xF;
        buf[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    buf[10] = 0;
    AX_DebugPuts(buf);
}

void AX_DebugDec(int v)
{
    char buf[16];
    int n = 0;
    unsigned u;

    if (v < 0) {
        AX_DebugPuts("-");
        u = (unsigned)(-v);
    } else {
        u = (unsigned)v;
    }

    if (!u) {
        AX_DebugPuts("0");
        return;
    }

    while (u && n < (int)sizeof(buf) - 1) {
        buf[n++] = (char)('0' + (u % 10u));
        u /= 10u;
    }
    while (n--)
        AX_DebugPuts((char[]){ buf[n], 0 });
}

void AX_Info(const char *fmt, ...)
{
    char text[MAX_PRINTMSG];
    va_list ap;

    va_start(ap, fmt);
    qvsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    AX_DebugPuts(text);
}

uint32_t AX_NowMS(void)
{
    return axg_ms();
}

/* ------------------------------------------------------------------ */
/* sys.h                                                              */
/* ------------------------------------------------------------------ */

void Sys_Printf(const char *fmt, ...)
{
    char text[MAX_PRINTMSG];
    va_list ap;

    va_start(ap, fmt);
    qvsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    AX_DebugPuts(text);
}

void Sys_DebugLog(const char *file, const char *fmt, ...)
{
    char text[MAX_PRINTMSG];
    va_list ap;

    (void)file;

    va_start(ap, fmt);
    qvsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    AX_DebugPuts(text);
}

void Sys_Error(const char *error, ...)
{
    char text[MAX_PRINTMSG];
    va_list ap;

    va_start(ap, error);
    qvsnprintf(text, sizeof(text), error, ap);
    va_end(ap);

    AX_DebugPuts("QUAKE ERROR: ");
    AX_DebugPuts(text);
    AX_DebugPuts("\n");

    exit(1);
}

void Sys_Quit(void)
{
    AX_DebugPuts("QUAKE: quit\n");
    exit(0);
}

double Sys_DoubleTime(void)
{
    return (double)AX_NowMS() / 1000.0;
}

void Sys_Sleep(void)
{
    axg_yield();
}

char *Sys_ConsoleInput(void)
{
    return NULL;
}

void Sys_SendKeyEvents(void)
{
    AX_PumpEvents();
}

/* ArtyomX64 всегда клиент, отдельных серверных сборок нет. */
qboolean isDedicated = false;

void Sys_RegisterVariables(void) { }
void Sys_Init(void) { }
void Sys_SetFPCW(void) { }
void Sys_HighFPPrecision(void) { }
void Sys_LowFPPrecision(void) { }

int64_t Sys_FileTime(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f) {
        fclose(f);
        return 1;
    }
    return -1;
}

void Sys_mkdir(const char *path)
{
    (void)path;
}

const char *Sys_UserDataDirectory(void)
{
    return NULL;
}

void Sys_MakeCodeWriteable(void *start_addr, void *end_addr)
{
    (void)start_addr;
    (void)end_addr;
}

void Sys_MakeCodeUnwriteable(void *start_addr, void *end_addr)
{
    (void)start_addr;
    (void)end_addr;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static char arg0[] = "quake";
static char arg1[] = "-mem";
static char arg2[] = "20";
static char arg3[] = "-nostdout";

static char *qargv[] = { arg0, arg1, arg2, arg3, NULL };
static int   qargc = 4;

/*
 * Зона Quake ("hunk": уровень, edicts, кэш текстур и моделей).
 *
 * Раньше она бралась из кучи mini-libc (16 МиБ), и её же делили с
 * ней прочие malloc'и приложения.  Для настоящей игры этого мало:
 * на загрузке карты Quake просит ~8.6 МиБ под edicts одним куском,
 * а следом ещё несколько мегабайт под саму геометрию, и всё это
 * должно лежать в зоне непрерывно.  16 МиБ -- рекомендуемый id
 * минимум, 20 даёт запас на кэш.
 *
 * Отдельный .bss-массив (а не malloc) гарантирует, что место не
 * зависит от того, сколько успела занять куча libc.
 */
#define QUAKE_HUNK_BYTES (20u * 1024u * 1024u)

static unsigned char quake_hunk[QUAKE_HUNK_BYTES]
    __attribute__((aligned(64)));

int main(int argc, char **argv)
{
    quakeparms_t parms;
    double oldtime;
    int memsize;

    (void)argc;
    (void)argv;

    AX_InitDebug();
    AX_DebugPuts("QUAKE: main\n");

    memset(&parms, 0, sizeof(parms));

    COM_InitArgv(qargc, (const char **)qargv);
    parms.argc = com_argc;
    parms.argv = com_argv;
    parms.basedir = "";             /* initrd плоский: файлы в корне */

    memsize = (int)Memory_GetSize();
    if (memsize <= 0 || (size_t)memsize > sizeof(quake_hunk))
        memsize = (int)sizeof(quake_hunk);

    parms.membase = quake_hunk;
    parms.memsize = memsize;

    AX_DebugPuts("QUAKE: heap ");
    AX_DebugDec(memsize / (1024 * 1024));
    AX_DebugPuts(" MiB\n");

    Host_Init(&parms, NULL);

    AX_DebugPuts("QUAKE: host initialised\n");

    oldtime = Sys_DoubleTime() - 0.1;

    for (;;) {
        static int dbg_frames;
        double newtime = Sys_DoubleTime();
        double time = newtime - oldtime;

        if (time > sys_ticrate.value * 2)
            oldtime = newtime;
        else
            oldtime += time;

        if (dbg_frames < 6) {
            AX_DebugPuts("QUAKE: frame ");
            AX_DebugDec(dbg_frames);
            AX_DebugPuts("\n");
            dbg_frames++;
        }

        Host_Frame((float)time);

        /*
         * Host_FilterTime может пропустить кадр (72 Гц).  Насос
         * событий вне Host_Frame нужен, чтобы клавиши не запаздывали,
         * а yield -- чтобы wserver успевал принимать ввод и рисовать.
         */
        AX_PumpEvents();
        axg_yield();
    }
    return 0;
}
