#ifndef AX_UNISTD_H
#define AX_UNISTD_H
#include "../lib/common.h"

#include <stddef.h>
#include <stdint.h>

#ifndef O_RDONLY
#define O_RDONLY 0x0000
#endif

#ifndef O_WRONLY
#define O_WRONLY 0x0001
#endif

#ifndef O_RDWR
#define O_RDWR   0x0002
#endif

#ifndef O_CREAT
#define O_CREAT  0x0040
#endif

#ifndef O_TRUNC
#define O_TRUNC  0x0200
#endif

#define TCFLSH 21515

#ifdef HOST_HARNESS
/*
 * Host harness: сборка сервера на обычном Linux (--define HOST_HARNESS)
 * для визуальной отладки GUI без ядра.  На хосте int $0x80 -- это
 * 32-bit syscall ABI (и часто запрещён seccomp), поэтому те же
 * имена отображаются на glibc syscall() (64-bit ABI).
 * В реальной сборке ОС ветка не используется.
 */
extern long syscall(long num, ...);

static void yield(void) { }

int read(int fd, void *buf, size_t count) {
    return (int)syscall(0 /* SYS_read */, (long)fd, (long)buf, (long)count);
}

int write(int fd, const void *buf, size_t count) {
    return (int)syscall(1 /* SYS_write */, (long)fd, (long)buf, (long)count);
}

int open(void *path, int flags, int mode) {
    return (int)syscall(257 /* SYS_openat */, (long)-100 /* AT_FDCWD */,
                        (long)path, (long)flags, (long)mode);
}

int close(int fd) {
    return (int)syscall(3 /* SYS_close */, (long)fd);
}

int unlink(void *path) {
    return -1;
}

int exec(const char *path, int argc, char *argv[]) {
    (void)path; (void)argc; (void)argv;
    return -1;
}

int ioctl(int fd, unsigned long com, char *data) {
    (void)fd; (void)com; (void)data;
    return -1;
}

int getdents(int fd, void *buf, u32int size) {
    (void)fd; (void)buf; (void)size;
    return -1;
}

int ipc_call(int endpoint, void *msg) {
    (void)endpoint; (void)msg;
    return -1;
}

void exit(int code) {
    syscall(60 /* SYS_exit */, (long)code);
    for (;;);
}

int compat_exec(const void *path) {
    (void)path;
    return -1;
}

#else /* HOST_HARNESS */

// unistd.h - используйте обычный read без yield

// unistd.h - замените read на блокирующую версию
static void yield(void){
     int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(151)
        : "memory"
    );
}

int read(int fd, void *buf, size_t count) {
    char *p = (char*)buf;
    size_t total = 0;
    int eof_seen = 0;
    
    while (total < count) {
        int ret;
        __asm__ volatile(
            "int $0x80"
            : "=a"(ret)
            : "a"(3), "D"((long)fd), "S"((long)(p + total)), "d"((long)(count - total))
            : "memory"
        );
        
        if (ret > 0) {
            total += ret;
            eof_seen = 0;
            /* Для консоли (fd 0) завершаем чтение по новой строке */
            if (fd == 0 && p[total - 1] == '\n') break;
        } else if (ret == 0) {
            /*
             * ret == 0: для файлов это EOF, для PTY — «пока нет данных».
             * PTY всегда на fd 0 (stdin). Для всех остальных fd
             * считаем 0 за EOF и выходим.
             */
            if (fd != 0) break;
            /* Для PTY: два нуля подряд = EOF, один ноль = просто ждём */
            eof_seen++;
            if (eof_seen > 64) break;  /* защита от зависания */
            yield();
        } else {
            return -1;
        }
    }
    
    /* Добавляем null-терминатор */
    if (total < count) p[total] = 0;
    return total;
}

// Если хотите блокирующий read, оставьте старую версию
// Но для консоли лучше не блокировать

int write(int fd, const void *buf, size_t count) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(4), "D"((long)fd), "S"((long)buf), "d"((long)count)
        : "memory"
    );
    return ret;
}
int open(void *path, int flags, int mode){
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(5), "D"((long)path), "S"((long)flags), "d"((long)mode)
        : "memory"
    );
    return ret;
}
int close(int fd){
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(6), "D"((long)fd)
        : "memory"
    );
    return ret;
}
int unlink(void *path){
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(10), "D"((long)path)
        : "memory"
    );
    return ret;
}
int exec(const char *path, int argc, char *argv[])
{
    int ret;

    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(11),
          "D"((long)path),
          "S"((long)argc),
          "d"((long)argv)
        : "memory"
    );

    return ret;
}

int ioctl(int fd, unsigned long com, char* data){
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(54), "D"((long)fd), "S"((long)com), "d"((long)data)
        : "memory"
    );
    return ret;
}

int getdents(int fd, void *buf, u32int size){
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(141), "D"((long)fd), "S"((long)buf), "d"((long)size)
        : "memory"
    );
    return ret;
}

int fptype(const void *path){
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(197), "D"((long)path)
        : "memory"
    );
    return ret;
}
int ipc_call(int endpoint, void* msg){
     int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(150), "D"((long)endpoint), "S"((long)msg)
        : "memory"
    );
    return ret;
}
void exit(int code){
     int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(1), "D"((long)code)
        : "memory"
    );
}

int compat_exec(const void *path){
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(170), "D"((long)path)
        : "memory"
    );
    return ret;
}

#endif /* HOST_HARNESS */

#endif /* AX_UNISTD_H */
