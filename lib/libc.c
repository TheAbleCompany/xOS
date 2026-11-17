#include "../lib/common.h"
#include "../mm/kheap.h"
#include <stdarg.h>

int write(int fd, void *buf, unsigned long count);


int memcmp(const void *ptr1, const void *ptr2, size_t num)
{
    const unsigned char *a = (const unsigned char *)ptr1;
    const unsigned char *b = (const unsigned char *)ptr2;

    for (size_t i = 0; i < num; i++)
    {
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    }
    return 0;
}

void *memcpy(void *dest, const void *src, size_t len)
{
    const u8int *sp = (const u8int *)src;
    u8int *dp = (u8int *)dest;
    for (; len != 0; len--) *dp++ = *sp++;
    return dest;
}

void *memset(void *dest, int val, size_t len)
{
    u8int *temp = (u8int *)dest;
    for ( ; len != 0; len--) *temp++ = (u8int)val;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) {
        return dest; // ничего не нужно делать
    }

    // Если область назначения перекрывает область источника, сначала копируем в буфер
    if (d < s || d >= s + n) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        // Если область назначения перекрывает область источника, копируем в обратном порядке
        d += n;
        s += n;
        while (n--) {
            *(--d) = *(--s);
        }
    }

    return dest;
}

// ---------------------------------------------------------------------------
// Шимы _FORTIFY_SOURCE.
//
// Debian/Ubuntu GCC (WSL в том числе) при -O1 и выше АВТОМАТИЧЕСКИ включает
// _FORTIFY_SOURCE, даже для freestanding-кода, если файл подключил хостовый
// <string.h>. Тогда memcpy/memset/strcpy/... редиректятся в
// __memcpy_chk/__memset_chk/__strcpy_chk/..., которых в ядре нет, и линкер
// падает: "undefined reference to `__memcpy_chk'".
//
// Ниже -- реализации с сигнатурами glibc ABI (x86_64). Они самодостаточны
// (не вызывают memcpy/memset), чтобы на любом тулчейне не возникло
// бесконечной рекурсии, если сама libc.c окажется собрана с фортификацией.
// ---------------------------------------------------------------------------

void *__memcpy_chk(void *dest, const void *src, size_t len, size_t destlen)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    (void)destlen;
    for (; len != 0; len--) *d++ = *s++;
    return dest;
}

void *__memmove_chk(void *dest, const void *src, size_t len, size_t destlen)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    (void)destlen;
    if (d == s || len == 0) return dest;
    if (d < s || d >= s + len) {
        while (len--) *d++ = *s++;
    } else {
        d += len;
        s += len;
        while (len--) *(--d) = *(--s);
    }
    return dest;
}

void *__memset_chk(void *dest, int ch, size_t len, size_t destlen)
{
    unsigned char *d = (unsigned char *)dest;
    (void)destlen;
    for (; len != 0; len--) *d++ = (unsigned char)ch;
    return dest;
}

char *__strcpy_chk(char *dest, const char *src, size_t destlen)
{
    char *d = dest;
    (void)destlen;
    while ((*d++ = *src++) != 0)
        ;
    return dest;
}

char *__strncpy_chk(char *dest, const char *src, size_t n, size_t destlen)
{
    size_t i;
    (void)destlen;
    for (i = 0; i < n && src[i] != '\0'; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

char *__strcat_chk(char *dest, const char *src, size_t destlen)
{
    char *d = dest;
    (void)destlen;
    while (*d) d++;
    while ((*d++ = *src++) != 0)
        ;
    return dest;
}

char *__strncat_chk(char *dest, const char *src, size_t n, size_t destlen)
{
    char *d = dest;
    size_t i;
    (void)destlen;
    while (*d) d++;
    for (i = 0; i < n && src[i] != '\0'; i++) *d++ = src[i];
    *d = '\0';
    return dest;
}

// Compare two strings. Should return -1 if
// str1 < str2, 0 if they are equal or 1 otherwise.
int strcmp(const char *str1, const char *str2)
{
    const unsigned char *a = (const unsigned char *)str1;
    const unsigned char *b = (const unsigned char *)str2;

    if (!a || !b)
        return a ? 1 : (b ? -1 : 0);

    while (*a && *a == *b) {
        a++;
        b++;
    }

    return (int)(*a) - (int)(*b);
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n--) {
        if (*s1 != *s2) {
            // Вычисляем разность значений символов.
            // Приводим к unsigned char, чтобы избежать проблем с отрицательными значениями
            return (unsigned char)*s1 - (unsigned char)*s2;
        }
        // Переходим к следующим символам
        s1++;
        s2++;
    }
    // Если все сравниваемые символы совпали (либо одна из строк закончилась раньше)
    return 0;
}
// Copy the NULL-terminated string src into dest, and
// return dest.
//
// NOTE: the old implementation exited BEFORE copying the
// terminating zero, leaving dest unterminated.  That broke
// runtime-created initrd files: their names kept heap garbage
// after the last character, so finddir/strcmp never matched
// and open() could not reopen a saved file.
char *strcpy(char *dest, const char *src)
{
    char *d = dest;

    while ((*d++ = *src++) != 0)
        ;

    return dest;
}

char* strncpy(char *dest, const char *src, size_t n)
{
    size_t i;

   for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for ( ; i < n; i++)
        dest[i] = '\0';

   return dest;
}

// Concatenate the NULL-terminated string src onto
// the end of dest, and return dest.
char *strcat(char *dest, const char *src)
{
    char *d = dest;

    if (!dest || !src)
        return dest;

    while (*d)
        d++;

    while ((*d++ = *src++) != 0)
        ;

    return dest;
}

char *strncat(char *dest, const char *src, size_t n)
{
    char *d = dest;
    size_t i;

    if (!dest || !src)
        return dest;

    while (*d)
        d++;

    for (i = 0; i < n && src[i]; i++)
        *d++ = src[i];

    *d = 0;
    return dest;
}

char *strchr(const char *s, int c)
{
    if (!s)
        return 0;

    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return 0;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = 0;

    if (!s)
        return 0;

    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *haystack, const char *needle)
{
    size_t i, j;

    if (!haystack || !needle)
        return 0;

    if (!needle[0])
        return (char *)haystack;

    for (i = 0; haystack[i]; i++) {
        for (j = 0; needle[j]; j++) {
            if (haystack[i + j] != needle[j])
                break;
        }
        if (!needle[j])
            return (char *)(haystack + i);
    }

    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;

    if (!s)
        return 0;

    while (n--) {
        if (*p == (unsigned char)c)
            return (void *)p;
        p++;
    }

    return 0;
}

size_t strlen(const char *src)
{
    size_t i = 0;

    if (!src)
        return 0;

    while (*src++)
        i++;

    return i;
}

unsigned int is_delim(char c, char *delim)
{
    while(*delim != '\0')
    {
        if(c == *delim)
            return 1;
        delim++;
    }
    return 0;
}
char *strtok(char *srcString, char *delim)
{
    static char *backup_string; // start of the next search
    if(!srcString)
    {
        srcString = backup_string;
    }
    if(!srcString)
    {
        // user is bad user
        return 0;
    }
    // handle beginning of the string containing delims
    while(1)
    {
        if(is_delim(*srcString, delim))
        {
            srcString++;
            continue;
        }
        if(*srcString == '\0')
        {
            // we've reached the end of the string
            return 0; 
        }
        break;
    }
    char *ret = srcString;
    while(1)
    {
        if(*srcString == '\0')
        {
            /*end of the input string and
            next exec will return NULL*/
            backup_string = srcString;
            return ret;
        }
        if(is_delim(*srcString, delim))
        {
            *srcString = '\0';
            backup_string = srcString + 1;
            return ret;
        }
        srcString++;
    }
}

// Простые функции ввода-вывода
void putchar(char c) {
    write(1, &c, 1);
}

void puts(const char *s) {
    write(1, s, strlen(s));
    putchar('\n');
}

int atoi(char *p) {
    int k = 0;
    while (*p) {
        k = (k << 3) + (k << 1) + (*p) - '0';
        p++;
     }
     return k;
}




// Вспомогательные функции
static void print_char(char c) {
    write(1, &c, 1);
}

static void print_string(const char *s) {
    if (!s) {
        write(1, "(null)", 6);
        return;
    }
    while (*s) {
        print_char(*s++);
    }
}

// Печать целого числа (знакового)
static void print_int(int num, int base) {
    char buf[32];
    int i = 0;
    int negative = 0;
    
    if (num == 0) {
        print_char('0');
        return;
    }
    
    if (num < 0 && base == 10) {
        negative = 1;
        num = -num;
    }
    
    while (num > 0) {
        int digit = num % base;
        buf[i++] = (digit < 10) ? '0' + digit : 'a' + digit - 10;
        num /= base;
    }
    
    if (negative) {
        print_char('-');
    }
    
    while (i > 0) {
        print_char(buf[--i]);
    }
}

// Печать беззнакового целого
static void print_uint(unsigned int num, int base) {
    char buf[32];
    int i = 0;
    
    if (num == 0) {
        print_char('0');
        return;
    }
    
    while (num > 0) {
        int digit = num % base;
        buf[i++] = (digit < 10) ? '0' + digit : 'a' + digit - 10;
        num /= base;
    }
    
    while (i > 0) {
        print_char(buf[--i]);
    }
}

// Печать 64-битного беззнакового (для указателей)
static void print_uint64(uint64_t num, int base) {
    char buf[32];
    int i = 0;
    
    if (num == 0) {
        print_char('0');
        return;
    }
    
    while (num > 0) {
        int digit = num % base;
        buf[i++] = (digit < 10) ? '0' + digit : 'a' + digit - 10;
        num /= base;
    }
    
    while (i > 0) {
        print_char(buf[--i]);
    }
}

// Главная функция форматирования
static void vprintf(const char *fmt, va_list args) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            print_char(*p);
            continue;
        }
        
        p++; // пропускаем '%'
        
        switch (*p) {
            case '%':
                print_char('%');
                break;
                
            case 'd':  // десятичное знаковое
            case 'i':
                print_int(va_arg(args, int), 10);
                break;
                
            case 'u':  // десятичное беззнаковое
                print_uint(va_arg(args, unsigned int), 10);
                break;
                
            case 'x':  // шестнадцатеричное (строчные)
                print_uint(va_arg(args, unsigned int), 16);
                break;
                
            case 'X':  // шестнадцатеричное (заглавные)
                print_uint(va_arg(args, unsigned int), 16); // можно допилить регистр
                break;
                
            case 'o':  // восьмеричное
                print_uint(va_arg(args, unsigned int), 8);
                break;
                
            case 's':  // строка
                print_string(va_arg(args, const char *));
                break;
                
            case 'c':  // символ
                print_char((char)va_arg(args, int));
                break;
                
            case 'p':  // указатель
                void *ptr = va_arg(args, void *);
                if (ptr == NULL) {
                    print_string("(nil)");
                } else {
                    print_string("0x");
                    print_uint64((uint64_t)ptr, 16);
                }
                break;
                
            case 'l':  // long (для 32-бит = 32 бита, для 64-бит = 64 бита)
                p++; // смотрим следующий символ
                if (*p == 'd' || *p == 'i') {
                    print_int(va_arg(args, long), 10);
                } else if (*p == 'u') {
                    print_uint(va_arg(args, unsigned long), 10);
                } else if (*p == 'x') {
                    print_uint(va_arg(args, unsigned long), 16);
                } else if (*p == 'l') { // ll
                    p++;
                    if (*p == 'd' || *p == 'i') {
                        print_int(va_arg(args, long long), 10);
                    } else if (*p == 'u') {
                        print_uint(va_arg(args, unsigned long long), 10);
                    } else if (*p == 'x') {
                        print_uint(va_arg(args, unsigned long long), 16);
                    }
                }
                break;
                
            case 'z':  // size_t
                p++;
                if (*p == 'u') {
                    print_uint(va_arg(args, size_t), 10);
                } else if (*p == 'x') {
                    print_uint(va_arg(args, size_t), 16);
                }
                break;
                
            default:
                // неизвестный спецификатор — печатаем как есть
                print_char('%');
                print_char(*p);
                break;
        }
    }
}

// Публичный API
int printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    return 0; // можно вернуть количество напечатанных символов
}


typedef struct {
    char  *dst;
    size_t cap;
    size_t pos;
    int    overflow;
} ax_fmt_out_t;

static void fmt_emit(ax_fmt_out_t *o, char c) {
    if (o->pos + 1 >= o->cap) {
        o->overflow = 1;
        return;
    }
    o->dst[o->pos++] = c;
}

static void fmt_pad(ax_fmt_out_t *o, char c, int count) {
    while (count-- > 0) fmt_emit(o, c);
}

static int vsnprintf_impl(ax_fmt_out_t *o, const char *fmt, va_list args) {
    const char *p;

    for (p = fmt; p && *p; p++) {
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
        int width = 0, prec = -1, lng = 0, shrt = 0;
        char spec;

        if (*p != '%') {
            fmt_emit(o, *p);
            continue;
        }

        p++;

        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '#') alt = 1;
            else if (*p == '0') zero = 1;
            else break;
        }

        if (*p == '*') {
            width = va_arg(args, int);
            if (width < 0) { left = 1; width = -width; }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        }

        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') { prec = va_arg(args, int); p++; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }

        for (;; p++) {
            if (*p == 'l') lng++;
            else if (*p == 'h') shrt++;
            else if (*p == 'z' || *p == 't' || *p == 'j') lng++;
            else break;
        }

        spec = *p;
        if (!spec) break;

        switch (spec) {
        case '%':
            fmt_emit(o, '%');
            break;

        case 'c': {
            char c = (char)va_arg(args, int);
            if (!left) fmt_pad(o, ' ', width - 1);
            fmt_emit(o, c);
            if (left) fmt_pad(o, ' ', width - 1);
            break;
        }

        case 's': {
            const char *s = va_arg(args, const char *);
            int len = 0, pad;
            if (!s) s = "(null)";
            while (s[len] && (prec < 0 || len < prec)) len++;
            pad = width - len;
            if (!left) fmt_pad(o, ' ', pad);
            while (len-- > 0) fmt_emit(o, *s++);
            if (left) fmt_pad(o, ' ', pad);
            break;
        }

        case 'd':
        case 'i': {
            int64_t v;
            uint64_t mag;
            int neg, digits = 0, pad;
            char sign = 0;
            char tmp[32];

            if (lng >= 2) v = va_arg(args, long long);
            else if (lng == 1) v = va_arg(args, long);
            else if (shrt) v = (short)va_arg(args, int);
            else v = va_arg(args, int);

            neg = (v < 0);
            mag = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;

            if (mag == 0u) tmp[digits++] = '0';
            else while (mag) { tmp[digits++] = (char)('0' + (mag % 10u)); mag /= 10u; }

            if (neg) sign = '-';
            else if (plus) sign = '+';
            else if (space) sign = ' ';

            pad = width - digits - (sign ? 1 : 0);

            if (!left) {
                if (zero) {
                    if (sign) { fmt_emit(o, sign); sign = 0; }
                    fmt_pad(o, '0', pad);
                } else {
                    fmt_pad(o, ' ', pad);
                }
            }
            if (sign) fmt_emit(o, sign);
            while (digits > 0) fmt_emit(o, tmp[--digits]);
            if (left) fmt_pad(o, ' ', pad);
            break;
        }

        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'p': {
            uint64_t v;
            unsigned base = 10u;
            int upper = 0, digits = 0, pad;
            char tmp[32];
            int prefix = 0;

            if (spec == 'p') {
                v = (uint64_t)(uintptr_t)va_arg(args, void *);
                base = 16u;
                if (v == 0u) {
                    const char *nil = "(nil)";
                    int len = 5, p2 = width - len;
                    if (!left) fmt_pad(o, ' ', p2);
                    while (*nil) fmt_emit(o, *nil++);
                    if (left) fmt_pad(o, ' ', p2);
                    break;
                }
                prefix = 2;
            } else if (spec == 'x' || spec == 'X') {
                base = 16u;
                upper = (spec == 'X');
            } else if (spec == 'o') {
                base = 8u;
                if (alt) prefix = 1;
            }

            if (spec != 'p') {
                if (lng >= 2) v = va_arg(args, unsigned long long);
                else if (lng == 1) v = va_arg(args, unsigned long);
                else if (shrt) v = (unsigned short)va_arg(args, unsigned int);
                else v = va_arg(args, unsigned int);
                if (alt && base == 16u && v != 0u) prefix = 2;
            }

            {
                static const char lo[] = "0123456789abcdef";
                static const char up[] = "0123456789ABCDEF";
                const char *d = upper ? up : lo;
                uint64_t t = v;
                if (t == 0u) tmp[digits++] = '0';
                else while (t) { tmp[digits++] = d[t % base]; t /= base; }
            }

            while (prec > digits) tmp[digits++] = '0';

            pad = width - digits - prefix;

            if (!left && !zero) fmt_pad(o, ' ', pad);

            if (prefix == 2) {
                fmt_emit(o, '0');
                fmt_emit(o, upper ? 'X' : 'x');
            } else if (prefix == 1) {
                fmt_emit(o, '0');
            }

            if (!left && zero) fmt_pad(o, '0', pad);

            while (digits > 0) fmt_emit(o, tmp[--digits]);
            if (left) fmt_pad(o, ' ', pad);
            break;
        }

        default:
            fmt_emit(o, '%');
            fmt_emit(o, spec);
            break;
        }
    }

    return (int)o->pos;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list args) {
    ax_fmt_out_t o;
    int n;

    if (!buf || cap == 0u || !fmt)
        return 0;

    o.dst = buf;
    o.cap = cap;
    o.pos = 0;
    o.overflow = 0;

    n = vsnprintf_impl(&o, fmt, args);
    buf[o.pos] = 0;
    return o.overflow ? (int)(cap - 1u) : n;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list args;
    int n;

    va_start(args, fmt);
    n = vsnprintf(buf, cap, fmt, args);
    va_end(args);
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list args;
    int n;

    va_start(args, fmt);
    n = vsnprintf(buf, 4096u, fmt, args);
    va_end(args);
    return n;
}
