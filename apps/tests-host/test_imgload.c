/*
 * test_imgload.c -- host unit test for the userspace image loader.
 *
 * Decodes the game assets and a reference BMP, then compares against
 * ARGB reference dumps produced by PIL (scripts/make_ref.py):
 *
 *   reference file format: binary, 8 bytes header (int32 w, int32 h)
 *   followed by w*h uint32 ARGB samples.
 *
 * Usage: test_imgload <image.png> <reference.bin> <tolerance>
 */

#include <stdio.h>
#include <stdlib.h>

#include "../imgload/imgload.h"

static int read_i32(FILE *fp, int *v)
{
    unsigned char b[4];

    if (fread(b, 1, 4, fp) != 4)
        return 0;

    *v = (int)((unsigned)b[0] | ((unsigned)b[1] << 8) |
               ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24));

    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s image reference.bin tolerance\n",
                argv[0]);
        return 2;
    }

    /* query first */
    int qw = 0, qh = 0;
    int qr = img_query_file(argv[1], &qw, &qh);

    if (qr != IMG_OK) {
        printf("FAIL %s: query error %d (%s)\n",
               argv[1], qr, img_strerror(qr));
        return 1;
    }

    /* load reference */
    FILE *rf = fopen(argv[2], "rb");

    if (!rf) {
        printf("FAIL %s: no reference %s\n", argv[1], argv[2]);
        return 1;
    }

    int rw = 0, rh = 0;

    read_i32(rf, &rw);
    read_i32(rf, &rh);

    unsigned int *ref =
        (unsigned int *)malloc((size_t)rw * rh * 4);

    if (fread(ref, 4, (size_t)rw * rh, rf) != (size_t)rw * rh) {
        printf("FAIL %s: short reference\n", argv[1]);
        return 1;
    }

    fclose(rf);

    if (qw != rw || qh != rh) {
        printf("FAIL %s: dims %dx%d != ref %dx%d\n",
               argv[1], qw, qh, rw, rh);
        return 1;
    }

    /* decode */
    unsigned int *px = (unsigned int *)malloc((size_t)rw * rh * 4);
    img_image_t   img;

    int rc = img_load_file(argv[1], px, rw * rh, &img);

    if (rc != IMG_OK) {
        printf("FAIL %s: decode error %d (%s)\n",
               argv[1], rc, img_strerror(rc));
        return 1;
    }

    /* compare */
    double tol = atof(argv[3]);
    double sum_diff = 0.0;
    long   alpha_mismatch = 0;
    long   worst = 0;

    for (long i = 0; i < (long)rw * rh; i++) {
        int dr = (int)((px[i] >> 16) & 0xFF) - (int)((ref[i] >> 16) & 0xFF);
        int dg = (int)((px[i] >>  8) & 0xFF) - (int)((ref[i] >>  8) & 0xFF);
        int db = (int)(px[i] & 0xFF) - (int)(ref[i] & 0xFF);
        int da = (int)((px[i] >> 24) & 0xFF) - (int)((ref[i] >> 24) & 0xFF);

        long ad = (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) +
                  (db < 0 ? -db : db);

        sum_diff += ad;

        if ((long)ad > worst)
            worst = ad;

        if (da != 0)
            alpha_mismatch++;
    }

    double mean = sum_diff / ((long)rw * rh * 3.0);

    int ok = (mean <= tol) && (alpha_mismatch == 0);

    printf("%s %s: %dx%d mean_diff=%.4f worst_px=%ld alpha_mismatch=%ld\n",
           ok ? "PASS" : "FAIL", argv[1], img.w, img.h,
           mean, worst, alpha_mismatch);

    return ok ? 0 : 1;
}
