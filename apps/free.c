/*
 * free -- вывод памяти (Итого/Использовано/Свободно).
 *
 * Данные берутся из ядра через sysctlbyname (syscall 274):
 *   vm.stats.vm.v_total_count / v_used_count / v_free_count (страницы)
 *   vm.stats.vm.v_page_size                                 (байты)
 *
 * Флаги (по умолчанию -- KiB, как у Linux free):
 *   -b  байты
 *   -k  килобайты
 *   -m  мегабайты
 *   -g  гигабайты
 *
 * Ядро передаёт exec_args_t* в RDI при старте задачи:
 *     typedef struct { int argc; char **argv; } exec_args_t;
 */
#include "../lib/unistd.h"

typedef struct exec_args
{
    int    argc;
    char **argv;
} exec_args_t;

#define FIELD_W 12   /* ширина колонки total/used/free */

static void free_put_char(char c)
{
    write(1, &c, 1);
}

static void free_put_str(const char *s)
{
    write(1, s, strlen((char *)s));
}

/* Число, прижатое вправо внутри колонки FIELD_W */
static void free_put_field_num(u64int v)
{
    char buf[24];
    int  n = 0;

    if (v == 0)
        buf[n++] = '0';

    while (v) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }

    for (int i = n; i < FIELD_W; i++)
        free_put_char(' ');

    while (n)
        free_put_char(buf[--n]);
}

/* Слово, прижатое вправо внутри колонки FIELD_W (как у Linux free) */
static void free_put_field_str(const char *s)
{
    int len = strlen((char *)s);

    for (int i = len; i < FIELD_W; i++)
        free_put_char(' ');

    free_put_str(s);
}

static int get_u64(const char *name, u64int *out)
{
    size_t len = sizeof(u64int);

    return sysctlbyname(name, out, &len, NULL, 0);
}

static void usage(void)
{
    free_put_str("Usage: free [-b|-k|-m|-g]\n");
}

/* одна строка вывода: label + total + used + free */
static void print_row(const char *label,
                      u64int total_b, u64int used_b, u64int free_b,
                      u64int unit)
{
    free_put_str(label);
    free_put_field_num(total_b / unit);
    free_put_field_num(used_b  / unit);
    free_put_field_num(free_b  / unit);
    free_put_char('\n');
}

int main(int argc, char **argv)
{
    u64int page_size = 0, total_p = 0, used_p = 0, free_p = 0;
    u64int unit = 1024;

    /*
     * v_page_size опционален: если ядро его ещё не знает,
     * считаем страницу 4 КиБ и не разваливаем команду.
     */
    get_u64("vm.stats.vm.v_page_size", &page_size);

    /*
     * Деградация на старом ядре: total/used опциональны (появились
     * вместе с таблицей sysctl), v_free_count был с самого начала.
     * Если ядро не отвечает и на него -- команда без цифр невозможна.
     */
    int have_total = (get_u64("vm.stats.vm.v_total_count", &total_p) == 0);
    int have_free  = (get_u64("vm.stats.vm.v_free_count",  &free_p)  == 0);

    /* used не фатален: на будущем «частичном» ядре просто 0 */
    get_u64("vm.stats.vm.v_used_count", &used_p);

    if (!have_free)
    {
        free_put_str("free: sysctl 'vm.stats.vm.v_free_count' failed\n");
        free_put_str("(ядро без syscall 274? пересобери main.elf)\n");
        return 1;
    }

    if (page_size == 0)
        page_size = 4096;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-b")) unit = 1;
        else if (!strcmp(argv[i], "-k")) unit = 1024;
        else if (!strcmp(argv[i], "-m")) unit = 1024ULL * 1024;
        else if (!strcmp(argv[i], "-g")) unit = 1024ULL * 1024 * 1024;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        }
        else {
            free_put_str("free: unknown flag\n");
            usage();
            return 1;
        }
    }

    u64int total_b = total_p * page_size;
    u64int used_b  = used_p  * page_size;
    u64int free_b  = free_p  * page_size;

    if (have_total)
    {
        /* Полный вывод: total / used / free */
        free_put_str("            ");
        free_put_field_str("total");
        free_put_field_str("used");
        free_put_field_str("free");
        free_put_char('\n');

        print_row("Mem:", total_b, used_b, free_b, unit);
    }
    else
    {
        /* Старое ядро (только v_free_count): колонка free */
        free_put_str("            ");
        free_put_field_str("free");
        free_put_char('\n');

        free_put_str("Mem:");
        free_put_field_num(free_b / unit);
        free_put_char('\n');
    }

    return 0;
}

void _start(void *raw_args)
{
    exec_args_t *args = (exec_args_t *)raw_args;

    exit(main(args ? args->argc : 0,
              (args && args->argv) ? args->argv : (char **)NULL));
}
