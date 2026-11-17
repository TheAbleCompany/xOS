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

    unsigned char fpu[512] __attribute__((aligned(16)));

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
     * Пользовательский ELF-образ (exec).
     *
     * image_phys != 0 -> задача владеет собственным адресным
     * пространством, в котором образ отображён по своему
     * линковочному адресу [image_lo .. image_hi).
     * Физические кадры (непрерывный блок от image_phys)
     * освобождаются в task_exit().
     */
    uint64_t image_lo;
    uint64_t image_hi;
    uint64_t image_phys;

    /*
     * Отдельный ядерный стек задачи для сисколлов.
     *
     * Кадр int 0x80, вытолкнутый CPU на стек задачи, накрывает
     * TCC-red-zone (см. kernel/asm/interrupt.asm), поэтому
     * обработчик сисколла работает на ОТДЕЛЬНОМ стеке задачи:
     * кадр копируется сюда, iretq выполняется отсюда, а стек
     * задачи ниже кадра не трогается вовсе. Освобождается он не
     * в task_exit (мы можем стоять на нём), а отложенно -- очередь
     * kstack_reap_push()/yield().
     */
    void    *kstack_base;
    uint64_t kstack_top;

    /*
     * BSD-подобный слой сокетов (fs/sock.c).  Таблица живёт
     * отдельно от files[], потому что сокет -- не fs_node и не
     * может обслуживаться read_fs/write_fs.  Нулевой указатель
     * означает "fd занят не сокетом"; task_alloc_sockfd() берёт
     * первый свободный индекс в ОБЩЕМ пространстве fd.
     */
    struct ax_socket *socks[TASK_MAX_FDS];

    /*
     * Блок TLS задачи и значение IA32_FS_BASE, которое планировщик
     * обязан установить при переключении. Mesa обращается к своим
     * thread_local-переменным через %fs (initial-exec, R_X86_64_TPOFF32),
     * поэтому смещение отсчитывается от КОНЦА блока: fs_base
     * всегда равен block + ax_tls_image_size().
     */
    void    *tls_block;
    uint64_t fs_base;

    /*
     * �������� ABI ������.
     *
     * ��������� ���, ��� ���� �������� int 0x80 � ����� ��������
     * ������� ��������� ������� ����������.  ������� ���������
     * ArtyomX64 -- AX_ABI_NATIVE (������ ���� � lib/syscalls.c).
     * ���������� ��������� Linux/BSD �������� ���� ABI, � �� ������
     * ����������� �� ��� ������������� ��������� (read/write/open/
     * close/lseek/unlink/exec/exit/...).
     */
    int abi;

    /*
     * ������ ������� ����������� ������� ����� ABI (�������).
     */
    int abi_trace;

} Task;

/*
 * Вершина ядерно-стека ТЕКУЩЕЙ задачи -- читается стабом
 * сисколла (interrupt.asm, SYSCALL_STUB). Обновляется
 * планировщиком при каждом переключении.
 */
extern uint64_t ax_cur_kstack;

/*
 * Отложенное освобождение стеков завершившейся задачи.
 * task_exit() не может вернуть память, на которой стоит сам,
 * поэтому указатели складываются сюда, а yield() следующей
 * задачи их освобождает.
 */
void kstack_reap_push(void *ptr);

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
    Registers *new,
    void *old_fpu,
    void *new_fpu
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

/*
 * ------------------------------------------------------------------
 * Pending ELF image (exec pipeline)
 * ------------------------------------------------------------------
 *
 * image_load() (multitask/loader.c) готовит физический блок с
 * копией образа и публикует его как "pending". task_spawn()
 * забирает его (pop), создаёт задаче отдельное адресное
 * пространство и отображает образ по линковочному vaddr.
 *
 * Если task_spawn не вызывается (ошибка exec) - pending
 * освобождается через aximage_pending_discard().
 */
int  aximage_pending_pop(
    uint64_t *lo,
    uint64_t *hi,
    uint64_t *phys
);

void aximage_pending_discard(void);

/*
 * Освобождение физического блока образа
 * (вызывается из task_exit для образов умерших задач).
 */
void aximage_free(
    uint64_t lo,
    uint64_t hi,
    uint64_t phys
);

/*
 * Пользовательские виртуальные адреса лежат ниже этой границы;
 * всё выше - ядро (HHDM ffff8000... и higher-half текст ffffffff80...).
 */
#define AX_USER_VA_MAX 0x0000800000000000ULL

/*
 * ------------------------------------------------------------------
 * �������� ABI (Task::abi)
 * ------------------------------------------------------------------
 *
 * AX_ABI_NATIVE -- родная программа: номера ядра (lib/syscalls.c).
 * AX_ABI_LINUX32 -- i386 ELF: int 0x80, eax = номер, аргументы в
 *                   ebx, ecx, edx, esi, edi, ebp.
 * AX_ABI_LINUX64 -- x86-64 ELF (особый режим): инструкция syscall,
 *                   rax = номер, аргументы в rdi, rsi, rdx, r10,
 *                   r8, r9; rcx/r11 затираются CPU.
 * AX_ABI_FREEBSD32 -- i386 ELF с ELFOSABI_FREEBSD: int 0x80,
 *                   eax = номер, первый аргумент в стеке,
 *                   остальные в edx, ecx, ebx, esi(, edi).
 *
 * Номера Linux/BSD не хардкодятся в ядре заново: каждый из них
 * переводится в общий Unix-номер примитива (AXU_*), который
 * обслуживается уже существующими read/write/open/close/...
 */
#define AX_ABI_NATIVE    0
#define AX_ABI_LINUX32   1
#define AX_ABI_LINUX64   2
#define AX_ABI_FREEBSD32 3


#endif