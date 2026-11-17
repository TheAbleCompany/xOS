#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define MAX_FILES 96

#define INITRD_MAGIC 0xBF

#define FS_FILE 0x01

#define FS_APP_NONE     0
#define FS_APP_CONSOLE  1
#define FS_APP_GUI      2


struct initrd_header
{
    unsigned char magic;

    unsigned char type;
    unsigned char app_type;

    char name[64];

    unsigned int offset;
    unsigned int length;
};


/*
 * ------------------------------------------------------------
 * app_type parser
 * ------------------------------------------------------------
 */

static unsigned char parse_app_type(
    const char *s
)
{
    if (!s)
        return FS_APP_NONE;

    if (strcmp(s, "console") == 0)
        return FS_APP_CONSOLE;

    if (strcmp(s, "gui") == 0)
        return FS_APP_GUI;

    if (strcmp(s, "none") == 0)
        return FS_APP_NONE;

    return 0xFF;
}


/*
 * ------------------------------------------------------------
 * main
 * ------------------------------------------------------------
 *
 * Usage:
 *
 *     makeinitrd host name type ...
 *
 * Example:
 *
 *     makeinitrd \
 *         calculator calculator gui \
 *         xsh xsh console \
 *         readme readme none
 *
 * ------------------------------------------------------------
 */

int main(
    int argc,
    char **argv
)
{
    /*
     * Каждый файл теперь описывается тремя аргументами:
     *
     *     host_file
     *     initrd_name
     *     app_type
     */

    if (argc < 4 ||
        ((argc - 1) % 3) != 0)
    {
        printf(
            "Usage:\n"
            "  %s host name type ...\n\n"
            "Types:\n"
            "  none\n"
            "  console\n"
            "  gui\n",
            argv[0]
        );

        return 1;
    }


    int nheaders =
        (argc - 1) / 3;


    if (nheaders > MAX_FILES)
    {
        printf(
            "Error: too many files "
            "(max %d)\n",
            MAX_FILES
        );

        return 1;
    }


    struct initrd_header headers[MAX_FILES];

    memset(
        headers,
        0,
        sizeof(headers)
    );


    /*
     * В начале:
     *
     *     int nfiles
     *
     * затем 64 заголовка.
     */

    unsigned int off =
        sizeof(int) +
        sizeof(struct initrd_header) *
        MAX_FILES;


    printf(
        "size of header: %zu\n",
        sizeof(struct initrd_header)
    );


    /*
     * --------------------------------------------------------
     * Подготовка заголовков
     * --------------------------------------------------------
     */

    int i;

    for (i = 0; i < nheaders; i++)
    {
        const char *host_file =
            argv[i * 3 + 1];

        const char *name =
            argv[i * 3 + 2];

        const char *app_name =
            argv[i * 3 + 3];


        unsigned char app_type =
            parse_app_type(app_name);


        if (app_type == 0xFF)
        {
            printf(
                "Error: invalid app type "
                "'%s'\n",
                app_name
            );

            return 1;
        }


        if (strlen(name) >= 64)
        {
            printf(
                "Error: filename too long: %s\n",
                name
            );

            return 1;
        }


        FILE *stream =
            fopen(
                host_file,
                "rb"
            );


        if (!stream)
        {
            printf(
                "Error: file not found: %s\n",
                host_file
            );

            return 1;
        }


        fseek(
            stream,
            0,
            SEEK_END
        );


        long file_length =
            ftell(stream);


        fclose(stream);


        if (file_length < 0)
        {
            printf(
                "Error: cannot determine size: %s\n",
                host_file
            );

            return 1;
        }


        /*
         * Заполняем metadata.
         */

        headers[i].magic =
            INITRD_MAGIC;

        headers[i].type =
            FS_FILE;

        headers[i].app_type =
            app_type;


        strcpy(
            headers[i].name,
            name
        );


        headers[i].offset =
            off;

        headers[i].length =
            (unsigned int)file_length;


        printf(
            "writing file %s -> %s "
            "(type=%s, offset=0x%x, size=%u)\n",
            host_file,
            name,
            app_name,
            headers[i].offset,
            headers[i].length
        );


        off +=
            headers[i].length;
    }


    /*
     * --------------------------------------------------------
     * Создаём initrd.img
     * --------------------------------------------------------
     */

    FILE *wstream =
        fopen(
            "./initrd.img",
            "wb"
        );


    if (!wstream)
    {
        printf(
            "Error: cannot create initrd.img\n"
        );

        return 1;
    }


    /*
     * Количество файлов.
     */

    fwrite(
        &nheaders,
        sizeof(int),
        1,
        wstream
    );


    /*
     * Всегда записываем все 64 заголовка.
     * Это сохраняет твою существующую архитектуру initrd.
     */

    fwrite(
        headers,
        sizeof(struct initrd_header),
        MAX_FILES,
        wstream
    );


    /*
     * --------------------------------------------------------
     * Записываем содержимое файлов
     * --------------------------------------------------------
     */

    for (i = 0; i < nheaders; i++)
    {
        FILE *stream =
            fopen(
                argv[i * 3 + 1],
                "rb"
            );


        if (!stream)
        {
            printf(
                "Error: cannot open: %s\n",
                argv[i * 3 + 1]
            );

            fclose(wstream);

            return 1;
        }


        unsigned char *buf =
            NULL;


        if (headers[i].length > 0)
        {
            buf =
                (unsigned char *)malloc(
                    headers[i].length
                );


            if (!buf)
            {
                printf(
                    "Error: out of memory\n"
                );

                fclose(stream);
                fclose(wstream);

                return 1;
            }


            size_t got =
                fread(
                    buf,
                    1,
                    headers[i].length,
                    stream
                );


            if (got != headers[i].length)
            {
                printf(
                    "Error: read failed: %s\n",
                    argv[i * 3 + 1]
                );

                free(buf);
                fclose(stream);
                fclose(wstream);

                return 1;
            }


            fwrite(
                buf,
                1,
                headers[i].length,
                wstream
            );


            free(buf);
        }


        fclose(stream);
    }


    fclose(wstream);


    printf(
        "initrd.img created: %u bytes\n",
        off
    );


    return 0;
}