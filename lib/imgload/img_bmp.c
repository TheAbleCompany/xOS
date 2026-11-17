/*
 * ============================================================
 * img_bmp.c -- BMP decoder for the ArtyomX64 userspace loader
 * ============================================================
 *
 * Supports:
 *   * "BM" files with BITMAPCOREHEADER (12 bytes) and
 *     BITMAPINFOHEADER / V4 / V5 (>= 40 bytes)
 *   * compression BI_RGB (0): 24/32 bpp truecolor and
 *     1/4/8 bpp paletted
 *   * bottom-up rows (positive height) and top-down rows
 *     (negative height)
 *   * row padding to 4-byte boundary
 *
 * Output: ARGB8888 (alpha 0xFF -- BMP carries no alpha here).
 * ============================================================
 */

#include "imgload.h"

typedef struct {
    const uint8_t *p;
    long           len;
} bmp_buf_t;

static int b_u8(const bmp_buf_t *b, long off, unsigned *out)
{
    if (off < 0 || off + 1 > b->len)
        return 0;

    *out = b->p[off];

    return 1;
}

static int b_u16(const bmp_buf_t *b, long off, unsigned *out)
{
    if (off < 0 || off + 2 > b->len)
        return 0;

    *out = (unsigned)b->p[off] | ((unsigned)b->p[off + 1] << 8);

    return 1;
}

static int b_u32(const bmp_buf_t *b, long off, unsigned *out)
{
    if (off < 0 || off + 4 > b->len)
        return 0;

    *out = (unsigned)b->p[off] |
           ((unsigned)b->p[off + 1] << 8) |
           ((unsigned)b->p[off + 2] << 16) |
           ((unsigned)b->p[off + 3] << 24);

    return 1;
}

static int b_s32(const bmp_buf_t *b, long off, long *out)
{
    unsigned v;

    if (!b_u32(b, off, &v))
        return 0;

    *out = (long)(int32_t)v;

    return 1;
}

static unsigned pack_argb(unsigned a, unsigned r, unsigned g, unsigned b)
{
    return ((a & 0xFFu) << 24) | ((r & 0xFFu) << 16) |
           ((g & 0xFFu) << 8)  |  (b & 0xFFu);
}

int img_decode_bmp(const uint8_t *data, long len,
                   uint32_t *dst, int dst_max_px, img_image_t *out)
{
    bmp_buf_t f = { data, len };

    if (len < 26 || f.p[0] != 'B' || f.p[1] != 'M')
        return IMG_ERR_FORMAT;

    unsigned data_offset = 0;

    if (!b_u32(&f, 10, &data_offset))
        return IMG_ERR_CORRUPT;

    unsigned hdr_size = 0;

    if (!b_u32(&f, 14, &hdr_size))
        return IMG_ERR_CORRUPT;

    long    width   = 0;
    long    height  = 0;        /* negative = top-down */
    unsigned bpp    = 0;
    unsigned comp   = 0;
    long    pal_off = 0;
    int     pal_bgr = 4;        /* bytes per palette entry */

    if (hdr_size == 12) {
        /* BITMAPCOREHEADER */
        unsigned v16 = 0;

        if (!b_u16(&f, 18, &v16))  return IMG_ERR_CORRUPT;
        width = (long)v16;

        if (!b_u16(&f, 20, &v16))  return IMG_ERR_CORRUPT;
        height = (long)(short)v16;

        if (!b_u16(&f, 24, &v16))  return IMG_ERR_CORRUPT;
        bpp = v16;

        comp = 0;
        pal_off = 14 + 12;
        pal_bgr = 3;
    } else if (hdr_size >= 40) {
        if (!b_s32(&f, 18, &width))   return IMG_ERR_CORRUPT;
        if (!b_s32(&f, 22, &height))  return IMG_ERR_CORRUPT;

        unsigned v16 = 0;

        if (!b_u16(&f, 28, &v16))     return IMG_ERR_CORRUPT;
        bpp = v16;

        if (!b_u32(&f, 30, &comp))    return IMG_ERR_CORRUPT;
        pal_off = 14 + hdr_size;
        pal_bgr = 4;
    } else {
        return IMG_ERR_UNSUPPORTED;
    }

    if (width <= 0 || width > IMG_MAX_DIM ||
        height == 0 || (height < 0 ? -height : height) > IMG_MAX_DIM)
        return IMG_ERR_TOO_BIG;

    if ((long)width * (height < 0 ? -height : height) > (long)dst_max_px)
        return IMG_ERR_TOO_BIG;

    if (comp != 0) {
        /* 1/2 = RLE-8/4, 3 = BI_BITFIELDS, 4/5 = JPEG/PNG */
        return IMG_ERR_UNSUPPORTED;
    }

    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
        return IMG_ERR_UNSUPPORTED;

    /* palette for 1/4/8 bpp */
    unsigned char pal[256][3];
    unsigned      pal_entries = 0;

    if (bpp <= 8) {
        pal_entries = 1u << bpp;

        for (unsigned i = 0; i < pal_entries; i++) {
            unsigned b0, g0, r0;

            long e = pal_off + (long)i * pal_bgr;

            if (!b_u8(&f, e, &b0) ||
                !b_u8(&f, e + 1, &g0) ||
                !b_u8(&f, e + 2, &r0))
                return IMG_ERR_CORRUPT;

            pal[i][0] = (unsigned char)r0;
            pal[i][1] = (unsigned char)g0;
            pal[i][2] = (unsigned char)b0;
        }
    }

    long row_bytes = ((long)width * (long)bpp + 31) / 32 * 4;
    long rows      = (height < 0) ? -height : height;
    int  top_down  = (height < 0);

    if (data_offset + row_bytes * rows > f.len)
        return IMG_ERR_CORRUPT;

    for (long rr = 0; rr < rows; rr++) {
        long src_row = top_down ? rr : (rows - 1 - rr);
        long src     = (long)data_offset + src_row * row_bytes;

        uint32_t *drow = dst + rr * (long)width;

        switch (bpp) {
            case 32:
                for (long x = 0; x < width; x++) {
                    long o = src + x * 4;

                    drow[x] = pack_argb(255,
                                        f.p[o + 2],
                                        f.p[o + 1],
                                        f.p[o + 0]);
                }

                break;

            case 24:
                for (long x = 0; x < width; x++) {
                    long o = src + x * 3;

                    drow[x] = pack_argb(255,
                                        f.p[o + 2],
                                        f.p[o + 1],
                                        f.p[o + 0]);
                }

                break;

            case 8:
                for (long x = 0; x < width; x++) {
                    unsigned idx = f.p[src + x];

                    if (idx >= pal_entries)
                        idx = 0;

                    drow[x] = pack_argb(255,
                                        pal[idx][0],
                                        pal[idx][1],
                                        pal[idx][2]);
                }

                break;

            case 4:
                for (long x = 0; x < width; x++) {
                    unsigned byte = f.p[src + (x >> 1)];
                    unsigned idx  = (x & 1) ? (byte & 0x0F)
                                            : (byte >> 4);

                    if (idx >= pal_entries)
                        idx = 0;

                    drow[x] = pack_argb(255,
                                        pal[idx][0],
                                        pal[idx][1],
                                        pal[idx][2]);
                }

                break;

            case 1:
                for (long x = 0; x < width; x++) {
                    unsigned byte = f.p[src + (x >> 3)];
                    unsigned idx  = (byte >> (7 - (x & 7))) & 1;

                    if (idx >= pal_entries)
                        idx = 0;

                    drow[x] = pack_argb(255,
                                        pal[idx][0],
                                        pal[idx][1],
                                        pal[idx][2]);
                }

                break;
        }
    }

    out->w   = (int)width;
    out->h   = (int)rows;
    out->pix = dst;

    return IMG_OK;
}
