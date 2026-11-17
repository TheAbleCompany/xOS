/*
 * ============================================================
 * cube.c -- AxGL demo: вращающийся куб и треугольник
 * ============================================================
 *
 * Первое OpenGL-приложение ArtyomX OS.  Демонстрирует
 * mini-OpenGL (apps/axgl.h) поверх GPU-стека ixg:
 *
 *   - матрицы PROJECTION/MODELVIEW, перспектива;
 *   - immediate mode: axgl_begin / axgl_color / axgl_vertex3f;
 *   - backface culling + Z-буфер;
 *   - рендер уходит через syscall 175 в GPU (NVIDIA/AMD/Intel
 *     через драйвер ixg), а на виртуалке -- в программный
 *     растеризатор ядра.
 *
 * Клавиши:
 *   1 -- треугольник        2 -- куб
 *   space -- пауза вращения r -- сброс угла
 *   a -- режим AUTO (GPU-сабмит + видимый canvas)
 *   s -- только программный растеризатор (canvas)
 *   g -- только GPU-сабмит (диагностика GPU на реальном железе)
 *
 * Заголовок окна показывает активный бэкенд, режим и FPS.
 */

#include "../lib/libax.h"
#include "../lib/unistd.h"
#include "axgl.h"

typedef unsigned int   u32int;
typedef long long      s64int;

#define CW 480
#define CH 320

/* ============================================================
 * Меню приложения (macOS model)
 * ============================================================ */

static const char *gl_menu_items[] = {
    "Triangle",
    "Cube",
    "Pause Rotation",
    "Close Window"
};

static const ax_menu_def_t demo_menus[] = {
    { "GL", gl_menu_items, 4 },
};

static const ax_menubar_def_t demo_menubar = {
    "GL Demo",
    demo_menus,
    1
};

/* ============================================================
 * Состояние сцены
 * ============================================================ */

#define MODE_TRIANGLE 0
#define MODE_CUBE     1

static int   mode        = MODE_CUBE;
static int   paused      = 0;
static float angle_x     = 25.0f;
static float angle_y     = 35.0f;
static float tri_angle   = 0.0f;

static int   rmode       = AXGL_MODE_AUTO;   /* путь рендеринга */

static uint32_t *cv;
static int       frames = 0;
static int       fps    = 0;
static int       last_sec = -1;

/* ============================================================
 * Геометрия куба: 6 граней, CCW наружу (GL-конвенция)
 * ============================================================ */

static const float cube_faces[6][4][3] = {
    /* +Z (front) */
    { { -1, -1,  1 }, {  1, -1,  1 }, {  1,  1,  1 }, { -1,  1,  1 } },
    /* -Z (back) */
    { {  1, -1, -1 }, { -1, -1, -1 }, { -1,  1, -1 }, {  1,  1, -1 } },
    /* +Y (top) */
    { { -1,  1,  1 }, {  1,  1,  1 }, {  1,  1, -1 }, { -1,  1, -1 } },
    /* -Y (bottom) */
    { { -1, -1, -1 }, {  1, -1, -1 }, {  1, -1,  1 }, { -1, -1,  1 } },
    /* +X (right) */
    { {  1, -1,  1 }, {  1, -1, -1 }, {  1,  1, -1 }, {  1,  1,  1 } },
    /* -X (left) */
    { { -1, -1, -1 }, { -1, -1,  1 }, { -1,  1,  1 }, { -1,  1, -1 } },
};

static const float cube_colors[6][3] = {
    { 0.90f, 0.26f, 0.22f },   /* red    */
    { 1.00f, 0.75f, 0.18f },   /* yellow */
    { 0.20f, 0.80f, 0.40f },   /* green  */
    { 0.25f, 0.55f, 0.95f },   /* blue   */
    { 0.55f, 0.35f, 0.90f },   /* violet */
    { 0.15f, 0.75f, 0.80f },   /* cyan   */
};

/* ============================================================
 * Рендер сцены
 * ============================================================ */

static void draw_triangle(void)
{
    axgl_matrix_mode(AXGL_MODELVIEW);
    axgl_load_identity();
    axgl_translatef(0.0f, -0.12f, -3.2f);
    axgl_rotatef(tri_angle, 0.0f, 0.0f, 1.0f);

    axgl_cull_face(0);          /* одиночный треугольник -- двухсторонний */

    axgl_begin(AXGL_TRIANGLES);

    axgl_color3f(1.00f, 0.30f, 0.35f);
    axgl_vertex3f( 0.0f,  1.1f, 0.0f);

    axgl_color3f(0.30f, 1.00f, 0.55f);
    axgl_vertex3f(-1.1f, -0.9f, 0.0f);

    axgl_color3f(0.35f, 0.55f, 1.00f);
    axgl_vertex3f( 1.1f, -0.9f, 0.0f);

    axgl_end();
}

static void draw_cube(void)
{
    axgl_matrix_mode(AXGL_MODELVIEW);
    axgl_load_identity();
    axgl_translatef(0.0f, 0.0f, -5.0f);
    axgl_rotatef(angle_x, 1.0f, 0.0f, 0.0f);
    axgl_rotatef(angle_y, 0.0f, 1.0f, 0.0f);

    axgl_cull_face(1);          /* замкнутый куб -- задние грани не нужны */

    axgl_begin(AXGL_QUADS);

    for (int f = 0; f < 6; f++) {
        axgl_color3f(cube_colors[f][0],
                     cube_colors[f][1],
                     cube_colors[f][2]);

        for (int v = 0; v < 4; v++) {
            const float *p = cube_faces[f][v];
            /* вершины куба в диапазоне [-1..1] -- слегка масштабируем */
            axgl_vertex3f(p[0] * 1.15f, p[1] * 1.15f, p[2] * 1.15f);
        }
    }

    axgl_end();
}

static void render_frame(void)
{
    ax_time_t t;

    /* Фон -- тёмно-синий, как у классических GL-туториалов. */
    axgl_clear(AXGL_RGB(18, 20, 32));

    axgl_matrix_mode(AXGL_PROJECTION);
    axgl_load_identity();
    axgl_perspective(55.0f, (float)CW / (float)CH, 0.1f, 100.0f);

    if (mode == MODE_CUBE)
        draw_cube();
    else
        draw_triangle();

    /* Заголовок с телеметрией поверх 3D. */
    {
        static char line1[96];
        static char line2[96];
        const char *be = axgl_backend();
        const char *mode_name = (mode == MODE_CUBE) ? "cube" : "triangle";
        const char *rm_name = (rmode == AXGL_MODE_AUTO)     ? "auto"
                            : (rmode == AXGL_MODE_FORCE_SW) ? "software"
                            :                                 "gpu-only";

        line1[0] = 0;
        {
            const char *a = "AxGL: GPU=";
            const char *c = " canvas=";
            const char *d = (rmode == AXGL_MODE_GPU_ONLY) ? "gpu"
                          :                                 "window";
            const char *e = " render=";
            int i = 0, j;
            for (j = 0; a[j]; j++) line1[i++] = a[j];
            for (j = 0; be[j]; j++) line1[i++] = be[j];
            for (j = 0; c[j]; j++) line1[i++] = c[j];
            for (j = 0; d[j]; j++) line1[i++] = d[j];
            for (j = 0; e[j]; j++) line1[i++] = e[j];
            for (j = 0; rm_name[j]; j++) line1[i++] = rm_name[j];
            line1[i] = 0;
        }

        line2[0] = 0;
        {
            const char *a = "mode: ";
            const char *b = "  [1] tri [2] cube  [A]uto [S]w [G]pu-only";
            int i = 0, j;
            for (j = 0; a[j]; j++) line2[i++] = a[j];
            for (j = 0; mode_name[j]; j++) line2[i++] = mode_name[j];
            for (j = 0; b[j]; j++) line2[i++] = b[j];
            line2[i] = 0;
        }

        ax_text(line1, 8, 6, AXGL_RGB(255, 255, 255));
        ax_text(line2, 8, CH - 14, AXGL_RGB(160, 170, 200));

        /* FPS справа на первой строке. */
        {
            static char fps_s[16];
            int v = fps, i = 13, k;
            fps_s[0] = 'F'; fps_s[1] = 'P'; fps_s[2] = 'S'; fps_s[3] = ':'; fps_s[4] = ' ';
            if (v > 9999) v = 9999;
            if (v == 0) { fps_s[5] = '0'; i = 6; }
            else {
                char tmp[8]; k = 0;
                while (v && k < 7) { tmp[k++] = (char)('0' + (v % 10)); v /= 10; }
                i = 5;
                while (k) fps_s[i++] = tmp[--k];
            }
            fps_s[i] = 0;
            ax_text(fps_s, CW - 64, 6, AXGL_RGB(120, 220, 140));
        }
    }

    ax_commit(cv);

    /* FPS: считаем кадры, раз в секунду фиксируем значение. */
    ax_time(&t);
    frames++;
    if (t.second != last_sec) {
        last_sec = t.second;
        fps      = frames;
        frames   = 0;
    }
}

/* ============================================================
 * main
 * ============================================================ */

void _start(void)
{
    ax_event ev;

    ax_menu_set(&demo_menubar);

    cv = ax_surface("GL Demo", CW, CH);
    if (!cv)
        for (;;)
            yield();

    ax_win_focus(cv);

    axgl_init(cv, CW, CH);

    render_frame();

    for (;;) {
        int has_ev = 0;

        while (ax_poll(cv, &ev) > 0) {
            has_ev = 1;

            if (ev.type == AX_EV_CLOSE)
                return;

            if (ev.type == AX_EV_QUIT)
                return;                             /* Cmd+Q */

            if (ev.type == AX_EV_MENU) {
                if (ev.key == 0) {                  /* меню GL */
                    switch (ev.w) {
                        case 0: mode = MODE_TRIANGLE; break;
                        case 1: mode = MODE_CUBE;     break;
                        case 2: paused = !paused;     break;
                        case 3: ax_win_close(cv); return;
                    }
                }
                continue;
            }

            if (ev.type == AX_EV_KEY) {
                switch (ev.key) {
                    case '1': mode = MODE_TRIANGLE; break;
                    case '2': mode = MODE_CUBE;     break;
                    case ' ': paused = !paused;     break;
                    case 'a':
                    case 'A': rmode = axgl_set_render_mode(AXGL_MODE_AUTO);     break;
                    case 's':
                    case 'S': rmode = axgl_set_render_mode(AXGL_MODE_FORCE_SW); break;
                    case 'g':
                    case 'G': rmode = axgl_set_render_mode(AXGL_MODE_GPU_ONLY); break;
                    case 'r':
                    case 'R':
                        angle_x = 25.0f;
                        angle_y = 35.0f;
                        tri_angle = 0.0f;
                        break;
                }
            }
        }

        (void)has_ev;

        /* Анимация: фиксированный шаг на кадр -- стабильно и
         * без зависимости от таймеров (в OS нет ms-времени). */
        if (!paused) {
            angle_y   += 1.6f;
            angle_x   += 0.9f;
            tri_angle += 2.2f;
            if (angle_x > 360.0f) angle_x -= 360.0f;
            if (angle_y > 360.0f) angle_y -= 360.0f;
            if (tri_angle > 360.0f) tri_angle -= 360.0f;
        }

        render_frame();
        yield();
    }
}
