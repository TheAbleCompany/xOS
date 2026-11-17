#ifndef AXTTF_H
#define AXTTF_H

/*
 * ============================================================
 * axttf.h -- минимальный TrueType-движок ArtyomXOS
 * ============================================================
 *
 * Основа: stb_truetype.h v1.27 (public domain, one-file C
 * library) -- самый компактный в мире рендерер TrueType без
 * потери качества: векторные контуры, антиалиасинг, корректные
 * метрики и кернинг.
 *
 * Обвязка (этот файл):
 *   - ленивая загрузка шрифта из initrd:
 *       font.ttf     -- пропорциональный (DejaVu Sans)
 *       fontmono.ttf -- моноширинный (DejaVu Sans Mono)
 *   - кэш альфа-глифов в СТАТИЧЕСКОЙ арене (malloc не нужен --
 *     движок работает и там, где нет кучи);
 *   - метрики: advance, baseline, line height, kerning;
 *   - загрузка файла -- прямые сисколлы open/read/close,
 *     никаких зависимостей от unistd.h.
 *
 * Рисование остаётся у вызывающего (у него свой блендинг и
 * клиппинг): движок отдаёт 8-битные альфа-битмапы глифов.
 *
 * Если шрифта в initrd нет -- axttf_ready() возвращает 0, и
 * вызывающий откатывается на bitmap font8x8 (никаких поломок).
 *
 * Замечание по математике: sqrt/fabs/fmod/pow/cos/acos нужны
 * stb-растеризатору только для CFF-контуров (.otf) и SDF; для
 * обычных TrueType (glyf) вызывается только ifloor/iceil.
 * Реализации всё равно честные, но на "холодных" путях.
 * ============================================================
 */

#include <stdint.h>
#include <stddef.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"

/* ============================================================
 * Математика без libm (сборка freestanding, -mno-sse)
 * ============================================================ */

static float axtf_fabsf(float x)
{
    return x < 0 ? -x : x;
}

static float axtf_floorf(float x)
{
    int i = (int)x;

    return (x < 0 && (float)i != x) ? (float)(i - 1) : (float)i;
}

static float axtf_sqrtf(float x)
{
    float r;
    int   i;

    if (x <= 0)
        return 0;

    r = x >= 1.0f ? x : 1.0f;
    for (i = 0; i < 30; i++)
        r = 0.5f * (r + x / r);
    return r;
}

static float axtf_fmodf(float a, float b)
{
    if (b == 0)
        return 0;
    return a - b * axtf_floorf(a / b);
}

/* ln(x), x > 0: приведение к [1,2) + ряд Тейлора (atanh) */
static float axtf_lnf(float x)
{
    float m, r, t;
    int   e = 0, i;

    if (x <= 0)
        return -1e30f;

    m = x;
    while (m >= 2.0f) { m *= 0.5f; e++; }
    while (m < 1.0f)  { m *= 2.0f; e--; }

    t = (m - 1.0f) / (m + 1.0f);
    r = 0;
    for (i = 9; i >= 1; i--)
        r = t * (2.0f / (float)i + r);

    return r + 0.69314718056f * (float)e;
}

/* exp(x): x = k*ln2 + r, ряд по r */
static float axtf_expf(float x)
{
    int   k = 0, i;
    float r, s;

    if (x < -87)
        return 0;

    while (x >  0.34657359f) { x -= 0.69314718056f; k++; }
    while (x < -0.34657359f) { x += 0.69314718056f; k--; }

    s = 1.0f;
    r = 1.0f;
    for (i = 1; i <= 8; i++) {
        r *= x / (float)i;
        s += r;
    }

    while (k > 0) { s *= 2.0f; k--; }
    while (k < 0) { s *= 0.5f; k++; }
    return s;
}

static float axtf_powf(float x, float y)
{
    if (x <= 0)
        return 0;
    return axtf_expf(y * axtf_lnf(x));
}

/* cos: приведение в [-pi, pi] + ряд Тейлора (схема Горнера) */
static float axtf_cosf(float x)
{
    static const float c[5] = { -0.5f, 1.0f / 24.0f, -1.0f / 720.0f,
                                1.0f / 40320.0f, -1.0f / 3628800.0f };
    float x2, r;
    int   i;

    x = axtf_fmodf(x, 6.28318530718f);
    if (x >  3.14159265359f) x -= 6.28318530718f;
    if (x < -3.14159265359f) x += 6.28318530718f;

    x2 = x * x;
    r  = c[4];
    for (i = 3; i >= 0; i--)
        r = c[i] + x2 * r;

    return 1.0f + x2 * r;
}

/* atan(x): два полушаговых понижения atan(x) = 2*atan(u), затем ряд */
static float axtf_atanf(float x)
{
    static const float c[9] = { -1.0f / 3.0f,  1.0f / 5.0f, -1.0f / 7.0f,
                                 1.0f / 9.0f, -1.0f / 11.0f, 1.0f / 13.0f,
                                -1.0f / 15.0f, 1.0f / 17.0f, -1.0f / 19.0f };
    int   i, neg = 0;
    float x2, r;

    if (x < 0) { x = -x; neg = 1; }

    /* atan(x) = 2*atan(x / (1 + sqrt(1+x^2))); два раза ->
     * |x| <= ~0.41, ряд сходится быстро. */
    for (i = 0; i < 2; i++)
        x = x / (1.0f + axtf_sqrtf(1.0f + x * x));

    x2 = x * x;
    r  = 0;
    for (i = 8; i >= 0; i--)
        r = x2 * (c[i] + r);
    r = x * (1.0f + r) * 4.0f;

    return neg ? -r : r;
}

static float axtf_acosf(float x)
{
    if (x <= -1.0f) return 3.14159265f;
    if (x >=  1.0f) return 0.0f;
    return 1.57079632679f - axtf_atanf(x / axtf_sqrtf(1.0f - x * x));
}

/* ============================================================
 * UTF-8 -> code point (та же семантика, что libax.h)
 * ============================================================ */

static unsigned int axttf_utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    unsigned int cp = s[0];

    if (cp < 0x80u) {
        *ps = (const char *)(s + 1);
        return cp;
    }
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

/* ============================================================
 * stb_truetype: статические функции, без внешних зависимостей
 * ============================================================ */

/* stb внутри (функции name-таблицы) зовёт assert/memcpy напрямую,
 * мимо своих же STBTT_-макросов -- закрываем это здесь. */
#define assert(x) ((void)0)
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

#define STBTT_STATIC            1
#define STBTT_assert(x)         ((void)0)
#define STBTT_ifloor(x)         ((int)axtf_floorf(x))
#define STBTT_iceil(x)          ((int)axtf_floorf((x) + 0.999999f))
#define STBTT_sqrt(x)           axtf_sqrtf(x)
#define STBTT_pow(x,y)          axtf_powf(x,y)
#define STBTT_fmod(x,y)         axtf_fmodf(x,y)
#define STBTT_cos(x)            axtf_cosf(x)
#define STBTT_acos(x)           axtf_acosf(x)
#define STBTT_fabs(x)           axtf_fabsf(x)
#define STBTT_strlen(s)         axtf_strlen_(s)
#define STBTT_memcpy            axtf_memcpy_
#define STBTT_memset            axtf_memset_

static size_t axtf_strlen_(const char *s)
{
    size_t n = 0;

    while (s[n]) n++;
    return n;
}

static void *axtf_memcpy_(void *d, const void *s, size_t n)
{
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;

    while (n--) *dd++ = *ss++;
    return d;
}

static void *axtf_memset_(void *d, int c, size_t n)
{
    unsigned char *dd = (unsigned char *)d;

    while (n--) *dd++ = (unsigned char)c;
    return d;
}

/* Крохотный bump-аллокатор для внутренних нужд stb (используется
 * только BakeFontBitmap/pack/SDF -- наш путь MakeCodepointBitmap
 * аллокаций не делает, но код должен компилироваться и
 * линковаться). */
#define AXTF_SCRATCH (256 * 1024)
static unsigned char axtf_scratch[AXTF_SCRATCH];
static int axtf_scratch_used;

static void *axtf_scratch_alloc(int size)
{
    void *p;

    size = (size + 15) & ~15;
    if (size <= 0 || axtf_scratch_used + size > AXTF_SCRATCH)
        return 0;
    p = axtf_scratch + axtf_scratch_used;
    axtf_scratch_used += size;
    return p;
}

#define STBTT_malloc(x,u)  axtf_scratch_alloc((int)(x))
#define STBTT_free(x,u)    ((void)(x), (void)(u))

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

/* ============================================================
 * Файловый ввод: прямые сисколлы ArtyomX64
 * ============================================================ */

#ifdef AXTTF_HOSTLOAD
#include <stdio.h>
#endif

static int axtf_fload(const char *path, unsigned char *buf, int cap)
{
    int fd, n, total = 0;

#ifdef AXTTF_HOSTLOAD
    /* Тест на Linux-хосте: обычные open/read/close. */
    {
        FILE *fp = fopen(path, "rb");

        if (!fp)
            return 0;
        total = (int)fread(buf, 1, (size_t)cap, fp);
        fclose(fp);
        return total;
    }
#endif

    __asm__ volatile("int $0x80"
        : "=a"(fd)
        : "a"(5L), "D"((long)path), "S"(0L), "d"(0L)
        : "memory");

    if (fd < 0)
        return 0;

    while (total < cap) {
        __asm__ volatile("int $0x80"
            : "=a"(n)
            : "a"(3L), "D"((long)fd), "S"((long)(buf + total)),
              "d"((long)(cap - total))
            : "memory");

        if (n <= 0)
            break;
        total += n;
    }

    __asm__ volatile("int $0x80"
        : "=a"(n)
        : "a"(6L), "D"((long)fd)
        : "memory");
    (void)n;

    return total;
}

/* ============================================================
 * Движок
 * ============================================================ */

/* Размеры по умолчанию (можно переопределить до include). */
#ifndef AXTTF_UI_SIZE
#define AXTTF_UI_SIZE 13    /* пропорциональный UI-шрифт         */
#endif
#ifndef AXTTF_UI_LINE
#define AXTTF_UI_LINE 16    /* межстрочный шаг пропорционального */
#endif
#ifndef AXTTF_MONO_SIZE
#define AXTTF_MONO_SIZE 15  /* моно-сетка (advance ~= 8px)       */
#endif
#ifndef AXTTF_MONO_CELL
#define AXTTF_MONO_CELL 8   /* ячейка моно-сетки (совместимость
                             * с font8x8-раскладками)           */
#endif

#define AXTTF_FONT_CAP (1 << 20)
#define AXTTF_TAB_N    1024             /* степень двойки    */
#define AXTTF_ARENA    (256 * 1024)

typedef struct {
    unsigned key;   /* 0 = пусто */
    int w, h;
    int xoff, yoff; /* относительно pen/baseline */
    int adv;        /* advance в px */
    int off;        /* смещение альфа-битмапа в арене */
} axttf_ce;

typedef struct {
    int w, h;
    int xoff, yoff;             /* относительно (pen, baseline) */
    int adv;                    /* advance в px                 */
    const unsigned char *aa;    /* альфа 0..255, stride = w     */
} axttf_glyph;

static stbtt_fontinfo axttf_font;
static int axttf_ok   = 0;      /* font.ttf загружен     */
static int axttf_try  = 0;      /* уже пробовали грузить */
static stbtt_fontinfo axttf_fontm;
static int axttf_okm  = 0;
static int axttf_trym = 0;

static axttf_ce axttf_tab[AXTTF_TAB_N];
static unsigned char axttf_arena[AXTTF_ARENA];
static int axttf_arena_used;

static unsigned char axttf_fbuf[AXTTF_FONT_CAP];
static unsigned char axttf_fbufm[AXTTF_FONT_CAP];
static int axttf_fsize;         /* байт загруженного font.ttf (диагностика) */

/* кэш вертикальных метрик (на размер) */
typedef struct { int size; float scale; int ascent, lh; } axttf_vm;
static axttf_vm axttf_vmc[16];
static int      axttf_vmc_n;
static axttf_vm axttf_vmcm[8];
static int      axttf_vmcm_n;

static int axtf_ready_(stbtt_fontinfo *f, const char *path,
                       unsigned char *buf, int *ok, int *tried)
{
    int n;

    if (*ok)
        return 1;
    if (*tried)
        return 0;
    *tried = 1;

    n = axtf_fload(path, buf, AXTTF_FONT_CAP);
    if (f == &axttf_font)
        axttf_fsize = n;
    if (n < 12)
        return 0;

    if (!stbtt_InitFont(f, buf, 0))
        return 0;

    *ok = 1;
    return 1;
}

static int axttf_ready(void)
{
    return axtf_ready_(&axttf_font, "font.ttf",
                       axttf_fbuf, &axttf_ok, &axttf_try);
}

static int axttf_mono_ready(void)
{
    return axtf_ready_(&axttf_fontm, "fontmono.ttf",
                       axttf_fbufm, &axttf_okm, &axttf_trym);
}

/* Кэш VMetrics */
static const axttf_vm *axtf_vm_(int size, int mono)
{
    stbtt_fontinfo *f = mono ? &axttf_fontm : &axttf_font;
    axttf_vm *c = mono ? axttf_vmcm : axttf_vmc;
    int *n      = mono ? &axttf_vmcm_n : &axttf_vmc_n;
    int max     = mono ? 8 : 16;
    int ascent = 0, descent = 0, linegap = 0;
    int i;

    for (i = 0; i < *n; i++)
        if (c[i].size == size)
            return &c[i];

    if (*n >= max) {
        /* вытесняем: метрики дёшевы */
        *n = 0;
        axtf_memset_(c, 0, sizeof(axttf_vm) * (size_t)max);
    }

    stbtt_GetFontVMetrics(f, &ascent, &descent, &linegap);

    {
        float scale = stbtt_ScaleForPixelHeight(f, (float)size);
        axttf_vm *v = &c[*n];

        v->size   = size;
        v->scale  = scale;
        v->ascent = (int)(ascent * scale + 0.5f);
        v->lh     = (int)((ascent - descent + linegap / 2) * scale + 0.5f);
        if (v->lh < size)
            v->lh = size;

        (*n)++;
        return v;
    }
}

static int axttf_ascent(int size, int mono)
{
    if (mono ? !axttf_mono_ready() : !axttf_ready())
        return 0;
    return axtf_vm_(size, mono)->ascent;
}

static int axttf_line_height(int size, int mono)
{
    if (mono ? !axttf_mono_ready() : !axttf_ready())
        return 0;
    return axtf_vm_(size, mono)->lh;
}

/* Поиск в хэш-таблице */
static axttf_ce *axtf_lookup_(unsigned key, int create)
{
    unsigned i = (key * 2654435761u) & (AXTTF_TAB_N - 1);
    unsigned n;

    for (n = 0; n < (unsigned)AXTTF_TAB_N; n++) {
        axttf_ce *e = &axttf_tab[(i + n) & (AXTTF_TAB_N - 1)];

        if (e->key == key)
            return e;
        if (!e->key) {
            if (!create)
                return 0;
            return e;
        }
    }
    return 0;
}

/* Растеризация глифа в кэш.
 * key = (mono << 31) | (cp * 64) | size */
static const axttf_glyph *axtf_bake_(unsigned cp, int size, int mono)
{
    stbtt_fontinfo *f;
    int adv, lsb;
    float scale;
    int ix0, iy0, ix1, iy1;
    int w, h, advpx, need, gi;
    unsigned key;
    axttf_ce *e;
    unsigned char *bits;
    static axttf_glyph g;

    if (size < 4)  size = 4;
    if (size > 72) size = 72;
    if (cp > 0x10FFFFu)
        return 0;

    key = ((unsigned)(mono ? 1u : 0u) << 31) | (cp * 64u) |
          (unsigned)(size & 63);

    e = axtf_lookup_(key, 0);
    if (e && e->key == key) {
        g.w = e->w; g.h = e->h;
        g.xoff = e->xoff; g.yoff = e->yoff; g.adv = e->adv;
        g.aa = axttf_arena + e->off;
        return &g;
    }

    f = (mono && axttf_mono_ready()) ? &axttf_fontm :
        (axttf_ready() ? &axttf_font : 0);
    if (!f)
        return 0;

    gi = stbtt_FindGlyphIndex(f, (int)cp);
    if (!gi) {
        cp = '?';                   /* нет глифа -> '?'        */
        gi = stbtt_FindGlyphIndex(f, '?');
        if (!gi)
            return 0;
    }

    scale = stbtt_ScaleForPixelHeight(f, (float)size);
    stbtt_GetCodepointHMetrics(f, (int)cp, &adv, &lsb);
    (void)lsb;

    advpx = (int)(adv * scale + 0.5f);
    if (advpx < 1)   advpx = 1;
    if (advpx > 128) advpx = 128;

    stbtt_GetCodepointBitmapBox(f, (int)cp, scale, scale,
                                &ix0, &iy0, &ix1, &iy1);
    w = ix1 - ix0;
    h = iy1 - iy0;
    if (w < 0 || h < 0 || w > 127 || h > 127) {
        w = 0;
        h = 0;
    }

    /*
     * r14-commafix: вертикальная посадка глифа в строке.
     *
     * Символы с хвостом ниже базовой линии уходят вниз заметно
     * сильнее остальных.  При кегле 15 px: ',' -2.10, ';' -2.10,
     * '/' -1.39, '|' -3.54, а 'a'/'O' всего -0.21, '.' ровно 0.
     * В мелкой сетке хвост вылезал из строки и символ не был виден.
     * Ограничиваем вынос вниз (limit px), сдвигая битмап вверх, но
     * не выше начала аса -- верх глифа не срезаем.
     */
    if (h > 0) {
        int asc0 = 0, dsc0 = 0, gap0 = 0;
        int limit = (size / 8) + 1;      /* 2 px при 15, 2 при 13 */

        stbtt_GetFontVMetrics(f, &asc0, &dsc0, &gap0);
        (void)dsc0; (void)gap0;

        if (iy1 > limit) {
            int shift = iy1 - limit;

            if (iy0 - shift < -asc0)
                shift = iy0 + asc0;
            if (shift > 0) {
                iy0 -= shift;
                iy1 -= shift;
            }
        }
    }

    need = w * h;
    if (axttf_arena_used + need > AXTTF_ARENA) {
        /* арена кончилась: полный сброс кэша (перепечется) */
        axttf_arena_used = 0;
        axtf_memset_(axttf_tab, 0, sizeof(axttf_tab));
    }

    bits = axttf_arena + axttf_arena_used;
    if (need) {
        /* Растеризатор stb берёт hheap-чанки из STBTT_malloc (наш
         * scratch) и возвращает их через STBTT_free (no-op).  Время
         * жизни аллокаций -- ровно один вызов MakeCodepointBitmap,
         * поэтому scratch надо СБРОСИТЬ перед каждым запеканием,
         * иначе он исчерпается и растеризатор начнёт выдавать
         * пустые битмапы (случайно пропадающие глифы). */
        axtf_scratch_used = 0;
        stbtt_MakeCodepointBitmap(f, bits, w, h, w, scale, scale, (int)cp);
    }
    axttf_arena_used += (need + 3) & ~3;

    e = axtf_lookup_(key, 1);
    if (!e)
        return 0;

    e->key  = key;
    e->w    = w;    e->h    = h;
    e->xoff = ix0;  e->yoff = iy0;
    e->adv  = advpx;
    e->off  = (int)(bits - axttf_arena);

    g.w = e->w; g.h = e->h;
    g.xoff = e->xoff; g.yoff = e->yoff; g.adv = e->adv;
    g.aa = axttf_arena + e->off;
    return &g;
}

static const axttf_glyph *axttf_get_glyph(unsigned cp, int size)
{
    return axtf_bake_(cp, size, 0);
}

/* Глиф моно-сетки: advance принудительно = AXTTF_MONO_CELL,
 * битмап центрируется в ячейке. */
static const axttf_glyph *axttf_get_glyph_mono(unsigned cp)
{
    static axttf_glyph m;
    const axttf_glyph *g = axtf_bake_(cp, AXTTF_MONO_SIZE, 1);

    if (!g)
        return 0;

    m = *g;
    m.adv = AXTTF_MONO_CELL;
    m.xoff += (AXTTF_MONO_CELL - g->w) / 2;

    /*
     * Вертикальная посадка в ячейке.  Глифы с выносом ниже базовой
     * линии ('/', '|', 'g', 'y') у Lucida Console уходят вниз на
     * 1.4-3.5 px при кегле 15 и вылезали из строки: '/' просто не
     * было видно.  Поднимаем битмап так, чтобы его низ не опускался
     * ниже 2 px от базовой линии, но не выше верха строки.
     */
    {
        int bottom = m.yoff + m.h;   /* низ глифа от базовой линии */
        int limit  = 2;              /* допустимый вынос вниз, px   */

        if (bottom > limit) {
            int shift = bottom - limit;
            int top   = m.yoff - shift;

            /* Не задирать глиф вверх до потери верхних частей. */
            if (top < -axttf_ascent(AXTTF_MONO_SIZE, 1))
                shift = m.yoff + axttf_ascent(AXTTF_MONO_SIZE, 1);
            if (shift < 0)
                shift = 0;
            m.yoff -= shift;
        }
    }

    return &m;
}

/* Кернинг между парой (в px) */
static int axtf_kern_(stbtt_fontinfo *f, float scale, unsigned a, unsigned b)
{
    float k = stbtt_GetCodepointKernAdvance(f, (int)a, (int)b);

    return (int)(k * scale + (k > 0 ? 0.5f : -0.5f));
}

/* Кернинг для пропорционального UI-шрифта */
static int axttf_kern(unsigned a, unsigned b, int size)
{
    if (!axttf_ready())
        return 0;
    if (size < 4)  size = 4;
    if (size > 72) size = 72;
    return axtf_kern_(&axttf_font, axtf_vm_(size, 0)->scale, a, b);
}

static int axttf_text_width(const char *s, int size)
{
    const axttf_vm *vm;
    const axttf_glyph *g;
    int w = 0;
    unsigned prev = 0;

    if (!axttf_ready())
        return 0;

    vm = axtf_vm_(size, 0);

    while (*s) {
        unsigned cp = axttf_utf8_next(&s);
        int k;

        if (cp == '\n' || cp == '\r' || cp == '\t') {
            prev = 0;
            continue;
        }

        g = axtf_bake_(cp, size, 0);
        if (!g)
            continue;

        if (prev)
            w += axtf_kern_(&axttf_font, vm->scale, prev, cp);
        w += g->adv;
        prev = cp;
    }

    return w;
}

/* Ширина строки в ячейках моно-сетки */
static int axttf_text_width_mono(const char *s)
{
    int n = 0;

    while (*s) {
        axttf_utf8_next(&s);
        n++;
    }
    return n * AXTTF_MONO_CELL;
}

#pragma GCC diagnostic pop

#endif /* AXTTF_H */
