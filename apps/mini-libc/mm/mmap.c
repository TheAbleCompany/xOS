// SPDX-License-Identifier: BSD-3-Clause
//
// mm/mmap.c — mmap для mini-libc на ArtyomX64.
//
// Ядро ArtyomX64 пока НЕ реализует mmap/munmap (диспетчер syscall
// возвращает -1 для неизвестных номеров), а run.c (порт llama2.c)
// грузит чекпоинт модели файловым mmap'ом (PROT_READ|MAP_PRIVATE).
//
// Стратегия:
//   1) пробуем настоящий syscall mmap (9) — заработает автоматически,
//      когда ядро ArtyomX64 (или хост-шим) реализует mmap; сейчас
//      ядро возвращает -1 для неизвестных номеров -> шаг 2;
//   2) при отказе эмулируем в libc:
//        - anonymous (MAP_ANONYMOUS / fd < 0) -> calloc из кучи;
//        - file-backed -> malloc + чтение файла read()'ом
//          (позиция fd сохраняется/восстанавливается, как требует
//          POSIX: mmap не двигает файловый offset);
//      все эмулированные отображения регистрируются, munmap()
//      на зарегистрированном адресе делает free().
//
// Ограничение эмуляции: объём ограничен кучей приложения
// (HEAP_SIZE, по умолчанию 16 МиБ — хватит для stories260K,
// см. mm/malloc.c). PROT_WRITE/MAP_SHARED файловые отображения
// эмулируются как private-копия (запись не попадает в файл).

#include <sys/mman.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <internal/syscall.h>

/* Реестр эмулированных отображений (без кучи: static-массив). */
#define EMAP_MAX        128

typedef struct {
        void   *addr;
        size_t  len;
} emap_entry_t;

/* Слоты с addr == NULL свободны (static-массив обнулён). */
static emap_entry_t emap_tab[EMAP_MAX];

static int emap_add(void *addr, size_t len)
{
        int i;

        for (i = 0; i < EMAP_MAX; i++) {
                if (emap_tab[i].addr == NULL) {
                        emap_tab[i].addr = addr;
                        emap_tab[i].len = len;
                        return 0;
                }
        }
        return -1;              /* таблица переполнена */
}

static emap_entry_t *emap_find(void *addr)
{
        int i;

        for (i = 0; i < EMAP_MAX; i++) {
                if (emap_tab[i].addr == addr)
                        return &emap_tab[i];
        }
        return NULL;
}

static void emap_del(emap_entry_t *e)
{
        e->addr = NULL;
        e->len = 0;
}

/* Чтение [offset, offset+length) из fd в buf, не двигая файловый offset. */
static int emap_read_file(int fd, void *buf, size_t length, off_t offset)
{
        long saved;
        size_t done = 0;
        long n;

        saved = lseek(fd, 0, SEEK_CUR);
        if (saved < 0)
                saved = 0;

        if (lseek(fd, offset, SEEK_SET) < 0) {
                lseek(fd, saved, SEEK_SET);
                return -1;
        }

        while (done < length) {
                n = read(fd, (char *)buf + done, length - done);
                if (n < 0) {
                        lseek(fd, saved, SEEK_SET);
                        return -1;
                }
                if (n == 0)
                        break;  /* файл короче length: остаток = нули */
                done += (size_t)n;
        }

        if (done < length)
                memset((char *)buf + done, 0, length - done);

        lseek(fd, saved, SEEK_SET);
        return 0;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
        void *buf;
        long ret;

        (void)prot;

        /* 1) настоящий mmap (хост-шим / будущее ядро). */
        ret = syscall(__NR_mmap, (long)addr, (long)length,
                      (long)prot, (long)flags, (long)fd, (long)offset);
        if (ret >= 0)
                return (void *)ret;

        /* 2) эмуляция. */
        if (length == 0) {
                errno = EINVAL;
                return MAP_FAILED;
        }

        if ((flags & MAP_ANONYMOUS) || fd < 0) {
                buf = calloc(1, length);
        } else {
                buf = malloc(length);
                if (buf != NULL &&
                    emap_read_file(fd, buf, length, offset) != 0) {
                        free(buf);
                        buf = NULL;
                }
        }

        if (buf == NULL) {
                errno = ENOMEM;
                return MAP_FAILED;
        }

        if (emap_add(buf, length) != 0) {
                /* Реестр переполнен: отображение всё же вернём —
                 * munmap его просто не узнает (утечка на free). */
                return buf;
        }

        return buf;
}

void *mremap(void *old_address, size_t old_size, size_t new_size, int flags)
{
        (void)old_address;
        (void)old_size;
        (void)new_size;
        (void)flags;

        errno = ENOSYS;
        return MAP_FAILED;
}

int munmap(void *addr, size_t length)
{
        emap_entry_t *e;

        if (addr == NULL) {
                errno = EINVAL;
                return -1;
        }

        /* Наше эмулированное отображение -> free(). */
        e = emap_find(addr);
        if (e != NULL) {
                emap_del(e);
                free(addr);
                return 0;
        }

        /*
         * ВНИМАНИЕ: fallback на syscall __NR_munmap (=11) здесь
         * ОПАСЕН и поэтому убран: в нумерации ArtyomX64 номер 11
         * занят exec() (см. syscall_list.h и lib/syscalls.c ядра).
         * Пока ядро не получит собственные номера mmap/munmap,
         * неизвестные адресу отображения считаются невалидными.
         */
        (void)length;
        errno = EINVAL;
        return -1;
}
