/*
 * ============================================================
 * imgload.c -- userspace image loader core for ArtyomX64
 * ============================================================
 *
 * Format dispatch + file I/O + tiny freestanding helpers.
 *
 * On ArtyomX64 the file is read with the standard userspace
 * syscalls (int 0x80: open/read/close) straight into a static
 * buffer -- the kernel is never asked to decode anything.
 *
 * Build with -DARTYOM_HOST_SHIM on Linux to test the same code
 * path against host stdio (see apps/tests-host).
 * ============================================================
 */

#include "imgload.h"

/* ------------------------------------------------------------------
 * Static scratch memory (the only RAM this module owns)
 * ------------------------------------------------------------------ */

/* compressed file image + raw inflate staging, see img_png.c */
unsigned char img_file_buf[IMG_FILE_MAX];
unsigned char img_raw_stage[IMG_RAW_MAX];

/* ------------------------------------------------------------------
 * Freestanding helpers (unique names: no libc collisions)
 * ------------------------------------------------------------------ */

void img_memcpy(void *d, const void *s, long n)
{
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;

    for (; n > 0; n--)
        *dd++ = *ss++;
}

void img_memset(void *d, int v, long n)
{
    unsigned char *dd = (unsigned char *)d;

    for (; n > 0; n--)
        *dd++ = (unsigned char)v;
}

int img_memcmp(const void *a, const void *b, long n)
{
    const unsigned char *aa = (const unsigned char *)a;
    const unsigned char *bb = (const unsigned char *)b;

    for (; n > 0; n--) {
        if (*aa != *bb)
            return (int)*aa - (int)*bb;

        aa++;
        bb++;
    }

    return 0;
}

long img_strlen(const char *s)
{
    long n = 0;

    while (s[n])
        n++;

    return n;
}

/* ------------------------------------------------------------------
 * File I/O (userspace syscalls on the OS, stdio under the shim)
 * ------------------------------------------------------------------ */

#ifdef ARTYOM_HOST_SHIM

#include <stdio.h>

static long host_read_file(const char *path, unsigned char *buf, long max)
{
    FILE *fp = fopen(path, "rb");

    if (!fp)
        return IMG_ERR_OPEN;                 /* -1 */

    long n = (long)fread(buf, 1, (size_t)max, fp);

    if (ferror(fp)) {
        fclose(fp);
        return IMG_ERR_READ;                 /* -2 */
    }

    fclose(fp);

    return n;
}

int img_query_file(const char *path, int *w, int *h)
{
    long n = host_read_file(path, img_file_buf, 64);

    if (n < 0)
        return (int)n;

    /* header-only sniff: PNG IHDR at offset 16, BMP at 18/22 */
    if (n >= 24 && img_memcmp(img_file_buf,
            "\x89PNG\r\n\x1a\n", 8) == 0) {
        *w = (int)(((unsigned)img_file_buf[16] << 24) |
                   ((unsigned)img_file_buf[17] << 16) |
                   ((unsigned)img_file_buf[18] << 8)  |
                    (unsigned)img_file_buf[19]);
        *h = (int)(((unsigned)img_file_buf[20] << 24) |
                   ((unsigned)img_file_buf[21] << 16) |
                   ((unsigned)img_file_buf[22] << 8)  |
                    (unsigned)img_file_buf[23]);
        return IMG_OK;
    }

    if (n >= 26 && img_file_buf[0] == 'B' && img_file_buf[1] == 'M') {
        /* BMP stores little-endian */
        *w = (int)((int32_t)( (unsigned)img_file_buf[18]       |
                             ((unsigned)img_file_buf[19] << 8) |
                             ((unsigned)img_file_buf[20] << 16)|
                             ((unsigned)img_file_buf[21] << 24)));
        *h = (int)((int32_t)( (unsigned)img_file_buf[22]       |
                             ((unsigned)img_file_buf[23] << 8) |
                             ((unsigned)img_file_buf[24] << 16)|
                             ((unsigned)img_file_buf[25] << 24)));
        if (*h < 0)
            *h = -*h;
        return IMG_OK;
    }

    return IMG_ERR_FORMAT;
}

#else /* !ARTYOM_HOST_SHIM -- real ArtyomX64 syscalls */

/* thin syscall wrappers (same ABI as lib/unistd.h, but a plain
 * non-retrying read: for initrd files fd != 0 a zero return is EOF) */
static int sc_open(const char *path, int flags, int mode)
{
    volatile int ret;

    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"(5), "D"((long)path), "S"((long)flags), "d"((long)mode)
        : "memory");

    return ret;
}

static int sc_read(int fd, void *buf, int count)
{
    volatile int ret;

    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"(3), "D"((long)fd), "S"((long)buf), "d"((long)count)
        : "memory");

    return ret;
}

static int sc_close(int fd)
{
    volatile int ret;

    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"(6), "D"((long)fd)
        : "memory");

    return ret;
}

static long os_read_file(const char *path, unsigned char *buf, long max)
{
    int fd = sc_open(path, 0 /* O_RDONLY */, 0);

    if (fd < 0)
        return IMG_ERR_OPEN;

    long total = 0;

    while (total < max) {
        int r = sc_read(fd, buf + total, (int)(max - total));

        if (r < 0) {
            sc_close(fd);
            return IMG_ERR_READ;
        }

        if (r == 0)
            break;                          /* EOF */

        total += r;
    }

    sc_close(fd);

    return total;
}

int img_query_file(const char *path, int *w, int *h)
{
    long n = os_read_file(path, img_file_buf, 64);

    if (n < 0)
        return (int)n;

    if (n >= 24 && img_memcmp(img_file_buf,
            "\x89PNG\r\n\x1a\n", 8) == 0) {
        *w = (int)(((unsigned)img_file_buf[16] << 24) |
                   ((unsigned)img_file_buf[17] << 16) |
                   ((unsigned)img_file_buf[18] << 8)  |
                    (unsigned)img_file_buf[19]);
        *h = (int)(((unsigned)img_file_buf[20] << 24) |
                   ((unsigned)img_file_buf[21] << 16) |
                   ((unsigned)img_file_buf[22] << 8)  |
                    (unsigned)img_file_buf[23]);
        return IMG_OK;
    }

    if (n >= 26 && img_file_buf[0] == 'B' && img_file_buf[1] == 'M') {
        /* BMP stores little-endian */
        *w = (int)((int32_t)( (unsigned)img_file_buf[18]       |
                             ((unsigned)img_file_buf[19] << 8)  |
                             ((unsigned)img_file_buf[20] << 16) |
                             ((unsigned)img_file_buf[21] << 24)));
        *h = (int)((int32_t)( (unsigned)img_file_buf[22]       |
                             ((unsigned)img_file_buf[23] << 8)  |
                             ((unsigned)img_file_buf[24] << 16) |
                             ((unsigned)img_file_buf[25] << 24)));
        if (*h < 0)
            *h = -*h;
        return IMG_OK;
    }

    return IMG_ERR_FORMAT;
}

#endif /* ARTYOM_HOST_SHIM */

/* ------------------------------------------------------------------
 * Dispatch
 * ------------------------------------------------------------------ */

int img_load_memory(const uint8_t *data, int len,
                    uint32_t *dst, int dst_max_px, img_image_t *out)
{
    if (!data || len <= 0 || !dst || !out)
        return IMG_ERR_FORMAT;

    static const unsigned char png_sig[8] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };

    if (len >= 8 && img_memcmp(data, png_sig, 8) == 0)
        return img_decode_png(data, len, dst, dst_max_px, out);

    if (len >= 2 && data[0] == 'B' && data[1] == 'M')
        return img_decode_bmp(data, len, dst, dst_max_px, out);

    return IMG_ERR_FORMAT;
}

int img_load_file(const char *path,
                  uint32_t *dst, int dst_max_px, img_image_t *out)
{
    if (!path || !dst || !out)
        return IMG_ERR_FORMAT;

#ifdef ARTYOM_HOST_SHIM
    long n = host_read_file(path, img_file_buf, IMG_FILE_MAX);
#else
    long n = os_read_file(path, img_file_buf, IMG_FILE_MAX);
#endif

    if (n == IMG_ERR_OPEN)
        return IMG_ERR_OPEN;

    if (n < 0)
        return IMG_ERR_READ;

    if (n == IMG_FILE_MAX)
        return IMG_ERR_TOO_BIG;             /* filled the whole buffer */

    return img_load_memory(img_file_buf, (int)n,
                           dst, dst_max_px, out);
}

const char *img_strerror(int err)
{
    switch (err) {
        case IMG_OK:             return "ok";
        case IMG_ERR_OPEN:       return "cannot open file";
        case IMG_ERR_READ:       return "read error";
        case IMG_ERR_TOO_BIG:    return "image too large for buffer";
        case IMG_ERR_FORMAT:     return "not a PNG/BMP file";
        case IMG_ERR_UNSUPPORTED: return "unsupported PNG/BMP feature";
        case IMG_ERR_CORRUPT:    return "corrupted image data";
        case IMG_ERR_NODATA:     return "missing image data";
        default:                 return "unknown error";
    }
}
