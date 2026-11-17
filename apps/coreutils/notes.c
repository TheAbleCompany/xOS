#include "../../lib/libax.h"
#include "../../lib/unistd.h"

typedef unsigned long long  u64int;
typedef          long long  s64int;
typedef unsigned int        u32int;
typedef          int        s32int;
typedef unsigned short      u16int;
typedef          short      s16int;
typedef unsigned char       u8int;
typedef          char       s8int;

#define CW 360
#define CH 280

#define BUFFER_SIZE 8192

/*
 * ASCII control codes.
 *
 * Ctrl+S = 0x13
 */
#define CTRL_S 0x13

/*
 * open() flags.
 *
 * Они также должны присутствовать в unistd.h.
 * Эти определения защищены от повторного объявления.
 */
#ifndef O_RDONLY
#define O_RDONLY 0x0000
#endif

#ifndef O_WRONLY
#define O_WRONLY 0x0001
#endif

#ifndef O_RDWR
#define O_RDWR   0x0002
#endif

#ifndef O_CREAT
#define O_CREAT  0x0040
#endif

#ifndef O_TRUNC
#define O_TRUNC  0x0200
#endif


static char buffer[BUFFER_SIZE];
static int  blen = 0;


/*
 * Имя текущего файла.
 */
static char current_file[256];


/*
 * ------------------------------------------------------------
 * Копирование строки
 * ------------------------------------------------------------
 */

static void copy_string(
    char *dst,
    const char *src,
    int maxlen
)
{
    int i;

    if (!dst || !src || maxlen <= 0)
        return;

    for (i = 0; i < maxlen - 1 && src[i]; i++)
        dst[i] = src[i];

    dst[i] = '\0';
}


/*
 * ------------------------------------------------------------
 * Рисование
 * ------------------------------------------------------------
 */

static void render(void)
{
    ax_fill(
        AX_ARGB(
            0xF0,
            28,
            28,
            34
        )
    );

    ax_rect(
        0,
        0,
        CW,
        22,
        AX_RGB(40,40,50)
    );

    ax_text(
        "Notes",
        10,
        7,
        AX_RGB(255,255,255)
    );

    int x = 10;
    int y = 32;

    for (int i = 0; i < blen; i++)
    {
        char c = buffer[i];

        if (c == '\n')
        {
            x = 10;
            y += 16;
            continue;
        }

        /*
         * Не рисуем управляющие символы.
         */
        if ((unsigned char)c < 32)
            continue;

        ax_char(
            c,
            x,
            y,
            AX_RGB(230,230,230)
        );

        x += 8;

        if (x > CW - 16)
        {
            x = 10;
            y += 16;
        }
    }

    /*
     * Курсор.
     */
    ax_rect(
        x,
        y,
        2,
        8,
        AX_RGB(120,180,255)
    );
}


/*
 * ------------------------------------------------------------
 * Сохранение
 * ------------------------------------------------------------
 */

static int save_file(void)
{
    if (current_file[0] == '\0')
        return -1;

    /*
     * O_CREAT:
     *     создать файл, если его нет.
     *
     * O_TRUNC:
     *     очистить старое содержимое.
     *
     * O_WRONLY:
     *     открыть для записи.
     */
    int fd =
        open(
            current_file,
            O_WRONLY | O_CREAT | O_TRUNC,
            0
        );

    if (fd < 0)
        return -1;

    int written = 0;

    while (written < blen)
    {
        int n =
            write(
                fd,
                &buffer[written],
                blen - written
            );

        if (n <= 0)
        {
            close(fd);
            return -1;
        }

        written += n;
    }

    close(fd);

    return 0;
}


/*
 * ------------------------------------------------------------
 * Загрузка файла
 * ------------------------------------------------------------
 */

static void load_file(void)
{
    blen = 0;

    if (current_file[0] == '\0')
        return;

    int fd =
        open(
            current_file,
            O_RDONLY,
            0
        );

    /*
     * Файл не существует.
     *
     * Это нормально:
     * notes просто открывает пустой документ.
     *
     * Он будет реально создан при Ctrl+S.
     */
    if (fd < 0)
        return;

    int n =
        read(
            fd,
            buffer,
            BUFFER_SIZE - 1
        );

    if (n > 0)
    {
        blen = n;
        buffer[blen] = '\0';
    }

    close(fd);
}


/*
 * ------------------------------------------------------------
 * main
 * ------------------------------------------------------------
 */

int main(
    int argc,
    char *argv[]
)
{
    g_canvas =
        ax_surface(
            "Notes",
            CW,
            CH
        );

    ax_canvas_dims(
        CW,
        CH
    );

    if (!g_canvas)
    {
        for (;;)
            yield();
    }


    /*
     * argv[1] — имя файла.
     */
    current_file[0] = '\0';

    if (argc > 1 && argv[1])
    {
        copy_string(
            current_file,
            argv[1],
            sizeof(current_file)
        );
    }


    /*
     * Загружаем существующий файл.
     *
     * Если файла нет — остаётся пустой buffer.
     */
    load_file();


    render();

    ax_commit(g_canvas);


    ax_event ev;


    for (;;)
    {
        while (
            ax_poll(
                g_canvas,
                &ev
            ) > 0
        )
        {
            /*
             * ------------------------------------------------
             * Закрытие окна
             * ------------------------------------------------
             */

            if (ev.type == AX_EV_CLOSE)
                return 0;


            /*
             * ------------------------------------------------
             * Клавиатура
             * ------------------------------------------------
             */

            if (ev.type == AX_EV_KEY)
            {
                /*
                 * ==============================================
                 * Ctrl+S
                 *
                 * ASCII:
                 *
                 * Ctrl+S = 0x13 = 19
                 * ==============================================
                 */

                if (ev.key ==  AX_KEY_CTRL_S)
                {
                    save_file();

                    /*
                     * Важно:
                     *
                     * Ctrl+S НЕ добавляется в buffer.
                     */

                    render();
                    ax_commit(g_canvas);

                    continue;
                }


                /*
                 * ==============================================
                 * Backspace
                 * ==============================================
                 */

                if (ev.key == '\b')
                {
                    if (blen > 0)
                        blen--;

                    buffer[blen] = '\0';
                }


                /*
                 * ==============================================
                 * Обычный символ
                 * ==============================================
                 */

                else if (
                    ev.key >= 32 ||
                    ev.key == '\n' ||
                    ev.key == '\t'
                )
                {
                    if (
                        blen <
                        BUFFER_SIZE - 1
                    )
                    {
                        buffer[blen++] =
                            (char)ev.key;

                        buffer[blen] =
                            '\0';
                    }
                }


                /*
                 * Перерисовываем.
                 */

                render();

                ax_commit(
                    g_canvas
                );
            }
        }

        yield();
    }

    return 0;
}

