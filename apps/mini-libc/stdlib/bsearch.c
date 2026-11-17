// SPDX-License-Identifier: BSD-3-Clause
//
// stdlib/bsearch.c — бинарный поиск для mini-libc (ArtyomX64).
//
// Нужен run.c (порт llama2.c): Tokenizer::str_lookup() ищет токен
// в отсортированном словаре через bsearch().
//
// Семантика POSIX/C99: возвращает указатель на найденный элемент
// или NULL, если key не найден (при равных элементах — не
// определено, какой из них будет возвращён).

#include <stdlib.h>
#include <stddef.h>

void *bsearch(const void *key, const void *base,
              size_t nmemb, size_t size,
              int (*compar)(const void *, const void *))
{
        const unsigned char *p = (const unsigned char *)base;
        size_t lo = 0;
        size_t hi = nmemb;

        if (key == NULL || base == NULL || size == 0)
                return NULL;

        while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                const void *elem = p + mid * size;
                int c = compar(key, elem);

                if (c == 0)
                        return (void *)elem;
                if (c < 0)
                        hi = mid;
                else
                        lo = mid + 1;
        }

        return NULL;
}
