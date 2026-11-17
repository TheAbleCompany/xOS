/*
 * ============================================================
 * inflate.c -- RFC 1951 DEFLATE decompressor for ArtyomX64
 * ============================================================
 *
 * Freestanding, allocation-free implementation used by the
 * userspace PNG decoder (img_png.c).  Handles:
 *
 *   * stored blocks        (type 00)
 *   * fixed Huffman        (type 01)
 *   * dynamic Huffman      (type 10) -- code length codes up to 19,
 *                            literal/length + distance tables
 *   * zlib wrapper (RFC 1950): CMF/FLG header check, Adler-32
 *     verification when the whole stream is consumed
 *
 * The caller supplies the output buffer; running out of space is
 * reported as IMG_ERR_TOO_BIG instead of overrunning.
 *
 * Reference: RFC 1950 / RFC 1951 (the classic Mark Adler "puff"
 * structure, rewritten from scratch for this OS).
 * ============================================================
 */

#include "imgload.h"

/* ------------------------------------------------------------------
 * Bit reader (LSB-first, as DEFLATE requires)
 * ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *data;
    long           len;      /* bytes available            */
    long           pos;      /* next byte index            */
    unsigned       bitbuf;   /* pending bits (LSB first)   */
    int            bitcnt;   /* number of bits in bitbuf   */
} img_bitreader_t;

static int br_bits(img_bitreader_t *br, int need)
{
    unsigned val = (unsigned)br->bitbuf;

    while (br->bitcnt < need) {
        if (br->pos >= br->len)
            return -1;                      /* out of input */

        val |= (unsigned)br->data[br->pos++] << br->bitcnt;
        br->bitcnt += 8;
    }

    br->bitbuf   = val >> need;
    br->bitcnt  -= need;

    return (int)(val & ((1u << need) - 1));
}

static void br_align_to_byte(img_bitreader_t *br)
{
    br->bitbuf = 0;
    br->bitcnt = 0;
}

/* ------------------------------------------------------------------
 * Huffman decoding
 * ------------------------------------------------------------------ */

typedef struct {
    const unsigned *counts;   /* symbols per code length       */
    const unsigned *symbols;  /* symbols ordered by (len,sym)  */
} img_huff_t;

static int huff_decode(img_bitreader_t *br, const img_huff_t *h)
{
    int code = 0, first = 0, index = 0;

    for (int len = 1; len <= 15; len++) {
        int b = br_bits(br, 1);

        if (b < 0)
            return IMG_ERR_CORRUPT;

        code |= b;
        int count = (int)h->counts[len];

        if (code - count < first)
            return h->symbols[index + (code - first)];

        index += count;
        first  = (first + count) << 1;
        code <<= 1;
    }

    return IMG_ERR_CORRUPT;                 /* ran out of codes */
}

static int huff_build(unsigned *counts_total,
                      unsigned char lens[], int n,
                      unsigned *counts_out,
                      unsigned *symbols_out)
{
    /* counts_total: scratch of at least 16, lens: code lengths,
     * counts_out: 16, symbols_out: n.  Canonical code assignment.  */

    for (int i = 0; i < 16; i++)
        counts_out[i] = 0;

    for (int i = 0; i < n; i++)
        counts_out[lens[i]]++;

    if (counts_out[0] == (unsigned)n)       /* no codes at all */
        return 0;                           /* legal (empty tree) */

    /* over-subscribed check */
    int left = 1;

    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left  -= (int)counts_out[len];

        if (left < 0)
            return IMG_ERR_CORRUPT;
    }

    /* offsets per length */
    unsigned offs[16];

    offs[1] = 0;

    for (int len = 1; len < 15; len++)
        offs[len + 1] = offs[len] + counts_out[len];

    for (int i = 0; i < n; i++) {
        if (lens[i])
            symbols_out[offs[lens[i]]++] = (unsigned)i;
    }

    (void)counts_total;

    return 1;
}

/* ------------------------------------------------------------------
 * Length / distance tables (RFC 1951 section 3.2.5)
 * ------------------------------------------------------------------ */

static const unsigned len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const unsigned dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* Fixed (predefined) Huffman tables -- built once, lazily. */
static unsigned  fix_lcount[16],  fix_lsym[288];
static unsigned  fix_dcount[16],  fix_dsym[30];
static int       fix_ready = 0;

static void build_fixed(void)
{
    unsigned char lens[288];

    for (int i = 0; i < 144; i++) lens[i] = 8;
    for (int i = 144; i < 256; i++) lens[i] = 9;
    for (int i = 256; i < 280; i++) lens[i] = 7;
    for (int i = 280; i < 288; i++) lens[i] = 8;

    huff_build(0, lens, 288, fix_lcount, fix_lsym);

    for (int i = 0; i < 30; i++)
        lens[i] = 5;

    /* all 30 distances get a 5-bit code; counts filled accordingly */
    for (int i = 0; i < 16; i++) fix_dcount[i] = 0;

    fix_dcount[5] = 30;

    for (int i = 0; i < 30; i++)
        fix_dsym[i] = (unsigned)i;

    fix_ready = 1;
}

/* ------------------------------------------------------------------
 * Stored / fixed / dynamic block decoding
 * ------------------------------------------------------------------ */

static long inflate_codes(img_bitreader_t *br,
                          const img_huff_t *lc,
                          const img_huff_t *dc,
                          uint8_t *dst, long dst_len, long *out_used)
{
    for (;;) {
        int sym = huff_decode(br, lc);

        if (sym < 0)
            return sym;

        if (sym < 256) {
            if (*out_used >= dst_len)
                return IMG_ERR_TOO_BIG;

            dst[(*out_used)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return IMG_OK;                  /* end of block */
        } else {
            sym -= 257;

            if (sym >= 29)
                return IMG_ERR_CORRUPT;

            int eb = (int)len_extra[sym];
            int lb = br_bits(br, eb);

            if (lb < 0)
                return IMG_ERR_CORRUPT;

            long length = (long)len_base[sym] + lb;

            int dsym = huff_decode(br, dc);

            if (dsym < 0 || dsym >= 30)
                return IMG_ERR_CORRUPT;

            eb = (int)dist_extra[dsym];
            int db = br_bits(br, eb);

            if (db < 0)
                return IMG_ERR_CORRUPT;

            long dist = (long)dist_base[dsym] + db;

            if (dist > *out_used)
                return IMG_ERR_CORRUPT;     /* reference before start */

            if (*out_used + length > dst_len)
                return IMG_ERR_TOO_BIG;

            long from = *out_used - dist;

            for (long i = 0; i < length; i++)
                dst[*out_used + i] = dst[from + i];

            *out_used += length;
        }
    }
}

static int read_dynamic_tables(img_bitreader_t *br,
                               img_huff_t *lc, img_huff_t *dc,
                               unsigned *lcount, unsigned *lsym,
                               unsigned *dcount, unsigned *dsym)
{
    static const unsigned short order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
        11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    unsigned char lens[288 + 32];
    unsigned      clcount[16], clsym[19];
    unsigned char clens[19];

    int nlen  = br_bits(br, 5) + 257;       /* HLIT */
    int ndist = br_bits(br, 5) + 1;         /* HDIST */
    int ncode = br_bits(br, 4) + 4;         /* HCLEN */

    if (nlen > 286 || ndist > 30)
        return IMG_ERR_CORRUPT;

    for (int i = 0; i < ncode; i++)
        clens[order[i]] = (unsigned char)br_bits(br, 3);

    for (int i = ncode; i < 19; i++)
        clens[order[i]] = 0;

    int r = huff_build(0, clens, 19, clcount, clsym);

    if (r < 0)
        return r;

    /* read nlen + ndist code lengths using the code-length alphabet */
    int n = nlen + ndist, i = 0;

    while (i < n) {
        int sym = huff_decode(br, &(img_huff_t){ clcount, clsym });

        if (sym < 0)
            return sym;

        if (sym < 16) {
            lens[i++] = (unsigned char)sym;
        } else {
            int len = 0, rep;

            if (sym == 16) {
                if (i == 0)
                    return IMG_ERR_CORRUPT;

                len = lens[i - 1];
                rep = br_bits(br, 2) + 3;
            } else if (sym == 17) {
                rep = br_bits(br, 3) + 3;
            } else {
                rep = br_bits(br, 7) + 11;
            }

            if (i + rep > n)
                return IMG_ERR_CORRUPT;

            while (rep--)
                lens[i++] = (unsigned char)len;
        }
    }

    /* distance table may legitimately be empty (single invalid code) */
    if (lens[nlen] == 0) {
        int all_zero = 1;

        for (int k = nlen; k < n; k++)
            if (lens[k]) { all_zero = 0; break; }

        if (!all_zero)
            return IMG_ERR_CORRUPT;
    }

    r = huff_build(0, lens, nlen, lcount, lsym);

    if (r < 0)
        return r;

    r = huff_build(0, lens + nlen, ndist, dcount, dsym);

    if (r < 0)
        return r;

    lc->counts = lcount;
    lc->symbols = lsym;
    dc->counts = dcount;
    dc->symbols = dsym;

    return IMG_OK;
}

long img_inflate(const uint8_t *src, long src_len,
                 uint8_t *dst, long dst_len)
{
    if (!src || src_len < 2)
        return IMG_ERR_CORRUPT;

    /* zlib wrapper (RFC 1950) */
    unsigned cmf = src[0], flg = src[1];

    if ((cmf & 0x0F) != 8)                  /* only deflate */
        return IMG_ERR_CORRUPT;

    if (((cmf << 8) | flg) % 31 != 0)
        return IMG_ERR_CORRUPT;

    if (flg & 0x20)                         /* FDICT: preset dict */
        return IMG_ERR_UNSUPPORTED;

    img_bitreader_t br;

    br.data   = src + 2;
    br.len    = src_len - 2;
    br.pos    = 0;
    br.bitbuf = 0;
    br.bitcnt = 0;

    long used = 0;
    int  last = 0;

    unsigned lcount[16], lsym[288];
    unsigned dcount[16], dsym[30];

    do {
        int bfinal = br_bits(&br, 1);
        int btype  = br_bits(&br, 2);

        if (bfinal < 0 || btype < 0)
            return IMG_ERR_CORRUPT;

        last = bfinal;
        long r;

        switch (btype) {
            case 0: {
                /* stored: skip to byte boundary, LEN/NLEN, copy */
                br_align_to_byte(&br);

                if (br.pos + 4 > br.len)
                    return IMG_ERR_CORRUPT;

                unsigned len  = br.data[br.pos] |
                               (br.data[br.pos + 1] << 8);
                unsigned nlen = br.data[br.pos + 2] |
                               (br.data[br.pos + 3] << 8);

                br.pos += 4;

                if ((len ^ 0xFFFF) != nlen)
                    return IMG_ERR_CORRUPT;

                if (br.pos + (long)len > br.len)
                    return IMG_ERR_CORRUPT;

                if (used + (long)len > dst_len)
                    return IMG_ERR_TOO_BIG;

                img_memcpy(dst + used, br.data + br.pos, len);
                used    += len;
                br.pos  += len;
                r = IMG_OK;

                break;
            }

            case 1: {
                if (!fix_ready)
                    build_fixed();

                img_huff_t lc = { fix_lcount, fix_lsym };
                img_huff_t dc = { fix_dcount, fix_dsym };

                r = inflate_codes(&br, &lc, &dc,
                                  dst, dst_len, &used);

                break;
            }

            case 2: {
                img_huff_t lc, dc;

                int tr = read_dynamic_tables(&br, &lc, &dc,
                                             lcount, lsym,
                                             dcount, dsym);

                if (tr != IMG_OK)
                    return tr;

                r = inflate_codes(&br, &lc, &dc,
                                  dst, dst_len, &used);

                break;
            }

            default:
                return IMG_ERR_CORRUPT;     /* type 11 reserved */
        }

        if (r != IMG_OK)
            return r;

    } while (!last);

    /* Adler-32 check (best effort): the checksum bytes may not be
     * in this buffer if IDAT was truncated, so warn-only would be
     * wrong; PNG feeds whole concatenated IDAT, so bytes are here.
     * Verify strictly when available.                               */
    br_align_to_byte(&br);

    long tail = br.len - br.pos;

    if (tail >= 4) {
        unsigned a = 1, b = 0;
        const unsigned NMAX = 5552;

        long i = 0;

        while (i < used) {
            long chunk = used - i;

            if (chunk > (long)NMAX)
                chunk = (long)NMAX;

            for (long k = 0; k < chunk; k++) {
                a += dst[i + k];
                b += a;
            }

            a %= 65521;
            b %= 65521;
            i += chunk;
        }

        unsigned want = ((unsigned)src[src_len - 4] << 24) |
                        ((unsigned)src[src_len - 3] << 16) |
                        ((unsigned)src[src_len - 2] <<  8) |
                         (unsigned)src[src_len - 1];
        unsigned got = ((b & 0xFFFF) << 16) | (a & 0xFFFF);

        if (want != got)
            return IMG_ERR_CORRUPT;
    }

    return used;
}
