/*
 * ============================================================
 * axshell_wallpaper.c
 * ============================================================
 *
 * Wallpaper engine: gradient presets, photo (BMP) loader, and
 * the slideshow tick.  Owns the wallpaper + accent state and
 * the photo table that the rest of the shell reads through the
 * axshell_internal.h externs.
 *
 * The BMP loader is a streaming single-row decoder: it only
 * keeps one padded scanline in memory at a time, so a 1920x1080
 * wallpaper costs ~4 KB of scratch + the final ARGB buffer.
 */

#include "axshell_internal.h"

#include "../fs/fs.h"
#include "../mm/kheap.h"

#define malloc kmalloc
#define free   kfree

/*
 * ============================================================
 * Module state
 * ============================================================
 */

uint32_t accent = RGB(10,132,255);

static const uint32_t wallpapers_top[] = {
    RGB(34,40,82), RGB(120,40,90), RGB(20,80,80), RGB(60,30,70), RGB(15,15,20)
};
static const uint32_t wallpapers_bot[] = {
    RGB(90,120,200), RGB(230,120,90), RGB(60,180,160), RGB(150,80,170), RGB(50,50,70)
};

int wallpaper_index = 0;
int wallpaper_mode  = AX_WALL_GRADIENT;

ax_photo_t photos[MAX_PHOTOS];
int photo_count = 0;
int current_photo = 0;
int slideshow_counter = 0;
int wallpaper_fade = 256;
int wallpaper_fade_dir = 0;
int prev_wallpaper_index = 0;
int prev_photo_index = 0;

/*
 * ============================================================
 * Public setters (axshell.h API)
 * ============================================================
 */

void ax_set_wallpaper(int index)
{
    if (index < 0) index = 0;
    if (index >= WALLPAPER_COUNT) index = WALLPAPER_COUNT - 1;
    if (index != wallpaper_index) {
        prev_wallpaper_index = wallpaper_index;
        wallpaper_index = index;
        wallpaper_fade = 0;
        wallpaper_fade_dir = 1;
    }
}

int ax_get_wallpaper(void) { return wallpaper_index; }

void ax_set_accent(uint32_t color) { accent = color; }

/*
 * ============================================================
 * BMP loader (initrd-backed, single-row streaming)
 * ============================================================
 */

static bool load_bmp_from_fs(const char *name, ax_photo_t *out)
{
    fs_node_t *node = finddir_fs(fs_root, (char*)name);

    if (!node) {
        serial_puts_ax("BMP: file not found: ");
        serial_puts_ax(name);
        serial_putc_ax('\n');
        return false;
    }

    u8int hdrbuf[54];

    u32int sz = read_fs(node, 0, 54, hdrbuf);

    if (sz < 54) {
        serial_puts_ax("BMP: header read failed\n");
        return false;
    }

    if (hdrbuf[0] != 'B' || hdrbuf[1] != 'M') {
        serial_puts_ax("BMP: invalid signature\n");
        return false;
    }

    uint32_t data_offset =
        ((uint32_t)hdrbuf[10]) |
        ((uint32_t)hdrbuf[11] << 8) |
        ((uint32_t)hdrbuf[12] << 16) |
        ((uint32_t)hdrbuf[13] << 24);

    int32_t width =
        ((int32_t)hdrbuf[18]) |
        ((int32_t)hdrbuf[19] << 8) |
        ((int32_t)hdrbuf[20] << 16) |
        ((int32_t)hdrbuf[21] << 24);

    int32_t height =
        ((int32_t)hdrbuf[22]) |
        ((int32_t)hdrbuf[23] << 8) |
        ((int32_t)hdrbuf[24] << 16) |
        ((int32_t)hdrbuf[25] << 24);

    uint16_t bpp =
        ((uint16_t)hdrbuf[28]) |
        ((uint16_t)hdrbuf[29] << 8);

    uint32_t compression =
        ((uint32_t)hdrbuf[30]) |
        ((uint32_t)hdrbuf[31] << 8) |
        ((uint32_t)hdrbuf[32] << 16) |
        ((uint32_t)hdrbuf[33] << 24);

    if (width <= 0 || height == 0) {
        serial_puts_ax("BMP: invalid dimensions\n");
        return false;
    }

    if (bpp != 24 && bpp != 32) {
        serial_puts_ax("BMP: unsupported BPP\n");
        return false;
    }

    if (compression != 0) {
        serial_puts_ax("BMP: compressed BMP unsupported\n");
        return false;
    }

    int abs_h = (height < 0) ? -height : height;

    /* Sanity limit so a stray huge BMP can't OOM the kernel heap */
    if (width > 1920 || abs_h > 1080) {
        serial_puts_ax("BMP: image too large\n");
        return false;
    }

    uint32_t pixel_count = (uint32_t)width * (uint32_t)abs_h;

    uint32_t *buf = (uint32_t*)malloc(pixel_count * sizeof(uint32_t));

    if (!buf) {
        serial_puts_ax("BMP: malloc failed\n");
        return false;
    }

    int bytes_per_pixel = bpp / 8;

    uint32_t row_bytes = (uint32_t)width * bytes_per_pixel;
    uint32_t padded   = (row_bytes + 3) & ~3;

    /* One-row scratch buffer (e.g. 3840 bytes for 1280x720x24) */
    u8int *rowbuf = (u8int*)malloc(padded);

    if (!rowbuf) {
        free(buf);
        serial_puts_ax("BMP: row malloc failed\n");
        return false;
    }

    for (int y = 0; y < abs_h; y++) {

        int src_y;

        /* BMP is bottom-up by default; negative height = top-down */
        if (height < 0)
            src_y = y;
        else
            src_y = abs_h - 1 - y;

        uint32_t file_offset = data_offset + (uint32_t)src_y * padded;

        uint32_t got = read_fs(node, file_offset, padded, rowbuf);

        if (got < row_bytes) {
            serial_puts_ax("BMP: row read failed\n");
            free(rowbuf);
            free(buf);
            return false;
        }

        for (int x = 0; x < width; x++) {
            u8int *pixel = rowbuf + x * bytes_per_pixel;
            u8int b = pixel[0];
            u8int g = pixel[1];
            u8int r = pixel[2];
            buf[y * width + x] = ARGB(0xFF, r, g, b);
        }
    }

    free(rowbuf);

    out->pixels = buf;
    out->w = width;
    out->h = abs_h;
    out->valid = true;

    serial_puts_ax("BMP loaded: ");
    serial_puts_ax(name);
    serial_puts_ax("\n");

    return true;
}

void scan_photos(void)
{
    static const char *names[] = {
        "wp1.bmp", "wp2.bmp", "wp3.bmp", "wp4.bmp",
        "wp5.bmp", "wp6.bmp", "wp7.bmp", "wp8.bmp"
    };
    photo_count = 0;
    for (int i = 0; i < MAX_PHOTOS; i++) {
        if (load_bmp_from_fs(names[i], &photos[photo_count]))
            photo_count++;
        if (photo_count >= MAX_PHOTOS) break;
    }
}

/*
 * ============================================================
 * Rendering + slideshow tick
 * ============================================================
 */

void draw_wallpaper(void)
{
    if (wallpaper_mode == AX_WALL_GRADIENT || photo_count == 0) {
        gfx_vgradient(0, 0, scr_w, scr_h,
                      wallpapers_top[wallpaper_index], wallpapers_bot[wallpaper_index]);
        return;
    }

    if (wallpaper_mode == AX_WALL_PHOTO || wallpaper_mode == AX_WALL_SLIDESHOW) {
        int idx = current_photo;
        if (idx < 0 || idx >= photo_count || !photos[idx].valid) {
            gfx_vgradient(0, 0, scr_w, scr_h,
                          wallpapers_top[wallpaper_index], wallpapers_bot[wallpaper_index]);
            return;
        }
        gfx_blit_argb_scaled(photos[idx].pixels, photos[idx].w, photos[idx].h,
                              0, 0, scr_w, scr_h);
        if (wallpaper_fade < 256 && wallpaper_fade_dir == 1) {
            uint32_t fade_a = (uint32_t)(256 - wallpaper_fade) * 0xFF / 256;
            gfx_fill_rect_alpha(0, 0, scr_w, scr_h, (fade_a << 24));
        }
    }
}

void wallpaper_tick(void)
{
    if (wallpaper_fade_dir == 1) {
        wallpaper_fade += 16;
        if (wallpaper_fade >= 256) { wallpaper_fade = 256; wallpaper_fade_dir = 0; }
    }
    if (wallpaper_mode == AX_WALL_SLIDESHOW && photo_count > 1) {
        slideshow_counter++;
        if (slideshow_counter >= SLIDESHOW_INTERVAL) {
            slideshow_counter = 0;
            prev_photo_index = current_photo;
            current_photo = (current_photo + 1) % photo_count;
            wallpaper_fade = 0;
            wallpaper_fade_dir = 1;
        }
    }
}
