/*
 * ============================================================
 * axgl.c -- AxGL kernel side: GL-треугольники через GPU ixg
 * ============================================================
 *
 * Обработчик syscall AX_SYS_GPU_TRIANGLES (175).
 *
 * Путь вершины:
 *
 *   app (apps/axgl.h: матрицы, клиппинг)
 *        ->  int 0x80, rax=175
 *        ->  ax_syscall_gpu_triangles()
 *              |
 *              |-- GPU найден ixg?
 *              |     да -> ixg_driver_accel_triangles()
 *              |            (NVIDIA / AMD / Intel 3D-движок,
 *              |             растеризация на видеокарте)
 *              |     нет -> axgl_raster_sw()
 *              |            (программный растеризатор ядра:
 *              |             Z-буфер + Gouraud, fixed-point,
 *              |             БЕЗ float/SSE -- по конвенции
 *              |             ядра -mno-sse)
 *              v
 *        пиксели в canvas окна -> ax_commit() -> shell
 *
 * Формат вершин axgl_user_vertex_t совпадает по раскладке с
 * ixg_hw_vertex_t из drivers/gpu/ixg_driver.h, поэтому GPU-путь
 * не требует преобразований: указатель уходит прямо в бэкенд.
 */

#include "axgl.h"

#include "axshell_internal.h"

#include "../drivers/gpu/ixg_driver.h"

#include "../mm/kheap.h"

/*
 * ============================================================
 * Проверка совместимости формата вершин
 * ============================================================
 */

typedef char axgl_static_assert_vertex_size[
    (sizeof(axgl_user_vertex_t) == sizeof(ixg_hw_vertex_t)) ? 1 : -1
];

/*
 * ============================================================
 * Состояние модуля
 * ============================================================
 */

static int axgl_gpu_probe_done = 0;

/* Режим рендеринга (AX_SYS_GPU_SETMODE):
 *   AUTO     -- GPU-сабмит (если есть) + всегда видимый SW-canvas;
 *   FORCE_SW -- только программный растеризатор;
 *   GPU_ONLY -- только GPU-сабмит (canvas не заполняется ядром).
 */
static int axgl_render_mode = AXGL_MODE_AUTO;

/* Логируем только ПЕРВЫЙ результат GPU-сабмита после пробы/смены
 * режима, чтобы не заспамить COM1 на 60 FPS. */
static int axgl_submit_log_done = 0;

/* Одноразовая диагностика SW-пути: что реально пришло в
 * программный растеризатор (число вершин, первая тройка
 * координат, результат растеризации). */
static int axgl_sw_debug_done = 0;
static uint32_t axgl_sw_debug_tris = 0;   /* счётчик нарис. треугольников */

/* Readback-цель GPU: кадр рендерится в CPU-видимый буфер
 * (системная RAM, отображённая в GPU через DMA-aperture ixg)
 * и после flush копируется в canvas окна.  Когда это работает,
 * на экране -- пиксели ВИДЕОКАРТЫ, а не SW-растеризатора. */
static uint32_t axgl_rt_w = 0;
static uint32_t axgl_rt_h = 0;
static int      axgl_visible_gpu = 0;  /* последний батч -- из GPU */
static char     axgl_backend_str[24];

/* Кэш Z-буфера (один слот -- типичное приложение рисует в одно
 * окно; при смене окна/размера буфер пересоздаётся). */
static uint32_t *axgl_zbuf   = 0;
static uint32_t  axgl_zbuf_w = 0;
static uint32_t  axgl_zbuf_h = 0;
static const uint32_t *axgl_zbuf_owner = 0;

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
        debug_putc(hx[(v >> i) & 0xFu]);
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
            debug_puts("AXGL: sw ZBUF_ALLOC_FAIL\n");
        }
        return -1;
    }

    if (!axgl_sw_debug_done) {
        union { float f; uint32_t u; } bx, by;
        bx.f = verts[0].x;
        by.f = verts[0].y;
        axgl_sw_debug_done = 1;
        debug_puts("AXGL: sw raster n=");
        debug_putnum((uint64_t)count);
        debug_puts(" cw=");
        debug_putnum((uint64_t)cw);
        debug_puts(" ch=");
        debug_putnum((uint64_t)ch);
        debug_puts(" v0.x=0x");
        axgl_debug_puthex32(bx.u);
        debug_puts(" v0.y=0x");
        axgl_debug_puthex32(by.u);
        debug_puts(" v0.sx=");
        debug_putnum((uint64_t)(uint32_t)axgl_f2i(verts[0].x));
        debug_puts(" v0.sy=");
        debug_putnum((uint64_t)(uint32_t)axgl_f2i(verts[0].y));
        debug_putc('\n');
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
                debug_puts("AXGL: sw REJECT tri0 v0.x=0x");
                axgl_debug_puthex32(rx.u);
                debug_puts(" v0.y=0x");
                axgl_debug_puthex32(ry.u);
                debug_putc('\n');
            }
            continue;
        }

        axgl_raster_tri(canvas, zbuf, cw, ch, &s[0], &s[1], &s[2]);
        axgl_sw_debug_tris++;
    }

    if (axgl_sw_debug_tris == 0) {
        debug_puts("AXGL: sw drew 0 triangles (all culled/rejected?)\n");
        axgl_sw_debug_tris = 1;   /* не спамить каждый кадр */
    }

    return 0;
}

/*
 * ============================================================
 * GPU-путь: ixg (NVIDIA / AMD / Intel)
 * ============================================================
 */

static void axgl_ensure_gpu_probe(void)
{
    ixg_driver_caps_t caps;

    if (axgl_gpu_probe_done)
        return;

    axgl_gpu_probe_done = 1;

    /*
     * Сканируем PCI и регистрируем бэкенд (NVIDIA/AMD/Intel),
     * если найдено поддерживаемое железо.  На виртуалке/QEMU
     * ничего не найдётся -- останется программный путь.
     */
    ixg_driver_init();

    ixg_driver_get_caps(&caps);

    debug_puts("AXGL: gpu probe vendor=0x");
    axgl_debug_puthex32(caps.vendor_id);
    debug_puts(" device=0x");
    axgl_debug_puthex32(caps.device_id);
    debug_puts(" backend=");
    debug_puts(ixg_driver_backend_name());
    debug_puts(" hw_triangles=");
    debug_putnum((uint64_t)ixg_driver_has_gpu_triangles());
    debug_puts(" mode=");
    debug_putnum((uint64_t)axgl_render_mode);
    debug_putc('\n');
}

/*
 * ============================================================
 * Syscall-обработчики
 * ============================================================
 */

static ax_surface_win *axgl_find_window(uint32_t canvas_ptr)
{
    int i;

    for (i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *s = &surfaces[i];
        if (s->used &&
            (uint32_t)(uintptr_t)s->canvas == canvas_ptr)
            return s;
    }

    return 0;
}

int ax_syscall_gpu_triangles(uint32_t canvas_ptr,
                             const axgl_user_vertex_t *uverts,
                             uint32_t vert_count)
{
    ax_surface_win *w;
    int have_gpu;
    int gpu_rc = -1;

    if (!uverts || vert_count < 3 || vert_count > AXGL_MAX_VERTS) {
        if (!axgl_sw_debug_done) {
            axgl_sw_debug_done = 1;
            debug_puts("AXGL: submit BAD_COUNT n=");
            debug_putnum((uint64_t)vert_count);
            debug_putc('\n');
        }
        return -1;
    }

    if (vert_count % 3 != 0) {
        if (!axgl_sw_debug_done) {
            axgl_sw_debug_done = 1;
            debug_puts("AXGL: submit ODD_COUNT n=");
            debug_putnum((uint64_t)vert_count);
            debug_putc('\n');
        }
        return -1;
    }

    /*
     * Валидность указателя на вершины.
     *
     * ВАЖНО (.ArtyomX reality): стек пользовательской задачи
     * выделяется malloc()'ом из kernel heap (HHDM: va = phys +
     * limine_hhdm_offset >= 0xFFFF800000000000), поэтому батч
     * вершин ЛЕГАЛЬНО живёт в старшей половине адресного
     * пространства.  Отвергаем только НЕканонические указатели
     * (дыра между user-частью и HHDM).
     */
    if ((uintptr_t)uverts >= 0x0000800000000000ULL &&
        (uintptr_t)uverts <  0xFFFF800000000000ULL) {
        if (!axgl_sw_debug_done) {
            axgl_sw_debug_done = 1;
            debug_puts("AXGL: submit BAD_UVERTS\n");
        }
        return -1;
    }

    w = axgl_find_window(canvas_ptr);
    if (!w || !w->canvas) {
        if (!axgl_sw_debug_done) {
            axgl_sw_debug_done = 1;
            debug_puts("AXGL: submit CANVAS_NOT_FOUND ptr=0x");
            axgl_debug_puthex32(canvas_ptr);
            debug_putc('\n');
        }
        return -1;
    }

    axgl_ensure_gpu_probe();

    have_gpu = ixg_driver_has_gpu_triangles();
    axgl_visible_gpu = 0;

    /*
     * GPU-путь: вершины в формате ixg_hw_vertex_t уходят в
     * 3D-движок видеокарты через бэкенд ixg (NVIDIA/AMD/Intel).
     *
     * Видимость результата:
     *   - если бэкенд умеет CPU-visible render target
     *     (hw_set_render_target + hw_readback_rect -- Intel,
     *     NVIDIA), кадр рендерится в буфер системной RAM,
     *     отображённый в GPU, и после flush копируется в canvas
     *     окна: НА ЭКРАНЕ ПИКСЕЛИ ВИДЕОКАРТЫ;
     *   - если readback недоступен (AMD/экзотика/QEMU) -- в AUTO
     *     canvas окна гарантированно заполняет программный
     *     растеризатор (SW-зеркало), чтобы окно не было пустым;
     *   - в GPU_ONLY canvas ядром не заполняется (диагностика
     *     настоящего GPU-пути на реальном железе);
     *   - в FORCE_SW GPU не трогаем совсем.
     */
    if (have_gpu && axgl_render_mode != AXGL_MODE_FORCE_SW) {
        /* Цель рендера обязана совпадать с canvas окна 1:1,
         * чтобы readback лёг в него без преобразований. */
        if (ixg_driver_has_render_readback()) {
            if (axgl_rt_w != (uint32_t)w->canvas_w ||
                axgl_rt_h != (uint32_t)w->canvas_h) {
                if (ixg_driver_accel_set_render_target(
                        (uint32_t)w->canvas_w,
                        (uint32_t)w->canvas_h) == 0) {
                    axgl_rt_w = (uint32_t)w->canvas_w;
                    axgl_rt_h = (uint32_t)w->canvas_h;
                } else {
                    axgl_rt_w = 0;
                    axgl_rt_h = 0;
                }
            }
        } else {
            axgl_rt_w = 0;
            axgl_rt_h = 0;
        }

        /* Пер-кадровая очистка CPU-visible цели: RT заливается
         * нулями только при аллокации, без этого на цели остаются
         * шлейфы предыдущего кадра. 0 = "фон": такие пиксели
         * readback "поверх" не переносит, сохраняя в canvas
         * 2D-оверлей приложения (axgl_clear + ax_text). */
        if (axgl_rt_w)
            ixg_driver_accel_rt_fill(0x00000000u);

        gpu_rc = ixg_driver_accel_triangles(
            (const ixg_hw_vertex_t *)uverts,
            vert_count / 3
        );

        if (gpu_rc == 0) {
            ixg_driver_flush();

            /* READBACK "поверх": геометрия из GPU-цели ложится в
             * canvas ПОВЕРХ его текущего содержимого; пиксели фона
             * (0) canvas не трогают. Текст/2D, нарисованные
             * приложением, живут в canvas и попадают на экран
             * через ax_commit так же, как в SW-пути. */
            if (axgl_rt_w &&
                ixg_driver_accel_readback_rect_over(w->canvas,
                                                    axgl_rt_w,
                                                    axgl_rt_h,
                                                    0x00000000u) == 0)
                axgl_visible_gpu = 1;
        }

        if (!axgl_submit_log_done) {
            axgl_submit_log_done = 1;

            debug_puts("AXGL: gpu submit ");
            debug_puts(ixg_driver_backend_name());
            debug_puts(" rc=");
            debug_putnum((uint64_t)(gpu_rc == 0 ? 0u : 1u));
            debug_puts(gpu_rc == 0 ? " (accepted)" : " (rejected, -1)");
            debug_putc('\n');

            debug_puts("AXGL: visible=");
            debug_puts(axgl_visible_gpu ? "gpu-readback\n" :
                          (ixg_driver_has_render_readback() && axgl_rt_w == 0) ?
                              "sw-mirror (target alloc failed)\n" :
                              "sw-mirror (no readback)\n");
        }
    }

    /* GPU_ONLY: canvas не трогаем -- как решит бэкенд. */
    if (axgl_render_mode == AXGL_MODE_GPU_ONLY)
        return (gpu_rc == 0) ? 1 : -1;

    /* AUTO / FORCE_SW: окно должно показывать картинку.
     * Если readback из GPU удался, canvas УЖЕ содержит кадр
     * видеокарты -- программный растеризатор не нужен. */
    if (!axgl_visible_gpu) {
        static int rb_stage = 0;
        uint32_t npx = (uint32_t)w->canvas_w * (uint32_t)w->canvas_h;
        int rc;

        if (rb_stage == 0) {
            uint32_t i, diff = 0, fidx = 0xFFFFFFFFu, fval = 0;

            for (i = 0; i < npx; i++) {
                uint32_t p = w->canvas[i] & 0xFFFFFFu;
                if (p != 0x121420u && p != 0x1C1C22u) {
                    if (fidx == 0xFFFFFFFFu) { fidx = i; fval = p; }
                    diff++;
                }
            }

            debug_puts("AXGL: rb0 canvas=0x");
            axgl_debug_puthex32((uint32_t)(((uint64_t)(uintptr_t)w->canvas) >> 32));
            axgl_debug_puthex32((uint32_t)(uintptr_t)w->canvas);
            debug_puts(" id=");
            debug_putnum((uint64_t)w->id);
            debug_puts(" wx=");
            debug_putnum((uint64_t)w->x);
            debug_puts(" wy=");
            debug_putnum((uint64_t)w->y);
            debug_puts(" ww=");
            debug_putnum((uint64_t)w->w);
            debug_puts(" wh=");
            debug_putnum((uint64_t)w->h);
            debug_puts(" diff=");
            debug_putnum((uint64_t)diff);

            if (fidx != 0xFFFFFFFFu) {
                debug_puts(" first=");
                debug_putnum((uint64_t)fidx);
                debug_puts("/0x");
                axgl_debug_puthex32(fval);
            }
            debug_putc('\n');

            rb_stage = 1;
        }

        rc = axgl_raster_sw(w->canvas,
                            (uint32_t)w->canvas_w,
                            (uint32_t)w->canvas_h,
                            uverts, vert_count);

        if (rb_stage == 1) {
            uint32_t i, diff = 0, fidx = 0xFFFFFFFFu, fval = 0;

            for (i = 0; i < npx; i++) {
                uint32_t p = w->canvas[i] & 0xFFFFFFu;
                if (p != 0x121420u && p != 0x1C1C22u) {
                    if (fidx == 0xFFFFFFFFu) { fidx = i; fval = p; }
                    diff++;
                }
            }

            debug_puts("AXGL: rb1 diff=");
            debug_putnum((uint64_t)diff);

            if (fidx != 0xFFFFFFFFu) {
                debug_puts(" first=");
                debug_putnum((uint64_t)fidx);
                debug_puts("/0x");
                axgl_debug_puthex32(fval);
            }
            debug_putc('\n');

            rb_stage = 2;
        }

        if (rc != 0)
            return -1;
    }

    return (gpu_rc == 0) ? 1 : 0;   /* 1 -- GPU тоже принял батч */
}

const char *ax_syscall_gpu_backend(uint32_t canvas_ptr)
{
    (void)canvas_ptr;

    axgl_ensure_gpu_probe();

    if (ixg_driver_has_gpu_triangles()) {
        const char *base = ixg_driver_backend_name();

        /* "+RB" -- окно показывает readback из GPU-цели,
         * т.е. на экране настоящие пиксели видеокарты. */
        if (axgl_visible_gpu) {
            int i = 0, j;
            for (j = 0; base[j] && i < 18; j++)
                axgl_backend_str[i++] = base[j];
            axgl_backend_str[i++] = '+';
            axgl_backend_str[i++] = 'R';
            axgl_backend_str[i++] = 'B';
            axgl_backend_str[i] = 0;
            return axgl_backend_str;
        }

        return base;
    }

    return "Software";
}

int ax_syscall_gpu_set_mode(uint32_t mode)
{
    if (mode > (uint32_t)AXGL_MODE_GPU_ONLY)
        return -1;

    if ((int)mode != axgl_render_mode) {
        axgl_render_mode     = (int)mode;
        axgl_submit_log_done = 0;

        debug_puts("AXGL: render mode -> ");
        debug_putnum(mode);
        debug_putc('\n');
    }

    return axgl_render_mode;
}

int ax_syscall_gpu_get_mode(void)
{
    return axgl_render_mode;
}
