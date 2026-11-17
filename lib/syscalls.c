/*
 * syscalls.c
 *
 * System calls + per-task file descriptor management.
 */

#include "syscalls.h"

#include "../drivers/monitor.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../kernel/isr.h"

#include "../fs/fs.h"
#include "../multitask/elf.h"
#include "../fs/initrd.h"
#include "../fs/ix/ix_automount.h"
#include "../multitask/task.h"
#include "../fs/file.h"

#include "../mm/kheap.h"

#include "../lib/common.h"
#include "../lib/axipc.h"
#include "../lib/axport.h"
#include "../kernel/ipc.h"

#include "../dev/console.h"

#include <stddef.h>
#include <stdint.h>

#define TCFLSH 21515

#define MAX_TASKS 10

#define malloc kmalloc
#define free   kfree

extern file_t *get_file(int fd);
file_t *make_file(
    fs_node_t *node,
    u32int flags
);
/*
 * ============================================================
 * Внешние сервисы ядра (GUI-нейтральные примитивы).
 *
 * ВАЖНО (модель NeXT/macOS): GUI-сисколлов в ядре БОЛЬШЕ НЕТ.
 * Окна, композитор, события -- всё это живёт в отдельном
 * процессе wserver (см. server/), клиенты ходят в него через
 * порты (178-181).  Ядро предоставляет только примитивы из
 * kernel/ipc.c.
 * ============================================================
 */

extern void *image_load(
    char *elf_start,
    unsigned int size
);

/*
 * ============================================================
 * POSIX syscalls
 * ============================================================
 */
extern int read(
    int fd,
    char *buf,
    int nbytes
);
extern int write(
    int fd,
    char *buf,
    int nbytes
);
extern int open(
    const char *path,
    int flags,
    int mode
);

extern int exec(
    const char *path,
    int argc,
    char *argv[]
);
extern int close(
    int fd
);
extern int exit(int code);

#include "../fs/sock.h"
#include "../drivers/net/ixn_l2l3.h"
#include "../drivers/net/ixn_netdev.h"

/*
 * ============================================================
 * Debug output
 * ============================================================
 */

/*
 * Всё, что уходит в COM1, дублируется в память (первые 64 КиБ), чтобы
 * на машинах без COM-порта лог можно было прочитать из ОС
 * (ядро кладёт его в файл storage.log).
 */
#define DBG_LOG_SIZE 65536
static char     g_dbg_log[DBG_LOG_SIZE];
static uint32_t g_dbg_len = 0;

const char *debug_log_data(uint32_t *len)
{
    if (len) *len = g_dbg_len;
    return g_dbg_log;
}

void debug_putc(char c)
{
    if (g_dbg_len < DBG_LOG_SIZE - 1)
        g_dbg_log[g_dbg_len++] = c;
    outb(0x3F8, c);
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

    if (n == 0)
    {
        debug_putc('0');
        return;
    }

    while (n)
    {
        buf[i++] =
            '0' + (n % 10);

        n /= 10;
    }

    while (i)
        debug_putc(buf[--i]);
}

/*
 * ============================================================
 * Path helper
 * ============================================================
 *
 * Формат initrd плоский (имена файлов без '/'), а приложения
 * и компилятор tcc оперируют путями вида "/stdio.h". Пропускаем
 * ведущие слэши перед поиском/созданием узла.
 */

int ax_disk_split_path(const char *name, int *idx, char *rel, size_t cap);

const char *skip_leading_slashes(const char *path)
{
    while (*path == '/')
        path++;

    return path;
}


/*
 * ============================================================
 * Standard descriptors
 * ============================================================
 */

static int setup_stdio(
    Task *task,
    fs_node_t *node
)
{
    if (!task || !node)
        return -1;

    for (int fd = 0;
         fd < 3;
         fd++)
    {
        if (task->files[fd])
        {
            free(
                task->files[fd]
            );

            task->files[fd] =
                NULL;
        }

        file_t *f =
            make_file(
                node,
                0
            );

        if (!f)
        {
            for (int i = 0;
                 i < fd;
                 i++)
            {
                if (task->files[i])
                {
                    free(
                        task->files[i]
                    );

                    task->files[i] =
                        NULL;
                }
            }

            return -1;
        }

        task->files[fd] =
            f;
    }

    return 0;
}

/*
 * ============================================================
 * getdents
 * ============================================================
 */

int getdents(
    int fd,
    void *buf,
    u32int size
)
{
    if (!buf || size < 1)
        return -1;

    file_t *f =
        get_file(fd);

    if (!f || !f->node)
        return -1;

    if ((f->node->flags & 0x7) !=
        FS_DIRECTORY)
    {
        return -1;
    }

    u8int *out =
        (u8int *)buf;

    u32int written = 0;

    u32int idx =
        f->pos;

    while (written + 10 < size)
    {
        struct dirent *de =
            readdir_fs(
                f->node,
                idx
            );

        if (!de)
            break;

        /*
         * Empty entries are skipped by
         * initrd_readdir(), but we still
         * protect against empty names here.
         */
        if (!de->name[0])
        {
            idx++;
            continue;
        }

        u32int namelen = 0;

        while (
            namelen < 127 &&
            de->name[namelen]
        )
        {
            namelen++;
        }

        u32int reclen =
            8 +
            namelen +
            1;

        if (written + reclen > size)
            break;

        struct linux_dirent *d =
            (struct linux_dirent *)
                (out + written);

        d->d_ino =
            de->ino;

        d->d_reclen =
            (u16int)reclen;

        d->d_type =
            (u8int)(
                de->type & 0xFF
            );

        for (u32int i = 0;
             i < namelen;
             i++)
        {
            d->d_name[i] =
                de->name[i];
        }

        d->d_name[namelen] =
            0;

        written += reclen;

        idx++;
    }

    f->pos =
        idx;

    return (int)written;
}



/*
 * ============================================================
 * lseek (syscall 8)
 * ============================================================
 *
 * Позиционирование в открытом файле. Нужен компилятору tcc:
 * tcc определяет размер исходников через lseek(SEEK_END) и
 * пишет ELF-вывод через lseek(SEEK_SET) + write.
 *
 * whence: 0 = SEEK_SET, 1 = SEEK_CUR, 2 = SEEK_END.
 *
 * Позиция ограничена [0, node->length]: запись за концом файла
 * всё равно сделает нулевой зазор (initrd_write умеет gaps),
 * а чтение за концом всегда даёт 0 байт.
 */

int lseek(
    int fd,
    int offset,
    int whence
)
{
    long new_pos;

    file_t *f =
        get_file(fd);

    if (!f || !f->node)
        return -1;

    if ((f->node->flags & 0x7) ==
        FS_DIRECTORY)
    {
        return -1;
    }

    switch (whence)
    {
        case 0:                     /* SEEK_SET */

            new_pos =
                (long)offset;

            break;

        case 1:                     /* SEEK_CUR */

            new_pos =
                (long)f->pos +
                (long)offset;

            break;

        case 2:                     /* SEEK_END */

            new_pos =
                (long)f->node->length +
                (long)offset;

            break;

        default:
            return -1;
    }

    if (new_pos < 0)
        return -1;

    /*
     * POSIX разрешает позицию за концом файла: запись сделает
     * нулевой зазор (initrd_write умеет gaps), чтение вернёт 0.
     * tcc как раз пишет ELF с lseek вперёд. Ограничиваем только
     * положительным int (возврат lseek — int).
     */
    if (new_pos > 0x7FFFFFFFL)
        return -1;

    f->pos =
        (u32int)new_pos;

    return (int)new_pos;
}




/*
 * ============================================================
 * unlink
 * ============================================================
 */

int unlink(
    const char *path
)
{
    const char *name;

    if (!path || !path[0])
        return -1;

    name = skip_leading_slashes(path);

    if (!name || !name[0])
        return -1;

    {
        int idx;
        char rel[192];

        if (ax_disk_split_path(name, &idx, rel, sizeof(rel)) == 0)
            return automount_unlink_at(idx, rel);
    }

    return initrd_remove(
        (char *)name
    )
        ? -1
        : 0;
}

int rename(
    const char *from,
    const char *to
)
{
    const char *a;
    const char *b;
    int idx_a;
    int idx_b;
    char rel_a[192];
    char rel_b[192];

    if (!from || !to || !from[0] || !to[0])
        return -1;

    a = skip_leading_slashes(from);
    b = skip_leading_slashes(to);

    if (!a || !a[0] || !b || !b[0])
        return -1;

    /*
     * Диск (disk0/...) -- через автомонтирование.
     * Иначе -- RAM-файл initrd: Doom пишет сохранение в temp.dsg
     * и переименовывает его в doomsavN.dsg.
     */
    if (ax_disk_split_path(a, &idx_a, rel_a, sizeof(rel_a)) == 0) {

        if (ax_disk_split_path(b, &idx_b, rel_b, sizeof(rel_b)) != 0)
            return -1;

        if (idx_a != idx_b)
            return -1;

        return automount_rename_at(idx_a, rel_a, rel_b);
    }

    return initrd_rename(a, b);
}


/*
 * ============================================================
 * ioctl
 * ============================================================
 */

int ioctl(
    int fd,
    unsigned long com,
    char *data
)
{
    file_t *f =
        get_file(fd);

    if (!f || !f->node)
        return -1;

    (void)com;
    (void)data;

    return 0;
}

/*
 * ============================================================
 * IPC
 * ============================================================
 */

int ipc_call(
    int endpoint,
    void *msg
)
{
    (void)endpoint;
    (void)msg;

    return -1;
}


/*
 * ============================================================
 * syscall dispatcher
 * ============================================================
 */

/*
 * Диспетчер родной системы ArtyomX64.
 *
 * Вызывается из abi_int80_handler() (kernel/abi.c) для задач с
 * AX_ABI_NATIVE.  Старое имя syscall_handler сохранено ниже
 * обёрткой, чтобы обработчики других ABI могли брать эту запись
 * таблицы напрямую (compat.c -- векторы 129/130).
 */
void syscall_native_handler(
    registers_t *regs
)
{
    if (!regs)
        return;

    /* Фикс регрессии v2: SYSCALL_STUB сохраняет пользовательский
     * r11 в ax_user_r11_save ДО переключения на ядерно-стек.
     * Восстанавливаем его в регистровый слот кадра -- поп-back
     * в isr_common_stub вернёт пользователю оригинальный r11.
     * (inline-asm int 0x80 в приложениях не объявляет r11
     * clobbered -- компиляторы держат там живые переменные.) */
    {
        extern u64int ax_user_r11_save;
        regs->r11 = ax_user_r11_save;
    }

    switch (regs->rax)
    {
        /*
         * exit
         */
        case 1:

            regs->rax =
                exit(
                    (int)regs->rdi
                );

            break;


        /*
         * read
         */
        case 3:

            regs->rax =
                read(
                    (int)regs->rdi,
                    (char *)regs->rsi,
                    (int)regs->rdx
                );

            break;


        /*
         * write
         */
        case 4:

            regs->rax =
                write(
                    (int)regs->rdi,
                    (char *)regs->rsi,
                    (int)regs->rdx
                );

            break;


        /*
         * lseek
         */
        case 8:

            regs->rax =
                (uint64_t)(int64_t)
                lseek(
                    (int)regs->rdi,
                    (int)regs->rsi,
                    (int)regs->rdx
                );

            break;


        /*
         * open
         */
        case 5:

            regs->rax =
                open(
                    (const char *)regs->rdi,
                    (int)regs->rsi,
                    (int)regs->rdx
                );

            break;


        /*
         * close
         */
        case 6:

            regs->rax =
                close(
                    (int)regs->rdi
                );

            break;


        /*
         * unlink
         */
        case 10:

            regs->rax =
                unlink(
                    (const char *)regs->rdi
                );

            break;


        /*
         * exec
         */
        case 11:

            regs->rax =
                exec(
                    (const char *)regs->rdi,
                    (int)regs->rsi,
                    (char **)regs->rdx
                );

            break;


        /*
         * ioctl
         */
        case 54:

            regs->rax =
                ioctl(
                    (int)regs->rdi,
                    (unsigned long)regs->rsi,
                    (char *)regs->rdx
                );

            break;


        /*
         * getdents
         */
        case 141:

            regs->rax =
                getdents(
                    (int)regs->rdi,
                    (void *)regs->rsi,
                    (u32int)regs->rdx
                );

            break;


        /*
         * IPC
         */
        case 150:

            regs->rax =
                ipc_call(
                    (int)regs->rdi,
                    (void *)regs->rsi
                );

            break;


        /*
         * yield
         */
        case 151:

            yield();

            regs->rax = 0;

            break;


        /*
         * task alive
         */
        case 167:

            regs->rax =
                task_is_alive(
                    (int)regs->rdi
                );

            break;

        /*
         * ========================================================
         * ПРИМИТИВЫ IPC (модель Mach) -- kernel/ipc.c
         *
         * Все GUI-сисколлы (160-166, 168-177) удалены: окна,
         * композитор и события живут в отдельном процессе wserver
         * (аналог WindowServer MacOS X / NeXTSTEP).  Клиенты общаются
         * с ним сообщениями через порты, а ядро видит только конверты
         * ax_msg_t и не знает их семантики.
         * ========================================================
         */

        /* 178 PORT_CREATE */
        case 178:

            regs->rax =
                (uint64_t)(int64_t)
                ipc_port_create((const char *)regs->rdi);

            break;

        /* 179 PORT_LOOKUP */
        case 179:

            regs->rax =
                (uint64_t)(int64_t)
                ipc_port_lookup((const char *)regs->rdi);

            break;

        /* 180 PORT_SEND */
        case 180:

            regs->rax =
                (uint64_t)(int64_t)
                ipc_port_send((int)regs->rdi, (const ax_msg_t *)regs->rsi);

            break;

        /* 181 PORT_RECV */
        case 181:

            regs->rax =
                (uint64_t)
                ipc_port_recv((int)regs->rdi, (ax_msg_t *)regs->rsi);

            break;

        /* 182 GETPID */
        case 182:

            regs->rax =
                (uint64_t)(int64_t)
                ipc_getpid();

            break;

        /* 183 SHM_ALLOC */
        case 183:

            regs->rax =
                (uint64_t)(uintptr_t)
                ipc_shm_alloc((uint64_t)regs->rdi);

            break;

        /* 184 SHM_FREE */
        case 184:

            ipc_shm_free((void *)(uintptr_t)regs->rdi);

            regs->rax = 0;

            break;

        /* 185 TERM_OPEN */
        case 185:

            regs->rax =
                (uint64_t)(int64_t)
                ipc_term_open();

            break;

        /* 186 TERM_READ */
        case 186:

            regs->rax =
                (uint64_t)(int64_t)
                ipc_term_read((int)regs->rdi, (uint8_t *)regs->rsi,
                              (uint32_t)regs->rdx);

            break;

        /* 187 TERM_WRITE */
        case 187:

            regs->rax =
                (uint64_t)(int64_t)
                ipc_term_write((int)regs->rdi, (const uint8_t *)regs->rsi,
                               (uint32_t)regs->rdx);

            break;

        /* 188 TERM_FOCUS */
        case 188:

            regs->rax =
                (uint64_t)(int64_t)
                ipc_term_focus((int)regs->rdi);

            break;

        /* 189 FB_INFO */
        case 189:

            regs->rax =
                (uint64_t)(int64_t)
                ipc_fb_info((ax_fb_info_t *)regs->rdi);

            break;

        /* 190 INPUT_MOUSE */
        case 190:

            regs->rax =
                (uint64_t)(int64_t)
                ipc_input_mouse((ax_mouse_info_t *)regs->rdi);

            break;

        /* 191 INPUT_KEY */
        case 191:

            regs->rax =
                (uint64_t)
                ipc_input_key((ax_key_info_t *)regs->rdi);

            break;

        /* 192 PROC_NAME */
        case 192:

            regs->rax =
                (uint64_t)(int64_t)
                ipc_proc_name((int)regs->rdi, (char *)regs->rsi,
                              (uint32_t)regs->rdx);

            break;

        /* 193 GPU_SUBMIT_RT (ixg, без понятия "окон") */
        case 193:

            regs->rax =
                (uint64_t)(int64_t)
                ipc_gpu_submit_rt((uint64_t)regs->rdi,
                                  (uint32_t)regs->r10,
                                  (uint32_t)regs->r8,
                                  (const void *)regs->rsi,
                                  (uint32_t)regs->rdx);

            break;

        /* 195 TERM_RELEASE */
        case 195:

            regs->rax =
                (uint64_t)(int64_t)
                ipc_term_release((int)regs->rdi);

            break;

        /*
         * 196 TICKS_MS -- монотонные миллисекунды с момента
         * init_timer(1000).  Нужен userspace-программам с
         * реальным временем (DOOM: DG_GetTicksMs).
         */
        case 196:

            regs->rax =
                timer_ticks_ms();

            break;


        /*
         * 197 FP_TYPE -- тип программы по пути
         * (PROGRAM_NONE / PROGRAM_CONSOLE / PROGRAM_GUI из
         * заголовка mkinitrd2; -1 если файла нет).  Файловому
         * менеджеру: console-ELF'ы запускаются в терминале.
         */
        case 197:

            regs->rax =
                (uint64_t)(int64_t)
                fp_type(
                    (const char *)regs->rdi
                );

            break;


        /*
         * ------------------------------------------------------------
         * BSD SOCKET LAYER (fs/sock.c) -- UNIX Socket / BSD socket
         * ------------------------------------------------------------
         *
         * 200 SOCKET(domain, type, protocol)          -> fd
         * 201 BIND(fd, const ax_sockaddr*, len)       -> 0 / -1
         * 202 CONNECT(fd, const ax_sockaddr*, len)    -> 0 / -1
         * 203 LISTEN(fd, backlog)                     -> 0 / -1
         * 204 ACCEPT(fd, addr*, len*)                 -> fd
         * 205 SENDTO(fd, buf, len, flags, addr, alen) -> bytes
         * 206 RECVFROM(fd, buf, len, flags, addr, alen*) -> bytes
         * 207 SHUTDOWN(fd, how)                       -> 0 / -1
         * 208 GETSOCKNAME(fd, addr*, len*)            -> 0 / -1
         * 209 GETPEERNAME(fd, addr*, len*)            -> 0 / -1
         * 210 SETSOCKOPT(fd, level, opt, val, olen)   -> 0 / -1
         * 211 GETSOCKOPT(fd, level, opt, val, olen*)  -> 0 / -1
         * 212 SOCKETPAIR(domain, type, proto, int[2]*) -> 0 / -1
         */

        case 200:

            regs->rax =
                (uint64_t)(int64_t)
                ax_sock_create((uint32_t)regs->rdi,
                               (uint32_t)regs->rsi,
                               (uint32_t)regs->rdx);

            break;

        case 201:

            regs->rax =
                (uint64_t)(int64_t)
                ax_sock_bind((int)regs->rdi,
                             (const ax_sockaddr_t *)(uintptr_t)regs->rsi,
                             (uint32_t)regs->rdx);

            break;

        case 202:

            regs->rax =
                (uint64_t)(int64_t)
                ax_sock_connect((int)regs->rdi,
                                (const ax_sockaddr_t *)(uintptr_t)regs->rsi,
                                (uint32_t)regs->rdx);

            break;

        case 203:

            regs->rax =
                (uint64_t)(int64_t)
                ax_sock_listen((int)regs->rdi, (uint32_t)regs->rsi);

            break;

        case 204:

            regs->rax =
                (uint64_t)(int64_t)
                ax_sock_accept((int)regs->rdi,
                               (ax_sockaddr_t *)(uintptr_t)regs->rsi,
                               (uint32_t *)(uintptr_t)regs->rdx);

            break;

        case 205:

            regs->rax =
                (uint64_t)(int64_t)
                ax_sock_sendto((int)regs->rdi,
                               (const void *)(uintptr_t)regs->rsi,
                               (uint32_t)regs->rdx,
                               (uint32_t)regs->r10,
                               (const ax_sockaddr_t *)(uintptr_t)regs->r8,
                               (uint32_t)regs->r9);

            break;

        case 206:

            regs->rax =
                (uint64_t)(int64_t)
                ax_sock_recvfrom((int)regs->rdi,
                                 (void *)(uintptr_t)regs->rsi,
                                 (uint32_t)regs->rdx,
                                 (uint32_t)regs->r10,
                                 (ax_sockaddr_t *)(uintptr_t)regs->r8,
                                 (uint32_t *)(uintptr_t)regs->r9);

            break;

        case 207:

            regs->rax =
                (uint64_t)(int64_t)
                ax_sock_shutdown((int)regs->rdi, (uint32_t)regs->rsi);

            break;

        case 208:

            regs->rax =
                (uint64_t)(int64_t)
                ax_sock_getsockname((int)regs->rdi,
                                    (ax_sockaddr_t *)(uintptr_t)regs->rsi,
                                    (uint32_t *)(uintptr_t)regs->rdx);

            break;

        case 209:

            regs->rax =
                (uint64_t)(int64_t)
                ax_sock_getpeername((int)regs->rdi,
                                    (ax_sockaddr_t *)(uintptr_t)regs->rsi,
                                    (uint32_t *)(uintptr_t)regs->rdx);

            break;

        case 210:

            regs->rax =
                (uint64_t)(int64_t)
                ax_sock_setsockopt((int)regs->rdi,
                                   (uint32_t)regs->rsi,
                                   (uint32_t)regs->rdx,
                                   (const void *)(uintptr_t)regs->r10,
                                   (uint32_t)regs->r8);

            break;

        case 211:

            regs->rax =
                (uint64_t)(int64_t)
                ax_sock_getsockopt((int)regs->rdi,
                                   (uint32_t)regs->rsi,
                                   (uint32_t)regs->rdx,
                                   (void *)(uintptr_t)regs->r10,
                                   (uint32_t *)(uintptr_t)regs->r8);

            break;

        case 212:

            regs->rax =
                (uint64_t)(int64_t)
                ax_sock_pair((uint32_t)regs->rdi,
                             (uint32_t)regs->rsi,
                             (uint32_t)regs->rdx,
                             (int *)(uintptr_t)regs->r10);

            break;

        /*
         * 213 PING
         *
         * sys_ping(ip_str, seq, wait_ms, out) -- ICMP echo через
         * ixn-стек: отправка echo-request, ожидание эхо-ответа из
         * ixn_ping_get_reply() не дольше wait_ms.
         *
         * out[0] = 1 ответ получен / 0 таймаут
         * out[1] = seq ответа
         * out[2] = адрес ответчика (host order)
         * out[3] = ttl
         * out[4] = размер пакета
         * out[5] = RTT в микросекундах
         */
        case 213: {

            extern int ixn_ping_get_reply(ixn_ping_reply_t *out);
            extern int ixn_icmp_send_echo(ixn_netdev_t *dev, uint32_t dst_ip,
                                          uint16_t id, uint16_t seq,
                                          const uint8_t *data, uint32_t len);
            extern ixn_netdev_t *ixn_netdev_get_default(void);
            extern ixn_netdev_t *ixn_netdev_get_loopback(void);
            extern int ixn_arp_ensure(ixn_netdev_t *dev, uint32_t dst_ip,
                                      uint32_t timeout_ms);
            extern void ixn_netdev_poll_all(uint32_t budget);
            extern uint32_t ixn_ipv4_parse_str(const char *s);
            extern uint64_t ixn_ticks_ms(void);

            ixn_ping_reply_t rep;
            ixn_netdev_t *ndev;
            int64_t *out = (int64_t *)(uintptr_t)regs->r10;
            uint32_t dst;
            uint64_t deadline;
            uint32_t seq = (uint32_t)regs->rsi;
            int rc = 0;
            int i;

            dst = ixn_ipv4_parse_str((const char *)(uintptr_t)regs->rdi);
            ndev = ixn_netdev_get_default();
            /*
             * 127/8 не должен уходить в физическую сеть: если loopback
             * поднято, пингуем через него.
             */
            if (ndev && (dst & 0xFF000000u) == 0x7F000000u) {
                ixn_netdev_t *lo = ixn_netdev_get_loopback();
                if (lo) ndev = lo;
            }
            if (dst == 0u || !out || !ndev) {
                regs->rax = (uint64_t)(int64_t)-1;
                break;
            }

            for (i = 0; i < 6; i++) out[i] = 0;

            /*
             * Греем ARP-кэш: ждём MAC до 200 мс, чтобы первый
             * echo-request после старта не улетал в никуда и не
             * приходилось повторять его восемь раз.  Для loopback и
             * собственного адреса это не нужно.
             */
            if (ndev->type != IXN_NETDEV_TYPE_LOOPBACK)
                (void)ixn_arp_ensure(ndev, dst, 200u);

            deadline = ixn_ticks_ms() + (uint64_t)(uint32_t)regs->rdx;
            ixn_ping_flush();

            if (ixn_icmp_send_echo(ndev, dst, (uint16_t)0x5049, (uint16_t)seq,
                                   (const uint8_t *)"ArtyomX64 ping", 14u) == 0) {
                for (;;) {
                    if (ixn_ping_get_reply(&rep) == 0) {
                        if (rep.seq == seq) {
                            out[0] = 1;
                            out[1] = (int64_t)rep.seq;
                            out[2] = (int64_t)rep.src_ip;
                            out[3] = (int64_t)rep.ttl;
                            out[4] = (int64_t)rep.size;
                            out[5] = (int64_t)((rep.ts_ms - (deadline - (uint64_t)(uint32_t)regs->rdx)) * 1000ull);
                            rc = 1;
                            break;
                        }
                    } else if (ixn_ticks_ms() >= deadline) {
                        break;
                    }
                    ixn_netdev_poll_all(64u);
                }
            }

            regs->rax = (uint64_t)(int64_t)rc;
            break;
        }

        /*
         * rename
         */
        case 82:

            regs->rax =
                rename(
                    (const char *)regs->rdi,
                    (const char *)regs->rsi
                );

            break;

        /*
         * mkdir
         *
         * Раньше это была заглушка "всегда успех" (Doom вызывает
         * M_MakeDirectory перед сейвом).  Теперь:
         *   diskN/... -- создаём каталог на носителе (FAT12/16/32);
         *   прочие пути -- настоящий узел-каталог в RAM-ФС initrd.
         */
        case 83: {

            const char *path = (const char *)regs->rdi;
            int rc = -1;

            if (path && path[0]) {
                const char *p = path;

                while (*p == '/')
                    p++;

                if (p[0] == 'd' && p[1] == 'i' && p[2] == 's' && p[3] == 'k' &&
                    p[4] >= '0' && p[4] <= '9') {
                    int idx = p[4] - '0';
                    const char *rel = p + 5;

                    while (*rel == '/')
                        rel++;

                    if (rel[0] == 0) {
                        rc = 0;                     /* корень тома уже есть */
                    } else if (automount_mkdir_at(idx, rel)) {
                        rc = 0;
                    }
                } else {
                    rc = initrd_mkdir(p);
                }
            }

            regs->rax = (uint64_t)(int64_t)rc;
            break;
        }

        /*
         * compat_exec
         */
        case 170:

            regs->rax = -1;

            break;


        /*
         * Explicit -1 syscall
         */
        case 0xFFFFFFFFFFFFFFFFULL:

            debug_puts(
                "SYSCALL_RAX_WAS_MINUS_ONE\n"
            );

            regs->rax = -1;

            break;


        /*
         * Unknown syscall
         */
        default:

            debug_puts(
                "UNKNOWN_SYSCALL: "
            );

            debug_putnum(
                regs->rax
            );

            debug_putc('\n');

            regs->rax = -1;

            break;
    }
}

/*
 * Совместимость: прежнее имя диспетчера.
 */
void syscall_handler(
    registers_t *regs
)
{
    syscall_native_handler(regs);
}

/*
 * ============================================================
 * Syscall initialization
 * ============================================================
 */

void initialise_syscalls(void)
{
    /* Порты/SHM/tty/ввод -- примитивы IPC. */
    ipc_init();

    /* BSD-слой сокетов поверх ixn (fs/sock.c). */
    ax_sock_init();

    register_interrupt_handler(
        0x80,
        &syscall_handler
    );
}