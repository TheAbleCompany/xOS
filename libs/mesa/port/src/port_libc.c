#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int ax_mesa_iflush(FILE *f);
int ax_mesa_lseek(int fd, int off, int whence);

int sprintf(char *buf, const char *fmt, ...);

int open(const char *path, int flags, int mode);
int close(int fd);
int read(int fd, char *buf, int nbytes);
int write(int fd, char *buf, int nbytes);


size_t strnlen(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i])
        i++;
    return i;
}

int strcasecmp(const char *a, const char *b)
{
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;

        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);

        if (ca != cb)
            return (int)ca - (int)cb;

        if (!ca)
            return 0;
    }
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;

        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);

        if (ca != cb)
            return (int)ca - (int)cb;

        if (!ca)
            return 0;

        n--;
    }
    return 0;
}

int strcoll(const char *a, const char *b)
{
    return strcmp(a, b);
}

size_t strxfrm(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);

    if (n) {
        size_t copy = len < n - 1 ? len : n - 1;
        memcpy(d, s, copy);
        d[copy] = 0;
    }
    return len;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);

    if (p)
        memcpy(p, s, n);

    return p;
}

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *p = (char *)malloc(len + 1);

    if (p) {
        memcpy(p, s, len);
        p[len] = 0;
    }

    return p;
}

char *strpbrk(const char *s, const char *accept)
{
    while (*s) {
        const char *a = accept;

        while (*a) {
            if (*a == *s)
                return (char *)s;
            a++;
        }
        s++;
    }

    return 0;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;

    while (s[n] && strchr(accept, s[n]))
        n++;

    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;

    while (s[n] && !strchr(reject, s[n]))
        n++;

    return n;
}

char *strtok_r(char *s, const char *delim, char **save)
{
    char *start;

    if (!s)
        s = *save;

    if (!s) {
        *save = 0;
        return 0;
    }

    while (*s && strchr(delim, *s))
        s++;

    if (!*s) {
        *save = s;
        return 0;
    }

    start = s;

    while (*s && !strchr(delim, *s))
        s++;

    if (*s) {
        *s = 0;
        *save = s + 1;
    } else {
        *save = s;
    }

    return start;
}

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int isspace(int c)
{
    return is_space(c) ? 1 : 0;
}

int isdigit(int c)
{
    return is_digit(c) ? 1 : 0;
}

int isalnum(int c)
{
    return (is_digit(c) || is_alpha(c)) ? 1 : 0;
}

int isalpha(int c)
{
    return is_alpha(c) ? 1 : 0;
}

int isupper(int c)
{
    return (c >= 'A' && c <= 'Z') ? 1 : 0;
}

int islower(int c)
{
    return (c >= 'a' && c <= 'z') ? 1 : 0;
}

int isprint(int c)
{
    return (c >= 0x20 && c <= 0x7E) ? 1 : 0;
}

int isgraph(int c)
{
    return (c > 0x20 && c <= 0x7E) ? 1 : 0;
}

int ispunct(int c)
{
    return (isgraph(c) && !isalnum(c)) ? 1 : 0;
}

int iscntrl(int c)
{
    return (c >= 0 && c < 0x20) || c == 0x7F;
}

int isxdigit(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int isblank(int c)
{
    return c == ' ' || c == '\t';
}

int tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int toupper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static int digit_value(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return 99;
}

long long strtoll(const char *s, char **end, int base)
{
    const char *p = s;
    int neg = 0;
    unsigned long long acc = 0;
    int any = 0;
    int d;

    while (is_space((unsigned char)*p))
        p++;

    if (*p == '-' || *p == '+') {
        neg = (*p == '-');
        p++;
    }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    while ((d = digit_value((unsigned char)*p)) < base) {
        acc = acc * (unsigned long long)base + (unsigned long long)d;
        any = 1;
        p++;
    }

    if (end)
        *end = (char *)(any ? p : s);

    return neg ? -(long long)acc : (long long)acc;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    const char *p = s;
    int neg = 0;
    unsigned long long acc = 0;
    int any = 0;
    int d;

    while (is_space((unsigned char)*p))
        p++;

    if (*p == '-' || *p == '+') {
        neg = (*p == '-');
        p++;
    }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    while ((d = digit_value((unsigned char)*p)) < base) {
        acc = acc * (unsigned long long)base + (unsigned long long)d;
        any = 1;
        p++;
    }

    if (end)
        *end = (char *)(any ? p : s);

    return neg ? 0ULL - acc : acc;
}

long strtol(const char *s, char **end, int base)
{
    return (long)strtoll(s, end, base);
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtoull(s, end, base);
}

double strtod(const char *s, char **end)
{
    const char *p = s;
    int neg = 0;
    double value = 0.0;
    double fraction = 1.0;
    int any = 0;
    int exponent = 0;
    int exp_neg = 0;
    double scale = 1.0;

    while (is_space((unsigned char)*p))
        p++;

    if (*p == '-' || *p == '+') {
        neg = (*p == '-');
        p++;
    }

    while (is_digit((unsigned char)*p)) {
        value = value * 10.0 + (double)(*p - '0');
        any = 1;
        p++;
    }

    if (*p == '.') {
        p++;
        while (is_digit((unsigned char)*p)) {
            fraction *= 0.1;
            value += (double)(*p - '0') * fraction;
            any = 1;
            p++;
        }
    }

    if (any && (*p == 'e' || *p == 'E')) {
        const char *save = p;
        p++;
        if (*p == '-' || *p == '+') {
            exp_neg = (*p == '-');
            p++;
        }
        if (!is_digit((unsigned char)*p)) {
            p = save;
        } else {
            while (is_digit((unsigned char)*p)) {
                exponent = exponent * 10 + (*p - '0');
                p++;
            }
            while (exponent-- > 0)
                scale *= 10.0;
            if (exp_neg)
                value /= scale;
            else
                value *= scale;
        }
    }

    if (end)
        *end = (char *)(any ? p : s);

    return neg ? -value : value;
}

float strtof(const char *s, char **end)
{
    return (float)strtod(s, end);
}

double atof(const char *s)
{
    return strtod(s, 0);
}

long atol(const char *s)
{
    return strtol(s, 0, 10);
}

long long atoll(const char *s)
{
    return strtoll(s, 0, 10);
}

int abs(int x)
{
    return x < 0 ? -x : x;
}

long labs(long x)
{
    return x < 0 ? -x : x;
}

long long llabs(long long x)
{
    return x < 0 ? -x : x;
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    unsigned char *b = (unsigned char *)base;

    if (n < 2 || !size)
        return;

    for (size_t i = 1; i < n; i++) {
        size_t j = i;
        while (j > 0 && cmp(b + (j - 1) * size, b + j * size) > 0) {
            unsigned char tmp;
            unsigned char *a = b + (j - 1) * size;
            unsigned char *c = b + j * size;

            for (size_t k = 0; k < size; k++) {
                tmp = a[k];
                a[k] = c[k];
                c[k] = tmp;
            }
            j--;
        }
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *))
{
    const unsigned char *b = (const unsigned char *)base;

    while (n) {
        size_t mid = n / 2;
        const unsigned char *p = b + mid * size;
        int r = cmp(key, p);

        if (r == 0)
            return (void *)p;

        if (r < 0) {
            n = mid;
        } else {
            b = p + size;
            n -= mid + 1;
        }
    }

    return 0;
}

static unsigned int rng_state = 0x12345678u;

void srand(unsigned int seed)
{
    rng_state = seed ? seed : 1u;
}

int rand(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (int)((rng_state >> 16) & 0x7FFFu);
}

struct mesa_port_file {
    int fd;
    int eof;
    int err;
    int own;
};

static struct mesa_port_file stdin_file  = { 0, 0, 0, 0 };
static struct mesa_port_file stdout_file = { 1, 0, 0, 0 };
static struct mesa_port_file stderr_file = { 2, 0, 0, 0 };

FILE *stdin  = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

int fileno(FILE *f)
{
    return f ? f->fd : -1;
}

int fflush(FILE *f)
{
    return ax_mesa_iflush(f);
}

int ferror(FILE *f)
{
    return f ? f->err : 0;
}

int feof(FILE *f)
{
    return f ? f->eof : 0;
}

void clearerr(FILE *f)
{
    if (f) {
        f->eof = 0;
        f->err = 0;
    }
}

int fputc(int c, FILE *f)
{
    char ch = (char)c;
    int r = write(f->fd, &ch, 1);

    if (r != 1) {
        f->err = 1;
        return -1;
    }

    return c;
}

int putc(int c, FILE *f)
{
    return fputc(c, f);
}

int getc(FILE *f)
{
    unsigned char ch;
    int r = read(f->fd, (char *)&ch, 1);

    if (r != 1) {
        f->eof = 1;
        return -1;
    }

    return (int)ch;
}

int fgetc(FILE *f)
{
    return getc(f);
}

int getchar(void)
{
    return getc(stdin);
}

int ungetc(int c, FILE *f)
{
    (void)c;
    (void)f;
    return -1;
}

int fputs(const char *s, FILE *f)
{
    size_t n = strlen(s);
    int r = write(f->fd, (char *)s, (int)n);

    if (r != (int)n) {
        f->err = 1;
        return -1;
    }

    return 0;
}

char *fgets(char *s, int n, FILE *f)
{
    int i = 0;

    if (n <= 0)
        return 0;

    while (i + 1 < n) {
        int c = getc(f);

        if (c < 0)
            break;

        s[i++] = (char)c;

        if (c == '\n')
            break;
    }

    s[i] = 0;

    return i ? s : 0;
}

size_t fread(void *p, size_t size, size_t nmemb, FILE *f)
{
    size_t want = size * nmemb;
    int r;

    if (!want)
        return 0;

    r = read(f->fd, (char *)p, (int)want);

    if (r <= 0) {
        f->eof = 1;
        return 0;
    }

    return (size_t)r / size;
}

size_t fwrite(const void *p, size_t size, size_t nmemb, FILE *f)
{
    size_t want = size * nmemb;
    int r;

    if (!want)
        return 0;

    r = write(f->fd, (char *)p, (int)want);

    if (r < 0) {
        f->err = 1;
        return 0;
    }

    return (size_t)r / size;
}

int fseek(FILE *f, long off, int whence)
{
    int r = ax_mesa_lseek(f->fd, (int)off, whence);

    if (r < 0) {
        f->err = 1;
        return -1;
    }

    f->eof = 0;
    return 0;
}

long ftell(FILE *f)
{
    return (long)ax_mesa_lseek(f->fd, 0, 1);
}

void rewind(FILE *f)
{
    fseek(f, 0, 0);
    clearerr(f);
}

FILE *fopen(const char *path, const char *mode)
{
    int flags = 0;
    int fd;
    struct mesa_port_file *f;

    if (!path || !mode)
        return 0;

    if (mode[0] == 'r' && mode[1] == '+')
        flags = 0x0002;
    else if (mode[0] == 'r')
        flags = 0x0000;
    else if (mode[0] == 'w')
        flags = 0x0001 | 0x0040 | 0x0200;
    else if (mode[0] == 'a')
        flags = 0x0001 | 0x0040;
    else
        return 0;

    fd = open(path, flags, 0);

    if (fd < 0)
        return 0;

    f = (struct mesa_port_file *)calloc(1, sizeof(*f));

    if (!f) {
        close(fd);
        return 0;
    }

    f->fd = fd;
    f->own = 1;

    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    struct mesa_port_file *f;

    (void)mode;

    if (fd < 0)
        return 0;

    f = (struct mesa_port_file *)calloc(1, sizeof(*f));

    if (!f)
        return 0;

    f->fd = fd;

    return f;
}

int flock(int fd, int operation)
{
    (void)fd;
    (void)operation;
    return 0;
}

int ftruncate(int fd, long length)
{
    long pos = ax_mesa_lseek(fd, 0, 1);

    if (pos < 0 || length < 0)
        return -1;

    if (ax_mesa_lseek(fd, (int)length, 0) < 0)
        return -1;

    if (ax_mesa_lseek(fd, (int)pos, 0) < 0)
        return -1;

    return 0;
}

int fclose(FILE *f)
{
    int r = 0;

    if (!f)
        return -1;

    if (f->own)
        r = close(f->fd);

    free(f);
    return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (n > 0)
        fputs(buf, f);

    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, 0x7FFFFFFFu, fmt, ap);
}

int vasprintf(char **out, const char *fmt, va_list ap)
{
    va_list copy;
    int n;
    char *buf;

    va_copy(copy, ap);
    n = vsnprintf(0, 0, fmt, copy);
    va_end(copy);

    if (n < 0)
        return -1;

    buf = (char *)malloc((size_t)n + 1);

    if (!buf)
        return -1;

    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    *out = buf;

    return n;
}

int asprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vasprintf(out, fmt, ap);
    va_end(ap);
    return n;
}

int vfscanf(FILE *f, const char *fmt, va_list ap)
{
    (void)f; (void)fmt; (void)ap;
    return 0;
}

int fscanf(FILE *f, const char *fmt, ...)
{
    (void)f; (void)fmt;
    return 0;
}

int scanf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

int sscanf(const char *s, const char *fmt, ...)
{
    (void)s; (void)fmt;
    return 0;
}

int vsscanf(const char *s, const char *fmt, va_list ap)
{
    (void)s; (void)fmt; (void)ap;
    return 0;
}

int vscanf(const char *fmt, va_list ap)
{
    (void)fmt; (void)ap;
    return 0;
}

void perror(const char *s)
{
    if (s)
        fputs(s, stderr);

    fputs(": error\n", stderr);
}

char *strerror(int e)
{
    (void)e;
    return (char *)"error";
}

int strerror_r(int err, char *buf, size_t n)
{
    const char *msg = strerror(err);
    size_t len = strlen(msg);

    if (n == 0)
        return 22;

    if (len >= n)
        len = n - 1;

    memcpy(buf, msg, len);
    buf[len] = 0;

    return 0;
}

char *getenv(const char *name)
{
    (void)name;
    return 0;
}

int mkdir(const char *path, int mode)
{
    (void)path; (void)mode;
    return -1;
}
