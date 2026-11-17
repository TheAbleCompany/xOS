// SPDX-License-Identifier: BSD-3-Clause
//
// malloc/calloc/realloc/free для mini-libc на ArtyomX64.
//
// Ядро ArtyomX64 не реализует mmap/brk (диспетчер syscall умеет только
// exit/read/write/open/close/unlink/exec/ioctl/getdents/yield/...),
// поэтому куча выделяется статически в .bss приложения и управляется
// простым first-fit аллокатором со списком свободных блоков и
// слиянием соседних блоков при free().
//
// Выравнивание всех блоков: 16 байт (для double/long long/SSE).

#include <string.h>
#include <stdlib.h>
#include <errno.h>

/*
 * Куча в .bss. Для компилятора tcc (порт 2026) размер поднят до 16 МиБ:
 * tcc держит в куче буферы секций, таблицы символов и строки препроцессора.
 * Размер можно переопределить при сборке: -DHEAP_SIZE=(N*1024*1024).
 */
#ifndef HEAP_SIZE
#define HEAP_SIZE       (16 * 1024 * 1024)      /* 16 МиБ кучи в .bss */
#endif
#define ALIGNMENT       16
#define MIN_BLOCK       32              /* минимальный размер блока (с заголовком) */

/* Сигнатура заголовка: ловим запись за границы блока. */
#define BLOCK_MAGIC     0xA710C000u

typedef struct block_header {
        unsigned int magic;             /* BLOCK_MAGIC для живого блока */
        unsigned int used;              /* 0 = свободен, 1 = занят */
        size_t size;                    /* размер полезной области */
        struct block_header *next;      /* следующий блок в куче */
        struct block_header *prev;      /* предыдущий блок в куче */
} block_header_t;

static unsigned char heap[HEAP_SIZE] __attribute__((aligned(16)));

static block_header_t *heap_head = NULL;
static int heap_ready = 0;

static size_t align_up(size_t n)
{
        return (n + (ALIGNMENT - 1)) & ~((size_t)(ALIGNMENT - 1));
}

static void heap_init(void)
{
        block_header_t *b = (block_header_t *)heap;

        b->magic = BLOCK_MAGIC;
        b->used = 0;
        b->size = HEAP_SIZE - sizeof(block_header_t);
        b->next = NULL;
        b->prev = NULL;

        heap_head = b;
        heap_ready = 1;
}

static int block_ok(block_header_t *b)
{
        return b != NULL &&
               b->magic == BLOCK_MAGIC &&
               b->used <= 1;
}

void *malloc(size_t size)
{
        block_header_t *b;

        if (size == 0)
                size = 1;

        if (!heap_ready)
                heap_init();

        size = align_up(size);

        /* First-fit поиск свободного блока. */
        for (b = heap_head; b != NULL; b = b->next) {
                if (!block_ok(b))
                        return NULL;

                if (b->used)
                        continue;

                if (b->size < size)
                        continue;

                /* Хвостовой остаток достаточно велик, чтобы разделить блок? */
                if (b->size >= size + sizeof(block_header_t) + MIN_BLOCK) {
                        block_header_t *tail =
                                (block_header_t *)((unsigned char *)b +
                                        sizeof(block_header_t) + size);

                        tail->magic = BLOCK_MAGIC;
                        tail->used = 0;
                        tail->size = b->size - size - sizeof(block_header_t);
                        tail->next = b->next;
                        tail->prev = b;

                        if (tail->next)
                                tail->next->prev = tail;

                        b->size = size;
                        b->next = tail;
                }

                b->used = 1;
                return (unsigned char *)b + sizeof(block_header_t);
        }

        errno = ENOMEM;
        return NULL;
}

void free(void *ptr)
{
        block_header_t *b;

        if (ptr == NULL)
                return;

        b = (block_header_t *)((unsigned char *)ptr - sizeof(block_header_t));

        if (!block_ok(b))
                return;                 /* чужой/битый указатель — игнорируем */

        b->used = 0;

        /* Слияние со следующим свободным блоком. */
        if (b->next && block_ok(b->next) && b->next->used == 0) {
                block_header_t *n = b->next;

                b->size += sizeof(block_header_t) + n->size;
                b->next = n->next;

                if (b->next)
                        b->next->prev = b;
        }

        /* Слияние с предыдущим свободным блоком. */
        if (b->prev && block_ok(b->prev) && b->prev->used == 0) {
                block_header_t *p = b->prev;

                p->size += sizeof(block_header_t) + b->size;
                p->next = b->next;

                if (p->next)
                        p->next->prev = p;
        }
}

void *calloc(size_t nmemb, size_t size)
{
        void *ret;
        size_t total;

        if (nmemb != 0 && nmemb > (size_t)-1 / size) {
                errno = ENOMEM;
                return NULL;
        }

        total = nmemb * size;

        ret = malloc(total);
        if (ret == NULL)
                return NULL;

        memset(ret, 0, total);
        return ret;
}

void *realloc(void *ptr, size_t size)
{
        block_header_t *b;
        void *nptr;

        if (ptr == NULL)
                return malloc(size);

        if (size == 0) {
                free(ptr);
                return NULL;
        }

        b = (block_header_t *)((unsigned char *)ptr - sizeof(block_header_t));

        if (!block_ok(b)) {
                errno = EINVAL;
                return NULL;
        }

        /* Текущего блока достаточно? */
        if (b->size >= align_up(size))
                return ptr;

        nptr = malloc(size);
        if (nptr == NULL)
                return NULL;

        memcpy(nptr, ptr, b->size < size ? b->size : size);
        free(ptr);

        return nptr;
}

void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
        if (nmemb != 0 && nmemb > (size_t)-1 / size) {
                errno = ENOMEM;
                return NULL;
        }

        return realloc(ptr, nmemb * size);
}
