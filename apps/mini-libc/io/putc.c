#include <stdio.h>

// putc обычно реализуется как макрос для скорости
// Но мы реализуем как функцию для ясности
int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

// putchar - запись символа в stdout
int putchar(int c) {
    return fputc(c, stdout);
}