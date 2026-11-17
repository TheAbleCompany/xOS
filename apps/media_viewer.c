 #include "../lib/libax.h"
 #include "imgload/imgload.h"
#include "../lib/unistd.h"
static int CW = 640;
static int CH = 400;
static uint32_t sanitar_px[192 * 100];       /*  76 KiB */
static uint32_t chan_px[320 * 192];          /* 240 KiB */
static uint32_t gover_px[512 * 320];         /* 640 KiB */

static img_image_t sanitar_img;
static img_image_t chan_img;
static img_image_t gover_img;
typedef unsigned int   u32;
typedef struct exec_args
{
    int    argc;
    char **argv;
} exec_args_t;

void _start(void *raw_args)
{
    exec_args_t *args = (exec_args_t *)raw_args;
        uint32_t *canvas = ax_surface("Media Viewer", CW, CH);
        if (!canvas)
            return 1;
        int w = 0;
        int h = 0;
       
        //ax_fill(AX_RGB(245, 245, 250));
        /*ax_text("Привет, ArtyomX64!", 24, 40, AX_RGB(30, 30, 40));
        ax_rect(24, 64, 180, 2, AX_RGB(10, 132, 255));*/
        //img_query_file(argv[1], w, h);
       img_load_file(args->argv[1],
                              gover_px, 512 * 320,
                              &gover_img);
         /* stretch 512x320 over the whole window */
        for (int y = 0; y < CH; y++) {
            int syy = y * gover_img.h / CH;

            const uint32_t *src = gover_img.pix + syy * gover_img.w;
            uint32_t *dstrow = canvas + y * g_cw;

            for (int x = 0; x < CW; x++) {
                int sxx = x * gover_img.w / CW;

                u32 c = src[sxx];

                dstrow[x] = AX_RGB((c >> 16) & 0xFF,
                                   (c >> 8) & 0xFF,
                                   c & 0xFF);
            }
        }
        ax_commit(canvas);

        for (;;) {
            ax_event ev;
            int rc = ax_poll(canvas, &ev);

            if (rc == 1) {
                if (ev.type == AX_EV_CLOSE)
                    break;

                if (ev.type == AX_EV_RESIZE) {
                    /* После изменения размера перечитайте размеры
                       канваса и перерисуйте содержимое. */
                }
            } else if (rc < 0) {
                break; /* соединение с GUI недоступно */
            }

            ax_yield();
        }

        ax_win_close(canvas);
        return 0;
    }
