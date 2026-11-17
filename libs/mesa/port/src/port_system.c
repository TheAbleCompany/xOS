#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

void serial_puts_ax(const char *s);
void serial_putc_ax(char c);
uint64_t timer_ticks_ms(void);

int ax_mesa_current_pid(void);
int ax_mesa_fd_stat(int fd, unsigned long *dev, unsigned long *ino,
                    unsigned int *mode, long *size);
void task_exit(int code);

int ax_mesa_iflush(void *f);
int ax_mesa_lseek(int fd, int off, int whence);

static size_t a_strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void a_puts(const char *s)
{
    serial_puts_ax(s);
}

static void a_putu(uint64_t v)
{
    char tmp[24];
    int i = 0;

    if (!v) {
        serial_putc_ax('0');
        return;
    }

    while (v && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }

    while (i--)
        serial_putc_ax(tmp[i]);
}

void mesa_port_assert_fail(const char *expr, const char *file, int line)
{
    a_puts("[MESA] assert failed: ");
    a_puts(expr ? expr : "?");
    a_puts(" at ");
    a_puts(file ? file : "?");
    a_puts(":");
    a_putu((uint64_t)(line < 0 ? -line : line));
    a_puts("\n");
    for (;;)
        __asm__ volatile("cli; hlt");
}

static int mesa_errno = 0;

int *mesa_port_errno_location(void)
{
    return &mesa_errno;
}

int ax_mesa_lseek(int fd, int off, int whence)
{
    return lseek(fd, off, whence);
}

int os_get_total_physical_memory(uint64_t *size)
{
    if (size)
        *size = 0x40000000ull;
    return 1;
}

int os_get_available_system_memory(uint64_t *size)
{
    if (size)
        *size = 0x20000000ull;
    return 1;
}

int ax_mesa_iflush(void *f)
{
    (void)f;
    return 0;
}

/* Executable-memory mapping is not provided by the kernel port yet.
 * Callers (e.g. gallium rtasm) handle failure gracefully. */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
    return MAP_FAILED;
}

int munmap(void *addr, size_t len)
{
    (void)addr; (void)len;
    return -1;
}

int mprotect(void *addr, size_t len, int prot)
{
    (void)addr; (void)len; (void)prot;
    return -1;
}

/* POSIX shims referenced by src/util but not implemented by the kernel
 * port. Callers treat failure (-1/NULL) as non-fatal. */
#include <sys/types.h>

void openlog(const char *ident, int option, int facility)
{
    (void)ident; (void)option; (void)facility;
}

void __syslog_chk(int priority, int flag, const char *format, ...)
{
    (void)priority; (void)flag; (void)format;
}

int setpriority(int which, int who, int prio)
{
    (void)which; (void)who; (void)prio;
    return -1;
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    (void)path; (void)buf; (void)bufsiz;
    return -1;
}

long syscall(long number, ...)
{
    (void)number;
    return -1;
}

char *os_read_file(const char *filename, size_t *size)
{
    (void)filename;
    if (size)
        *size = 0;
    return NULL;
}

void __stack_chk_fail(void)
{
    mesa_port_assert_fail("__stack_chk_fail called", __FILE__, __LINE__);
    for (;;)
        ;
}

void os_log_message(const char *message)
{
    if (!message)
        return;

    a_puts("[MESA] ");
    a_puts(message);

    if (a_strlen(message) == 0 || message[a_strlen(message) - 1] != '\n')
        a_puts("\n");
}

const char *os_get_option(const char *name)
{
    (void)name;
    return 0;
}

const char *os_get_option_cached(const char *name)
{
    (void)name;
    return 0;
}

int64_t os_time_get_nano(void)
{
    return (int64_t)timer_ticks_ms() * 1000000LL;
}

int64_t os_time_get_absolute_timeout(uint64_t timeout)
{
    if (timeout == 0xffffffffffffffffull)
        return (int64_t)timeout;

    return os_time_get_nano() + (int64_t)timeout;
}

int util_get_current_cpu(void)
{
    return 0;
}

void u_thread_setname(const char *name)
{
    (void)name;
}

int64_t util_thread_get_time_nano(unsigned long thread)
{
    (void)thread;
    return os_time_get_nano();
}

int util_set_thread_affinity(unsigned long thread, const uint32_t *mask,
                             uint32_t *old_mask, unsigned num_mask_bits)
{
    (void)thread;
    (void)num_mask_bits;

    if (old_mask) {
        for (unsigned i = 0; i < num_mask_bits; i++)
            old_mask[i] = (i == 0) ? 1u : 0u;
    }

    (void)mask;
    return 1;
}

const char *debug_symbol_name_cached(const void *addr)
{
    (void)addr;
    return "";
}

void debug_symbol_print(const void *addr)
{
    (void)addr;
}

const char *driQueryOptionstr(void *config, const char *name)
{
    (void)config;
    (void)name;
    return 0;
}

int driQueryOptioni(void *config, const char *name)
{
    (void)config;
    (void)name;
    return 0;
}

unsigned char driQueryOptionb(void *config, const char *name)
{
    (void)config;
    (void)name;
    return 0;
}

int os_same_file_description(int fd1, int fd2)
{
    return (fd1 == fd2) ? 0 : 1;
}

FILE *os_file_create_unique(const char *filename, int filemode)
{
    FILE *f = fopen(filename, "wb");

    (void)filemode;
    return f;
}

int os_mkdir(const char *pathname, int mode)
{
    (void)pathname;
    (void)mode;
    errno = ENOSYS;
    return -1;
}


void u_cnd_monotonic_init(void *cnd)
{
    (void)cnd;
}

void u_cnd_monotonic_destroy(void *cnd)
{
    (void)cnd;
}

void u_cnd_monotonic_wait(void *cnd, void *mtx)
{
    (void)cnd;
    (void)mtx;
}

void u_cnd_monotonic_timedwait(void *cnd, void *mtx, int64_t timeout)
{
    (void)cnd;
    (void)mtx;
    (void)timeout;
}

void u_cnd_monotonic_broadcast(void *cnd)
{
    (void)cnd;
}

struct file;

int getpid(void)
{
    return ax_mesa_current_pid();
}

unsigned int getuid(void)
{
    return 0;
}

unsigned int geteuid(void)
{
    return 0;
}

unsigned int getgid(void)
{
    return 0;
}

unsigned int getegid(void)
{
    return 0;
}

int isatty(int fd)
{
    (void)fd;
    return 1;
}

int access(const char *path, int mode)
{
    int fd;

    (void)mode;

    if (!path)
        return -1;

    fd = open(path, 0, 0);

    if (fd < 0)
        return -1;

    close(fd);

    return 0;
}

int fstat(int fd, struct stat *st)
{
    if (!st)
        return -1;

    memset(st, 0, sizeof(*st));

    if (ax_mesa_fd_stat(fd, &st->st_dev, &st->st_ino, &st->st_mode,
                        &st->st_size) < 0)
        return -1;

    st->st_nlink = 1;
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;

    return 0;
}

long mesa_port_sysconf(int name)
{
    (void)name;
    return 4096;
}

time_t time(time_t *t)
{
    time_t v = (time_t)(timer_ticks_ms() / 1000u);

    if (t)
        *t = v;

    return v;
}

void mesa_port_exit(int code)
{
    task_exit(code);
    for (;;)
        ;
}
