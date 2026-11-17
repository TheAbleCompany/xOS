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
#include "../mm/kheap.h"
#include "../drivers/monitor.h"
#include "../lib/common.h"

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

static Task *taskListHead = NULL;

static int task_count = 0;
static int next_pid   = 1;


/* ------------------------------------------------------------- */
/* Debug                                                          */
/* ------------------------------------------------------------- */

static void debug_putc(char c)
{
    outb(0x3F8, c);
}

static void debug_puts(const char *s)
{
    if (!s)
        return;

    while (*s)
        debug_putc(*s++);
}

static void debug_putnum(uint64_t n)
{
    char buf[32];
    int i = 0;

    if (n == 0) {
        debug_putc('0');
        return;
    }

    while (n) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }

    while (i)
        debug_putc(buf[--i]);
}


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

    /*
     * �� ��������� ������ -- ������ ��������� ArtyomX64.
     * exec() �������������� �������� �� ��������� ������.
     */
    task->abi = AX_ABI_NATIVE;
    task->abi_trace = 0;
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

        if (task->files[fd] == NULL) {
            task->current_fd = fd + 1;
            return fd;
        }
    }

    /*
     * Wrap around once. This is useful if descriptors have
     * previously been closed.
     */
    for (int fd = 3; fd < TASK_MAX_FDS; fd++) {

        if (task->files[fd] == NULL)
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

    task_files_init(root);

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
     * Clear saved register context.
     */
    memset(
        (uint8_t *)&task->regs,
        0,
        sizeof(Registers)
    );


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
     * New task uses the current address space.
     */
    uint64_t cr3;

    __asm__ volatile(
        "mov %%cr3, %0"
        : "=r"(cr3)
    );

    task->regs.cr3 = cr3;


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


    switchTask(
        old ? &old->regs : NULL,
        &next->regs
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
     * Save pointer to stack because after switching
     * away we must never touch it again.
     */
    void *stack = self->stack;

    self->stack = NULL;
    self->stack_base = NULL;


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


    /*
     * We must NOT free the current stack before switchTask()
     * has finished using it.
     *
     * switchTask(NULL, ...) does not save the old context,
     * so it is safe to switch away permanently.
     */
    switchTask(
        NULL,
        &next->regs
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
     * This code is unreachable in normal execution.
     *
     * The free is intentionally omitted here because
     * freeing self->stack before switchTask() can corrupt
     * the currently executing stack.
     *
     * Stack reclamation should be performed by the next
     * scheduler pass, or by a deferred reaper.
     */
    (void)stack;
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
