/*
 * lion_harness.c -- хост-харнесс WindowServer ArtyomX64.
 *
 * Собирается ВМЕСТО ядра с -DHOST_HARNESS на обычном Linux:
 *   gcc -m64 -O1 -DHOST_HARNESS -Iserver -Ilib \
 *       server/*.c scripts/lion_harness.c -o lion_harness
 *
 * Рисует реальные кадры compose() в памяти и вываливает BMP:
 *   desktop / launchpad / mission / fullscreen.
 * Нужен только для визуальной проверки GUI без запуска ОС в QEMU.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
/* NOTE: <string.h> не включаем -- lib/common.h объявляет strtok
 * с kernel-сигнатурой, конфликтующей с glibc. */

#include "wserver_internal.h"

void gfx_host_init(uint32_t *buf, uint32_t *blurtmp_buf, int w, int h);

/* ---------------- resolution ---------------- */
#define HW 1024
#define HH 768

static uint32_t backbuf[HW * HH];
static uint32_t blurbuf[HW * HH];

/* ---------------- BMP writer ---------------- */
static void write_bmp(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); return; }

    int row_bytes = w * 4;
    int data_size = row_bytes * h;
    int file_size = 54 + data_size;

    unsigned char hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = file_size & 0xFF; hdr[3] = (file_size >> 8) & 0xFF;
    hdr[4] = (file_size >> 16) & 0xFF; hdr[5] = (file_size >> 24) & 0xFF;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
    hdr[20] = (w >> 16) & 0xFF; hdr[21] = (w >> 24) & 0xFF;
    hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
    hdr[24] = (h >> 16) & 0xFF; hdr[25] = (h >> 24) & 0xFF;
    hdr[26] = 1; hdr[28] = 32;

    fwrite(hdr, 1, 54, f);

    unsigned char *row = (unsigned char *)malloc(row_bytes);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t c = px[y * w + x];
            row[x * 4 + 0] = (c >> 0)  & 0xFF;  /* B */
            row[x * 4 + 1] = (c >> 8)  & 0xFF;  /* G */
            row[x * 4 + 2] = (c >> 16) & 0xFF;  /* R */
            row[x * 4 + 3] = 0;
        }
        fwrite(row, 1, row_bytes, f);
    }
    free(row);
    fclose(f);
    printf("wrote %s\n", path);
}

/* ---------------- fake app content ---------------- */

/* имитация содержимого приложения: шапка + строки "текста" */
static void fill_app_canvas(ax_surface_win *w, uint32_t accent, int rows)
{
    if (!w->canvas) return;
    int cw = w->canvas_w, ch = w->canvas_h;

    /* header band */
    for (int y = 0; y < 40 && y < ch; y++)
        for (int x = 0; x < cw; x++)
            w->canvas[y * cw + x] = gfx_lerp_color(accent, RGB(20,22,28),
                                                   y * 256 / 40);
    /* paper body */
    for (int y = 40; y < ch; y++)
        for (int x = 0; x < cw; x++)
            w->canvas[y * cw + x] = gfx_lerp_color(RGB(252,252,250),
                                                   RGB(236,236,232),
                                                   y * 160 / (ch > 40 ? ch : 1));
    /* fake text lines */
    for (int r = 0; r < rows; r++) {
        int ly = 56 + r * 22;
        if (ly + 6 >= ch) break;
        int len = 60 + ((r * 53) % (cw / 3));
        if (len > cw - 40) len = cw - 40;
        for (int x = 20; x < 20 + len && x < cw - 8; x++)
            for (int t = 0; t < 5; t++)
                w->canvas[(ly + t) * cw + x] = RGB(120,124,134);
    }
}

/* ---------------- инъекция кликов (автотест фикса ввода) ---------------- */

static int clicks_checked = 0;
static int clicks_failed = 0;

static void click(int x, int y)
{
    mouse.prev_x = mouse.x; mouse.prev_y = mouse.y;
    mouse.x = x; mouse.y = y;
    mouse.prev_left = mouse.left;
    mouse.left = true;
    mouse.clicked = true;
    mouse.released = false;

    process_input();

    /* отпускание */
    mouse.prev_left = mouse.left;
    mouse.left = false;
    mouse.clicked = false;
    mouse.released = true;
    process_input();
    mouse.released = false;
}

#define CHECK(name, cond) do { \
    clicks_checked++; \
    if (cond) printf("TEST OK   : %s\n", name); \
    else { printf("TEST FAIL : %s\n", name); clicks_failed++; } \
} while (0)

/* ---------------- сценарий ---------------- */

int main(void)
{
    gfx_host_init(backbuf, blurbuf, HW, HH);

    scr_w = HW;
    scr_h = HH;

    mouse.x = HW / 2; mouse.y = HH / 2;
    mouse.prev_x = mouse.x; mouse.prev_y = mouse.y;
    mouse.left = false; mouse.prev_left = false;
    mouse.clicked = false; mouse.released = false;

    memset((void *)surfaces, 0, sizeof(surfaces));
    memset((void *)photos, 0, sizeof(photos));

    init_menus();
    scan_photos();          /* на хосте ничего не найдёт -- space WP */

    /* окно 1: Notes */
    int id1 = surf_alloc("Notes", 430, 320, 0);
    if (id1 >= 0) {
        surfaces[id1].owner_pid = 101;
        fill_app_canvas(&surfaces[id1], RGB(255,200,80), 9);
    }

    /* окно 2: Temu (второе -- проверка z/hit-test на перекрытии) */
    int id2 = surf_alloc("Temu", 380, 260, 0);
    if (id2 >= 0) {
        surfaces[id2].owner_pid = 102;
        surfaces[id2].x = 480; surfaces[id2].y = 120;
        fill_app_canvas(&surfaces[id2], RGB(40,180,90), 6);
    }

    /* --- кадр: обычный десктоп, курсор над доком --- */
    for (int f = 0; f < 40; f++) { mouse.x = 512; mouse.y = 730; compose(); }
    write_bmp("/tmp/lion_shots/desktop.bmp", backbuf, HW, HH);

    /* ====================================================
     * АВТОТЕСТ ВВОДА: воспроизведение и проверка бага
     * "окна при масштабировании не реагируют на кнопки
     * (светофоры тоже)".
     * ==================================================== */

    /* 1. зелёная кнопка обычного окна -> fullscreen
     *    (AX_BTN_C3 -- ЛОКАЛЬНАЯ x кнопки: экранная = x окна + C3) */
    click(surfaces[id1].x + AX_BTN_C3, surfaces[id1].y + 14);
    CHECK("green button enters fullscreen",
          surfaces[id1].fullscreen && surfaces[id1].w == HW);

    /* 2. в fullscreen жёлтая кнопка полноэкранного окна должна
     *    работать: раньше верхнюю полосу (24px) глотал невидимый
     *    меню-бар, и светофоры были мертвы.  Минимизация играет
     *    genie-анимацию -- прокручиваем кадры до её завершения. */
    click(AX_BTN_C2, 14);
    for (int f = 0; f < AX_FADE_FRAMES * 2; f++) compose();
    CHECK("yellow button works in fullscreen",
          surfaces[id1].minimized);

    /* 3. разворачиваем окно обратно (вручную) */
    surfaces[id1].minimized = false;
    surfaces[id1].fullscreen = false;
    surfaces[id1].w = HW; surfaces[id1].h = HH - AX_TITLEBAR_H;
    compose();

    /* 4. клик по СКРЫТОМУ доку в fullscreen не запускает приложения
     *    (раньше невидимый док ел клики нижней полосы экрана) */
    toggle_fullscreen(&surfaces[id1]);
    int lr_before = launch_request;
    click(512, HH - 28);
    {
        int cx = dock_icon_cx_by_elf("temu");
        if (cx < 0) cx = 512;
        click(cx, HH - 28);
    }
    CHECK("hidden dock in fullscreen does not eat clicks",
          launch_request == lr_before);

    /* 5. клик по тайтлбару fullscreen-окна не открывает меню и не
     *    тащит окно (хром мёртв вне зоны reveal) */
    click(300, 14);
    CHECK("fullscreen titlebar click does not open menubar/drag",
          open_menu < 0 && !surfaces[id1].dragging);

    /* 6. масштаб координат мыши: канвас 430x320 в окне 1024x740 --
     *    клик (512,400) обязан прийти приложению как (215,160).
     *    Значения печатаются и дублируются в serial-логе
     *    "EVENT: MOUSE to Notes at X,Y" (проверяется снаружи). */
    printf("TEST INFO : canvas %ux%u window %dx%d -> click (512,400) must log "
           "(%d,%d) for Notes\n",
           surfaces[id1].canvas_w, surfaces[id1].canvas_h,
           surfaces[id1].w, surfaces[id1].h,
           512 * (int)surfaces[id1].canvas_w / surfaces[id1].w,
           (400 - 28) * (int)surfaces[id1].canvas_h /
               (surfaces[id1].h > 0 ? surfaces[id1].h : 1));
    click(512, 400);

    /* 7. выход из fullscreen зелёной кнопкой */
    click(AX_BTN_C3, 14);
    CHECK("green button exits fullscreen",
          !surfaces[id1].fullscreen);

    printf("TESTS: %d checked, %d failed\n", clicks_checked, clicks_failed);

    /* --- восстановление геометрии для скриншотов --- */
    if (id1 >= 0) {
        surfaces[id1].x = (HW - 430) / 2;
        surfaces[id1].y = AX_MENUBAR_H + 40;
        surfaces[id1].w = 430; surfaces[id1].h = 320;
        surfaces[id1].minimized = false;
        surfaces[id1].fullscreen = false;
        surfaces[id1].maximized = false;
    }
    if (id2 >= 0) {
        surfaces[id2].x = 480; surfaces[id2].y = 120;
        surfaces[id2].w = 380; surfaces[id2].h = 260;
        surf_focus(&surfaces[id2]);
    }

    /* --- Launchpad (F4), курсор над одной из иконок --- */
    lion_toggle_launchpad();
    for (int f = 0; f < 16; f++) { mouse.x = 384; mouse.y = 330; compose(); }
    write_bmp("/tmp/lion_shots/launchpad.bmp", backbuf, HW, HH);
    lion_overlays_close();

    /* --- Mission Control (F3) --- */
    lion_toggle_mission();
    for (int f = 0; f < 16; f++) { mouse.x = 260; mouse.y = 300; compose(); }
    write_bmp("/tmp/lion_shots/mission.bmp", backbuf, HW, HH);
    lion_overlays_close();

    /* --- Fullscreen (зелёная кнопка / F11) --- */
    if (id1 >= 0) {
        toggle_fullscreen(&surfaces[id1]);
        for (int f = 0; f < 20; f++) { mouse.x = 512; mouse.y = 400; compose(); }
        write_bmp("/tmp/lion_shots/fullscreen.bmp", backbuf, HW, HH);

        /* reveal меню-бара у верхнего края */
        for (int f = 0; f < 4; f++) { mouse.x = 512; mouse.y = 1; compose(); }
        write_bmp("/tmp/lion_shots/fullscreen_bar.bmp", backbuf, HW, HH);

        toggle_fullscreen(&surfaces[id1]);
        for (int f = 0; f < 20; f++) { mouse.x = 512; mouse.y = 400; compose(); }
    }

    printf("HARNESS OK\n");
    return clicks_failed ? 1 : 0;
}
