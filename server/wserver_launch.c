/*
 * ============================================================
 * wserver_launch.c
 * ============================================================
 *
 * LaunchServices WindowServer'а: запуск приложений из дока и
 * меню и фокусировка уже запущенных (macOS dock model).
 *
 * Бывший kernel/axshell_launch.c сильно УПРОСТИЛСЯ: обвязка
 * image_load + task_spawn + app_task_wrapper больше не нужна --
 * сервер запускает приложения обычным exec() (сисколл 11), тем
 * же, каким приложения запускают друг друга.  Дети wserver'а
 * получают собственные адресные пространства и имена по ELF-пути
 * (по ним dock находит уже запущенные копии).
 *
 * request_launch() только запоминает имя; фактический exec()
 * выполняется в app_trampoline() из главного цикла compose() --
 * как и раньше, чтобы док не блокировался на время загрузки ELF.
 */

#include "wserver_internal.h"

/*
 * ============================================================
 * Module state
 * ============================================================
 */

char     pending_launch[32];
volatile int launch_request = 0;

/*
 * ============================================================
 * Launch request (just stashes the name; the actual exec()
 * happens in app_trampoline() on the next compose() tick)
 * ============================================================
 */

void request_launch(const char *elf)
{
    if (launch_request) return;

    serial_puts_ax("WSERVER REQUEST_LAUNCH: ");
    serial_puts_ax(elf);
    serial_putc_ax('\n');

    int k = 0;
    while (elf[k] && k < 31) { pending_launch[k] = elf[k]; k++; }
    pending_launch[k] = 0;
    launch_request = 1;
}

/*
 * ============================================================
 * Running-app lookup (macOS dock model)
 * ============================================================
 *
 * Find a window owned by a live task whose task name matches
 * `elf_name` (task names are the ELF paths, e.g. "temu",
 * "notes" -- их assigns exec()).  If found, bring that window
 * to the front (and un-minimize it) and return the owner PID;
 * otherwise return -1 so the caller launches a new instance.
 *
 * Имя задачи берётся сисколлом PROC_NAME (192) -- сервер, как
 * обычный процесс, не видит таблицу задач ядра.
 */

int app_focus_running(const char *elf_name)
{
    if (!elf_name)
        return -1;

    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *w = &surfaces[i];

        if (!w->used)
            continue;

        char tname[32];
        if (proc_name_sys(w->owner_pid, tname, sizeof(tname)) != 0)
            continue;

        if (strcmp(tname, (char *)elf_name) != 0)
            continue;

        bool was_minimized = w->minimized;

        w->minimized = false;
        w->visible = true;

        /* Lion: restoring from the dock plays the genie in reverse */
        if (was_minimized) {
            w->anim_kind = AX_ANIM_RESTR;
            w->anim_t = 0;
        }

        surf_focus(w);

        return w->owner_pid;
    }

    return -1;
}

/*
 * True when a live task with this ELF name owns at least one
 * window -- feeds the dock running-app indicator dots.
 * (Аналог axshell_launch.c, но через PROC_NAME-сисколл.)
 */
bool app_is_running(const char *elf_name)
{
    if (!elf_name)
        return false;

    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *w = &surfaces[i];

        if (!w->used)
            continue;

        char tname[32];
        if (proc_name_sys(w->owner_pid, tname, sizeof(tname)) != 0)
            continue;

        if (strcmp(tname, (char *)elf_name) == 0)
            return true;
    }

    return false;
}

/*
 * ============================================================
 * Trampoline (called from compose() when launch_request is set)
 * ============================================================
 */

void app_trampoline(void)
{
    char path[32];
    int k = 0;
    while (pending_launch[k] && k < 31) {
        path[k] = pending_launch[k];
        k++;
    }
    path[k] = 0;

    serial_puts_ax("WSERVER APP_TRAMPOLINE: ");
    serial_puts_ax(path);
    serial_putc_ax('\n');

    /*
     * LaunchServices в одном вызове: exec() сам читает ELF из
     * initrd, создаёт адресное пространство и задачу.  GUI-приложения
     * stdio не получают (program_type = gui).
     */
    int pid = exec_sys(path, 0, 0);
    if (pid < 0) {
        serial_puts_ax("WSERVER APP_EXEC_FAIL: ");
        serial_puts_ax(path);
        serial_putc_ax('\n');
        launch_request = 0;
        return;
    }

    serial_puts_ax("WSERVER APP_SPAWNED: PID=");
    {
        int p = pid; char t[12]; int n = 0;
        if (p == 0) t[n++] = '0';
        while (p) { t[n++] = '0' + (p % 10); p /= 10; }
        while (n) serial_putc_ax(t[--n]);
        serial_putc_ax('\n');
    }

    launch_request = 0;
}
