/*
 * ============================================================
 * img_png.c -- PNG decoder for the ArtyomX64 userspace loader
 * ============================================================
 *
 * Streaming-free, single-pass implementation:
 *
 *   1. walk chunks, verify CRC32 and sizes
 *   2. compact all IDAT payloads in place (contiguous zlib stream)
 *   3. img_inflate() into a static raw staging buffer
 *   4. defilter scanlines (filters 0..4)
 *   5. expand samples into caller-provided ARGB8888 buffer
 *
 * Supported: bit depth 1/2/4/8/16, color types 0/2/3/4/6, tRNS,
 * non-interlaced.  Adam7 (interlace=1) is rejected explicitly.
 * 16-bit samples use the high byte.
 * ============================================================
 */

#include "imgload.h"

/* ------------------------------------------------------------------
 * CRC-32 (PNG polynomial, table computed on first use)
 * ------------------------------------------------------------------ */

static unsigned img_crc_table[256];
static int      img_crc_ready = 0;

static void crc_init(void)
{
    for (unsigned n = 0; n < 256; n++) {
        unsigned c = n;

        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);

        img_crc_table[n] = c;
    }

    img_crc_ready = 1;
}

static unsigned crc32_buf(const unsigned char *buf, long len)
{
    if (!img_crc_ready)
        crc_init();

    unsigned c = 0xFFFFFFFFu;

    for (long i = 0; i < len; i++)
        c = img_crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);

    return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------
 * Little-endian readers with bounds checks
 * ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    long           len;
} img_buf_t;

static int rd_u32(const img_buf_t *b, long off, unsigned *out)
{
    if (off < 0 || off + 4 > b->len)
        return 0;

    *out = ((unsigned)b->p[off] << 24) |
           ((unsigned)b->p[off + 1] << 16) |
           ((unsigned)b->p[off + 2] << 8) |
            (unsigned)b->p[off + 3];

    return 1;
}

static int rd_u16(const img_buf_t *b, long off, unsigned *out)
{
    if (off < 0 || off + 2 > b->len)
        return 0;

    *out = ((unsigned)b->p[off] << 8) | (unsigned)b->p[off + 1];

    return 1;
}

static int rd_u8(const img_buf_t *b, long off, unsigned *out)
{
    if (off < 0 || off + 1 > b->len)
        return 0;

    *out = b->p[off];

    return 1;
}

/* ------------------------------------------------------------------
 * Sample expansion helpers
 * ------------------------------------------------------------------ */

/* fetch the n-th sample of given bit depth from a defiltered row */
static unsigned row_sample(const unsigned char *row, long index, int depth)
{
    switch (depth) {
        case 16:
            return (unsigned)row[index * 2];            /* high byte */

        case 8:
            return (unsigned)row[index];

        case 4: {
            unsigned v = row[index >> 1];
            return (v >> (4 - 4 * (index & 1))) & 0x0Fu;
        }

        case 2: {
            unsigned v = row[index >> 2];
            return (v >> (6 - 2 * (index & 3))) & 0x03u;
        }

        case 1: {
            unsigned v = row[index >> 3];
            return (v >> (7 - (index & 7))) & 0x01u;
        }

        default:
            return 0;
    }
}

static unsigned expand_gray(unsigned v, int depth)
{
    switch (depth) {
        case 1:  return v ? 255u : 0u;
        case 2:  return (unsigned)(v * 85u);        /* 0..3 -> 0..255 */
        case 4:  return (unsigned)(v * 17u);        /* 0..15 -> 0..255 */
        default: return v;                          /* 8/16 (hi byte) */
    }
}

static unsigned pack_argb(unsigned a, unsigned r, unsigned g, unsigned b)
{
    return ((a & 0xFFu) << 24) | ((r & 0xFFu) << 16) |
           ((g & 0xFFu) << 8)  |  (b & 0xFFu);
}

/* ------------------------------------------------------------------
 * Decoder
 * ------------------------------------------------------------------ */

/* static inflate staging buffer shared with imgload.c (no malloc) */
extern unsigned char img_raw_stage[IMG_RAW_MAX];

int img_decode_png(const uint8_t *data, long len,
                   uint32_t *dst, int dst_max_px, img_image_t *out)
{
    img_buf_t f = { data, len };

    static const unsigned char sig[8] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };

    if (len < 8 || img_memcmp(f.p, sig, 8) != 0)
        return IMG_ERR_FORMAT;

    /* ---- IHDR ---- */
    unsigned clen = 0;
    long     pos  = 8;

    if (!rd_u32(&f, pos, &clen) || clen != 13)
        return IMG_ERR_CORRUPT;

    long ihdr_data = pos + 8;

    unsigned w = 0, h = 0;

    if (!rd_u32(&f, ihdr_data, &w) || !rd_u32(&f, ihdr_data + 4, &h))
        return IMG_ERR_CORRUPT;

    unsigned depth = 0, ctype = 0, compress = 0, filter = 0, interlace = 0;

    if (!rd_u8(&f, ihdr_data + 8, &depth) ||
        !rd_u8(&f, ihdr_data + 9, &ctype) ||
        !rd_u8(&f, ihdr_data + 10, &compress) ||
        !rd_u8(&f, ihdr_data + 11, &filter) ||
        !rd_u8(&f, ihdr_data + 12, &interlace))
        return IMG_ERR_CORRUPT;

    if (w == 0 || h == 0 ||
        w > IMG_MAX_DIM || h > IMG_MAX_DIM)
        return IMG_ERR_TOO_BIG;

    if ((long)w * (long)h > (long)dst_max_px)
        return IMG_ERR_TOO_BIG;

    if (compress != 0 || filter != 0)
        return IMG_ERR_UNSUPPORTED;

    if (interlace != 0)
        return IMG_ERR_UNSUPPORTED;             /* Adam7 not supported */

    /* valid (color type, bit depth) combinations, PNG spec table 11.1:
     * depths run 1..16, so the column index must reach 16           */
    static const unsigned char d_ok[8][17] = {
        [0] = { [1] = 1, [2] = 1, [4] = 1, [8] = 1, [16] = 1 },  /* gray    */
        [2] = { [8] = 1, [16] = 1 },                             /* rgb     */
        [3] = { [1] = 1, [2] = 1, [4] = 1, [8] = 1 },            /* palette */
        [4] = { [8] = 1, [16] = 1 },                             /* gray+a  */
        [6] = { [8] = 1, [16] = 1 }                              /* rgba    */
    };

    if (ctype == 1 || ctype == 5 || ctype > 6)
        return IMG_ERR_CORRUPT;

    if (!d_ok[ctype][depth])
        return IMG_ERR_CORRUPT;

    int channels = (ctype == 2) ? 3 : (ctype == 4) ? 2 :
                   (ctype == 6) ? 4 : 1;

    long bits_per_px  = (long)channels * depth;
    long row_bytes    = ((long)w * bits_per_px + 7) / 8;
    long stride       = row_bytes + 1;          /* + filter byte */

    if (stride > IMG_RAW_MAX / (long)h + 1)
        return IMG_ERR_TOO_BIG;

    if (stride * h > IMG_RAW_MAX)
        return IMG_ERR_TOO_BIG;

    /* ---- walk chunks: collect PLTE/tRNS, compact IDAT ---- */
    unsigned char palette[256][3];
    unsigned      pal_alpha[256];
    unsigned      pal_count = 0;
    unsigned      trns_gray = 0, trns_rgb[3];
    int           has_trns_gray = 0, has_trns_rgb = 0;

    for (int i = 0; i < 256; i++)
        pal_alpha[i] = 255;

    long idat_write = 0;                        /* compaction cursor */
    int  have_idat = 0;
    int  seen_iend = 0;

    pos = 8 + 12 + 13;                          /* after IHDR chunk */

    while (pos + 8 <= f.len) {
        if (!rd_u32(&f, pos, &clen))
            return IMG_ERR_CORRUPT;

        long type_off  = pos + 4;
        long data_off  = pos + 8;
        long data_end  = data_off + (long)clen;

        if (clen > 0x7FFFFFFFu || data_end + 4 > f.len)
            return IMG_ERR_CORRUPT;

        unsigned ctype_tag =
            ((unsigned)f.p[type_off] << 24) |
            ((unsigned)f.p[type_off + 1] << 16) |
            ((unsigned)f.p[type_off + 2] << 8) |
             (unsigned)f.p[type_off + 3];

        /* CRC over type + data */
        unsigned want_crc = 0;

        if (!rd_u32(&f, data_end, &want_crc))
            return IMG_ERR_CORRUPT;

        unsigned got_crc = crc32_buf(f.p + type_off, 4 + (long)clen);

        if (got_crc != want_crc)
            return IMG_ERR_CORRUPT;

        if (ctype_tag == 0x504C5445u) {         /* PLTE */
            if (clen == 0 || clen > 768 || clen % 3 != 0)
                return IMG_ERR_CORRUPT;

            pal_count = clen / 3;

            for (unsigned i = 0; i < pal_count; i++) {
                palette[i][0] = f.p[data_off + i * 3 + 0];
                palette[i][1] = f.p[data_off + i * 3 + 1];
                palette[i][2] = f.p[data_off + i * 3 + 2];
            }
        } else if (ctype_tag == 0x74524E53u) {  /* tRNS */
            if (ctype == 3) {
                if (clen > pal_count)
                    return IMG_ERR_CORRUPT;

                for (unsigned i = 0; i < clen; i++)
                    pal_alpha[i] = f.p[data_off + i];
            } else if (ctype == 0) {
                unsigned v;

                if (!rd_u16(&f, data_off, &v))
                    return IMG_ERR_CORRUPT;

                /* keep the RAW sample: compared against raw row samples
                 * (for depth <= 8 only the low byte is meaningful)      */
                trns_gray = v;
                has_trns_gray = 1;
            } else if (ctype == 2) {
                unsigned r, g, b;

                if (!rd_u16(&f, data_off, &r) ||
                    !rd_u16(&f, data_off + 2, &g) ||
                    !rd_u16(&f, data_off + 4, &b))
                    return IMG_ERR_CORRUPT;

                trns_rgb[0] = r;
                trns_rgb[1] = g;
                trns_rgb[2] = b;
                has_trns_rgb = 1;
            }
        } else if (ctype_tag == 0x49444154u) {  /* IDAT */
            if (data_off + (long)clen > f.len)
                return IMG_ERR_CORRUPT;

            if (idat_write + (long)clen > f.len)
                return IMG_ERR_CORRUPT;

            /* compact in place (IDATs are contiguous in the file) */
            if (data_off != idat_write)
                img_memcpy((void *)(f.p + idat_write),
                           f.p + data_off, clen);

            idat_write += clen;
            have_idat = 1;
        } else if (ctype_tag == 0x49454E44u) {  /* IEND */
            seen_iend = 1;
            break;
        }

        pos = data_end + 4;                     /* next chunk */
    }

    if (!seen_iend)
        return IMG_ERR_CORRUPT;

    if (!have_idat)
        return IMG_ERR_NODATA;

    if (ctype == 3 && pal_count == 0)
        return IMG_ERR_NODATA;                  /* palette required */

    /* ---- inflate the compacted stream ---- */
    unsigned char *raw = NULL;
    /* the staging buffer lives in imgload.c (static, no malloc) */
    extern unsigned char img_raw_stage[IMG_RAW_MAX];

    raw = img_raw_stage;

    long raw_len = img_inflate(f.p, idat_write, raw, stride * (long)h);

    if (raw_len < 0)
        return (int)raw_len;

    if (raw_len < stride * (long)h)
        return IMG_ERR_CORRUPT;                 /* truncated image */

    /* ---- defilter + expand ---- */
    int bpp = (int)((bits_per_px + 7) / 8);     /* filter step in bytes */

    if (bpp < 1)
        bpp = 1;

    for (long y = 0; y < h; y++) {
        unsigned char *row  = raw + y * stride;
        unsigned char *prev = (y > 0) ? raw + (y - 1) * stride : 0;

        unsigned ftype = row[0];
        unsigned char *d = row + 1;

        switch (ftype) {
            case 1:                             /* Sub */
                for (long i = bpp; i < row_bytes; i++)
                    d[i] = (unsigned char)(d[i] + d[i - bpp]);

                break;

            case 2:                             /* Up */
                if (prev)
                    for (long i = 0; i < row_bytes; i++)
                        d[i] = (unsigned char)(d[i] + prev[1 + i]);

                break;

            case 3:                             /* Average */
                for (long i = 0; i < row_bytes; i++) {
                    unsigned a = (i >= bpp) ? d[i - bpp] : 0;
                    unsigned b = prev ? prev[1 + i] : 0;

                    d[i] = (unsigned char)(d[i] +
                        ((a + b) >> 1));
                }

                break;

            case 4: {                           /* Paeth */
                for (long i = 0; i < row_bytes; i++) {
                    unsigned a = (i >= bpp) ? d[i - bpp] : 0;
                    unsigned b = prev ? prev[1 + i] : 0;
                    unsigned c = (prev && i >= bpp)
                               ? prev[1 + i - bpp] : 0;

                    int p  = (int)(a + b - c);
                    int pa = p - (int)a; if (pa < 0) pa = -pa;
                    int pb = p - (int)b; if (pb < 0) pb = -pb;
                    int pc = p - (int)c; if (pc < 0) pc = -pc;

                    unsigned pred =
                        (pa <= pb && pa <= pc) ? a :
                        (pb <= pc)             ? b : c;

                    d[i] = (unsigned char)(d[i] + pred);
                }

                break;
            }

            default:                            /* 0 = None */
                break;
        }

        /* expand this row into ARGB */
        uint32_t *outrow = dst + y * (long)w;

        switch (ctype) {
            case 0: {                           /* gray */
                for (long x = 0; x < w; x++) {
                    unsigned s = row_sample(d, x, depth);
                    unsigned g = expand_gray(s, depth);
                    unsigned a = 255;

                    if (has_trns_gray) {
                        /* compare RAW sample values */
                        unsigned raws =
                            (depth == 16)
                                ? ((unsigned)d[x * 2] << 8) | d[x * 2 + 1]
                                : s;

                        if (raws == trns_gray)
                            a = 0;
                    }

                    outrow[x] = pack_argb(a, g, g, g);
                }

                break;
            }

            case 2: {                           /* rgb */
                for (long x = 0; x < w; x++) {
                    unsigned r = row_sample(d, x * 3 + 0, depth);
                    unsigned g = row_sample(d, x * 3 + 1, depth);
                    unsigned b = row_sample(d, x * 3 + 2, depth);

                    unsigned a = 255;

                    if (has_trns_rgb && depth == 8) {
                        if (r == trns_rgb[0] &&
                            g == trns_rgb[1] &&
                            b == trns_rgb[2])
                            a = 0;
                    }

                    outrow[x] = pack_argb(a, r, g, b);
                }

                break;
            }

            case 3: {                           /* palette */
                for (long x = 0; x < w; x++) {
                    unsigned idx = row_sample(d, x, depth);

                    if (idx >= pal_count)
                        idx = 0;

                    outrow[x] = pack_argb(pal_alpha[idx],
                                          palette[idx][0],
                                          palette[idx][1],
                                          palette[idx][2]);
                }

                break;
            }

            case 4: {                           /* gray + alpha */
                for (long x = 0; x < w; x++) {
                    unsigned g = expand_gray(
                        row_sample(d, x * 2 + 0, depth), depth);
                    unsigned a =
                        (depth == 16)
                            ? (unsigned)d[(x * 2 + 1) * 2]
                            : row_sample(d, x * 2 + 1, 8);

                    outrow[x] = pack_argb(a, g, g, g);
                }

                break;
            }

            case 6: {                           /* rgba */
                for (long x = 0; x < w; x++) {
                    unsigned r = row_sample(d, x * 4 + 0, depth);
                    unsigned g = row_sample(d, x * 4 + 1, depth);
                    unsigned b = row_sample(d, x * 4 + 2, depth);
                    unsigned a = (depth == 16)
                               ? (unsigned)d[(x * 4 + 3) * 2]
                               : row_sample(d, x * 4 + 3, 8);

                    outrow[x] = pack_argb(a, r, g, b);
                }

                break;
            }
        }
    }

    out->w   = (int)w;
    out->h   = (int)h;
    out->pix = dst;

    return IMG_OK;
}
