/*
 * Cooperative task scheduler.
 *
 * Responsibilities:
 *
 *   - task creation
 *   - task destruction
 *   - cooperative scheduling
 *   - task-local file descriptor table
 *   - controlling terminal inheritance
 *
 * The scheduler is cooperative.
 *
 * Every long-running task MUST periodically call yield().
 */

#include "task.h"

#include "../fs/fs.h"
#include "../fs/file.h"
#include "../fs/sock.h"
#include "../mm/kheap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../drivers/monitor.h"
#include "../lib/common.h"
#include "../kernel/tls.h"

#include <stddef.h>
#include <stdint.h>

#define malloc kmalloc
#define free   kfree

/*
 * Стек задачи.
 *
 * Раньше было 16 КиБ -- под это не помещался даже tcc (компиляция
 * реального hello.c требует ~14 КиБ, файл побольше -- 16+ КиБ), а
 * кадры обработчика syscall'а ядро дописывает на ТОТ ЖЕ стек.
 * Переполнение затирало соседние блоки kheap (MMU-защиты нет) и
 * вешало всю систему. 2 МиБ покрывают компиляцию крупных файлов.
 */
#define STACK_SIZE (2 * 1024 * 1024)
#define MAX_TASKS  16

static Task tasks[MAX_TASKS];

Task *runningTask = NULL;

/*
 * Вершина ядерно-стека текущей задачи (см. task.h).
 * Читается SYSCALL_STUB в interrupt.asm при каждом int 0x80.
 */
uint64_t ax_cur_kstack = 0;

/* Размер отдельного ядерно-стека задачи под сисколлы. */
#define KSTACK_SIZE (16 * 1024)

static Task *taskListHead = NULL;

static int task_count = 0;
static int next_pid   = 1;


/* ------------------------------------------------------------- */
/* Deferred stack reaper                                          */
/* ------------------------------------------------------------- */

/*
 * A task cannot free the memory it is executing on. Both the task stack and
 * the syscall stack are handed here by task_exit() and released by the next
 * scheduler pass, which by construction runs on a different stack.
 */

#define KSTACK_REAP_DEPTH 32

static void  *reap_pending[KSTACK_REAP_DEPTH];
static unsigned reap_head;
static unsigned reap_used;

void kstack_reap_push(void *ptr)
{
    if (!ptr)
        return;

    if (reap_used == KSTACK_REAP_DEPTH) {
        kfree(reap_pending[reap_head]);
        reap_pending[reap_head] = ptr;
        reap_head = (reap_head + 1u) % KSTACK_REAP_DEPTH;
        return;
    }

    reap_pending[(reap_head + reap_used) % KSTACK_REAP_DEPTH] = ptr;
    reap_used++;
}

static void kstack_reap_drain(void)
{
    while (reap_used) {
        void *ptr = reap_pending[reap_head];
        reap_pending[reap_head] = 0;
        reap_head = (reap_head + 1u) % KSTACK_REAP_DEPTH;
        reap_used--;
        kfree(ptr);
    }
}


/* ------------------------------------------------------------- */
/* Debug                                                          */
/* ------------------------------------------------------------- */
/*
 * debug_putc/puts/putnum are declared in lib/common.h and defined
 * in lib/syscalls.c.  Earlier this file carried its own static
 * copies, but that conflicts with the shared non-static
 * declarations now that other subsystems (fs/exec.c, axshell, ...)
 * also link against them.
 */


/* ------------------------------------------------------------- */
/* Task-local FD table                                            */
/* ------------------------------------------------------------- */

/*
 * Initialize the FD table of a newly created task.
 */
void task_files_init(Task *task)
{
    if (!task)
        return;

    for (int fd = 0; fd < TASK_MAX_FDS; fd++)
        task->files[fd] = NULL;

    /*
     * 0,1,2 are reserved for stdio.
     */
    task->current_fd = 3;
}


/*
 * Destroy all file objects belonging to a task.
 *
 * fs_node_t itself is NOT freed here.
 *
 * file_t is allocated by syscalls/open and belongs to Task.
 */
void task_files_destroy(Task *task)
{
    if (!task)
        return;

    for (int fd = 0; fd < TASK_MAX_FDS; fd++) {

        if (task->socks[fd]) {
            ax_sock_close(fd);
            task->socks[fd] = NULL;
        }

        if (task->files[fd]) {
            free(task->files[fd]);
            task->files[fd] = NULL;
        }
    }

    task->current_fd = 3;
}


/*
 * Find a free FD.
 *
 * We deliberately start from 3 because 0,1,2 are stdio.
 */
int task_alloc_fd(Task *task)
{
    if (!task)
        return -1;

    for (int fd = 3; fd < TASK_MAX_FDS; fd++) {

        if (task->files[fd] == NULL && task->socks[fd] == NULL) {
            task->current_fd = fd + 1;
            return fd;
        }
    }

    /*
     * Wrap around once. This is useful if descriptors have
     * previously been closed.
     */
    for (int fd = 3; fd < TASK_MAX_FDS; fd++) {

        if (task->files[fd] == NULL && task->socks[fd] == NULL)
            return fd;
    }

    return -1;
}


struct file *task_get_file(Task *task, int fd)
{
    if (!task)
        return NULL;

    if (fd < 0 || fd >= TASK_MAX_FDS)
        return NULL;

    return task->files[fd];
}


int task_set_file(Task *task, int fd, struct file *file)
{
    if (!task)
        return -1;

    if (fd < 0 || fd >= TASK_MAX_FDS)
        return -1;

    task->files[fd] = file;

    return 0;
}


void task_close_fd(Task *task, int fd)
{
    if (!task)
        return;

    if (fd < 0 || fd >= TASK_MAX_FDS)
        return;

    if (task->socks[fd]) {
        ax_sock_close(fd);
        task->socks[fd] = NULL;
    }

    if (task->files[fd]) {
        free(task->files[fd]);
        task->files[fd] = NULL;
    }
}


/* ------------------------------------------------------------- */
/* Task name                                                      */
/* ------------------------------------------------------------- */

static void copy_name(
    char *dst,
    const char *src,
    size_t dstsz
)
{
    size_t i = 0;

    if (!dst || dstsz == 0)
        return;

    if (src) {
        while (src[i] && i < dstsz - 1) {
            dst[i] = src[i];
            i++;
        }
    }

    dst[i] = 0;
}


/* ------------------------------------------------------------- */
/* Task list                                                       */
/* ------------------------------------------------------------- */

static void task_list_insert(Task *t)
{
    if (!t)
        return;

    if (taskListHead == NULL) {

        taskListHead = t;

        t->next = t;
        t->prev = t;

        return;
    }

    Task *last = taskListHead->prev;

    last->next = t;

    t->prev = last;
    t->next = taskListHead;

    taskListHead->prev = t;
}


static void task_list_remove(Task *t)
{
    if (!t || !taskListHead)
        return;

    if (t->next == t) {

        taskListHead = NULL;

        t->next = NULL;
        t->prev = NULL;

        return;
    }

    t->prev->next = t->next;
    t->next->prev = t->prev;

    if (taskListHead == t)
        taskListHead = t->next;

    t->next = NULL;
    t->prev = NULL;
}


/* ------------------------------------------------------------- */
/* FPU state                                                      */
/* ------------------------------------------------------------- */

#include "../kernel/fpu.h"

/* ------------------------------------------------------------- */
/* Initialization                                                  */
/* ------------------------------------------------------------- */

void initTasking(void)
{
    debug_puts("TASK: init\n");

    for (int i = 0; i < MAX_TASKS; i++) {

        tasks[i].state = TASK_TERMINATED;

        tasks[i].stack = NULL;
        tasks[i].stack_base = NULL;

        tasks[i].next = NULL;
        tasks[i].prev = NULL;

        tasks[i].pid = 0;

        tasks[i].name[0] = 0;

        tasks[i].ctty_slave = NULL;

        tasks[i].current_fd = 3;

        for (int fd = 0; fd < TASK_MAX_FDS; fd++)
            tasks[i].files[fd] = NULL;
    }

    task_count   = 0;
    taskListHead = NULL;
    runningTask  = NULL;
    next_pid     = 1;


    /*
     * Slot 0 is the kernel/root task.
     *
     * It represents the context that called initTasking().
     */
    Task *root = &tasks[0];

    root->state = TASK_RUNNING;

    root->pid = next_pid++;

    copy_name(
        root->name,
        "kernel",
        sizeof(root->name)
    );

    root->ctty_slave = NULL;

    fpu_state_init(root->fpu);

    task_files_init(root);

    root->tls_block = ax_tls_block_alloc();

    if (ax_tls_image_size() && !root->tls_block) {

        debug_puts("TASK: root TLS alloc failed\n");
    }

    root->fs_base = ax_tls_block_fs_base(root->tls_block);

    ax_tls_write_fs_base(root->fs_base);

    debug_puts("TASK: tls image=");
    debug_putnum((uint64_t)ax_tls_image_size());
    debug_puts(" block=0x");
    debug_putnum((uint64_t)(uintptr_t)root->tls_block);
    debug_puts(" fs_base=0x");
    debug_putnum(root->fs_base);
    debug_puts(" msr=0x");
    debug_putnum(ax_tls_read_fs_base());
    debug_putc('\n');

    task_list_insert(root);

    runningTask = root;

    task_count = 1;


    debug_puts("TASK: init done (root pid=");

    debug_putnum(root->pid);

    debug_puts(")\n");
}


/* ------------------------------------------------------------- */
/* Spawn                                                            */
/* ------------------------------------------------------------- */

int task_spawn(
    void (*entry)(void *arg),
    void *arg,
    const char *name
)
{
    debug_puts("TASK: spawn ");

    debug_puts(
        name ? name : "unknown"
    );

    debug_putc('\n');


    if (!entry) {

        debug_puts(
            "TASK: entry is NULL\n"
        );

        return -1;
    }


    if (task_count >= MAX_TASKS) {

        debug_puts(
            "TASK: too many tasks\n"
        );

        return -1;
    }


    int slot = -1;

    for (int i = 0; i < MAX_TASKS; i++) {

        if (tasks[i].state == TASK_TERMINATED) {
            slot = i;
            break;
        }
    }


    if (slot < 0) {

        debug_puts(
            "TASK: no free slot\n"
        );

        return -1;
    }


    Task *task = &tasks[slot];


    /*
     * Clear the task structure.
     *
     * We do this explicitly instead of relying on the previous
     * contents of a terminated slot.
     */
    memset(
        (uint8_t *)task,
        0,
        sizeof(Task)
    );


    task->state = TASK_READY;


    /*
     * Every task starts with an empty private FD table.
     */
    task_files_init(task);


    /*
     * Inherit controlling terminal from the task that spawned us.
     *
     * This is the important PTY connection.
     */
    if (runningTask)
        task->ctty_slave = runningTask->ctty_slave;
    else
        task->ctty_slave = NULL;


    /*
     * Allocate user/kernel task stack.
     */
    task->stack = malloc(STACK_SIZE);

    if (!task->stack) {

        debug_puts(
            "TASK: malloc failed\n"
        );

        task->state = TASK_TERMINATED;

        return -1;
    }

    task->stack_base = task->stack;

    /*
     * Отдельный ядерно-стек под сисколлы: кадр int 0x80 копируется
     * сюда, обработчик и iretq живут здесь. Стек задачи (и его
     * TCC-red-zone) прерываниями-сисколлами не трогается.
     */
    task->kstack_base = malloc(KSTACK_SIZE);

    if (!task->kstack_base) {

        debug_puts("TASK: kstack malloc failed\n");

        free(task->stack);
        task->stack = NULL;
        task->stack_base = NULL;
        task->state = TASK_TERMINATED;

        return -1;
    }

    task->kstack_top =
        ((uint64_t)(uintptr_t)task->kstack_base + KSTACK_SIZE) & ~0xFULL;

    task->tls_block = ax_tls_block_alloc();

    if (ax_tls_image_size() && !task->tls_block) {

        debug_puts("TASK: TLS malloc failed\n");

        free(task->kstack_base);
        task->kstack_base = NULL;
        task->kstack_top  = 0;
        free(task->stack);
        task->stack = NULL;
        task->stack_base = NULL;
        task->state = TASK_TERMINATED;

        return -1;
    }

    task->fs_base = ax_tls_block_fs_base(task->tls_block);


    /*
     * Clear saved register context.
     */
    memset(
        (uint8_t *)&task->regs,
        0,
        sizeof(Registers)
    );

    fpu_state_init(task->fpu);


    /*
     * Initial stack.
     *
     * If entry() returns, execution enters task_exit().
     */
    uint64_t *stack_top =
        (uint64_t *)(
            (uint64_t)task->stack + STACK_SIZE
        );

    /*
     * x86-64 ABI requires stack alignment.
     *
     * Keep it 16-byte aligned before pushing the return address.
     */
    stack_top =
        (uint64_t *)(
            ((uint64_t)stack_top) & ~0xFULL
        );

    *--stack_top =
        (uint64_t)task_exit;


    task->regs.rsp =
        (uint64_t)stack_top;

    task->regs.rip =
        (uint64_t)entry;

    task->regs.rdi =
        (uint64_t)arg;

    task->regs.rflags =
        0x202;


    /*
     * Address space selection.
     *
     * Kernel-internal tasks (GUI wrappers, services) keep running
     * in the parent address space, as before.
     *
     * User ELF images (entry below AX_USER_VA_MAX) get their own
     * address space with the image mapped at its linked vaddr:
     * non-PIE binaries contain absolute references (jump tables,
     * immediates) that are only valid at that address.
     */
    /*
     * ��������� �������� ABI �� ���������� ������.  exec() ���
     * ������� ������ ��������� �������������� � ����� spawn.
     */
    if (runningTask) {
        task->abi       = runningTask->abi;
        task->abi_trace = runningTask->abi_trace;
    }

    if ((uint64_t)entry != 0 &&
        (uint64_t)entry < AX_USER_VA_MAX)
    {
        uint64_t ilo, ihi, iphys;

        if (!aximage_pending_pop(&ilo, &ihi, &iphys)) {

            debug_puts(
                "TASK: no pending image for user entry\n"
            );

            free(task->stack);
            task->stack = NULL;
            task->stack_base = NULL;
            task->state = TASK_TERMINATED;

            return -1;
        }

        vmm_space_t ns =
            vmm_new_address_space();

        if (!ns) {

            debug_puts(
                "TASK: vmm_new_address_space failed\n"
            );

            aximage_free(ilo, ihi, iphys);

            free(task->stack);
            task->stack = NULL;
            task->stack_base = NULL;
            task->state = TASK_TERMINATED;

            return -1;
        }

        uint64_t n = (ihi - ilo) >> 12;

        for (uint64_t i = 0; i < n; i++) {

            if (vmm_map_page(
                    ns,
                    ilo + (i << 12),
                    iphys + (i << 12),
                    PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0)
            {
                debug_puts(
                    "TASK: vmm_map_page failed\n"
                );

                aximage_free(ilo, ihi, iphys);

                free(task->stack);
                task->stack = NULL;
                task->stack_base = NULL;
                task->state = TASK_TERMINATED;

                return -1;
            }
        }

        task->image_lo   = ilo;
        task->image_hi   = ihi;
        task->image_phys = iphys;

        task->regs.cr3 = ns;
    }
    else
    {
        /*
         * Kernel task: current address space.
         */
        uint64_t cr3;

        __asm__ volatile(
            "mov %%cr3, %0"
            : "=r"(cr3)
        );

        task->regs.cr3 = cr3;
    }


    /*
     * Allocate PID.
     */
    task->pid = next_pid++;


    copy_name(
        task->name,
        name,
        sizeof(task->name)
    );


    task_list_insert(task);

    task_count++;


    debug_puts(
        "TASK: spawned PID="
    );

    debug_putnum(
        (uint64_t)task->pid
    );

    debug_puts(
        " entry="
    );

    debug_putnum(
        (uint64_t)entry
    );

    debug_putc('\n');


    return task->pid;
}


/* ------------------------------------------------------------- */
/* Scheduler                                                       */
/* ------------------------------------------------------------- */

/*
 * Find the next READY task.
 *
 * Root is NOT excluded.
 *
 * The scan starts immediately after the currently running task,
 * which gives us round-robin behaviour.
 */
static Task *pick_next_ready(void)
{
    if (!runningTask)
        return NULL;

    if (task_count <= 1)
        return NULL;


    int start =
        (int)(runningTask - tasks);


    for (int off = 1;
         off <= MAX_TASKS;
         off++) {

        int index =
            (start + off) % MAX_TASKS;

        Task *t = &tasks[index];


        if (t->state == TASK_READY)
            return t;
    }


    return NULL;
}


/* ------------------------------------------------------------- */
/* PID                                                             */
/* ------------------------------------------------------------- */

bool task_is_alive(int pid)
{
    for (int i = 0; i < MAX_TASKS; i++) {

        if (tasks[i].pid == pid &&
            tasks[i].state != TASK_TERMINATED) {

            return true;
        }
    }

    return false;
}
Task *task_get_by_pid(int pid)
{
    if (pid <= 0)
        return NULL;

    for (int i = 0; i < MAX_TASKS; i++) {

        if (tasks[i].pid == pid &&
            tasks[i].state != TASK_TERMINATED) {

            return &tasks[i];
        }
    }

    return NULL;
}
int task_setup_stdio(int pid, fs_node_t *node)
{
    Task *task;

    if (!node)
        return -1;

    task = task_get_by_pid(pid);

    if (!task)
        return -1;

    /*
     * stdin / stdout / stderr
     *
     * Каждый FD получает свой file_t,
     * но все три указывают на один PTY slave.
     */

    for (int fd = 0; fd < 3; fd++) {

        if (task->files[fd]) {
            free(task->files[fd]);
            task->files[fd] = NULL;
        }

        file_t *f =
            (file_t *)malloc(sizeof(file_t));

        if (!f) {

            /*
             * Откатываем уже созданные FD.
             */

            for (int i = 0; i < fd; i++) {

                if (task->files[i]) {
                    free(task->files[i]);
                    task->files[i] = NULL;
                }
            }

            return -1;
        }

        f->node  = node;
        f->pos   = 0;
        f->flags = 0;

        task->files[fd] = f;
    }

    task->current_fd = 3;

    return 0;
}


/* ------------------------------------------------------------- */
/* Controlling terminal                                            */
/* ------------------------------------------------------------- */

int task_set_ctty(
    int pid,
    fs_node_t *node
)
{
    if (!node)
        return -1;

    for (int i = 0; i < MAX_TASKS; i++) {

        Task *task = &tasks[i];

        if (task->pid != pid)
            continue;

        if (task->state == TASK_TERMINATED)
            return -1;

        task->ctty_slave = node;

        return 0;
    }

    return -1;
}

/* ------------------------------------------------------------- */
/* Yield                                                            */
/* ------------------------------------------------------------- */

void yield(void)
{
    Task *old = runningTask;

    kstack_reap_drain();

    Task *next =
        pick_next_ready();


    /*
     * Nothing else can run.
     */
    if (!next)
        return;


    /*
     * Save current task state logically.
     *
     * switchTask() will actually save registers.
     */
    if (old) {

        old->state = TASK_READY;

        uint64_t cr3;

        __asm__ volatile(
            "mov %%cr3, %0"
            : "=r"(cr3)
        );

        old->regs.cr3 = cr3;
    }


    next->state = TASK_RUNNING;

    runningTask = next;

    /* Стаб сисколлов будет резвиться на ядерно-стеке новой задачи. */
    ax_cur_kstack = next->kstack_top;

    /*
     * Каждая задача несёт собственный блок thread_local-переменных
     * (Mesa: dispatch/context GL, call_once, буфер отладки).
     */
    ax_tls_write_fs_base(next->fs_base);


    switchTask(
        old ? &old->regs : NULL,
        &next->regs,
        old ? old->fpu : NULL,
        next->fpu
    );


    /*
     * Execution resumes here when this task becomes
     * the next task again.
     */
}


/* ------------------------------------------------------------- */
/* Exit                                                             */
/* ------------------------------------------------------------- */

void task_exit(int code)
{
    (void)code;


    Task *self = runningTask;

    if (!self)
        return;


    debug_puts(
        "TASK: exit PID="
    );

    debug_putnum(
        (uint64_t)self->pid
    );

    debug_putc('\n');


    /*
     * Root is the kernel/compositor context.
     *
     * It cannot be destroyed because there may be no
     * valid context behind it.
     */
    if (self->pid == TASK_ROOT_PID) {

        debug_puts(
            "TASK: root task tried to exit -- halting\n"
        );

        for (;;) {
            __asm__ volatile(
                "cli; hlt"
            );
        }
    }


    /*
     * Remove task from scheduler BEFORE switching away.
     */
    self->state = TASK_TERMINATED;


    /*
     * Close all task-local files.
     *
     * This includes stdin/stdout/stderr.
     */
    task_files_destroy(self);

    /*
     * Address space first: vmm_release_space() walks the user half
     * of the PML4 and returns every page table, and (with
     * free_user_frames) every mapped frame, to the PMM. The ELF
     * image below was mapped page-for-page into that space, so it
     * goes back together with the tables that described it.
     */
    if (self->regs.cr3 && self->regs.cr3 != vmm_kernel_space()) {

        vmm_release_space(self->regs.cr3, 1);

        self->regs.cr3 = vmm_kernel_space();
    }

    /*
     * The ELF image frames were mapped page-for-page into the space that
     * vmm_release_space() just tore down, so they are already back in the
     * PMM; calling aximage_free() here would free them a second time.
     * Only the bookkeeping is left to drop.
     */
    self->image_lo   = 0;
    self->image_hi   = 0;
    self->image_phys = 0;



    /*
     * Remove from circular task list.
     */
    task_list_remove(self);

    task_count--;


    /*
     * Find next runnable task.
     *
     * Since self is already TERMINATED, it cannot be selected.
     */
    Task *next =
        pick_next_ready();


    /*
     * Normally root will be READY or another task will be ready.
     */
    if (!next) {

        /*
         * As a final fallback use root.
         */
        next = &tasks[0];

        if (next->state == TASK_TERMINATED) {

            debug_puts(
                "TASK: no runnable task -- halting\n"
            );

            for (;;) {
                __asm__ volatile(
                    "cli; hlt"
                );
            }
        }
    }


    next->state = TASK_RUNNING;

    runningTask = next;

    /* Стаб сисколлов -- на ядерно-стеке следующей задачи. */
    ax_cur_kstack = next->kstack_top;

    ax_tls_write_fs_base(next->fs_base);


    /*
     * The terminating task's kernel stack cannot be freed while we are
     * still running on it, and it is not an arena block of the current
     * task's heap either. Park it in the deferred reaper; the root task
     * releases it on its next pass over the scheduler, long after
     * switchTask() has moved RSP somewhere else.
     */
    if (self->stack) {
        kstack_reap_push(self->stack);
        self->stack = NULL;
    }

    self->stack_base = NULL;

    if (self->kstack_base) {
        kstack_reap_push(self->kstack_base);
        self->kstack_base = NULL;
        self->kstack_top  = 0;
    }

    if (self->tls_block) {
        kstack_reap_push(self->tls_block);
        self->tls_block = NULL;
        self->fs_base   = 0;
    }


    /*
     * We must NOT free the current stack before switchTask()
     * has finished using it.
     *
     * switchTask(NULL, ...) does not save the old context,
     * so it is safe to switch away permanently.
     */
    switchTask(
        NULL,
        &next->regs,
        NULL,
        next->fpu
    );


    /*
     * We should never return here.
     *
     * At this point our old stack may already be freed.
     */
    for (;;) {
        __asm__ volatile(
            "cli; hlt"
        );
    }


    /*
     * Unreachable: switchTask(NULL, ...) never returns here.
     */
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
int task_setup_stdio_pid(int pid, fs_node_t *node)
{
    if (!node)
        return -1;

    for (int i = 0; i < MAX_TASKS; i++) {

        Task *task = &tasks[i];

        if (task->pid != pid)
            continue;

        if (task->state == TASK_TERMINATED)
            return -1;

        /*
         * fd 0,1,2 принадлежат новой задаче.
         * task_spawn() уже создал пустую таблицу.
         */

        for (int fd = 0; fd < 3; fd++) {

            if (task->files[fd]) {
                free(task->files[fd]);
                task->files[fd] = NULL;
            }

            file_t *f =
                (file_t *)malloc(sizeof(file_t));

            if (!f) {

                /*
                 * Если не смогли создать очередной
                 * descriptor, откатываем уже созданные.
                 */

                for (int j = 0; j < fd; j++) {

                    if (task->files[j]) {
                        free(task->files[j]);
                        task->files[j] = NULL;
                    }
                }

                return -1;
            }

            f->node  = node;
            f->pos   = 0;
            f->flags = 0;

            task->files[fd] = f;
        }

        return 0;
    }

    return -1;
}
