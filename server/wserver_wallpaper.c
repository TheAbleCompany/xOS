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
 * Файлы читаются сисколлами open/read/lseek -- сервер не имеет
 * прямого доступа к fs_node_t ядра.
 */

#include "wserver_internal.h"

/*
 * BMP-загрузчик читает файлы сисколлами open/read/lseek (как
 * обычное приложение); буферы выделяются SHM_ALLOC.
 * Обвязки open/read/close живут в wserver.c (u_open/u_read/u_close).
 */

#ifndef O_RDONLY
#define O_RDONLY 0x0000
#endif

#ifdef HOST_HARNESS
#include <stdlib.h>   /* host malloc/free for the wallpaper cache */
#endif

static void *wsh_malloc(unsigned long size)
#ifdef HOST_HARNESS
/* Host harness: no kernel heap -- plain host malloc for the
 * wallpaper cache and photo buffers. */
{ return malloc(size); }
#else
{
    volatile unsigned long ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)183), "D"(size)
        : "memory");
    return (void *)(uintptr_t)ret;
}
#endif /* HOST_HARNESS */

static void wsh_free(void *p)
#ifdef HOST_HARNESS
{ free(p); }
#else
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)184), "D"((long)(uintptr_t)p)
        : "memory");
    (void)ret;
}
#endif /* HOST_HARNESS */

#define malloc wsh_malloc
#define free   wsh_free

/* lseek(8) -- обёртки нет в unistd.h, добавляем локально. */
static int wsh_lseek(int fd, int offset, int whence)
#ifdef HOST_HARNESS
{ (void)fd; (void)offset; (void)whence; return -1; }
#else
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)8), "D"((long)fd), "S"((long)offset), "d"((long)whence)
        : "memory");
    return ret;
}
#endif /* HOST_HARNESS */

/* Чтение ровно n байт по смещению off (0 = EOF/ошибка). */
static u32int wsh_pread(int fd, u32int off, u32int size, u8int *buf)
{
    if (wsh_lseek(fd, (int)off, 0) < 0)
        return 0;
    return (u32int)u_read(fd, buf, size);
}

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
/* Lion edition boots into the Andromeda-style space wallpaper. */
int wallpaper_mode  = AX_WALL_SPACE;

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
    int fd = u_open((void *)name, O_RDONLY, 0);

    if (fd < 0) {
        serial_puts_ax("WSERVER BMP: file not found: ");
        serial_puts_ax(name);
        serial_putc_ax('\n');
        return false;
    }

    u8int hdrbuf[54];

    if (wsh_pread(fd, 0, 54, hdrbuf) < 54) {
        u_close(fd);
        serial_puts_ax("WSERVER BMP: header read failed\n");
        return false;
    }

    if (hdrbuf[0] != 'B' || hdrbuf[1] != 'M') {
        u_close(fd);
        serial_puts_ax("WSERVER BMP: invalid signature\n");
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
        u_close(fd);
        serial_puts_ax("WSERVER BMP: invalid dimensions\n");
        return false;
    }

    if (bpp != 24 && bpp != 32) {
        u_close(fd);
        serial_puts_ax("WSERVER BMP: unsupported BPP\n");
        return false;
    }

    if (compression != 0) {
        u_close(fd);
        serial_puts_ax("WSERVER BMP: compressed BMP unsupported\n");
        return false;
    }

    int abs_h = (height < 0) ? -height : height;

    /* Sanity limit so a stray huge BMP can't OOM the server heap */
    if (width > 1920 || abs_h > 1080) {
        u_close(fd);
        serial_puts_ax("WSERVER BMP: image too large\n");
        return false;
    }

    uint32_t pixel_count = (uint32_t)width * (uint32_t)abs_h;

    uint32_t *buf = (uint32_t*)malloc(pixel_count * sizeof(uint32_t));

    if (!buf) {
        u_close(fd);
        serial_puts_ax("WSERVER BMP: malloc failed\n");
        return false;
    }

    int bytes_per_pixel = bpp / 8;

    uint32_t row_bytes = (uint32_t)width * bytes_per_pixel;
    uint32_t padded   = (row_bytes + 3) & ~3;

    /* One-row scratch buffer (e.g. 3840 bytes for 1280x720x24) */
    u8int *rowbuf = (u8int*)malloc(padded);

    if (!rowbuf) {
        free(buf);
        u_close(fd);
        serial_puts_ax("WSERVER BMP: row malloc failed\n");
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

        uint32_t got = wsh_pread(fd, file_offset, padded, rowbuf);

        if (got < row_bytes) {
            serial_puts_ax("WSERVER BMP: row read failed\n");
            free(rowbuf);
            free(buf);
            u_close(fd);
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
    u_close(fd);

    out->pixels = buf;
    out->w = width;
    out->h = abs_h;
    out->valid = true;

    serial_puts_ax("WSERVER BMP loaded: ");
    serial_puts_ax(name);
    serial_puts_ax("\n");

    return true;
}

void scan_photos(void)
{
    /* Имена фиксированные: wp1.bmp .. wp8.bmp в initrd. */
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
 * Lion "Andromeda" wallpaper (procedural deep space)
 * ============================================================
 *
 * A homage to the Mac OS X Lion default wallpaper: a purple-blue
 * galaxy field with soft nebula glows and a scattering of stars
 * concentrated along a diagonal milky band.  Everything is drawn
 * from a deterministic LCG star table, so no asset files are
 * needed and the picture is stable from boot to boot.
 *
 * The wallpaper is RENDERED ONCE into a cache buffer and then
 * blitted to the backbuffer every frame (a plain row copy, much
 * cheaper than repainting gradients + nebula per frame).  If the
 * cache allocation fails the code falls back to a simple gradient.
 */

#define SPACE_STARS 360

static uint16_t  star_tab[SPACE_STARS];  /* x10 | y10 packed   */
static bool      stars_ready = false;
static uint32_t *space_cache = 0;
static int       space_cache_w = 0, space_cache_h = 0;

static void space_init_stars(void)
{
    uint32_t s = 0xC0FFEE;              /* fixed seed */
    for (int i = 0; i < SPACE_STARS; i++) {
        s = s * 1103515245u + 12345u;
        int x = (int)((s >> 8) & 0x3FF);
        s = s * 1103515245u + 12345u;
        int y = (int)((s >> 8) & 0x3FF);
        s = s * 1103515245u + 12345u;
        int kind = (int)((s >> 16) & 3);   /* 0..3 */

        /*
         * Half of the stars are pulled toward the diagonal
         * "milky" band running from the lower-left to the
         * upper-right (like the Andromeda galaxy plane).
         */
        if (kind & 1) {
            int band = (x * scr_h) / scr_w;        /* y of the band  */
            s = s * 1103515245u + 12345u;
            int spread = (int)((s >> 10) % 160) - 80;
            y = band * 1024 / (scr_h > 0 ? scr_h : 1) + spread;
            if (y < 0) y = -y;
            if (y > 1023) y = 1023;
        }

        star_tab[i] = (uint16_t)((x & 0x3FF) | ((y & 0x3FF) << 10));
    }
    stars_ready = true;
}

static inline void space_px(uint32_t *buf, int x, int y, uint32_t argb)
{
    if (x < 0 || y < 0 || x >= space_cache_w || y >= space_cache_h) return;
    uint32_t *p = &buf[(size_t)y * (size_t)space_cache_w + (size_t)x];
    *p = gfx_blend(*p, argb);
}

/* soft translucent radial glow (nebula) into the cache buffer */
static void space_glow(uint32_t *buf, int cx, int cy, int r, uint32_t argb)
{
    int r2 = r * r;
    int x0 = cx - r, x1 = cx + r;
    int y0 = cy - r, y1 = cy + r;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > space_cache_w) x1 = space_cache_w;
    if (y1 > space_cache_h) y1 = space_cache_h;

    uint32_t base_a = (argb >> 24) & 0xFF;

    for (int y = y0; y < y1; y++) {
        int dy = y - cy;
        uint32_t *row = &buf[(size_t)y * (size_t)space_cache_w];
        for (int x = x0; x < x1; x++) {
            int dx = x - cx;
            int d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;
            int a = (int)base_a * (r2 - d2) / r2;
            row[x] = gfx_blend(row[x], ((uint32_t)a << 24) | (argb & 0x00FFFFFF));
        }
    }
}

static void space_build_cache(void)
{
    uint32_t *buf = space_cache;
    int w = space_cache_w, h = space_cache_h;

    /* base: three-band deep-space gradient near-black->violet->indigo */
    int h3 = h / 3;
    for (int y = 0; y < h; y++) {
        uint32_t top, bot;
        int t;
        if (y < h3)              { top = RGB(6,6,20);   bot = RGB(28,17,58);  t = y * 256 / (h3 > 1 ? h3 - 1 : 1); }
        else if (y < h3 * 2)     { top = RGB(28,17,58); bot = RGB(46,26,88);  t = (y - h3) * 256 / (h3 > 1 ? h3 - 1 : 1); }
        else                     { top = RGB(46,26,88); bot = RGB(10,10,34);  t = (y - h3*2) * 256 / ((h - h3*2) > 1 ? (h - h3*2) - 1 : 1); }
        if (t < 0) t = 0;
        if (t > 256) t = 256;
        uint32_t c = gfx_lerp_color(top, bot, t);
        uint32_t *row = &buf[(size_t)y * (size_t)w];
        for (int x = 0; x < w; x++) row[x] = c;
    }

    /* nebula glows around the "galaxy" core */
    int nx = (w * 63) / 100, ny = (h * 36) / 100;
    space_glow(buf, nx,                    ny,                  h * 46 / 100, ARGB(0x34,110,60,230));
    space_glow(buf, nx + w / 14,           ny + h / 18,         h * 30 / 100, ARGB(0x2C,220,80,190));
    space_glow(buf, nx - w / 9,            ny + h / 14,         h * 26 / 100, ARGB(0x26,60,110,240));
    space_glow(buf, w * 22 / 100,          h * 76 / 100,        h * 30 / 100, ARGB(0x20,40,60,150));
    space_glow(buf, nx + w / 22,           ny - h / 22,         h * 16 / 100, ARGB(0x30,200,220,255));

    /* stars */
    for (int i = 0; i < SPACE_STARS; i++) {
        int x = (star_tab[i] & 0x3FF) * w / 1024;
        int y = ((star_tab[i] >> 10) & 0x3FF) * h / 1024;
        int kind = i & 3;

        if (kind == 0) {
            /* bright star + tiny cross shine */
            space_px(buf, x,     y,     ARGB(0xE0,255,255,255));
            space_px(buf, x + 1, y,     ARGB(0xA0,230,240,255));
            space_px(buf, x,     y - 1, ARGB(0x70,255,255,255));
            space_px(buf, x,     y + 1, ARGB(0x70,255,255,255));
            space_px(buf, x - 1, y,     ARGB(0x60,220,230,255));
        } else if (kind == 1) {
            space_px(buf, x, y, ARGB(0x90,190,205,255));
        } else if (kind == 2) {
            space_px(buf, x, y, ARGB(0x78,255,230,200));
        } else {
            space_px(buf, x, y, ARGB(0x70,170,175,195));
        }
    }

    /* subtle horizon glow at the bottom */
    for (int y = h * 86 / 100; y < h; y++) {
        int t = (y - h * 86 / 100) * 256 / ((h - h * 86 / 100) > 0 ? (h - h * 86 / 100) : 1);
        int a = 0x30 * t / 256;
        uint32_t *row = &buf[(size_t)y * (size_t)w];
        for (int x = 0; x < w; x++)
            row[x] = gfx_blend(row[x], ((uint32_t)a << 24) | 0x00283296);
    }
}

static void draw_wall_space(void)
{
    if (!stars_ready) space_init_stars();

    /* (re)build the cache when missing or resized */
    if (!space_cache || space_cache_w != scr_w || space_cache_h != scr_h) {
        if (space_cache) {
            free(space_cache);
            space_cache = 0;
        }
        space_cache = (uint32_t*)malloc((size_t)scr_w * (size_t)scr_h * 4);
        if (!space_cache) {
            /* out of memory: cheap fallback gradient, try again later */
            space_cache_w = 0;
            gfx_vgradient(0, 0, scr_w, scr_h, RGB(10,10,34), RGB(46,26,88));
            return;
        }
        space_cache_w = scr_w;
        space_cache_h = scr_h;
        space_build_cache();
    }

    /* blit the cached wallpaper (opaque -> straight row copy) */
    uint32_t *bb = gfx_backbuffer();
    for (int y = 0; y < scr_h; y++) {
        const uint32_t *src = &space_cache[(size_t)y * (size_t)scr_w];
        uint32_t *dst = &bb[(size_t)y * (size_t)scr_w];
        for (int x = 0; x < scr_w; x++) dst[x] = src[x];
    }
}

/*
 * ============================================================
 * Rendering + slideshow tick
 * ============================================================
 */

void draw_wallpaper(void)
{
    if (wallpaper_mode == AX_WALL_SPACE) {
        draw_wall_space();
        return;
    }

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
