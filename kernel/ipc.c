/*
 * ============================================================
 * ipc.c -- межпроцессные примитивы ядра (модель Mach/BSD)
 * ============================================================
 *
 * Долишь примитивы, никакого GUI:
 *
 *   - порты сообщений: именованные очереди ax_msg_t в kernel heap.
 *     PORT_SEND копирует сообщение вызывающего в очередь (как Mach
 *     msg), PORT_RECV забирает.  Порты умерших задач снимаются
 *     лениво при следующем PORT_CREATE.
 *
 *   - SHM_ALLOC/SHM_FREE: блоки kernel heap.  Через общий
 *     higher-half (HHDM) они видны в КАЖДОМ адресном пространстве,
 *     поэтому "разделяемая память" здесь -- это просто выделение
 *     в старшей половине (тот же механизм, на котором раньше
 *     жили канвасы окон).
 *
 *   - TERM_*: pty как обычный tty-драйвер (аналог BSD tty в
 *     macOS).  Таблица из 8 слотов, TERM_OPEN выделяет pty и
 *     подключает ctty вызывающего к slave-стороне.
 *
 *   - INPUT_MOUSE/INPUT_KEY: сырые HID-примитивы.  Ядро дёргает
 *     свои драйверы (handle_mouse / keyboard_pop_event) и
 *     возвращает плоское состояние.  Политику (куда klik, какой
 *     процесс в фокусе) решает wserver, не ядро.
 *
 *   - FB_INFO: параметры framebuffer'а от Limine (адрес HHDM,
 *     размеры).  На этом wserver строит весь вывод.
 *
 *   - GPU_SUBMIT_RT: сырой сабмит треугольников в ixg-бэкенд
 *     (NVIDIA/AMD/Intel) с readback'ом в указанный буфер.
 *     Буфер + размеры -- БЕЗ понятия "окно".
 */

#include "ipc.h"

#include "isr.h"
#include "../mm/kheap.h"
#include "../mm/pmm.h"
#include "../multitask/task.h"
#include "../dev/pty.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/framebuffer.h"
#include "../drivers/gpu/ixg_driver.h"

#include <stdint.h>
#include <stddef.h>

/* Состояние мыши живёт в drivers/mouse.c; mouse.h экспортирует
 * только функции, поэтому глобалы объявляем здесь. */
extern int           m_cursor_x;
extern int           m_cursor_y;
extern volatile int  mouse_buttons;

#define malloc kmalloc
#define free   kfree

/* ============================================================
 * Порты сообщений
 * ============================================================ */

typedef struct {
    int       used;
    int       id;                 /* 1..AX_PORTS_MAX               */
    char      name[16];           /* "" = анонимный                */
    int       owner_pid;
    ax_msg_t *q;                  /* кольцо AX_PORT_QLEN сообщений */
    int       head, tail;
} ax_port_t;

static ax_port_t ports[AX_PORTS_MAX];
static int ipc_ready = 0;

void ipc_init(void)
{
    for (int i = 0; i < AX_PORTS_MAX; i++) {
        ports[i].used = 0;
        ports[i].q = 0;
    }
    ipc_ready = 1;
    debug_puts("IPC: port table ready\n");
}

/*
 * Снять порты задач, которых больше нет.  Вызывается лениво
 * из port_create -- отдельного reaper-потока в системе нет.
 */
void ipc_ports_reap_dead(void)
{
    for (int i = 0; i < AX_PORTS_MAX; i++) {
        ax_port_t *p = &ports[i];
        if (!p->used) continue;
        if (p->owner_pid > 0 && !task_is_alive(p->owner_pid)) {
            debug_puts("IPC: reap port ");
            debug_putnum((uint64_t)p->id);
            debug_putc('\n');
            if (p->q) { free(p->q); p->q = 0; }
            p->used = 0;
        }
    }
}

static int port_copy_name(char *dst, const char *src)
{
    int i = 0;
    if (src) {
        while (src[i] && i < 15) { dst[i] = src[i]; i++; }
    }
    dst[i] = 0;
    return i;
}

int ipc_port_create(const char *name)
{
    if (!ipc_ready) return -1;

    /* Сначала приберём порты умерших владельцев. */
    ipc_ports_reap_dead();

    for (int i = 0; i < AX_PORTS_MAX; i++) {
        ax_port_t *p = &ports[i];
        if (p->used) continue;

        p->q = (ax_msg_t *)malloc(sizeof(ax_msg_t) * AX_PORT_QLEN);
        if (!p->q) {
            debug_puts("IPC: port queue malloc failed\n");
            return -1;
        }

        p->used = 1;
        p->id = i + 1;
        port_copy_name(p->name, name);
        p->owner_pid = runningTask ? runningTask->pid : -1;
        p->head = 0;
        p->tail = 0;

        return p->id;
    }

    debug_puts("IPC: no free ports\n");
    return -1;
}

int ipc_port_lookup(const char *name)
{
    char want[16];

    if (!ipc_ready || !name) return -1;

    port_copy_name(want, name);
    if (!want[0]) return -1;

    for (int i = 0; i < AX_PORTS_MAX; i++) {
        ax_port_t *p = &ports[i];
        if (p->used && p->name[0] &&
            strcmp(p->name, want) == 0)
            return p->id;
    }
    return -1;
}

static ax_port_t *port_by_id(int id)
{
    if (id < 1 || id > AX_PORTS_MAX) return 0;
    ax_port_t *p = &ports[id - 1];
    return p->used ? p : 0;
}

int ipc_port_send(int port, const ax_msg_t *msg)
{
    ax_port_t *p = port_by_id(port);

    if (!p || !msg) return -1;


    int next = (p->head + 1) % AX_PORT_QLEN;
    if (next == p->tail)
        return -1;              /* очередь полна -- как и раньше
                                    с ev_queue[16], событие теряется */

    /* Копия сообщения -- вызывающий может сразу переиспользовать
     * свой буфер (Mach-семантика: сообщение принадлежит очереди). */
    p->q[p->head] = *msg;
    p->head = next;

    return 0;
}

int ipc_port_recv(int port, ax_msg_t *out)
{
    ax_port_t *p = port_by_id(port);

    if (!p || !out) return -1;

    if (p->tail == p->head)
        return 0;               /* пусто */

    *out = p->q[p->tail];
    p->tail = (p->tail + 1) % AX_PORT_QLEN;


    return 1;
}

/* ============================================================
 * Разделяемая память
 * ============================================================ */

void *ipc_shm_alloc(uint64_t size)
{
    if (!size || size > (64u << 20)) return 0;
    return malloc((size_t)size);
}

void ipc_shm_free(void *ptr)
{
    if (ptr) free(ptr);
}

/* ============================================================
 * tty-сервис (pty как BSD tty)
 * ============================================================ */

#define TTY_SLOTS 8

static ax_pty_t *tty_table[TTY_SLOTS];

static int tty_slot_free(void)
{
    for (int i = 0; i < TTY_SLOTS; i++)
        if (!tty_table[i]) return i;
    return -1;
}

int ipc_term_open(void)
{
    int slot = tty_slot_free();
    if (slot < 0) {
        debug_puts("IPC: tty table full\n");
        return -1;
    }

    ax_pty_t *pty = pty_alloc();
    if (!pty) return -1;

    tty_table[slot] = pty;

    /*
     * Вызывающий (терминальное приложение) получает pty как
     * свой controlling terminal: exec() детей подключит их
     * stdio к slave-стороне.
     */
    if (runningTask)
        runningTask->ctty_slave = &pty->slave_node;

    return slot;
}

static ax_pty_t *tty_by_id(int id)
{
    if (id < 0 || id >= TTY_SLOTS) return 0;
    return tty_table[id];
}

int ipc_term_read(int id, uint8_t *buf, uint32_t maxlen)
{
    ax_pty_t *pty = tty_by_id(id);
    if (!pty || !buf || !maxlen) return -1;
    return (int)pty->master_node.read(&pty->master_node, 0, maxlen, buf);
}

int ipc_term_write(int id, const uint8_t *buf, uint32_t len)
{
    ax_pty_t *pty = tty_by_id(id);
    if (!pty || !buf || !len) return -1;
    return (int)pty->master_node.write(&pty->master_node, 0, len, (u8int *)buf);
}

int ipc_term_focus(int id)
{
    ax_pty_t *pty = tty_by_id(id);
    if (!pty) return -1;
    keyboard_set_pty(pty);
    return 0;
}

int ipc_term_release(int id)
{
    ax_pty_t *pty = tty_by_id(id);
    if (!pty) return -1;
    pty_free(pty);
    tty_table[id] = 0;
    return 0;
}

/* ============================================================
 * Дисплей / ввод / процессы
 * ============================================================ */

int ipc_fb_info(ax_fb_info_t *out)
{
    if (!out) return -1;

    out->addr   = framebuffer_addr;
    out->width  = framebuffer_width;
    out->height = framebuffer_height;
    out->pitch  = framebuffer_pitch;
    out->bpp    = framebuffer_bpp;

    return 0;
}

int ipc_input_mouse(ax_mouse_info_t *out)
{
    if (!out) return -1;

    /* Драйвер сам разберёт накопленные пакеты PS/2. */
    handle_mouse();

    /*
     * Сырые координаты курсора могут уйти за край экрана (relative
     * PS/2-дельты не ограничены), а клиенты ждут координаты внутри
     * кадра.  Отрицательные значения превращались в огромные
     * беззнаковые и вешали window server.
     */
    int x = m_cursor_x;
    int y = m_cursor_y;
    int w = (int)framebuffer_width;
    int h = (int)framebuffer_height;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (w > 0 && x >= w) x = w - 1;
    if (h > 0 && y >= h) y = h - 1;

    out->x       = (uint32_t)x;
    out->y       = (uint32_t)y;
    out->buttons = (uint32_t)mouse_buttons;

    return 0;
}

int ipc_input_key(ax_key_info_t *out)
{
    if (!out) return -1;

    int key = keyboard_pop_event();
    if (key < 0) return 0;

    out->key   = key;
    out->shift = (uint32_t)ax_shift_pressed;
    out->caps  = (uint32_t)is_caps_pressed;

    return 1;
}

int ipc_proc_name(int pid, char *buf, uint32_t len)
{
    Task *t;

    if (!buf || len < 2) return -1;

    t = task_get_by_pid(pid);
    if (!t) return -1;

    uint32_t i = 0;
    while (t->name[i] && i < len - 1) {
        buf[i] = t->name[i];
        i++;
    }
    buf[i] = 0;

    return 0;
}

int ipc_getpid(void)
{
    return runningTask ? runningTask->pid : -1;
}

/* ============================================================
 * Сырой GPU-сабмит (ixg), без понятия "окно"
 * ============================================================
 *
 * canvas -- произвольный буфер в HHDM (обычно канвас окна),
 * w/h -- его размерность.  Ядро НЕ знает, чей это буфер.
 *
 * Логика повторяет старый GPU-путь AxGL:
 *   - есть CPU-visible render target -> рендер в GPU и readback
 *     "поверх" canvas (0 = фон, такие пиксели не переносятся);
 *   - иначе -- просто сабмит (видимость решает бэкенд).
 */

static int  gpu_probed = 0;

static void gpu_ensure_probe(void)
{
    ixg_driver_caps_t caps;

    if (gpu_probed) return;
    gpu_probed = 1;

    ixg_driver_init();
    ixg_driver_get_caps(&caps);

    debug_puts("IPC: gpu probe vendor=0x");
    debug_putnum(caps.vendor_id);
    debug_puts(" backend=");
    debug_puts(ixg_driver_backend_name());
    debug_puts(" hw_tri=");
    debug_putnum((uint64_t)ixg_driver_has_gpu_triangles());
    debug_putc('\n');
}

int ipc_gpu_submit_rt(uint64_t canvas, uint32_t w, uint32_t h,
                      const void *verts, uint32_t count)
{
    int rc = -1;

    if (!verts || count < 3 || count % 3 != 0) return -1;
    if ((canvas >> 32) != 0xFFFFull) return -1;   /* только HHDM */

    gpu_ensure_probe();

    if (!ixg_driver_has_gpu_triangles()) return -1;

    if (ixg_driver_has_render_readback()) {
        if (ixg_driver_accel_set_render_target(w, h) == 0) {
            ixg_driver_accel_rt_fill(0x00000000u);
            rc = ixg_driver_accel_triangles(
                (const ixg_hw_vertex_t *)verts, count / 3);
            if (rc == 0) {
                ixg_driver_flush();
                if (ixg_driver_accel_readback_rect_over(
                        (ix_u32 *)(uintptr_t)canvas, w, h, 0x00000000u) != 0)
                    rc = -2;      /* приняли, но readback не удался */
            }
        } else {
            rc = -3;
        }
    } else {
        rc = ixg_driver_accel_triangles(
            (const ixg_hw_vertex_t *)verts, count / 3);
    }

    return rc;
}
