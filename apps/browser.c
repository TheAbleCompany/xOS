/*
 * browser.c — минимальный HTML-рендерер для ArtyomX64 (libax + axquartz)
 *
 * Поддерживает:
 *   блоки:   <h1>..<h6>, <p>
 *   inline:  <b>, <i>, <br>
 *   сущности: &amp; &lt; &gt; &quot; &nbsp;
 *
 * Сборка: gcc browser.c -o browser -mno-sse
 */

#include "libax.h"
#include "axquartz.h"
#include "unistd.h"
/* ============================================================
 * HTML-сущности
 * ============================================================ */
static const char* decode_entity(const char* s, char* out)
{
    if (!strncmp(s, "&amp;",  5)) { *out = '&';  return s + 5; }
    if (!strncmp(s, "&lt;",   4)) { *out = '<';  return s + 4; }
    if (!strncmp(s, "&gt;",   4)) { *out = '>';  return s + 4; }
    if (!strncmp(s, "&quot;", 6)) { *out = '"';  return s + 6; }
    if (!strncmp(s, "&nbsp;", 6)) { *out = ' ';  return s + 6; }
    *out = *s;
    return s + 1;
}

static size_t decode_run(const char* s, size_t len, char* buf, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < cap; ) {
        if (s[i] == '&') {
            char c;
            const char* next = decode_entity(&s[i], &c);
            buf[n++] = c;
            i = (size_t)(next - s);
        } else {
            buf[n++] = s[i++];
        }
    }
    buf[n] = '\0';
    return n;
}

/* ============================================================
 * Стиль run'а
 * ============================================================ */
typedef struct {
    int scale;
    int bold;
    int italic;
} style_t;

/* ============================================================
 * Отрисовка глифов с псевдокурсивом и масштабом
 * ============================================================ */

/* Обычный текст (scale=1) с опциональным псевдокурсивом. */
static void draw_plain(const char* s, int x, int y, uint32_t color, int bold, int italic)
{
    if (!italic) {
        if (bold) aq_text_bold(s, x, y, color);
        else      aq_text(s, x, y, color);
        return;
    }

    /* Псевдокурсив: рисуем глиф построчно, сдвигая каждую строку на cy/3. */
    const char* p = s;
    int ox = x;
    while (*p) {
        if (*p == '\n') { y += 8; x = ox; p++; continue; }
        unsigned int cp = ax_utf8_next(&p);
        const unsigned char* g = ax_font_glyph(cp);
        int shear = 8 / 3 + 1;  /* максимум сдвига внизу */
        for (int cy = 0; cy < 8; cy++) {
            int shift = (cy * shear) / 8;
            for (int cx = 0; cx < 8; cx++) {
                if (g[cy] & (1 << cx)) {
                    aq_px(x + cx + shift, y + cy, color);
                    if (bold) aq_px(x + cx + shift + 1, y + cy, color);
                }
            }
        }
        x += 8;
    }
}

/* Масштабированный текст (scale>1). aq_text_scaled не умеет bold/italic,
   поэтому для bold дублируем со сдвигом, для italic добавляем наклон
   на уровне повторов строк. */
static void draw_scaled(const char* s, int x, int y, uint32_t color,
                        int scale, int bold, int italic)
{
    if (scale <= 1) { draw_plain(s, x, y, color, bold, italic); return; }

    const char* p = s;
    int ox = x;
    int shear = italic ? (8 * scale) / 3 + 1 : 0;

    while (*p) {
        if (*p == '\n') { y += 8 * scale; x = ox; p++; continue; }
        unsigned int cp = ax_utf8_next(&p);
        const unsigned char* g = ax_font_glyph(cp);

        for (int cy = 0; cy < 8; cy++) {
            int shift = shear ? (cy * scale * shear) / (8 * scale) : 0;
            for (int rep_y = 0; rep_y < scale; rep_y++) {
                int yy = y + cy * scale + rep_y;
                for (int cx = 0; cx < 8; cx++) {
                    if (g[cy] & (1 << cx)) {
                        aq_px(x + cx + shift, yy, color);
                        if (bold) aq_px(x + cx + shift + 1, yy, color);
                    }
                }
            }
        }
        x += 8;
    }
}

static void draw_run(const char* txt, int x, int y, uint32_t color, const style_t* st)
{
    if (st->scale <= 1)
        draw_plain(txt, x, y, color, st->bold, st->italic);
    else
        draw_scaled(txt, x, y, color, st->scale, st->bold, st->italic);
}

/* ============================================================
 * Блочные теги
 * ============================================================ */
typedef enum {
    TAG_P = 0,
    TAG_H1, TAG_H2, TAG_H3, TAG_H4, TAG_H5, TAG_H6,
    TAG_UNKNOWN
} block_tag;

static block_tag tag_of(const char* s, size_t* tag_len)
{
    struct { const char* name; block_tag tag; } map[] = {
        { "<h1>", TAG_H1 }, { "<h2>", TAG_H2 }, { "<h3>", TAG_H3 },
        { "<h4>", TAG_H4 }, { "<h5>", TAG_H5 }, { "<h6>", TAG_H6 },
        { "<p>",  TAG_P  },
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        size_t n = strlen(map[i].name);
        if (!strncmp(s, map[i].name, n)) {
            if (tag_len) *tag_len = n;
            return map[i].tag;
        }
    }
    return TAG_UNKNOWN;
}

static const char* close_tag_of(block_tag t)
{
    switch (t) {
    case TAG_H1: return "</h1>";
    case TAG_H2: return "</h2>";
    case TAG_H3: return "</h3>";
    case TAG_H4: return "</h4>";
    case TAG_H5: return "</h5>";
    case TAG_H6: return "</h6>";
    case TAG_P:  return "</p>";
    default:     return "";
    }
}

static style_t block_style(block_tag t)
{
    style_t st = { .scale = 1, .bold = 0, .italic = 0 };
    switch (t) {
    case TAG_H1: st.scale = 3; st.bold = 1; break;
    case TAG_H2: st.scale = 2; st.bold = 1; break;
    case TAG_H3: st.scale = 2; st.bold = 0; break;
    case TAG_H4: st.scale = 1; st.bold = 1; break;
    case TAG_H5: st.scale = 1; st.bold = 0; break;
    case TAG_H6: st.scale = 1; st.bold = 0; break;
    case TAG_P:  st.scale = 1; st.bold = 0; break;
    default: break;
    }
    return st;
}

static int line_height_for(style_t st) { return 8 * st.scale + 4; }

/* ============================================================
 * Отрисовка inline-содержимого с word-wrap.
 * Возвращает y после последней строки.
 * ============================================================ */
static int render_inline(const char* p, const char* close,
                         int x0, int y0, int max_w,
                         style_t base, uint32_t color)
{
    int x = x0, y = y0;
    int line_h = line_height_for(base);

    int bold_depth   = base.bold ? 1 : 0;
    int italic_depth = base.italic ? 1 : 0;
    const char* run_start = p;
    int run_bold   = base.bold;
    int run_italic = base.italic;

    char buf[512];

    /* Применяет один run: переносит по словам, если не влезает. */
    #define APPLY_RUN(ptr, len)                                          \
        do {                                                             \
            decode_run((ptr), (len), buf, sizeof(buf));                  \
            style_t s = base;                                            \
            s.bold = run_bold; s.italic = run_italic;                    \
            int w = aq_text_width(buf);                                  \
            if (x > x0 && x + w > x0 + max_w) {                          \
                x = x0; y += line_h;                                     \
            }                                                            \
            draw_run(buf, x, y, color, &s);                              \
            x += w;                                                      \
            if (x > x0 + max_w) { x = x0; y += line_h; }                 \
        } while (0)

    while (p < close) {
        int is_tag = 0;
        if      (!strncmp(p, "<b>", 3))  is_tag = 1;
        else if (!strncmp(p, "</b>", 4)) is_tag = 1;
        else if (!strncmp(p, "<i>", 3))  is_tag = 1;
        else if (!strncmp(p, "</i>", 4)) is_tag = 1;
        else if (!strncmp(p, "<br>", 4)) is_tag = 1;

        if (is_tag) {
            if (p > run_start) APPLY_RUN(run_start, (size_t)(p - run_start));

            if      (!strncmp(p, "<b>", 3))  { bold_depth++;   run_bold = 1; p += 3; }
            else if (!strncmp(p, "</b>", 4)) { if (bold_depth) bold_depth--;   run_bold = bold_depth ? 1 : base.bold; p += 4; }
            else if (!strncmp(p, "<i>", 3))  { italic_depth++; run_italic = 1; p += 3; }
            else if (!strncmp(p, "</i>", 4)) { if (italic_depth) italic_depth--; run_italic = italic_depth ? 1 : base.italic; p += 4; }
            else if (!strncmp(p, "<br>", 4)) { x = x0; y += line_h; p += 4; }

            run_start = p;
            continue;
        }
        p++;
    }

    if (p > run_start) APPLY_RUN(run_start, (size_t)(p - run_start));

    #undef APPLY_RUN

    return y + line_h;
}

/* ============================================================
 * Проход по документу
 * ============================================================ */
static void render_html(const char* doc, int x0, int y0, int max_w)
{
    const char* p = doc;
    int y = y0;

    while (*p) {
        size_t tlen = 0;
        block_tag tag = tag_of(p, &tlen);
        if (tag == TAG_UNKNOWN) { p++; continue; }

        const char* content = p + tlen;
        const char* close   = strstr(content, close_tag_of(tag));
        if (!close) break;  /* незакрытый блок — стоп */

        style_t st = block_style(tag);
        uint32_t color = (tag == TAG_P) ? AX_RGB(60, 60, 70) : AX_RGB(25, 25, 35);

        int pad_top = (st.scale > 1) ? 8 : 4;
        y += pad_top;

        /* Карточка под <p> (можно убрать — просто закомментировать) */
        if (tag == TAG_P) {
            int box_h = 8 + 12;   /* минимум одна строка + padding */
            /* грубая оценка высоты: посчитаем по количеству <br> */
            for (const char* q = content; q < close; q++)
                if (!strncmp(q, "<br>", 4)) box_h += line_height_for(st);
            int box_w = max_w;
            aq_soft_shadow(x0 - 4, y - 4, box_w + 8, box_h + 8, 8, 3);
            aq_rounded_fill_alpha(x0 - 4, y - 4, box_w + 8, box_h + 8, 8,
                                  ((uint32_t)0xF0 << 24) | 0xFFFFFF);
            aq_rounded_outline(x0 - 4, y - 4, box_w + 8, box_h + 8, 8,
                               ((uint32_t)0x30 << 24) | 0x000000);
            y += 4;
            max_w -= 12;
            x0  += 6;
        }

        int y_end = render_inline(content, close, x0, y, max_w, st, color);
        y = y_end + 6;

        if (tag == TAG_P) {
            /* восстановим x0/max_w для следующих блоков */
            x0  -= 6;
            max_w += 12;
        }

        p = close + strlen(close_tag_of(tag));
    }
}

/* ============================================================
 * main
 * ============================================================ */
int main(void)
{
    const char* doc =
        "<h1>Заголовок H1</h1>"
        "<h2>Подзаголовок H2</h2>"
        "<h3>Третий уровень</h3>"
        "<p>Параграф с <b>жирным</b>, <i>курсивом</i><br>"
        "и второй строкой, а также &amp; &lt; &gt; entities.</p>"
        "<h4>H4</h4>"
        "<p>Ещё <b>один</b> параграф, в котором есть довольно длинная строка, "
        "чтобы проверить перенос по словам внутри блока.</p>"
        "<h5>H5</h5>"
        "<h6>H6</h6>"
        "<p>Финальный абзац.</p>";

    uint32_t *canvas = ax_surface("Моё приложение", 480, 320);
    if (!canvas) return 1;

    ax_fill(AX_RGB(245, 245, 250));
    render_html(doc, 20, 10, 440);
    ax_commit(canvas);

    for (;;) {
        ax_event ev;
        int rc = ax_poll(canvas, &ev);
        if (rc == 1) {
            if (ev.type == AX_EV_CLOSE) break;
        } else if (rc < 0) {
            break;
        }
        ax_yield();
    }

    ax_win_close(canvas);
    return 0;
}