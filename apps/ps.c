/*
 * ps -- список процессов (задач) ОС.
 *
 * Данные берутся из ядра через task_list (syscall 275):
 * ядро отдаёт массив {int pid; int state; char name[24];}
 * по живым задачам (state != TERMINATED).
 *
 * Обозначения состояния:
 *   R  -- running (выполняется сейчас)
 *   r  -- ready   (готов, ждёт квант)
 *   B  -- blocked (ждёт события)
 */
#include "../lib/unistd.h"

typedef struct exec_args
{
    int    argc;
    char **argv;
} exec_args_t;

/* Раскладка обязана совпадать с task_info_t в multitask/task.h */
typedef struct {
    int  pid;
    int  state;
    char name[24];
} ps_entry_t;

#define PS_MAX 16   /* не больше MAX_TASKS в ядре */

static void ps_put_char(char c)
{
    write(1, &c, 1);
}

static void ps_put_str(const char *s)
{
    write(1, s, strlen((char *)s));
}

/* Число, прижатое вправо в колонку шириной w */
static void ps_put_num_right(int v, int w)
{
    char buf[16];
    int  n = 0;

    if (v == 0)
        buf[n++] = '0';

    if (v < 0) {
        ps_put_char('-');
        v = -v;
    }

    while (v) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }

    for (int i = n; i < w; i++)
        ps_put_char(' ');

    while (n)
        ps_put_char(buf[--n]);
}

static char state_char(int state)
{
    switch (state) {
        case 2:  return 'R';   /* TASK_RUNNING  */
        case 1:  return 'r';   /* TASK_READY    */
        case 3:  return 'B';   /* TASK_BLOCKED  */
        default: return '?';
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ps_entry_t tab[PS_MAX];

    int n = task_list(tab, PS_MAX);

    if (n < 0) {
        ps_put_str("ps: syscall task_list failed\n");
        return 1;
    }

    if (n == 0) {
        ps_put_str("ps: no tasks\n");
        return 0;
    }

    /* Шапка */
    ps_put_str("  PID STATE NAME\n");

    for (int i = 0; i < n; i++) {
        ps_put_num_right(tab[i].pid, 5);
        ps_put_char(' ');
        ps_put_char(state_char(tab[i].state));
        ps_put_char(' ');
        ps_put_char(' ');
        ps_put_str(tab[i].name);
        ps_put_char('\n');
    }

    return 0;
}

void _start(void *raw_args)
{
    exec_args_t *args = (exec_args_t *)raw_args;

    exit(main(args ? args->argc : 0,
              (args && args->argv) ? args->argv : (char **)NULL));
}
