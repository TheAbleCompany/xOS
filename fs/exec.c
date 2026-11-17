/*
 * ============================================================
 * fs/exec.c -- exec() syscall (number 11), kernel-side exit()
 *              and the exec_args_t plumbing shared with the
 *              axshell app trampoline.
 * ============================================================
 *
 * exec(path, argc, argv) loads an ELF from the initrd, spawns a
 * new task with its own address space (via image_load() +
 * task_spawn()) and returns the child PID to the caller.
 *
 * Argument model (matches every caller):
 *
 *   temu:   exec("notes", 2, {"notes", "hw.txt"})
 *           -> child sees argc=2, argv[1]="hw.txt"
 *   files:  exec("notes", 0, NULL)
 *           -> child sees argc=1, argv[0]="notes"
 *   xsh:    exec("lua", 2, {"lua", "/x.lua"})
 *           -> child sees the argv verbatim
 *
 * The child inherits the caller's controlling terminal: fd 0..2
 * are wired to the caller's ctty slave, so console children
 * spawned from a terminal window read/write the same PTY.
 */

#include "fs.h"
#include "initrd.h"
#include "../multitask/task.h"
#include "../multitask/elf.h"
#include "../mm/kheap.h"
#include "../lib/common.h"

#include <stdint.h>
#include <stddef.h>

#define malloc kmalloc
#define free   kfree

/*
 * exec_args_t is placed in RDI when the child task starts
 * (see app_trampoline / mini-libc crt).
 */
typedef struct exec_args
{
    int    argc;
    char **argv;
} exec_args_t;

void *image_load(char *elf_start, unsigned int size);
int aximage_pending_abi(void);

/*Defined in multitask/task.c*/
void aximage_pending_discard(void);

#define EXEC_MAX_ARGS   16
#define EXEC_ARG_MAXLEN 128

/*
 * Copy the caller's argv into kernel memory so the child owns
 * private argument storage.  Called while the caller's address
 * space is active, so user-space pointers are dereferenceable.
 */
exec_args_t *exec_copy_args(int argc, char *user_argv[], const char *path)
{
    exec_args_t *args =
        (exec_args_t *)malloc(sizeof(exec_args_t));

    if (!args)
        return 0;

    char **argv =
        (char **)malloc(sizeof(char *) * (EXEC_MAX_ARGS + 1));

    if (!argv) {
        free(args);
        return 0;
    }

    memset((u8int *)argv, 0, sizeof(char *) * (EXEC_MAX_ARGS + 1));

    int n = 0;

    if (argc > 0 && user_argv) {
        for (int i = 0; i < argc && n < EXEC_MAX_ARGS; i++) {
            const char *src = user_argv[i];

            if (!src)
                src = "";

            char *dst = (char *)malloc(EXEC_ARG_MAXLEN);

            if (!dst)
                break;

            int j = 0;

            while (src[j] && j < EXEC_ARG_MAXLEN - 1) {
                dst[j] = src[j];
                j++;
            }
            dst[j] = 0;

            argv[n++] = dst;
        }
    }

    if (n == 0) {
        /* No usable argv -- synthesize argv[0] from the path. */
        char *dst = (char *)malloc(EXEC_ARG_MAXLEN);

        if (!dst) {
            free(argv);
            free(args);
            return 0;
        }

        int j = 0;

        if (path) {
            while (path[j] && j < EXEC_ARG_MAXLEN - 1) {
                dst[j] = path[j];
                j++;
            }
        }
        dst[j] = 0;

        argv[n++] = dst;
    }

    args->argc = n;
    args->argv = argv;

    return args;
}

int exec(const char *path, int argc, char *argv[])
{
    if (!path || !path[0])
        return -1;

   fs_node_t *node = resolve_path(path);
    if (!node) {
        debug_puts("EXEC: not found: ");
        debug_puts((char *)path);
        debug_putc('\n');
        return -1;
    }

    if (node->length < 4)
        return -1;

    u32int sz = node->length;

    char *buf = (char *)malloc(sz);

    if (!buf) {
        debug_puts("EXEC: malloc failed\n");
        return -1;
    }

    memset((u8int *)buf, 0, sz);

    u32int got = read_fs(node, 0, sz, (u8int *)buf);

    if (!got) {
        free(buf);
        debug_puts("EXEC: read failed\n");
        return -1;
    }

    void *entry = image_load(buf, got);

    free(buf);   /* image_load copies the image into task memory */

    if (!entry) {
        debug_puts("EXEC: image_load failed: ");
        debug_puts((char *)path);
        debug_putc('\n');
        return -1;
    }

    exec_args_t *args = exec_copy_args(argc, argv, path);

    if (!args) {
        aximage_pending_discard();
        debug_puts("EXEC: args alloc failed\n");
        return -1;
    }

    int pid = task_spawn(
        (void (*)(void *))entry,
        args,
        path
    );

    if (pid < 0) {
        aximage_pending_discard();
        debug_puts("EXEC: spawn failed\n");
        return -1;
    }

    /*
     * Личность ABI: образ Linux i386 / x86-64 / FreeBSD получает
     * собственную нумерацию системных вызовов (kernel/abi.c).
     * Родные программы остаются с AX_ABI_NATIVE.
     */
    {
        int abi = aximage_pending_abi();
        Task *child = task_get_by_pid(pid);

        if (child) {
            child->abi = abi;

            if (abi != AX_ABI_NATIVE) {
                debug_puts("EXEC: ABI mode ");
                debug_putnum((uint64_t)abi);
                debug_puts(" for ");
                debug_puts((char *)path);
                debug_putc('\n');
            }
        }
    }

    /*
     * Console children inherit the caller's controlling terminal:
     * fd 0..2 -> the caller's ctty slave node.  GUI apps keep
     * their FD table empty (they never read stdin).
     */
    if (node->program_type != PROGRAM_GUI && runningTask && runningTask->ctty_slave)
        task_setup_stdio_pid(pid, runningTask->ctty_slave);

    debug_puts("EXEC: spawned PID=");
    debug_putnum((uint64_t)pid);
    debug_puts(" (");
    debug_puts((char *)path);
    debug_puts(")\n");

    return pid;
}

/*
 * Тип программы по пути (syscall 197 FP_TYPE):
 *
 *   -1             файл не найден
 *   PROGRAM_NONE   не помечен (все файлы disk0/FAT/EXT)
 *   PROGRAM_CONSOLE помечен "console" в mkinitrd2
 *   PROGRAM_GUI    помечен "gui" в mkinitrd2
 *
 * Тип живёт в fs_node_t::program_type -- initrd переносит его из
 * заголовка образа (fs/initrd.c).  Файловому менеджеру этого
 * достаточно, чтобы запускать консольные ELF'ы в терминале.
 */
int fp_type(const char *path)
{
    if (!path || !path[0])
        return -1;

    fs_node_t *node = resolve_path(path);

    if (!node)
        return -1;

    return (int)node->program_type;
}

/*
 * Kernel-side exit(): syscall 1 lands here.  task_exit() never
 * returns -- it switches to the next runnable task.
 */
int exit(int code)
{
    task_exit(code);

    /*
     * Unreachable, but keep the compiler happy: if the root task
     * tried to exit, task_exit() halts instead of returning.
     */
    return code;
}
