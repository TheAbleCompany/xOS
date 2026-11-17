/*
 * stdlib/qsort.c — qsort для mini-libc (ArtyomX64).
 *
 * Heapsort: гарантированные O(n log n) и O(1) стековой глубины —
 * важно при стеке задачи 16 КиБ (глубокая рекурсия quicksort опасна).
 * Неустойчивая сортировка, как и разрешено стандартом.
 */

#include <stdlib.h>
#include <string.h>

typedef int (*cmp_t)(const void *, const void *);

static void swap_bytes(char *a, char *b, size_t width)
{
        /* Короткие элементы меняем через size_t, длинные — по байтам. */
        size_t k;

        if (a == b)
                return;

        if (!(width & (sizeof(unsigned long) - 1))) {
                unsigned long *pa = (unsigned long *)a;
                unsigned long *pb = (unsigned long *)b;

                for (k = 0; k < width; k += sizeof(unsigned long)) {
                        unsigned long t = pa[k / sizeof(unsigned long)];

                        pa[k / sizeof(unsigned long)] =
                            pb[k / sizeof(unsigned long)];
                        pb[k / sizeof(unsigned long)] = t;
                }
                return;
        }

        for (k = 0; k < width; k++) {
                char t = a[k];

                a[k] = b[k];
                b[k] = t;
        }
}

static void sift_down(char *base, size_t n, size_t i, size_t width, cmp_t cmp)
{
        while (1) {
                size_t l = 2 * i + 1;
                size_t r = 2 * i + 2;
                size_t m = i;
                char *pm, *pl, *pr;

                if (l < n &&
                    cmp(base + l * width, base + m * width) > 0)
                        m = l;

                if (r < n &&
                    cmp(base + r * width, base + m * width) > 0)
                        m = r;

                if (m == i)
                        return;

                pm = base + m * width;
                pl = base + i * width;

                (void)pl;
                (void)pr;
                swap_bytes(base + i * width, pm, width);
                i = m;
        }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
        char *p = (char *)base;
        size_t i;

        if (p == NULL || nmemb < 2 || size == 0 || compar == NULL)
                return;

        /* Построение max-heap. */
        for (i = nmemb / 2; i-- > 0; )
                sift_down(p, nmemb, i, size, compar);

        /* Извлечение максимума в конец. */
        for (i = nmemb - 1; i > 0; i--) {
                swap_bytes(p, p + i * size, size);
                sift_down(p, i, 0, size, compar);
        }
}
