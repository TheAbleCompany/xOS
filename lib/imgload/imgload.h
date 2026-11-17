/*
 * ============================================================
 * imgload.h -- userspace image loader for ArtyomX64
 * ============================================================
 *
 * A small, freestanding image decoding library that lives ENTIRELY
 * in user space: no kernel involvement beyond the standard
 * open/read/close syscalls.  Designed for ArtyomX64 applications
 * (linked with -nostdlib, no malloc required -- all scratch memory
 * is static).
 *
 * Supported formats
 * -----------------
 *
 *   PNG  (RFC 2083)
 *     * bit depths 1/2/4/8/16
 *     * color types: 0 (gray), 2 (RGB), 3 (palette), 4 (gray+alpha),
 *       6 (RGBA)
 *     * tRNS transparency (palette alpha + gray/RGB color key)
 *     * non-interlaced only (Adam7 returns IMG_ERR_UNSUPPORTED)
 *     * zlib stream must not use a preset dictionary
 *     * 16-bit samples are reduced to 8 bits (high byte)
 *
 *   BMP  ("BM", Windows)
 *     * BITMAPCOREHEADER (12) and BITMAPINFOHEADER/V4/V5 (>=40)
 *     * compression BI_RGB (0) only; BI_BITFIELDS/RLE return
 *       IMG_ERR_UNSUPPORTED
 *     * 1/4/8 bpp (palette) and 24/32 bpp truecolor
 *     * bottom-up and top-down (negative height) rows
 *
 * Output format: 32-bit ARGB (0xAARRGGBB), alpha 0xFF = opaque.
 * The caller provides the destination pixel buffer, so memory use
 * is explicit and controlled by the application.
 *
 * Link order: just add imgload.o img_png.o img_bmp.o inflate.o to
 * the app's ld line (see apps/Makefile hovertank target).
 *
 * Host testing: build with -DARTYOM_HOST_SHIM to use stdio instead
 * of int 0x80 (see apps/tests-host).
 * ============================================================
 */

#ifndef IMGLOAD_H
#define IMGLOAD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (img_load_* return 0 = IMG_OK on success). */
#define IMG_OK             0
#define IMG_ERR_OPEN      -1   /* open() failed / host fopen failed        */
#define IMG_ERR_READ      -2   /* read() failed or file shrank mid-read    */
#define IMG_ERR_TOO_BIG   -3   /* file > IMG_FILE_MAX, or image does not
                                  fit dst_max_px, or dims > IMG_MAX_DIM   */
#define IMG_ERR_FORMAT    -4   /* magic bytes not PNG/BMP                  */
#define IMG_ERR_UNSUPPORTED -5 /* valid family, unsupported feature
                                  (interlaced PNG, RLE/bitfields BMP...)  */
#define IMG_ERR_CORRUPT   -6   /* broken structure / bad CRC / bad zlib    */
#define IMG_ERR_NODATA    -7   /* structurally required chunk/table absent */

/* Static scratch limits (raise if you need bigger images):
 *   IMG_FILE_MAX  -- max compressed file size read into memory
 *   IMG_RAW_MAX   -- max size of the INFLATED raw scanline data
 *                    (e.g. 512x192 RGBA = 384 KiB, 512x320 RGB = 480 KiB)
 * The .bss cost is IMG_FILE_MAX + IMG_RAW_MAX + ~1 KiB.            */
#ifndef IMG_FILE_MAX
#define IMG_FILE_MAX  (1024u * 1024u)
#endif
#ifndef IMG_RAW_MAX
#define IMG_RAW_MAX   (4u * 1024u * 1024u)
#endif
#ifndef IMG_MAX_DIM
#define IMG_MAX_DIM   4096
#endif

/* Decoded image description.  `pix` is the caller-provided buffer,
 * w*h ARGB pixels.  Fully transparent pixels are 0x00000000. */
typedef struct {
    int            w;
    int            h;
    uint32_t      *pix;      /* == dst passed by caller */
} img_image_t;

/* Load from the initrd/filesystem (userspace open/read/close).
 * dst must hold w*h uint32_t pixels; use img_query_file() first to
 * size the buffer, or pass a max-size buffer + dst_max_px.        */
int img_load_file(const char       *path,
                  uint32_t         *dst,
                  int               dst_max_px,
                  img_image_t      *out);

/* Decode from an in-memory buffer (useful for packed resources). */
int img_load_memory(const uint8_t  *data,
                    int             len,
                    uint32_t       *dst,
                    int             dst_max_px,
                    img_image_t    *out);

/* Query dimensions without decoding pixels.
 * Reads only the header (still needs open/read on the OS).        */
int img_query_file(const char *path, int *w, int *h);

/* Human-readable error string (static). */
const char *img_strerror(int err);

/* ------------------------------------------------------------------
 * Internal helpers shared by the decoders (exported for tests).
 * ------------------------------------------------------------------ */

/* RFC 1951 DEFLATE: inflate `src`[src_len] into `dst`[dst_len].
 * `src` must be the raw zlib stream (RFC 1950 wrapper included).
 * Returns number of bytes written, or negative IMG_ERR_*.          */
long img_inflate(const uint8_t *src, long src_len,
                 uint8_t *dst, long dst_len);

/* PNG / BMP decoders on in-memory data (used by img_load_memory). */
int img_decode_png(const uint8_t *data, long len,
                   uint32_t *dst, int dst_max_px, img_image_t *out);
int img_decode_bmp(const uint8_t *data, long len,
                   uint32_t *dst, int dst_max_px, img_image_t *out);

/* Shared small helpers (imgload.c). */
void img_memcpy(void *d, const void *s, long n);
void img_memset(void *d, int v, long n);
int  img_memcmp(const void *a, const void *b, long n);
long img_strlen(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* IMGLOAD_H */
