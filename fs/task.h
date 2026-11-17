#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../fs/initrd.h"

/*
 * Максимальное количество файловых дескрипторов
 * на одну задачу.
 */
#define TASK_MAX_FDS 64

/*
 * Forward declaration.
 *
 * Полное определение struct file находится в fs/file.h.
 * task.c не должен знать внутреннее устройство file_t.
 */
struct file;

/*
 * ------------------------------------------------------------------
 * Registers
 * ------------------------------------------------------------------
 *
 * Layout является частью ABI switch.S.
 *
 * switch.S обращается к этим полям по фиксированным offset'ам:
 *
 *   0   r15
 *   8   r14
 *   ...
 *   120 rip
 *   128 rsp
 *   136 rflags
 *   144 cr3
 */
typedef struct {
    uint64_t r15;       /*   0 */
    uint64_t r14;       /*   8 */
    uint64_t r13;       /*  16 */
    uint64_t r12;       /*  24 */
    uint64_t r11;       /*  32 */
    uint64_t r10;       /*  40 */
    uint64_t r9;        /*  48 */
    uint64_t r8;        /*  56 */

    uint64_t rdi;       /*  64 */
    uint64_t rsi;       /*  72 */
    uint64_t rbp;       /*  80 */
    uint64_t rdx;       /*  88 */
    uint64_t rcx;       /*  96 */
    uint64_t rbx;       /* 104 */
    uint64_t rax;       /* 112 */

    uint64_t rip;       /* 120 */
    uint64_t rsp;       /* 128 */
    uint64_t rflags;    /* 136 */
    uint64_t cr3;       /* 144 */
} Registers;

typedef enum {
    TASK_TERMINATED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED
} TaskState;

typedef struct Task {
    Registers regs;

    TaskState state;
    int       pid;

    void *stack;
    void *stack_base;

    struct Task *next;
    struct Task *prev;

    char name[32];

    /*
     * Controlling terminal.
     *
     * Обычно это PTY slave.
     */
    fs_node_t *ctty_slave;

    /*
     * Файловая таблица конкретной задачи.
     *
     * fd 0 = stdin
     * fd 1 = stdout
     * fd 2 = stderr
     */
    struct file *files[TASK_MAX_FDS];

    /*
     * Следующий FD для open().
     * Реальный поиск свободного FD всё равно производится
     * через task_alloc_fd().
     */
    int current_fd;

    /*
     * �������� ABI (AX_ABI_*).  ��������� �������� int 0x80:
     * ������ ��������� ���������� �������� ����, ����������
     * ��������� Linux/BSD -- ������ ��������, ������� �����������
     * �� �� �� ��������� read/write/open/close/...
     *
     * ������ �������� ��������� -- multitask/task.h.
     */
    int abi;
    int abi_trace;

} Task;

/*
 * Личности ABI (Task::abi).  Полное описание -- multitask/task.h.
 */
#define AX_ABI_NATIVE    0
#define AX_ABI_LINUX32   1
#define AX_ABI_LINUX64   2
#define AX_ABI_FREEBSD32 3

/*
 * PID корневой задачи.
 */
#define TASK_ROOT_PID 1

extern Task *runningTask;

void initTasking(void);

int task_spawn(
    void (*entry)(void *arg),
    void *arg,
    const char *name
);

void yield(void);

void task_exit(int code);

void switchTask(
    Registers *old,
    Registers *new
);

bool task_is_alive(int pid);

/*
 * Controlling terminal.
 */
int task_set_ctty(int pid, fs_node_t *node);

/*
 * FD management.
 */
void task_files_init(Task *task);
void task_files_destroy(Task *task);

int task_alloc_fd(Task *task);

struct file *task_get_file(Task *task, int fd);
int task_set_file(Task *task, int fd, struct file *file);
void task_close_fd(Task *task, int fd);

int task_setup_stdio_pid(int pid, fs_node_t *node);

Task *task_get_by_pid(int pid);

int task_setup_stdio(
    int pid,
    fs_node_t *node
);
#endif