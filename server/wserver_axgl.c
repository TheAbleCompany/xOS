/*
 * ============================================================
 * wserver_axgl.c -- AxGL на стороне WindowServer
 * ============================================================
 *
 * Бывший kernel/axgl.c, перенесённый в процесс wserver (модель
 * NeXT/macOS: 3D-пайплайн -- часть GUI-сервера, а не ядра).
 *
 *   - программный растеризатор (fixed-point, -mno-sse) рисует
 *     прямо в канвас окна (разделяемая память);
 *   - GPU-путь ixg (NVIDIA/AMD/Intel) вызывается через
 *     GPU_SUBMIT_RT (193) -- сырой драйверный сабмит в ядро;
 *     канвас для ядра -- просто буфер w x h.
 *
 * API для диспетчера (wserver_surface.c):
 *   axgl_begin(token, w, h)      -- начало батча (сброс zbuf)
 *   axgl_triangles(token, p, n)  -- пачка вершин (кратно 3)
 *   axgl_backend_name(out, cap)  -- имя активного бэкенда
 *   axgl_set_mode(mode)          -- AUTO / FORCE_SW / GPU_ONLY
 */

#include "wserver_internal.h"
#include "axgl_server.h"

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * User-side syscall shims
 * ============================================================ */

static void *wsh_malloc(unsigned long size)
{
    volatile unsigned long ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)183), "D"(size)
        : "memory");
    return (void *)(uintptr_t)ret;
}

static void wsh_free(void *p)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)184), "D"((long)(uintptr_t)p)
        : "memory");
    (void)ret;
}

#define kmalloc wsh_malloc
#define kfree   wsh_free

/*
 * GPU_SUBMIT_RT(193): rdi = canvas (64-bit), rsi = вершины,
 * rdx = число вершин, r10 = ширина, r8 = высота.
 */
static int gpu_submit_rt(uint64_t canvas, uint32_t w, uint32_t h,
                         const void *verts, uint32_t count)
{
    volatile int ret;
    register uint64_t r10 __asm__("r10") = (uint64_t)w;
    register uint64_t r8v __asm__("r8")  = (uint64_t)h;

    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"((long)193), "D"((long)canvas), "S"((long)verts),
          "d"((long)count), "r"(r10), "r"(r8v)
        : "memory");
    return ret;
}

/*
 * ============================================================
 * Состояние модуля
 * ============================================================
 */

static int axgl_render_mode = AXGL_MODE_AUTO;
static int axgl_submit_log_done = 0;
static int axgl_sw_debug_done = 0;
static uint32_t axgl_sw_debug_tris = 0;

/* Активный батч: канвас-владелец и его размеры (от GL_BEGIN). */
static uint32_t *axgl_cur_canvas = 0;
static uint32_t  axgl_cur_w = 0;
static uint32_t  axgl_cur_h = 0;

/* Логируем только ПЕРВЫЙ результат GPU-сабмита после пробы. */
static int      axgl_visible_gpu = 0;
static char     axgl_backend_str[24];
static uint32_t axgl_rt_w = 0;
static uint32_t axgl_rt_h = 0;

/* Кэш Z-буфера (один слот). */
static uint32_t *axgl_zbuf   = 0;
static uint32_t  axgl_zbuf_w = 0;
static uint32_t  axgl_zbuf_h = 0;
static const uint32_t *axgl_zbuf_owner = 0;

/* Проба GPU -- один раз, через ядро. */
static int axgl_gpu_probed = 0;
static int axgl_gpu_has_hw = 0;
static char axgl_gpu_name[16];

static void axgl_gpu_probe(void)
{
    const char *b;

    if (axgl_gpu_probed) return;
    axgl_gpu_probed = 1;

    /*
     * GPU_SUBMIT_RT без вершин не сделать, поэтому наличие
     * GPU выясняем лениво при первом сабмите: rc == -1 с
     * валидным батчем означает "GPU нет".  Здесь только
     * дефолтное имя бэкенда.
     */
    (void)b;
    axgl_gpu_has_hw = 0;
    axgl_gpu_name[0] = 0;
}

/* ============================================================
 * Диагностика (serial)
 * ============================================================ */

static void wserver_dbg_putc(char c) { serial_putc_ax(c); }

static void wserver_dbg_putnum(uint64_t n)
{
    char buf[32];
    int i = 0;
    if (n == 0) { serial_putc_ax('0'); return; }
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i) serial_putc_ax(buf[--i]);
}

/*
 * ============================================================
 * IEEE-754: float -> int без единой FP-инструкции
 * ============================================================
 *
 * Ядро собрано с -mno-sse (и не сохраняет FPU-состояние при
 * переключении задач), поэтому преобразование float->int делаем
 * чисто целочисленным разбором битов IEEE-754.  Экранные
 * координаты и Z конечны; NaN/Inf/гиганты отсекаются диапазоном.
 */

/* Вывод uint32 в hex в COM1 (диагностика пробы GPU). */
static void axgl_debug_puthex32(uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    int i;
    for (i = 28; i >= 0; i -= 4)
        wserver_dbg_putc(hx[(v >> i) & 0xFu]);
}

static int32_t axgl_f2i(float f)
{
    union { float f; uint32_t u; } bits;
    uint32_t sign, exp, mant;
    int32_t  e;
    uint32_t v;

    bits.f = f;
    sign = (bits.u >> 31) & 1u;
    exp  = (bits.u >> 23) & 0xFFu;
    mant = (bits.u & 0x007FFFFFu) | 0x00800000u;   /* implicit bit */
    e    = (int32_t)exp - 127;                     /* true exponent */

    if (exp == 0)  return 0;                       /* subnormal ~ 0 */
    if (e < 0)     return 0;                       /* |f| < 1       */
    if (e > 30)    return sign ? (int32_t)0x80000000 : (int32_t)0x7FFFFFFF;

    if (e >= 23)
        v = mant << (e - 23);
    else
        v = mant >> (23 - e);

    return sign ? -(int32_t)v : (int32_t)v;
}

/* float -> 16.16 fixed point. */
static int32_t axgl_f2fx(float f)
{
    union { float f; uint32_t u; } bits;
    uint32_t sign, exp, mant;
    int32_t  e;
    int64_t  v;

    bits.f = f;
    sign = (bits.u >> 31) & 1u;
    exp  = (bits.u >> 23) & 0xFFu;
    mant = (bits.u & 0x007FFFFFu) | 0x00800000u;
    e    = (int32_t)exp - 127;

    if (exp == 0) return 0;
    if (e < -16)  return 0;
    if (e > 15)   return sign ? (int32_t)0x80000000 : (int32_t)0x7FFFFFFF;

    if (e >= 23)
        v = (int64_t)mant << (e - 23);
    else
        v = (int64_t)mant >> (23 - e);

    /* мантисса в Q23 -> сдвигаем до Q16.16 */
    v <<= 7;

    if (v >  0x7FFFFFFFLL) v =  0x7FFFFFFFLL;
    if (v < -0x80000000LL) v = -0x80000000LL;

    return sign ? -(int32_t)v : (int32_t)v;
}

/*
 * ============================================================
 * Программный растеризатор (fallback, если GPU не найден)
 * ============================================================
 *
 * Bounding box + edge functions, барицентрическая интерполяция
 * Z и цвета в 16.16 fixed point, Z-тест GL_LESS, альфа-смешение
 * при a < 255.
 */

static uint32_t *axgl_zbuf_get(const uint32_t *canvas, uint32_t w, uint32_t h)
{
    if (axgl_zbuf && axgl_zbuf_owner == canvas &&
        axgl_zbuf_w == w && axgl_zbuf_h == h)
        return axgl_zbuf;

    if (axgl_zbuf) {
        kfree(axgl_zbuf);
        axgl_zbuf = 0;
    }

    axgl_zbuf = (uint32_t *)kmalloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!axgl_zbuf) {
        axgl_zbuf_owner = 0;
        return 0;
    }

    axgl_zbuf_owner = canvas;
    axgl_zbuf_w     = w;
    axgl_zbuf_h     = h;

    return axgl_zbuf;
}

typedef struct {
    int32_t x, y;        /* экранные, пиксели */
    int32_t z;           /* 16.16             */
    int32_t r, g, b, a;  /* 0..255            */
} axgl_sv_t;

static void axgl_raster_tri(uint32_t *canvas, uint32_t *zbuf,
                            uint32_t cw, uint32_t ch,
                            const axgl_sv_t *p0,
                            const axgl_sv_t *p1,
                            const axgl_sv_t *p2)
{
    axgl_sv_t a = *p0, b = *p1, c = *p2;
    int32_t area;
    int32_t minx, miny, maxx, maxy;
    int32_t x, y;
    int32_t w0_row, w1_row, w2_row;
    int32_t a0, a1, a2;   /* x-шаги edge functions */
    int32_t b0, b1, b2;   /* y-шаги edge functions */


    /* Вырожденные треугольники не рисуем. */
    area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (area == 0)
        return;

    /* Нормируем обход так, чтобы area > 0 (front-facing вопрос
     * решает пользовательский culling). */
    if (area < 0) {
        axgl_sv_t t = b; b = c; c = t;
        area = -area;
    }

    minx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    maxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);

    if (minx < 0)            minx = 0;
    if (miny < 0)            miny = 0;
    if (maxx >= (int32_t)cw) maxx = (int32_t)cw - 1;
    if (maxy >= (int32_t)ch) maxy = (int32_t)ch - 1;
    if (minx > maxx || miny > maxy)
        return;

    /*
     * Стандартные edge functions:
     *   E(u, v, p) = (v.x - u.x) * (p.y - u.y)
     *              - (v.y - u.y) * (p.x - u.x)
     *   w0 = E(b, c, p)  (вес вершины a)
     *   w1 = E(c, a, p)  (вес вершины b)
     *   w2 = E(a, b, p)  (вес вершины c)
     * Для треугольника с area > 0 точка внутри имеет все
     * w >= 0, и w_i / area -- барицентрические координаты.
     */
    w0_row = (c.x - b.x) * (miny - b.y) - (c.y - b.y) * (minx - b.x);
    w1_row = (a.x - c.x) * (miny - c.y) - (a.y - c.y) * (minx - c.x);
    w2_row = (b.x - a.x) * (miny - a.y) - (b.y - a.y) * (minx - a.x);

    /* Шаги: dE/dx и dE/dy. */
    a0 = (b.y - c.y);  b0 = (c.x - b.x);
    a1 = (c.y - a.y);  b1 = (a.x - c.x);
    a2 = (a.y - b.y);  b2 = (b.x - a.x);

    for (y = miny; y <= maxy; y++) {
        int32_t w0 = w0_row;
        int32_t w1 = w1_row;
        int32_t w2 = w2_row;
        uint32_t *drow = &canvas[(uint32_t)y * cw];
        uint32_t *zrow = &zbuf[(uint32_t)y * cw];

        for (x = minx; x <= maxx; x++) {

            if ((w0 | w1 | w2) >= 0) {
                int64_t l0 = (int64_t)w0 * 65536 / area;
                int64_t l1 = (int64_t)w1 * 65536 / area;
                int64_t l2 = 65536 - l0 - l1;   /* точная сумма = 1.0 */
                int64_t z64;
                int32_t z, cr, cg, cb, ca;
                uint32_t zi, dst, out;

                /* Z: интерполяция в 16.16 (линейно в screen-space). */
                z64 = ((int64_t)a.z * l0 +
                       (int64_t)b.z * l1 +
                       (int64_t)c.z * l2) >> 16;
                if (z64 < 0)                z64 = 0;
                if (z64 > 0x7FFFFFFFLL)     z64 = 0x7FFFFFFFLL;
                z = (int32_t)z64;

                /* Gouraud: барицентрически взвешенная сумма --
                 * l0 -- вес вершины a, l1 -- b, l2 -- c. */
                cr = (int32_t)(((int64_t)a.r * l0 +
                                (int64_t)b.r * l1 +
                                (int64_t)c.r * l2) >> 16);
                cg = (int32_t)(((int64_t)a.g * l0 +
                                (int64_t)b.g * l1 +
                                (int64_t)c.g * l2) >> 16);
                cb = (int32_t)(((int64_t)a.b * l0 +
                                (int64_t)b.b * l1 +
                                (int64_t)c.b * l2) >> 16);
                ca = (int32_t)(((int64_t)a.a * l0 +
                                (int64_t)b.a * l1 +
                                (int64_t)c.a * l2) >> 16);

                if (cr < 0) cr = 0; else if (cr > 255) cr = 255;
                if (cg < 0) cg = 0; else if (cg > 255) cg = 255;
                if (cb < 0) cb = 0; else if (cb > 255) cb = 255;
                if (ca < 0) ca = 0; else if (ca > 255) ca = 255;

                zi = (uint32_t)z;

                /* Z-тест GL_LESS: пишем только более близкие пиксели. */
                if (zi < zrow[x]) {
                    dst = drow[x];

                    if (ca >= 255) {
                        out = 0xFF000000u |
                              ((uint32_t)cr << 16) |
                              ((uint32_t)cg << 8)  |
                              (uint32_t)cb;
                    } else {
                        uint32_t ia = (uint32_t)(255 - ca);
                        uint32_t sr = ((uint32_t)cr * (uint32_t)ca +
                                       ((dst >> 16) & 0xFF) * ia) / 255;
                        uint32_t sg = ((uint32_t)cg * (uint32_t)ca +
                                       ((dst >> 8)  & 0xFF) * ia) / 255;
                        uint32_t sb = ((uint32_t)cb * (uint32_t)ca +
                                       (dst & 0xFF) * ia) / 255;
                        out = 0xFF000000u | (sr << 16) | (sg << 8) | sb;
                    }

                    drow[x] = out;
                    zrow[x] = zi;
                }
            }

            /* Шаг вправо. */
            w0 += a0;
            w1 += a1;
            w2 += a2;
        }

        /* Шаг вниз. */
        w0_row += b0;
        w1_row += b1;
        w2_row += b2;
    }
}

static int axgl_raster_sw(uint32_t *canvas, uint32_t cw, uint32_t ch,
                          const axgl_user_vertex_t *verts, uint32_t count)
{
    uint32_t *zbuf;
    uint32_t  tri, i;

    zbuf = axgl_zbuf_get(canvas, cw, ch);
    if (!zbuf) {
        if (!axgl_sw_debug_done) {
            axgl_sw_debug_done = 1;
            serial_puts_ax("AXGL: sw ZBUF_ALLOC_FAIL\n");
        }
        return -1;
    }

    if (!axgl_sw_debug_done) {
        union { float f; uint32_t u; } bx, by;
        bx.f = verts[0].x;
        by.f = verts[0].y;
        axgl_sw_debug_done = 1;
        serial_puts_ax("AXGL: sw raster n=");
        wserver_dbg_putnum((uint64_t)count);
        serial_puts_ax(" cw=");
        wserver_dbg_putnum((uint64_t)cw);
        serial_puts_ax(" ch=");
        wserver_dbg_putnum((uint64_t)ch);
        serial_puts_ax(" v0.x=0x");
        axgl_debug_puthex32(bx.u);
        serial_puts_ax(" v0.y=0x");
        axgl_debug_puthex32(by.u);
        serial_puts_ax(" v0.sx=");
        wserver_dbg_putnum((uint64_t)(uint32_t)axgl_f2i(verts[0].x));
        serial_puts_ax(" v0.sy=");
        wserver_dbg_putnum((uint64_t)(uint32_t)axgl_f2i(verts[0].y));
        wserver_dbg_putc('\n');
    }

    /* Сброс Z-буфера: "далеко" = максимум. */
    for (i = 0; i < cw * ch; i++)
        zbuf[i] = 0x7FFFFFFFu;

    for (tri = 0; tri + 2 < count; tri += 3) {
        const axgl_user_vertex_t *uv = &verts[tri];
        axgl_sv_t s[3];
        uint32_t  k;
        int       rejected = 0;

        for (k = 0; k < 3; k++) {
            int32_t x = axgl_f2i(uv[k].x);
            int32_t y = axgl_f2i(uv[k].y);

            /* Мусорные/улетевшие вершины -- весь батч игнорируем.
             * Пользовательский axgl клиппит по near и клэмпит
             * экранные координаты в [-4096..20480]. */
            if (x < -5000 || x > 21000 ||
                y < -5000 || y > 21000) {
                rejected = 1;
                break;
            }

            s[k].x = x;
            s[k].y = y;
            s[k].z = axgl_f2fx(uv[k].z);

            s[k].a = (int32_t)((uv[k].color >> 24) & 0xFF);
            s[k].r = (int32_t)((uv[k].color >> 16) & 0xFF);
            s[k].g = (int32_t)((uv[k].color >>  8) & 0xFF);
            s[k].b = (int32_t)((uv[k].color       ) & 0xFF);
        }

        if (rejected) {
            if (tri == 0 && axgl_sw_debug_tris == 0) {
                union { float f; uint32_t u; } rx, ry;
                rx.f = uv[0].x;
                ry.f = uv[0].y;
                serial_puts_ax("AXGL: sw REJECT tri0 v0.x=0x");
                axgl_debug_puthex32(rx.u);
                serial_puts_ax(" v0.y=0x");
                axgl_debug_puthex32(ry.u);
                wserver_dbg_putc('\n');
            }
            continue;
        }

        axgl_raster_tri(canvas, zbuf, cw, ch, &s[0], &s[1], &s[2]);
        axgl_sw_debug_tris++;
    }

    if (axgl_sw_debug_tris == 0) {
        serial_puts_ax("AXGL: sw drew 0 triangles (all culled/rejected?)\n");
        axgl_sw_debug_tris = 1;   /* не спамить каждый кадр */
    }

    return 0;
}

/*
 * ============================================================
 * Публичный API сервера (вызывает wserver_surface.c)
 * ============================================================
 */

int axgl_set_mode(int mode)
{
    if (mode < AXGL_MODE_AUTO || mode > AXGL_MODE_GPU_ONLY)
        return -1;

    if (mode != axgl_render_mode) {
        axgl_render_mode     = mode;
        axgl_submit_log_done = 0;

        serial_puts_ax("AXGL: render mode -> ");
        wserver_dbg_putnum((uint64_t)mode);
        serial_putc_ax('\n');
    }

    return axgl_render_mode;
}

/*
 * Начало батча: запоминаем канвас-владелец и размеры (от окна),
 * сбрасываем флаг "GPU уже нарисовал" -- как старый сброс
 * Z-буфера на каждый сисколл.
 */
int axgl_begin(uint32_t token, int w, int h)
{
    ax_surface_win *s = surf_by_token(token);

    if (!s || !s->canvas || w <= 0 || h <= 0)
        return -1;

    axgl_cur_canvas  = s->canvas;
    axgl_cur_w       = (uint32_t)w;
    axgl_cur_h       = (uint32_t)h;
    axgl_visible_gpu = 0;
    axgl_submit_log_done = 0;

    return 0;
}

/*
 * Пачка треугольников (кратно 3).  Первый сабмит батча
 * пробует GPU-путь (GPU_SUBMIT_RT в ядро), затем -- если
 * режим требует видимый результат и GPU не нарисовал --
 * программный растеризатор.
 */
int axgl_triangles(uint32_t token, const void *verts, uint32_t count)
{
    int gpu_rc = -1;
    uint64_t canvas64;

    (void)token;

    if (!axgl_cur_canvas || !verts || count < 3 || count % 3 != 0)
        return -1;
    if (count > AXGL_MAX_VERTS)
        return -1;

    canvas64 = (uint64_t)(uintptr_t)axgl_cur_canvas;

    /*
     * GPU-путь.  Ядро возвращает:
     *   0  -- приняли (и, возможно, readback отрисован);
     *  >0  -- приняли, но канал не CPU-visible;
     *  <0  -- GPU нет / брак.  Тогда рисуем программно.
     *
     * Пробуем только в AUTO/GPU_ONLY; FORCE_SW GPU не трогает.
     */
    if (axgl_render_mode != AXGL_MODE_FORCE_SW) {
        gpu_rc = gpu_submit_rt(canvas64, axgl_cur_w, axgl_cur_h,
                               verts, count);

        if (gpu_rc >= 0 && !axgl_submit_log_done) {
            axgl_submit_log_done = 1;
            serial_puts_ax("AXGL: gpu submit accepted\n");
        }
    }

    /* GPU_ONLY: канвас не трогаем -- как решит бэкенд. */
    if (axgl_render_mode == AXGL_MODE_GPU_ONLY)
        return (gpu_rc == 0) ? 1 : -1;

    /*
     * AUTO / FORCE_SW: окно обязано показывать картинку.
     * GPU_SUBMIT_RT сам делает readback "поверх" канваса, когда
     * бэкенд умеет; если он ответил <0 -- считаем, что кадра нет,
     * и рисуем программно.
     */
    if (gpu_rc < 0) {
        int rc = axgl_raster_sw(axgl_cur_canvas, axgl_cur_w, axgl_cur_h,
                                (const axgl_user_vertex_t *)verts, count);
        if (rc != 0)
            return -1;
        axgl_visible_gpu = 0;
    } else {
        axgl_visible_gpu = 1;
    }

    return (gpu_rc == 0) ? 1 : 0;
}

int axgl_backend_name(char *out, int cap)
{
    const char *name;

    if (!out || cap <= 0) return -1;

    axgl_gpu_probe();

    if (axgl_gpu_has_hw && axgl_gpu_name[0]) {
        name = axgl_gpu_name;
    } else {
        name = "Software";
    }

    int i = 0;
    while (name[i] && i < cap - 1) { out[i] = name[i]; i++; }
    out[i] = 0;

    return 0;
}
