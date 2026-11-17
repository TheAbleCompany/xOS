/*
 * ============================================================
 * axshell_launch.c
 * ============================================================
 *
 * App launching.  The shell runs in its own kernel task; when
 * the user picks an app from the dock or the menu, request_launch()
 * just stashes the name and the main loop (compose()) calls
 * app_trampoline() on the next frame to actually load the ELF
 * and spawn the child task.
 *
 * Keeping this in its own file means the ELF + exec_args_t +
 * task_spawn interaction is isolated from the rest of the
 * shell -- the same way fs/exec.c isolates the exec() syscall.
 */

#include "axshell_internal.h"

#include "../fs/fs.h"
#include "../fs/initrd.h"
#include "../fs/task.h"
#include "../mm/kheap.h"

#define malloc kmalloc
#define free   kfree

/*
 * ============================================================
 * Module state
 * ============================================================
 */

/* Legacy hook -- kept for compatibility, not actually used
 * inside the shell.  User-space apps set their own entry. */
void (*current_app_entry)(void);

char     pending_launch[32];
volatile int launch_request = 0;

static int app_running = 0;
void *app_entry_ptr = NULL;
uint32_t *app_canvas_ptr = NULL;

/*
 * ============================================================
 * Launch request (just stashes the name; the actual spawn
 * happens in app_trampoline() on the next compose() tick)
 * ============================================================
 */

void request_launch(const char *elf)
{
    if (launch_request) return;

    serial_puts_ax("REQUEST_LAUNCH: ");
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
 * `elf_name` (task names are the ELF paths the app was spawned
 * with, e.g. "temu", "notes", "files").  If found, bring that
 * window to the front (and un-minimize it) and return the owner
 * PID; otherwise return -1 so the caller launches a new
 * instance.
 */
int app_focus_running(const char *elf_name)
{
    if (!elf_name)
        return -1;

    for (int i = 0; i < AX_MAX_WINDOWS; i++) {
        ax_surface_win *w = &surfaces[i];

        if (!w->used)
            continue;

        Task *t = task_get_by_pid(w->owner_pid);

        if (!t)
            continue;

        if (strcmp(t->name, (char *)elf_name) != 0)
            continue;

        w->minimized = false;
        w->visible = true;
        surf_focus(w);

        return w->owner_pid;
    }

    return -1;
}

/*
 * ============================================================
 * Task wrapper
 * ============================================================
 *
 * Runs inside the child task.  Two cases:
 *
 *   1. entry < AX_USER_VA_MAX: it's a user ELF image.  We can't
 *      just call it from kernel space (its vaddr isn't mapped
 *      here), so we spawn a SECOND task that owns the image and
 *      wait for it to exit.
 *
 *   2. otherwise: it's a kernel-resident app, call directly.
 */

static void app_task_wrapper(void *arg)
{
    uint32_t *canvas = (uint32_t*)arg;

    serial_puts_ax("APP_TASK: starting\n");

    if (app_entry_ptr) {
        uint64_t user_entry = (uint64_t)app_entry_ptr;

        if (user_entry != 0 && user_entry < AX_USER_VA_MAX) {
            /*
             * User ELF image.  image_load() published a pending
             * image; spawn it as a separate task in its own
             * address space.  Direct kernel-space call is
             * impossible: user vaddrs aren't mapped here, the
             * CPU would page-fault forever.
             */
            /*
             * Name the user task after THIS wrapper's task name
             * (the ELF path, e.g. "notes").  The macOS dock uses
             * task names to find running apps, and the window's
             * owner_pid is this user task.
             */
            int pid = task_spawn(
                (void (*)(void *))user_entry,
                arg,      /* exec_args_t* -> RDI */
                runningTask ? runningTask->name : "app"
            );

            if (pid < 0) {
                serial_puts_ax("APP_TASK: user spawn failed\n");
                aximage_pending_discard();
            } else {
                /* Stay alive until the app exits so the shell
                   can track it via task_is_alive(). */
                while (task_is_alive(pid))
                    yield();
            }
        } else {
            void (*app_entry)(uint32_t*) = (void(*)(uint32_t*))app_entry_ptr;
            app_entry(canvas);
        }
    }

    serial_puts_ax("APP_TASK: finished\n");
    task_exit(0);
}

/*
 * ============================================================
 * Trampoline (called from compose() when launch_request is set)
 * ============================================================
 */

/*
 * exec_args_t is defined in fs/exec.c.  We only need the
 * pointer type here; declare it manually so this file doesn't
 * have to pull in the full exec.c declarations.
 */
typedef struct exec_args
{
    int argc;
    char **argv;
} exec_args_t;
extern exec_args_t *exec_copy_args(int argc, char *user_argv[], const char *path);

void app_trampoline(void)
{
    char path[32];
    int k = 0;
    while (pending_launch[k] && k < 31) {
        path[k] = pending_launch[k];
        k++;
    }
    path[k] = 0;

    serial_puts_ax("PATH: ");
    serial_puts_ax(path);
    serial_putc_ax('\n');

    serial_puts_ax("APP_TRAMPOLINE_BEGIN: ");
    serial_puts_ax(path);
    serial_putc_ax('\n');

    /* Locate ELF in initrd */
    fs_node_t *fsnode = finddir_fs(fs_root, path);
    if (!fsnode) {
        serial_puts_ax("APP_FS_NOT_FOUND\n");
        launch_request = 0;
        return;
    }

    /*
     * Allocate the ELF buffer by file size.  The old static
     * 64KB buffer truncated lua-1.0 (~104KB ELF, PT_LOAD
     * reaching ~90KB into the file) -- image_load() got junk
     * for .data and the system hung.  Same disease as the
     * exec() path in fs/exec.c.
     */
    u32int sz = fsnode->length;
    char *buf = (char *)kmalloc(sz ? sz : 1);
    if (!buf) {
        serial_puts_ax("APP_MALLOC_FAIL\n");
        launch_request = 0;
        return;
    }
    memset((u8int*)buf, 0, sz);
    sz = read_fs(fsnode, 0, sz, (u8int*)buf);
    if (!sz) {
        serial_puts_ax("APP_READ_ZERO\n");
        kfree(buf);
        launch_request = 0;
        return;
    }

    serial_puts_ax("APP_IMAGE_LOAD\n");
    void *entry = image_load(buf, sz);
    kfree(buf);   /* image already copied into task memory */
    buf = NULL;
    if (!entry) {
        serial_puts_ax("APP_IMAGE_LOAD_FAIL\n");
        launch_request = 0;
        return;
    }

    serial_puts_ax("APP_JUMP: entry at ");
    uint64_t addr = (uint64_t)entry;
    char t[16];
    int n = 0;
    if (addr == 0) t[n++] = '0';
    while (addr) { t[n++] = '0' + (addr % 10); addr /= 10; }
    while (n) serial_putc_ax(t[--n]);
    serial_putc_ax('\n');

    app_entry_ptr = entry;

    char *app_argv[2];
    app_argv[0] = path;
    app_argv[1] = NULL;

    exec_args_t *args = exec_copy_args(1, app_argv, path);
    if (!args) {
        serial_puts_ax("ARGS ALLOC FAILED\n");
        return;
    }

    int pid = task_spawn(app_task_wrapper, args, path);
    if (pid < 0) {
        serial_puts_ax("TASK_SPAWN_FAIL\n");
        launch_request = 0;
        return;
    }

    serial_puts_ax("APP_TASK_SPAWNED: PID=");
    debug_putnum(pid);
    serial_putc_ax('\n');

    (void)app_running;
    launch_request = 0;
}
