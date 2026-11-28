/*
 * ============================================================
 * wserver_gfx.c -- 2D-графика WindowServer
 * ============================================================
 *
 * Это lib/gfxlib.c, портированная из ядра в userspace-процесс
 * wserver (модель NeXT/macOS: framebuffer принадлежит
 * WindowServer'у, а не ядру).
 *
 * Отличия от ядровой версии:
 *   - параметры дисплея берутся сисколлом FB_INFO (189),
 *       а не из глобалов драйверов ядра;
 *   - backbuffer/blurtmp и канвасы окон выделяются через
 *     SHM_ALLOC (183) -- блоки kernel heap в HHDM, которые
 *     видны всем адресным пространствам (на них клиенты
 *     рисуют свои канвасы);
 *   - легаси-функции draw_vga_* (шрифт vgafnt ядра) удалены --
 *     WindowServer использует только font8x8.
 */

#include "wserver_gfx.h"
#include "../lib/axport.h"
#include "../lib/common.h"
#include "../lib/axttf.h"
#include "font8x8_basic.h"
#include "font8x8_cyr.h"

/*
 * UTF-8 -> code point.  Application titles and menu entries come from
 * file names, which are UTF-8 (FAT LFN is converted from UTF-16LE),
 * so the server must decode them instead of drawing raw bytes.
 */
static unsigned int wx_utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    unsigned int cp = s[0];

    if (cp < 0x80u) { *ps = (const char *)(s + 1); return cp; }
    if ((cp & 0xE0u) == 0xC0u && s[1]) {
        cp = ((cp & 0x1Fu) << 6) | (s[1] & 0x3Fu);
        *ps = (const char *)(s + 2);
        return cp < 0x80u ? 0xFFFDu : cp;
    }
    if ((cp & 0xF0u) == 0xE0u && s[1] && s[2]) {
        cp = ((cp & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
        *ps = (const char *)(s + 3);
        return cp < 0x800u ? 0xFFFDu : cp;
    }
    if ((cp & 0xF8u) == 0xF0u && s[1] && s[2] && s[3]) {
        cp = ((cp & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) |
             ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
        *ps = (const char *)(s + 4);
        return (cp < 0x10000u || cp > 0x10FFFFu) ? 0xFFFDu : cp;
    }
    *ps = (const char *)(s + 1);
    return 0xFFFDu;
}

static const unsigned char *wx_glyph(unsigned int cp)
{
    if (cp < 0x80u) return font8x8_basic[cp];
    if (cp >= 0x0410u && cp <= 0x044Fu) return font8x8_cyr[cp - 0x0410u];
    if (cp == 0x0401u) return font8x8_cyr[64];
    if (cp == 0x0451u) return font8x8_cyr[65];
    return font8x8_basic[(unsigned char)'?'];
}

/*
 * Пользовательская обвязка над сисколлами SHM_ALLOC/SHM_FREE:
 * "куча" сервера лежит в kernel heap (HHDM), поэтому указатели
 * валидны и в адресных пространствах клиентов.
 */
#ifdef HOST_HARNESS
/* Host harness: no kernel heap -- plain host malloc. */
static void *wsh_malloc(unsigned long size) { return malloc(size); }
static void  wsh_free(void *ptr)            { free(ptr); }
#else
static void *wsh_malloc(unsigned long size)
{
    volatile unsigned long ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)183), "D"(size)
        : "memory");
    return (void *)(uintptr_t)ret;
}

static void wsh_free(void *ptr)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)184), "D"((long)(uintptr_t)ptr)
        : "memory");
    (void)ret;
}
#endif /* HOST_HARNESS */

#define malloc wsh_malloc
#define free  wsh_free
 
static uint8_t  *fb;
static uint32_t  fb_w, fb_h, fb_pitch;
 
static uint32_t *backbuffer;
static uint32_t *blurtmp;
#define LOG_W fb_w
 
int gfx_scale = 1;
static bool gfx_gpu_active = false;
static bool gfx_gpu_tried  = true;//false;
 
static const int sin_table[91] = {
       0,  17,  35,  52,  70,  87, 105, 122, 139, 156,
     174, 191, 208, 225, 242, 259, 276, 292, 309, 326,
     342, 358, 375, 391, 407, 423, 438, 454, 469, 485,
     500, 515, 530, 545, 559, 574, 588, 602, 616, 629,
     643, 656, 669, 682, 695, 707, 719, 731, 743, 755,
     766, 777, 788, 799, 809, 819, 829, 839, 848, 857,
     866, 875, 883, 891, 899, 906, 914, 921, 927, 934,
     940, 946, 951, 956, 961, 966, 970, 974, 978, 982,
     985, 988, 990, 993, 995, 996, 998, 999, 999, 1000,
    1000
};
 
int gfx_sin_deg(int deg)
{
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg <= 90)  return  sin_table[deg];
    if (deg <= 180) return  sin_table[180 - deg];
    if (deg <= 270) return -sin_table[deg - 180];
    return -sin_table[360 - deg];
}
 
int gfx_cos_deg(int deg) { return gfx_sin_deg(deg + 90); }
 
/*static void gfx_try_gpu_init(void)
{
    if (gfx_gpu_tried) return;
    gfx_gpu_tried = true;
    ixg_driver_init();
    if (ixg_driver_is_accel_ready()) {
        gfx_gpu_active = true;
    }
}*/
 
void gfx_gpu_fallback(void) { gfx_gpu_active = false; }
bool gfx_gpu_available(void) { return false;/*gfx_gpu_active;*/ }

#ifdef HOST_HARNESS
/*
 * Host-harness hook (compiled only with -DHOST_HARNESS): skip the
 * FB_INFO syscall and point the renderer at buffers provided by
 * the harness.  fb stays 0, so gfx_present() is a no-op and the
 * harness reads the backbuffer directly to write PNG screenshots.
 */
void gfx_host_init(uint32_t *buf, uint32_t *blurtmp_buf, int w, int h)
{
    fb         = 0;
    fb_w       = (uint32_t)w;
    fb_h       = (uint32_t)h;
    fb_pitch   = (uint32_t)w * 4;
    backbuffer = buf;
    blurtmp    = blurtmp_buf;
}
#endif
 
void gfx_init(void)
{
    /*
     * Параметры дисплея -- от ядра (FB_INFO, сисколл 189), не от
     * глобалов драйверов: wserver -- обычный процесс со своим
     * адресным пространством.
     */
    ax_fb_info_t fi;
    volatile int gfx_ret;
    __asm__ volatile("int $0x80"
        : "=a"(gfx_ret)
        : "a"((long)189), "D"((long)(uintptr_t)&fi)
        : "memory");

    fb       = (uint8_t*)(uintptr_t)fi.addr;
    fb_w     = fi.width;
    fb_h     = fi.height;
    fb_pitch = fi.pitch;
 
    if (fb_w > 1920) fb_w = 1920;
    if (fb_h > 1200) fb_h = 1200;
 
    backbuffer = (uint32_t*)malloc(fb_w * fb_h * sizeof(uint32_t));
    blurtmp    = (uint32_t*)malloc(fb_w * fb_h * sizeof(uint32_t));
    if (!backbuffer || !blurtmp) {
        fb_w = GUI_W;
        fb_h = GUI_H;
        backbuffer = (uint32_t*)malloc(fb_w * fb_h * sizeof(uint32_t));
        blurtmp    = (uint32_t*)malloc(fb_w * fb_h * sizeof(uint32_t));
    }
    if (!backbuffer || !blurtmp) {
        fb_w = 640;
        fb_h = 480;
        backbuffer = (uint32_t*)malloc(fb_w * fb_h * sizeof(uint32_t));
        blurtmp    = (uint32_t*)malloc(fb_w * fb_h * sizeof(uint32_t));
    }
 
    //gfx_try_gpu_init();
}
 
uint32_t gfx_width(void)  { return fb_w; }
uint32_t gfx_height(void) { return fb_h; }
uint32_t gfx_fb_width(void)  { return fb_w; }
uint32_t gfx_fb_height(void) { return fb_h; }
uint32_t *gfx_backbuffer(void) { return backbuffer; }
 
/*
 * Optimized framebuffer flush.
 *
 * The original implementation walked the framebuffer one uint32_t at
 * a time inside a nested C loop.  At 1920x1080 that is ~2 million
 * 32-bit stores per frame, plus loop overhead -- far slower than
 * the display refresh, which means the backbuffer-to-framebuffer
 * copy straddles two (or more) scan-out positions and the user
 * sees a torn / flickering image.
 *
 * This version uses a single inline "rep movsq" per scanline (plus
 * a tail "rep movsb" for the odd bytes when the row is not a
 * multiple of 8 bytes).  On the same hardware it is roughly an
 * order of magnitude faster and comfortably finishes inside the
 * vertical blanking interval, eliminating tearing caused by a
 * slow copy.
 *
 * The copy also tolerates a framebuffer pitch that is wider than
 * our logical stride (framebuffer_pitch >= fb_w * 4): we only
 * overwrite fb_w * 4 bytes per row, leaving any padding intact.
 *
 * NOTE: this routine is intentionally free of SSE/AVX because the
 * kernel is built with -mno-sse -mno-sse2 -mno-mmx (see the
 * top-level Makefile) -- saving XMM state on every task switch
 * would be wasteful.  General-purpose "rep movsq" works under
 * those constraints and still saturates the memory bus.
 */
static inline void gfx_present_row(uint8_t *dst, const uint8_t *src,
                                   uint32_t byte_len)
{
    uint64_t qwords = byte_len >> 3;        /* full 8-byte chunks */
    uint32_t tail   = byte_len & 7;         /* leftover bytes      */

    __asm__ volatile (
        "rep movsq\n\t"
        "mov %3, %2\n\t"                    /* load tail count into rcx */
        "rep movsb\n\t"
        : "+D"(dst), "+S"(src), "+c"(qwords)
        : "r"((uint64_t)tail)
        : "memory"
    );
}

void gfx_present(void)
{
    if (!backbuffer || !fb) return;

    uint32_t row_bytes = fb_w * sizeof(uint32_t);

    /*
     * Fast path: most Limine / VESA framebuffers have
     * framebuffer_pitch == width * 4 and we can blast the whole
     * backbuffer to the framebuffer in one bulk copy.  This is by
     * far the most common case in QEMU and on real hardware with
     * a linear 32-bpp mode.
     */
    if (fb_pitch == row_bytes) {
        gfx_present_row(fb, (const uint8_t *)backbuffer,
                        row_bytes * fb_h);
        return;
    }

    /*
     * Slow path: pitch != stride -- copy one scanline at a time,
     * honouring the framebuffer's stride.
     */
    for (uint32_t y = 0; y < fb_h; y++) {
        uint8_t *dst = fb + (uintptr_t)y * fb_pitch;
        const uint8_t *src =
            (const uint8_t *)&backbuffer[y * fb_w];
        gfx_present_row(dst, src, row_bytes);
    }
}
 
void gfx_pixel(int x, int y, uint32_t color)
{
    if (x < 0 || y < 0 || x >= fb_w || y >= fb_h) return;
    backbuffer[y * fb_w + x] = color;
}
 
uint32_t gfx_get_pixel(int x, int y)
{
    if (x < 0 || y < 0 || x >= fb_w || y >= fb_h) return 0;
    return backbuffer[y * fb_w + x];
}
 
void gfx_clear_rgb(uint32_t color)
{
    uint32_t n = fb_w * fb_h;
    for (uint32_t i = 0; i < n; i++) backbuffer[i] = color;
}
 
uint32_t gfx_blend(uint32_t dst, uint32_t src)
{
    uint32_t a = (src >> 24) & 0xFF;
    if (a == 0)   return dst;
    if (a == 255) return src | 0xFF000000;
 
    uint32_t ia = 255 - a;
    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >> 8)  & 0xFF;
    uint32_t sb = (src)       & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8)  & 0xFF;
    uint32_t db = (dst)       & 0xFF;
    uint32_t rr = (sr * a + dr * ia) / 255;
    uint32_t rg = (sg * a + dg * ia) / 255;
    uint32_t rb = (sb * a + db * ia) / 255;
    return 0xFF000000 | (rr << 16) | (rg << 8) | rb;
}
 
uint32_t gfx_lerp_color(uint32_t a, uint32_t b, int t)
{
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    int it = 256 - t;
    /* interpolate ALL four channels, alpha included: the
     * translucent gradient primitives (_vgradient_alpha) and
     * the dock slab rely on the alpha byte surviving the lerp
     * -- forcing it to 0xFF here made every "glass" panel
     * opaque (white shelf, white menus).  Same fix as the
     * kernel gfxlib (round 2). */
    uint32_t aa = (a >> 24) & 0xFF, ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint32_t ba = (b >> 24) & 0xFF, br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint32_t ra = (aa * it + ba * t) >> 8;
    uint32_t rr = (ar * it + br * t) >> 8;
    uint32_t rg = (ag * it + bg * t) >> 8;
    uint32_t rb = (ab * it + bb * t) >> 8;
    return (ra << 24) | (rr << 16) | (rg << 8) | rb;
}
 
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
 
    /*if (gfx_gpu_active && ixg_driver_is_accel_ready()) {
        ixg_driver_accel_rect(x, y, w, h, color);
    }*/
    for (int j = 0; j < h; j++) {
        uint32_t *row = &backbuffer[(y + j) * fb_w + x];
        for (int i = 0; i < w; i++) row[i] = color;
    }
}
 
void gfx_fill_rect_alpha(int x, int y, int w, int h, uint32_t argb)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
 
    /*if (gfx_gpu_active && ixg_driver_is_accel_ready()) {
        if (ixg_driver_accel_rect_blend(x, y, w, h, argb, 0) == IXG_BIND_OK)
            return;
    }*/
    for (int j = 0; j < h; j++) {
        uint32_t *row = &backbuffer[(y + j) * fb_w + x];
        for (int i = 0; i < w; i++) row[i] = gfx_blend(row[i], argb);
    }
}
 
static int in_rounded(int px, int py, int w, int h, int r)
{
    if (px >= r && px < w - r) return 1;
    if (py >= r && py < h - r) return 1;
    int cx, cy;
    if (px < r)      cx = r;       else cx = w - 1 - r;
    if (py < r)      cy = r;       else cy = h - 1 - r;
    int dx = px - cx;
    int dy = py - cy;
    return (dx * dx + dy * dy) <= (r * r);
}

/*
 * Покрытие пикселя (0..255) кругом радиуса r с центром (cx, cy):
 * 16 подвыборок 4x4.  Нужно для сглаживания скруглённых углов окна --
 * без него край идёт ступенькой.
 */
static int arc_cov(int i, int j, int cx, int cy, int r)
{
    int n = 0;
    int r4 = r * 4;
    for (int sy = 1; sy < 8; sy += 2)
        for (int sx = 1; sx < 8; sx += 2) {
            int dx = i * 4 + sx - cx * 4;
            int dy = j * 4 + sy - cy * 4;
            if (dx * dx + dy * dy <= r4 * r4) n++;
        }
    return n * 255 / 16;
}

/* Наложить цвет с заданным покрытием, сохранив альфу самого цвета. */
static void put_pixel_cov(int x, int y, uint32_t color, int cov)
{
    if (x < 0 || y < 0 || x >= (int)fb_w || y >= (int)fb_h) return;
    uint32_t *p = &backbuffer[y * fb_w + x];
    if (cov >= 255) { *p = gfx_blend(*p, color); return; }
    uint32_t a = ((color >> 24) & 0xFF) * (uint32_t)cov / 255;
    *p = gfx_blend(*p, (a << 24) | (color & 0x00FFFFFF));
}
 
void gfx_rounded_rect(int x, int y, int w, int h, int radius, uint32_t color)
{
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (in_rounded(i, j, w, h, radius))
                gfx_pixel(x + i, y + j, color);
}
 
void gfx_rounded_rect_alpha(int x, int y, int w, int h, int radius, uint32_t argb)
{
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (in_rounded(i, j, w, h, radius)) {
                int gx = x + i, gy = y + j;
                if (gx < 0 || gy < 0 || gx >= fb_w || gy >= fb_h) continue;
                uint32_t *p = &backbuffer[gy * LOG_W + gx];
                *p = gfx_blend(*p, argb);
            }
}
 
void gfx_hline(int x, int y, int w, uint32_t color)
{
    for (int i = 0; i < w; i++) gfx_pixel(x + i, y, color);
}
 
void gfx_vline(int x, int y, int h, uint32_t color)
{
    for (int j = 0; j < h; j++) gfx_pixel(x, y + j, color);
}
 
void gfx_rect_outline(int x, int y, int w, int h, uint32_t color)
{
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}
 
void gfx_rounded_outline(int x, int y, int w, int h, int radius, uint32_t color)
{
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int inside = in_rounded(i, j, w, h, radius);
            int inner  = (i >= 1 && j >= 1 && i < w - 1 && j < h - 1)
                         ? in_rounded(i, j, w, h, radius) &&
                           in_rounded(i - 1, j, w, h, radius) &&
                           in_rounded(i + 1, j, w, h, radius) &&
                           in_rounded(i, j - 1, w, h, radius) &&
                           in_rounded(i, j + 1, w, h, radius)
                         : 0;
            if (inside && !inner)
                gfx_pixel(x + i, y + j, color);
        }
}
 
void gfx_line(int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1;
    int sy = dy < 0 ? -1 : 1;
    int err = (adx > ady ? adx : -ady) / 2;
    while (1) {
        gfx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err;
        if (e2 > -adx) { err -= ady; x0 += sx; }
        if (e2 <  ady) { err += adx; y0 += sy; }
    }
}
 
void gfx_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    for (int j = 0; j < h; j++) {
        int t = (h > 1) ? (j * 256) / (h - 1) : 0;
        uint32_t c = gfx_lerp_color(top, bottom, t);
        gfx_hline(x, y + j, w, c);
    }
}
 
void gfx_blur_region(int x, int y, int w, int h, int passes)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
 
    for (int p = 0; p < passes; p++) {
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                int gx = x + i, gy = y + j;
                uint32_t cc = backbuffer[gy * LOG_W + gx];
                uint32_t l  = (i > 0)     ? backbuffer[gy * LOG_W + gx - 1] : cc;
                uint32_t r  = (i < w - 1) ? backbuffer[gy * LOG_W + gx + 1] : cc;
                uint32_t u  = (j > 0)     ? backbuffer[(gy - 1) * LOG_W + gx] : cc;
                uint32_t d  = (j < h - 1) ? backbuffer[(gy + 1) * LOG_W + gx] : cc;
                uint32_t rr = (((cc>>16)&0xFF)*2 + ((l>>16)&0xFF) + ((r>>16)&0xFF) + ((u>>16)&0xFF) + ((d>>16)&0xFF)) / 6;
                uint32_t rg = (((cc>>8)&0xFF)*2  + ((l>>8)&0xFF)  + ((r>>8)&0xFF)  + ((u>>8)&0xFF)  + ((d>>8)&0xFF))  / 6;
                uint32_t rb = (((cc)&0xFF)*2     + ((l)&0xFF)     + ((r)&0xFF)     + ((u)&0xFF)     + ((d)&0xFF))     / 6;
                blurtmp[gy * LOG_W + gx] = 0xFF000000 | (rr << 16) | (rg << 8) | rb;
            }
        }
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                int gx = x + i, gy = y + j;
                backbuffer[gy * LOG_W + gx] = blurtmp[gy * LOG_W + gx];
            }
    }
}
 
void gfx_glass_region(int x, int y, int w, int h, int radius, uint32_t tint_argb, int blur_passes)
{
    gfx_blur_region(x, y, w, h, blur_passes);
    gfx_rounded_rect_alpha(x, y, w, h, radius, tint_argb);
    gfx_rounded_outline(x, y, w, h, radius, 0x40FFFFFF);
}
 
void gfx_glass_highlight(int x, int y, int w, int h, int radius)
{
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius < 1) radius = 1;
    int hl_h = h / 3;
    if (hl_h < 2) hl_h = 2;
    for (int j = 0; j < hl_h && j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (!in_rounded(i, j, w, h, radius)) continue;
            int gx = x + i, gy = y + j;
            if (gx < 0 || gy < 0 || gx >= fb_w || gy >= fb_h) continue;
            int alpha = 0x60 - (j * 0x60 / hl_h);
            if (alpha < 0) alpha = 0;
            uint32_t hl = ((uint32_t)alpha << 24) | 0x00FFFFFF;
            uint32_t *p = &backbuffer[gy * LOG_W + gx];
            *p = gfx_blend(*p, hl);
        }
    }
}
 
void gfx_glass_specular(int x, int y, int w, int h, int radius)
{
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius < 1) radius = 1;
    int cx = w / 4;
    int cy = h / 4;
    int sr = (w < h ? w : h) / 6;
    if (sr < 2) sr = 2;
    for (int j = -sr; j <= sr; j++) {
        for (int i = -sr; i <= sr; i++) {
            int dist = i * i + j * j;
            if (dist > sr * sr) continue;
            int gx = x + cx + i, gy = y + cy + j;
            if (gx < 0 || gy < 0 || gx >= fb_w || gy >= fb_h) continue;
            if (!in_rounded(cx + i, cy + j, w, h, radius)) continue;
            int alpha = 0x90 - (dist * 0x90 / (sr * sr));
            if (alpha < 0) alpha = 0;
            uint32_t sp = ((uint32_t)alpha << 24) | 0x00FFFFFF;
            uint32_t *p = &backbuffer[gy * LOG_W + gx];
            *p = gfx_blend(*p, sp);
        }
    }
}
 
void gfx_glass_shadow(int x, int y, int w, int h, int radius, int blur_passes)
{
    int sh_off = 3;
    gfx_rounded_rect_alpha(x + sh_off, y + sh_off, w, h, radius, 0x30000000);
    gfx_blur_region(x + sh_off, y + sh_off, w, h, blur_passes);
}
 
void gfx_liquid_glass(int x, int y, int w, int h, int radius, uint32_t tint_argb, int blur_passes)
{
    gfx_glass_shadow(x, y, w, h, radius, blur_passes);
    gfx_blur_region(x, y, w, h, blur_passes);
    gfx_rounded_rect_alpha(x, y, w, h, radius, tint_argb);
    gfx_rounded_outline(x, y, w, h, radius, 0x60FFFFFF);
    gfx_glass_highlight(x, y, w, h, radius);
    gfx_glass_specular(x, y, w, h, radius);
    int edge_alpha = 0x30;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (!in_rounded(i, j, w, h, radius)) continue;
            int gx = x + i, gy = y + j;
            if (gx < 0 || gy < 0 || gx >= fb_w || gy >= fb_h) continue;
            if (j >= h - 3) {
                int fade = (h - 1 - j) * edge_alpha / 3;
                uint32_t sh = ((uint32_t)fade << 24);
                uint32_t *p = &backbuffer[gy * LOG_W + gx];
                *p = gfx_blend(*p, sh);
            }
        }
    }
}
 
void gfx_draw_char_8x8(char c, int x, int y, uint32_t fg, uint32_t bg, bool bgon)
{
    const unsigned char *g = wx_glyph((uint8_t)c);
    for (int cy = 0; cy < 8; cy++) {
        uint8_t row = g[cy];
        for (int cx = 0; cx < 8; cx++) {
            if (row & (1 << cx)) gfx_pixel(x + cx, y + cy, fg);
            else if (bgon)       gfx_pixel(x + cx, y + cy, bg);
        }
    }
}

void gfx_draw_string_8x8(const char *s, int x, int y, uint32_t fg, uint32_t bg, bool bgon)
{
    while (*s) {
        unsigned int cp = wx_utf8_next(&s);
        const unsigned char *g = wx_glyph(cp);
        for (int cy = 0; cy < 8; cy++) {
            uint8_t row = g[cy];
            for (int cx = 0; cx < 8; cx++) {
                if (row & (1 << cx)) gfx_pixel(x + cx, y + cy, fg);
                else if (bgon)       gfx_pixel(x + cx, y + cy, bg);
            }
        }
        x += 8;
    }
}

/*
 * Текст: TrueType (stb_truetype) с откатом на font8x8, если
 * шрифта (font.ttf) в initrd нет.
 *
 * gfx_text        -- пропорциональный кегль AXTTF_UI_SIZE,
 * gfx_text_scaled -- кегль AXTTF_UI_SIZE * scale,
 * gfx_text_width  -- ширина, согласованна с отрисовкой.
 */
void gfx_text(const char *s, int x, int y, uint32_t color)
{
    if (axttf_ready()) {
        const int size = AXTTF_UI_SIZE;
        const int asc  = axttf_ascent(size, 0);
        int pen = x, base = y + asc, ox = x;
        unsigned prev = 0;

        while (*s) {
            unsigned cp;
            const axttf_glyph *g;
            int yy, xx;

            if (*s == '\n') {
                y += AXTTF_UI_LINE;
                base = y + asc;
                pen = ox;
                s++;
                prev = 0;
                continue;
            }

            cp = wx_utf8_next(&s);

            g = axttf_get_glyph(cp, size);
            if (!g)
                continue;

            if (prev)
                pen += axttf_kern(prev, cp, size);

            {
                int px = pen + g->xoff, py = base + g->yoff;

                for (yy = 0; yy < g->h; yy++) {
                    const unsigned char *row = g->aa + yy * g->w;

                    for (xx = 0; xx < g->w; xx++) {
                        int a = row[xx];

                        if (!a || px + xx < 0 || py + yy < 0 ||
                            px + xx >= (int)fb_w || py + yy >= (int)fb_h)
                            continue;

                        backbuffer[(py + yy) * fb_w + (px + xx)] =
                            gfx_blend(backbuffer[(py + yy) * fb_w + (px + xx)],
                                      ((uint32_t)a << 24) | (color & 0x00FFFFFFu));
                    }
                }
            }

            pen += g->adv;
            prev = cp;
        }

        return;
    }

    {
        int ox = x;

        while (*s) {
            if (*s == '\n') { y += 9; x = ox; s++; continue; }
            unsigned int cp = wx_utf8_next(&s);
            const unsigned char *g = wx_glyph(cp);
            for (int cy = 0; cy < 8; cy++) {
                uint8_t row = g[cy];
                for (int cx = 0; cx < 8; cx++)
                    if (row & (1 << cx)) gfx_pixel(x + cx, y + cy, color);
            }
            x += 8;
        }
    }
}

void gfx_text_scaled(const char *s, int x, int y, uint32_t color, int scale)
{
    if (axttf_ready()) {
        const int size = AXTTF_UI_SIZE * scale;
        const int asc  = axttf_ascent(size, 0);
        int pen = x, base = y + asc, ox = x;
        unsigned prev = 0;

        while (*s) {
            unsigned cp;
            const axttf_glyph *g;
            int yy, xx;

            if (*s == '\n') {
                y += AXTTF_UI_LINE * scale;
                base = y + asc;
                pen = ox;
                s++;
                prev = 0;
                continue;
            }

            cp = wx_utf8_next(&s);

            g = axttf_get_glyph(cp, size);
            if (!g)
                continue;

            if (prev)
                pen += axttf_kern(prev, cp, size);

            {
                int px = pen + g->xoff, py = base + g->yoff;

                for (yy = 0; yy < g->h; yy++) {
                    const unsigned char *row = g->aa + yy * g->w;

                    for (xx = 0; xx < g->w; xx++) {
                        int a = row[xx];

                        if (!a || px + xx < 0 || py + yy < 0 ||
                            px + xx >= (int)fb_w || py + yy >= (int)fb_h)
                            continue;

                        backbuffer[(py + yy) * fb_w + (px + xx)] =
                            gfx_blend(backbuffer[(py + yy) * fb_w + (px + xx)],
                                      ((uint32_t)a << 24) | (color & 0x00FFFFFFu));
                    }
                }
            }

            pen += g->adv;
            prev = cp;
        }

        return;
    }

    {
        int ox = x;

        while (*s) {
            if (*s == '\n') { y += 9 * scale; x = ox; s++; continue; }
            unsigned int cp = wx_utf8_next(&s);
            const unsigned char *g = wx_glyph(cp);
            for (int cy = 0; cy < 8; cy++) {
                uint8_t row = g[cy];
                for (int cx = 0; cx < 8; cx++)
                    if (row & (1 << cx))
                        gfx_fill_rect(x + cx * scale, y + cy * scale, scale, scale, color);
            }
            x += 8 * scale;
        }
    }
}

int gfx_text_width(const char *s)
{
    if (axttf_ready())
        return axttf_text_width(s, AXTTF_UI_SIZE);

    {
        int n = 0;

        while (*s++) n++;
        return n * 8;
    }
}

/*
 * Glyph height of the active text font (TrueType 13px, fallback 8px)
 * so callers can vertically center strings in fixed-height rows
 * (menu items, menubar, titlebar).
 */
int gfx_font_h(void)
{
    return axttf_ready() ? AXTTF_UI_SIZE : 8;
}
 
void gfx_blit_argb(const uint32_t *src, int sw, int sh, int dx, int dy)
{
    for (int j = 0; j < sh; j++)
        for (int i = 0; i < sw; i++) {
            uint32_t c = src[j * sw + i];
            int gx = dx + i, gy = dy + j;
            if (gx < 0 || gy < 0 || gx >= fb_w || gy >= fb_h) continue;
            uint32_t *p = &backbuffer[gy * LOG_W + gx];
            *p = gfx_blend(*p, c);
        }
}
 
void gfx_blit_argb_round_bottom(const uint32_t *src, int sw, int sh,
                                int dx, int dy, int dw, int dh, int radius)
{
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (radius * 2 > dw) radius = dw / 2;
    if (radius * 2 > dh) radius = dh / 2;

    int xs = (dw == sw), ys = (dh == sh);
    int cy = dh - 1 - radius;

    for (int j = 0; j < dh; j++) {
        int sy = ys ? j : j * sh / dh;
        /* проверка угла нужна только в полосе снизу */
        int band = radius > 0 && j >= dh - radius;
        for (int i = 0; i < dw; i++) {
            int cov = 255;
            if (band && !(i >= radius && i < dw - radius)) {
                int cx = (i < radius) ? radius : dw - 1 - radius;
                cov = arc_cov(i, j, cx, cy, radius);
                if (cov == 0) continue;
            }
            int sx = xs ? i : i * sw / dw;
            uint32_t c = src[sy * sw + sx];
            put_pixel_cov(dx + i, dy + j, c, cov);
        }
    }
}

void gfx_blit_argb_scaled(const uint32_t *src, int sw, int sh, int dx, int dy, int dw, int dh)
{
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int j = 0; j < dh; j++) {
        int sy = j * sh / dh;
        for (int i = 0; i < dw; i++) {
            int sx = i * sw / dw;
            uint32_t c = src[sy * sw + sx];
            int gx = dx + i, gy = dy + j;
            if (gx < 0 || gy < 0 || gx >= fb_w || gy >= fb_h) continue;
            uint32_t *p = &backbuffer[gy * LOG_W + gx];
            *p = gfx_blend(*p, c);
        }
    }
}
 
void gfx_clear(uint32_t c)        { gfx_clear_rgb(c); }
void gfx_putpixel_raw(int x, int y, uint32_t c) { gfx_pixel(x, y, c); }
 
void gfx_rect(int x, int y, int w, int h, uint32_t c)
{
    gfx_fill_rect(x, y, w, h, c);
}
 
void gfx_circle(int cx, int cy, int r, uint32_t c)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x*x + y*y <= r*r)
                gfx_pixel(cx + x, cy + y, c);
}
 
/*
 * Легаси draw_vga_* / draw_rectangle / draw_circle удалены:
 * они тянули vgafnt[4096] из ядра, а WindowServer текст рисует
 * только через font8x8 (gfx_text / gfx_draw_string_8x8).
 */

/*
 * ============================================================
 * Lion/Aqua primitives (r12->v5 Aqua edition)
 * ============================================================
 *
 * Побайтово те же алгоритмы, что в lib/axquartz.h (клиентский
 * AxQuartz -- это зеркальная копия этих функций).  Благодаря
 * этому хром сервера и содержимое окон выглядят идентично:
 * одинаковые градиенты, AA-края, gloss и тени.
 */

/* Точный целый sqrt (без FPU, побитово) -- как aq_isqrt. */
static int sqrt_i32(int v)
{
    int res = 0, bit = 1 << 30;
    if (v <= 0) return 0;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) { v -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return res;
}

/*
 * Vertical gradient that BLENDS with whatever is already on the
 * backbuffer (both stops carry their own alpha).  This is the
 * Lion translucent menu bar / dock shelf primitive: the caller
 * blurs the backdrop first, then lays this tint over it.
 */
void gfx_vgradient_alpha(int x, int y, int w, int h,
                         uint32_t top_argb, uint32_t bot_argb)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_w) w = (int)fb_w - x;
    if (y + h > (int)fb_h) h = (int)fb_h - y;
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++) {
        int t = (h > 1) ? (j * 256) / (h - 1) : 0;
        uint32_t c = gfx_lerp_color(top_argb, bot_argb, t);
        uint32_t *row = &backbuffer[(y + j) * LOG_W + x];
        for (int i = 0; i < w; i++) row[i] = gfx_blend(row[i], c);
    }
}

/* Rounded rectangle filled with an opaque vertical gradient. */
void gfx_rounded_vgradient(int x, int y, int w, int h, int radius,
                           uint32_t top, uint32_t bottom)
{
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    for (int j = 0; j < h; j++) {
        int t = (h > 1) ? (j * 256) / (h - 1) : 0;
        uint32_t c = gfx_lerp_color(top, bottom, t);
        for (int i = 0; i < w; i++)
            if (in_rounded(i, j, w, h, radius))
                gfx_pixel(x + i, y + j, c);
    }
}

/* Как gfx_rounded_vgradient, но скруглены ТОЛЬКО верхние углы:
 * низ заголовка должен стыковаться с телом окна без выемок. */
void gfx_vgradient_top_rounded(int x, int y, int w, int h, int radius,
                               uint32_t top, uint32_t bottom)
{
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    for (int j = 0; j < h; j++) {
        int t = (h > 1) ? (j * 256) / (h - 1) : 0;
        uint32_t c = gfx_lerp_color(top, bottom, t);
        int band = radius > 0 && j < radius;
        for (int i = 0; i < w; i++) {
            int cov = 255;
            if (band && !(i >= radius && i < w - radius)) {
                int cx = (i < radius) ? radius : w - 1 - radius;
                cov = arc_cov(i, j, cx, radius, radius);
                if (cov == 0) continue;
            }
            put_pixel_cov(x + i, y + j, c, cov);
        }
    }
}

/* Как in_rounded, но скруглены только нижние углы. */
static int in_round_bottom(int px, int py, int w, int h, int r)
{
    if (py < h - r) return 1;
    if (px >= r && px < w - r) return 1;
    int cx = (px < r) ? r : w - 1 - r;
    int cy = h - 1 - r;
    int dx = px - cx, dy = py - cy;
    return (dx * dx + dy * dy) <= (r * r);
}

/*
 * Панель выпадающего меню из меню-бара (Lion): верхние углы прямые --
 * панель примыкает к бару, -- нижние скруглены, тело с вертикальным
 * градиентом и 1px рамкой.  Прозрачность обоих стопов в старшем байте.
 */
void gfx_menu_panel(int x, int y, int w, int h, int radius,
                    uint32_t top_argb, uint32_t bot_argb, uint32_t edge_argb)
{
    if (w <= 0 || h <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    int cy = h - 1 - radius;

    for (int j = 0; j < h; j++) {
        int t = (h > 1) ? (j * 256) / (h - 1) : 0;
        uint32_t c = gfx_lerp_color(top_argb, bot_argb, t);
        int band = radius > 0 && j >= h - radius;
        for (int i = 0; i < w; i++) {
            int cov = 255;
            if (band && !(i >= radius && i < w - radius)) {
                int cx = (i < radius) ? radius : w - 1 - radius;
                cov = arc_cov(i, j, cx, cy, radius);
                if (cov == 0) continue;
            }
            put_pixel_cov(x + i, y + j, c, cov);
        }
    }

    /* рамка: прямые верх и бока + дуга снизу */
    gfx_fill_rect_alpha(x, y, w, 1, edge_argb);
    gfx_fill_rect_alpha(x, y + 1, 1, h - 1, edge_argb);
    gfx_fill_rect_alpha(x + w - 1, y + 1, 1, h - 1, edge_argb);
    for (int j = h - radius; j < h; j++)
        for (int i = 0; i < w; i++) {
            if (!in_round_bottom(i, j, w, h, radius)) continue;
            int inner = (i >= 1 && i < w - 1 && j >= 1 &&
                         in_round_bottom(i - 1, j, w, h, radius) &&
                         in_round_bottom(i + 1, j, w, h, radius) &&
                         in_round_bottom(i, j - 1, w, h, radius) &&
                         in_round_bottom(i, j + 1, w, h, radius));
            if (!inner)
                gfx_fill_rect_alpha(x + i, y + j, 1, 1, edge_argb);
        }
}

/* Rounded rectangle filled with a translucent vertical gradient. */
void gfx_rounded_vgradient_alpha(int x, int y, int w, int h, int radius,
                                 uint32_t top_argb, uint32_t bot_argb)
{
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    for (int j = 0; j < h; j++) {
        int t = (h > 1) ? (j * 256) / (h - 1) : 0;
        uint32_t c = gfx_lerp_color(top_argb, bot_argb, t);
        for (int i = 0; i < w; i++)
            if (in_rounded(i, j, w, h, radius)) {
                int gx = x + i, gy = y + j;
                if (gx < 0 || gy < 0 || gx >= (int)fb_w || gy >= (int)fb_h) continue;
                uint32_t *p = &backbuffer[gy * LOG_W + gx];
                *p = gfx_blend(*p, c);
            }
    }
}

/*
 * Aqua traffic-light dot -- smooth, perfectly round, FLAT:
 * 1/16-subpixel distance test (exact circle), 1px anti-aliased
 * skirt, vertical gradient body + thin darker rim.  NO gloss dome.
 */
void gfx_gloss_circle(int cx, int cy, int r,
                      uint32_t top, uint32_t bottom, uint32_t rim)
{
    int rad16 = r * 16;
    int rad2  = (r + 2) * (r + 2);

    for (int dy = -r - 1; dy <= r + 1; dy++) {
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            if (dx * dx + dy * dy > rad2) continue;

            int dist16 = sqrt_i32((dx * dx + dy * dy) << 8);

            int cov = (rad16 + 8) - dist16;
            if (cov <= 0) continue;
            cov *= 16;
            if (cov > 255) cov = 255;

            uint32_t col;
            if (dist16 >= rad16 - 20)
                col = rim;        /* thin dark rim, ~1.2 px */
            else {
                int t = (r > 0) ? ((dy + r) * 256) / (2 * r) : 0;
                col = gfx_lerp_color(top, bottom, t);
            }

            int gx = cx + dx, gy = cy + dy;
            if (gx < 0 || gy < 0 || gx >= (int)fb_w || gy >= (int)fb_h) continue;
            uint32_t *p = &backbuffer[gy * LOG_W + gx];
            *p = gfx_blend(*p, ((uint32_t)cov << 24) | (col & 0x00FFFFFF));
        }
    }
}

/*
 * Small anti-aliased disc with a flat colour -- dock running
 * indicator dots and soft round accents (alpha in the top byte).
 */
void gfx_circle_aa(int cx, int cy, int r, uint32_t argb)
{
    uint32_t base = argb & 0x00FFFFFF;
    uint32_t a    = (argb >> 24) & 0xFF;
    if (!a || r <= 0) return;

    int rad16 = r * 16;
    int rad2  = (r + 2) * (r + 2);

    for (int dy = -r - 1; dy <= r + 1; dy++) {
        for (int dx = -r - 1; dx <= r + 1; dx++) {
            if (dx * dx + dy * dy > rad2) continue;
            int dist16 = sqrt_i32((dx * dx + dy * dy) << 8);
            int cov = (rad16 + 8) - dist16;
            if (cov <= 0) continue;
            cov *= 16;
            if (cov > 255) cov = 255;
            cov = (cov * a) / 255;

            int gx = cx + dx, gy = cy + dy;
            if (gx < 0 || gy < 0 || gx >= (int)fb_w || gy >= (int)fb_h) continue;
            uint32_t *p = &backbuffer[gy * LOG_W + gx];
            *p = gfx_blend(*p, ((uint32_t)cov << 24) | base);
        }
    }
}

/*
 * Soft drop shadow: N layered translucent rounded rects growing
 * outward.  Cheap and close to the Lion window shadow at 1x.
 */
void gfx_soft_shadow(int x, int y, int w, int h, int radius, int layers)
{
    static const uint8_t alphas[] = { 0x1E, 0x16, 0x10, 0x0A, 0x06 };
    if (layers > 5) layers = 5;
    for (int i = layers - 1; i >= 0; i--) {
        int grow = i * 2;
        uint32_t a = alphas[i];
        gfx_rounded_rect_alpha(x - grow, y - grow + 3,
                               w + grow * 2, h + grow * 2,
                               radius + grow, ((uint32_t)a << 24));
    }
}

/*
 * Top gloss overlay for rectangular Aqua elements (dock icons,
 * buttons): the upper `depth` rows fade from white to nothing.
 */
void gfx_gloss_top(int x, int y, int w, int h, int radius, int depth)
{
    if (depth > h) depth = h;
    if (depth < 1) depth = 1;
    for (int j = 0; j < depth; j++) {
        int alpha = 0x55 - (j * 0x55) / depth;
        if (alpha <= 0) continue;
        uint32_t c = ((uint32_t)alpha << 24) | 0x00FFFFFF;
        int x0 = 0, x1 = w;
        for (int i = 0; i < w; i++)
            if (in_rounded(i, j, w, h, radius)) { x0 = i; break; }
        for (int i = w - 1; i >= 0; i--)
            if (in_rounded(i, j, w, h, radius)) { x1 = i + 1; break; }
        if (x1 > x0)
            gfx_fill_rect_alpha(x + x0, y + j, x1 - x0, 1, c);
    }
}

/*
 * Tiny line-drawn glyphs on the Lion traffic lights.
 * kind: 0 = close (x), 1 = minimize (-), 2 = zoom (+).
 * Every stroke is 2px thick (two adjacent lines).
 */
void gfx_glyph(int kind, int cx, int cy, uint32_t color)
{
    int d = 3;
    switch (kind) {
    case 0:
        gfx_line(cx - d, cy - d, cx + d, cy + d, color);
        gfx_line(cx - d + 1, cy - d, cx + d + 1, cy + d, color);
        gfx_line(cx + d, cy - d, cx - d, cy + d, color);
        gfx_line(cx + d, cy - d + 1, cx - d, cy + d + 1, color);
        break;
    case 1:
        gfx_hline(cx - d, cy - 1, d * 2 + 1, color);
        gfx_hline(cx - d, cy,     d * 2 + 1, color);
        break;
    case 2:
        gfx_hline(cx - d, cy - 1, d * 2 + 1, color);
        gfx_hline(cx - d, cy,     d * 2 + 1, color);
        gfx_vline(cx - 1, cy - d, d * 2 + 1, color);
        gfx_vline(cx,     cy - d, d * 2 + 1, color);
        break;
    default:
        break;
    }
}

/*
 * FONT SELFTEST (диагностика TrueType): печатает в COM1 готовность
 * шрифта, размер загрузки и метрики растеризованных глифов.
 */
static void ft_num(int v)
{
    char t[12]; int n = 0;
    if (v < 0) { serial_putc_ax('-'); v = -v; }
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) serial_putc_ax(t[--n]);
}

void gfx_font_selftest(void)
{
    static const char cs[] = "DeOSTW1eM";
    const char *p;

    serial_puts_ax("FONTTEST ready=");
    serial_putc_ax(axttf_ready() ? '1' : '0');
    serial_puts_ax(" bytes=");
    ft_num(axttf_fsize);
    serial_putc_ax('\n');

    for (p = cs; *p; p++) {
        const axttf_glyph *g = axttf_get_glyph((unsigned char)*p, AXTTF_UI_SIZE);

        serial_puts_ax("  '"); serial_putc_ax(*p); serial_puts_ax("': ");
        if (!g) { serial_puts_ax("NULL\n"); continue; }
        ft_num(g->w); serial_putc_ax('x'); ft_num(g->h);
        serial_puts_ax(" adv="); ft_num(g->adv);
        serial_puts_ax(" aa0="); ft_num(g->w && g->h ? g->aa[0] : -1);
        serial_putc_ax('\n');
    }
}