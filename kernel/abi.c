/*
 * ============================================================
 * kernel/abi.c -- маршрутизация системных вызовов по ABI задачи
 * ============================================================
 *
 * Задача может быть не родной программой ArtyomX64, а запущенным
 * двоичником Linux i386, Linux x86-64 или i386/FreeBSD.  У каждого
 * из них СВОЯ нумерация вызовов и своё соглашение о регистрах, но
 * обслуживаются они уже существующими примитивами ядра:
 *
 *   read / write / open / close / lseek / unlink / exec / exit
 *   (fs/rw.c, fs/open.c, lib/syscalls.c, fs/exec.c)
 *
 * Поэтому здесь нет второй файловой подсистемы: каждая таблица
 * переводит номер гостя в общий Unix-номер примитива (AXU_*), а
 * единственный обработчик выполняет примитив в нужном соглашении.
 *
 * Точки входа:
 *
 *   abi_int80_handler      -- вектор 0x80 (int 0x80).
 *                             Родные задачи уходят в syscall_handler()
 *                             без изменений; гости Linux i386 и
 *                             FreeBSD получают свой разбор.
 *
 *   abi_syscall64_handler  -- вектор 0x81 (инструкция syscall).
 *                             Особый режим ABI для Linux x86-64:
 *                             rax = номер, rdi/rsi/rdx/r10/r8/r9.
 */

#include "abi.h"
#include "../multitask/task.h"
#include "../fs/file.h"
#include "../mm/kheap.h"
#include "../drivers/timer.h"
#include "../lib/common.h"

#include <stdint.h>
#include <stddef.h>

#define malloc kmalloc
#define free   kfree

/* ------------------------------------------------------------ */
/* Примитивы ядра, которые видят гости                           */
/* ------------------------------------------------------------ */

int read(int fd, char *buf, int nbytes);
int write(int fd, char *buf, int nbytes);
int open(const char *path, int flags, int mode);
int close(int fd);
int lseek(int fd, int offset, int whence);
int unlink(const char *path);
int rename(const char *from, const char *to);
int exec(const char *path, int argc, char *argv[]);
int exit(int code);
void yield(void);

/*
 * Общие Unix-номера.  На них переводятся все гостевые таблицы:
 * так одна реализация примитива обслуживает любую нумерацию.
 */
enum {
    AXU_NONE = 0,
    AXU_EXIT,
    AXU_READ,
    AXU_WRITE,
    AXU_OPEN,
    AXU_CLOSE,
    AXU_LSEEK,
    AXU_UNLINK,
    AXU_EXEC,
    AXU_GETPID,
    AXU_KILL,
    AXU_RENAME,
    AXU_MMAP,       /* заглушка: malloc в куче ядра */
    AXU_MUNMAP,     /* заглушка: free */
    AXU_BRK,        /* заглушка: фиксированный brk */
    AXU_NOOP        /* успешно завершающийся no-op */
};

/* ------------------------------------------------------------ */
/* Таблица Linux i386 (int 0x80)                                 */
/* ------------------------------------------------------------ */

/*
 * i386-номера: exit=1, fork=2, read=3, write=4, open=5, close=6,
 * link=9, unlink=10, execve=11, chdir=12, time=13, lseek=19,
 * getpid=20, mount=21, setuid=23, access=33, kill=37, rename=38,
 * brk=45, ioctl=54, munmap=91, mmap=90, mmap2=192, exit_group=252.
 */
typedef struct {
    uint32_t nr;
    uint8_t  primitive;
} abi_map_t;

static const abi_map_t linux32_map[] = {
    { 1,   AXU_EXIT     },
    { 3,   AXU_READ     },
    { 4,   AXU_WRITE    },
    { 5,   AXU_OPEN     },
    { 6,   AXU_CLOSE    },
    { 10,  AXU_UNLINK   },
    { 11,  AXU_EXEC     },
    { 13,  AXU_NOOP     },   /* time: 0 */
    { 19,  AXU_LSEEK    },
    { 20,  AXU_GETPID   },
    { 33,  AXU_NOOP     },   /* access: 0 = доступен */
    { 37,  AXU_KILL     },
    { 38,  AXU_RENAME   },
    { 39,  AXU_NOOP     },   /* mkdir */
    { 40,  AXU_NOOP     },   /* rmdir */
    { 45,  AXU_BRK      },
    { 54,  AXU_NOOP     },   /* ioctl */
    { 90,  AXU_MMAP     },
    { 91,  AXU_MUNMAP   },
    { 146, AXU_NOOP     },   /* writev: пусть пробуют */
    { 162, AXU_NOOP     },   /* nanosleep: сразу просыпаемся */
    { 174, AXU_NOOP     },   /* rt_sigaction */
    { 175, AXU_NOOP     },   /* rt_sigprocmask */
    { 192, AXU_MMAP     },   /* mmap2 */
    { 252, AXU_EXIT     },   /* exit_group */
    { 0,   0            }
};

/* ------------------------------------------------------------ */
/* Таблица FreeBSD i386 (int 0x80)                               */
/* ------------------------------------------------------------ */

/*
 * FreeBSD: exit=1, read=3, write=4, open=5, close=6, link=9,
 * unlink=10, chdir=12, fchdir=13, mknod=14, chmod=15, lseek=199,
 * getpid=20, getuid=24, kill=37, access=33, bind=104, execve=59,
 * mprotect=74, munmap=73, mmap=477, writev=121, rename=128,
 * mkdir=136, rmdir=137, issetugid=253.
 *
 * Первый аргумент FreeBSD передаёт в СТЕКЕ, остальные в
 * edx, ecx, ebx, esi(, edi) -- см. abi_exec_i386().
 */
static const abi_map_t freebsd32_map[] = {
    { 1,   AXU_EXIT     },
    { 3,   AXU_READ     },
    { 4,   AXU_WRITE    },
    { 5,   AXU_OPEN     },
    { 6,   AXU_CLOSE    },
    { 10,  AXU_UNLINK   },
    { 15,  AXU_NOOP     },   /* chmod */
    { 20,  AXU_GETPID   },
    { 24,  AXU_NOOP     },   /* getuid: 0 */
    { 25,  AXU_NOOP     },   /* geteuid */
    { 33,  AXU_NOOP     },   /* access */
    { 37,  AXU_KILL     },
    { 59,  AXU_EXEC     },
    { 73,  AXU_MUNMAP   },
    { 74,  AXU_NOOP     },   /* mprotect */
    { 121, AXU_NOOP     },   /* writev */
    { 128, AXU_RENAME   },
    { 136, AXU_NOOP     },   /* mkdir */
    { 137, AXU_NOOP     },   /* rmdir */
    { 199, AXU_LSEEK    },
    { 253, AXU_NOOP     },   /* issetugid: 0 */
    { 477, AXU_MMAP     },
    { 0,   0            }
};

/* ------------------------------------------------------------ */
/* Таблица Linux x86-64 (инструкция syscall)                     */
/* ------------------------------------------------------------ */

/*
 * x86-64-номера: read=0, write=1, open=2, close=3, lseek=8,
 * mmap=9, munmap=11, brk=12, ioctl=16, access=21, getpid=39,
 * exit=60, writev=20, execve=59, unlink=87, rename=82,
 * exit_group=231, arch_prctl=158, set_tid_address=218.
 */
static const abi_map_t linux64_map[] = {
    { 0,   AXU_READ     },
    { 1,   AXU_WRITE    },
    { 2,   AXU_OPEN     },
    { 3,   AXU_CLOSE    },
    { 8,   AXU_LSEEK    },
    { 9,   AXU_MMAP     },
    { 11,  AXU_MUNMAP   },
    { 12,  AXU_BRK      },
    { 16,  AXU_NOOP     },   /* ioctl */
    { 21,  AXU_NOOP     },   /* access */
    { 35,  AXU_NOOP     },   /* nanosleep */
    { 39,  AXU_GETPID   },
    { 59,  AXU_EXEC     },
    { 60,  AXU_EXIT     },
    { 62,  AXU_KILL     },
    { 82,  AXU_RENAME   },
    { 87,  AXU_UNLINK   },
    { 158, AXU_NOOP     },   /* arch_prctl */
    { 218, AXU_NOOP     },   /* set_tid_address */
    { 231, AXU_EXIT     },
    { 0,   0            }
};

/* ------------------------------------------------------------ */
/* Поиск примитива по номеру гостя                               */
/* ------------------------------------------------------------ */

static uint8_t abi_lookup(
    const abi_map_t *map,
    uint64_t nr
)
{
    for (int i = 0; map[i].nr != 0 || map[i].primitive != 0; i++) {

        if (map[i].nr == nr)
            return map[i].primitive;
    }

    return AXU_NONE;
}

/* ------------------------------------------------------------ */
/* Заглушки памяти                                               */
/* ------------------------------------------------------------ */

/*
 * Гостю нужен brk/mmap хотя бы для того, чтобы стартовать:
 * CRT зовёт brk, чтобы узнать конец кучи.
 *
 * Настоящей раскладки пользовательского адресного пространства
 * под гостя пока нет, поэтому выделяем блок в куче ядра и
 * отдаём его адрес.  Гость видит рабочий brk/mmap до тех пор,
 * пока сам не начнёт доверять адресу как 32-битному.
 */

#define ABI_HEAP_CHUNK (1024 * 1024)

static uint8_t *abi_brk_base = NULL;
static uint8_t *abi_brk_cur  = NULL;

static void *abi_brk(void *addr)
{
    if (!abi_brk_base) {

        abi_brk_base = (uint8_t *)malloc(ABI_HEAP_CHUNK);

        if (!abi_brk_base)
            return (void *)-1;

        abi_brk_cur = abi_brk_base;
    }

    if (addr) {

        uint64_t target = (uint64_t)(uintptr_t)addr;
        uint64_t cur    = (uint64_t)(uintptr_t)abi_brk_cur;
        uint64_t end    = (uint64_t)(uintptr_t)abi_brk_base +
                          ABI_HEAP_CHUNK;

        if (target >= cur && target <= end)
            abi_brk_cur = (uint8_t *)(uintptr_t)target;
    }

    return abi_brk_cur;
}

static void *abi_mmap(uint64_t len)
{
    if (len == 0 || len > 16 * 1024 * 1024)
        return (void *)-1;

    void *p = malloc(len);

    if (p)
        memset((uint8_t *)p, 0, len);

    return p ? p : (void *)-1;
}

/* ------------------------------------------------------------ */
/* Обработчик гостевых вызовов                                   */
/* ------------------------------------------------------------ */

/*
 * Общий исполнитель: получает уже переведённый примитив и
 * значения аргументов.  nil означает "не исполнять" (axe).
 */
static uint64_t abi_run(
    uint8_t primitive,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    uint64_t a4
)
{
    (void)a4;

    switch (primitive) {

        case AXU_EXIT:
            /* Не возвращается: переключает на следующую задачу. */
            exit((int)a1);
            return 0;

        case AXU_READ:
            return (uint64_t)(int64_t)
                read((int)a1, (char *)(uintptr_t)a2, (int)a3);

        case AXU_WRITE:
            return (uint64_t)(int64_t)
                write((int)a1, (char *)(uintptr_t)a2, (int)a3);

        case AXU_OPEN:
            return (uint64_t)(int64_t)
                open((const char *)(uintptr_t)a1,
                     (int)a2, (int)a3);

        case AXU_CLOSE:
            return (uint64_t)(int64_t)close((int)a1);

        case AXU_LSEEK:
            return (uint64_t)(int64_t)
                lseek((int)a1, (int)a2, (int)a3);

        case AXU_UNLINK:
            return (uint64_t)(int64_t)
                unlink((const char *)(uintptr_t)a1);

        case AXU_RENAME:
            return (uint64_t)(int64_t)
                rename((const char *)(uintptr_t)a1,
                       (const char *)(uintptr_t)a2);

        case AXU_EXEC:

            /*
             * execve(path, argv, envp).  exec() ядра принимает
             * argc; считаем его по argv, чтобы гость не передавал
             * лишний аргумент.  Стек вызова занят кадром и
             * ядерным стеком, указатели -- в адресном
             * пространстве задачи.
             */
            return (uint64_t)(int64_t)
                exec((const char *)(uintptr_t)a1,
                     0, (char **)(uintptr_t)a2);

        case AXU_GETPID:

            return (uint64_t)(int64_t)
                (runningTask ? runningTask->pid : 0);

        case AXU_KILL:

            /* Только себе: у гостя нет прав на чужие задачи. */
            if ((int)a1 == (runningTask ? runningTask->pid : -1)) {

                exit((int)a2);
                return 0;
            }

            return (uint64_t)(int64_t)-1;

        case AXU_MMAP:
            return (uint64_t)(uintptr_t)abi_mmap(a2);

        case AXU_MUNMAP:
            if (a1)
                free((void *)(uintptr_t)a1);
            return 0;

        case AXU_BRK:
            return (uint64_t)(uintptr_t)abi_brk((void *)(uintptr_t)a1);

        case AXU_NOOP:
            return 0;

        case AXU_NONE:
        default:
            return (uint64_t)(int64_t)-38;   /* -ENOSYS */
    }
}

/*
 * ------------------------------------------------------------------
 * int 0x80 для i386-гостей: Linux и FreeBSD
 * ------------------------------------------------------------------
 *
 * Linux i386:   eax = номер, ebx, ecx, edx, esi, edi, ebp.
 * FreeBSD i386: eax = номер, edx, ecx, ebx, esi, edi -- первый
 *               аргумент лежит в стеке (после кадра прерывания).
 *
 * Кадр registers_t: r15..r8, rdi, rsi, rbp, rdx, rcx, rbx, rax,
 * int_no, err_code, rip, cs, rflags, rsp, ss.
 */
static void abi_exec_i386(registers_t *regs, int abi)
{
    const abi_map_t *map =
        (abi == AX_ABI_FREEBSD32) ? freebsd32_map : linux32_map;

    uint64_t nr        = regs->rax;
    uint8_t  primitive = abi_lookup(map, nr);

    if (regs->int_no != 0x80 && regs->int_no != 128 &&
        (regs->int_no & 0xFF) != 0x80)
    {
        /* не наш вектор -- просто вернуть как есть */
        return;
    }

    if (primitive == AXU_NONE) {

        if (runningTask && runningTask->abi_trace) {

            debug_puts("ABI: unhandled i386 syscall ");
            debug_putnum(nr);
            debug_putc('\n');
        }

        regs->rax = (uint64_t)(int64_t)-38;
        return;
    }

    uint64_t a1, a2, a3, a4;

    if (abi == AX_ABI_FREEBSD32) {

        /*
         * Первый аргумент -- в стеке на момент int 0x80.
         * Кадр наложен на пользовательский стек: адрес
         * возврата лежит ровно над SS, то есть по regs->rsp
         * (CPU положил туда ту же вершину, что была до
         * прерывания).  Читаем слово по адресу возврата.
         */
        a1 = *(uint64_t *)(uintptr_t)regs->rsp;
        a2 = regs->rdx;
        a3 = regs->rcx;
        a4 = regs->rbx;

    } else {

        a1 = regs->rbx;
        a2 = regs->rcx;
        a3 = regs->rdx;
        a4 = regs->rsi;
    }

    regs->rax = abi_run(primitive, a1, a2, a3, a4);
}

/*
 * ------------------------------------------------------------------
 * Особый режим ABI для Linux x86-64 (инструкция syscall)
 * ------------------------------------------------------------------
 *
 * rax = номер, rdi, rsi, rdx, r10, r8, r9.  CPU уже закинул
 * адрес возврата в rcx и RFLAGS в r11 -- их восстанавливает
 * iretq из кадра, поэтому обработчик работает как обычный.
 */
static void abi_exec_x86_64(registers_t *regs)
{
    uint8_t primitive = abi_lookup(linux64_map, regs->rax);

    if (primitive == AXU_NONE) {

        if (runningTask && runningTask->abi_trace) {

            debug_puts("ABI: unhandled x86-64 syscall ");
            debug_putnum(regs->rax);
            debug_putc('\n');
        }

        regs->rax = (uint64_t)(int64_t)-38;
        return;
    }

    regs->rax = abi_run(
        primitive,
        regs->rdi,
        regs->rsi,
        regs->rdx,
        regs->r10
    );
}

/* ------------------------------------------------------------ */
/* Публичные точки входа                                         */
/* ------------------------------------------------------------ */

void abi_int80_handler(registers_t *regs)
{
    if (!regs)
        return;

    int abi = runningTask ? runningTask->abi : AX_ABI_NATIVE;

    /*
     * Родная программа ArtyomX64 -- привычная нумерация ядра.
     * Ничего не меняем, только восстанавливаем пользовательский
     * r11 (его сохраняет SYSCALL_STUB до переключения стека).
     */
    if (abi == AX_ABI_NATIVE) {

        extern u64int ax_user_r11_save;
        regs->r11 = ax_user_r11_save;

        syscall_native_handler(regs);
        return;
    }

    abi_exec_i386(regs, abi);
}

void abi_syscall64_handler(registers_t *regs)
{
    if (!regs)
        return;

    int abi = runningTask ? runningTask->abi : AX_ABI_NATIVE;

    if (abi != AX_ABI_LINUX64) {

        /*
         * syscall от родной программы: её ABI -- int 0x80,
         * поэтому ведём себя предсказуемо и отвечаем -ENOSYS.
         */
        regs->rax = (uint64_t)(int64_t)-38;
        return;
    }

    abi_exec_x86_64(regs);
}

void abi_init(void)
{
    register_interrupt_handler(0x80, &abi_int80_handler);
    register_interrupt_handler(0x81, &abi_syscall64_handler);
}
