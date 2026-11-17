/*
 * axglue.h -- минимальный клиент WindowServer'а для Quake.
 *
 * Почему не lib/libax.h: он тянет lib/common.h, где memcpy/memset/
 * strlen объявлены с ядровыми сигнатурами (u8int*, u32int), а это
 * конфликтует с <string.h> из mini-libc, который нужен исходникам
 * Quake.  Здесь повторены только нужные обёртки над сисколлами
 * портов (178-181), время (196) и yield (151).
 *
 * Модель протокола -- lib/axipc.h / lib/axport.h (только типы).
 */
#ifndef AXGLUE_H
#define AXGLUE_H

#include <stdint.h>

#include "axipc.h"
#include "axport.h"

#define AXG_PORT_NAME "wserver"
#define AXG_RING_MAX  32

/*
 * Состояние соединения -- РОВНО ОДИН экземпляр на процесс (axglue.c).
 *
 * Раньше это были static-переменные в заголовке, то есть своя копия в
 * каждом .c-файле, подключившем axglue.h.  Окно создавал vid_artyom.c
 * и записывал клиентский порт только в свою копию, а AX_PumpEvents()
 * в in_artyom.c смотрел на свою (там -1) и считал, что соединения нет.
 * Из-за этого события wserver'а (клавиши, мышь) никогда не выбирались
 * из порта.
 */
extern int      axg_srv_port;
extern int      axg_cli_port;
extern int      axg_pid;
extern ax_msg_t axg_ring[AXG_RING_MAX];
extern int      axg_ring_n;

/* ---------------- сисколлы ---------------- */

static inline int axg_port_create(const char *name)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"((long)178), "D"((long)name) : "memory");
    return ret;
}

static inline int axg_port_lookup(const char *name)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"((long)179), "D"((long)name) : "memory");
    return ret;
}

static inline int axg_port_send(int port, const ax_msg_t *m)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"((long)180), "D"((long)port), "S"((long)m) : "memory");
    return ret;
}

static inline int axg_port_recv(int port, ax_msg_t *m)
{
    volatile int ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret) : "a"((long)181), "D"((long)port), "S"((long)m) : "memory");
    return ret;
}

static inline int axg_getpid(void)
{
    volatile int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"((long)182) : "memory");
    return ret;
}

static inline void axg_yield(void)
{
    volatile int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"((long)151) : "memory");
    (void)ret;
}

static inline uint32_t axg_ms(void)
{
    volatile uint32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"((uint32_t)196) : "memory");
    return ret;
}

/* ---------------- очередь сообщений ---------------- */

static inline void axg_zero(void *p, unsigned n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--)
        *b++ = 0;
}

static inline void axg_ring_push(const ax_msg_t *m)
{
    if (axg_ring_n < AXG_RING_MAX)
        axg_ring[axg_ring_n++] = *m;
}

static inline int axg_ring_find(uint32_t type, uint32_t win, ax_msg_t *out)
{
    for (int i = 0; i < axg_ring_n; i++) {
        if (axg_ring[i].type != type)
            continue;
        if (win != 0 && axg_ring[i].win != win)
            continue;
        if (out)
            *out = axg_ring[i];
        for (int j = i; j + 1 < axg_ring_n; j++)
            axg_ring[j] = axg_ring[j + 1];
        axg_ring_n--;
        return 1;
    }
    return 0;
}

static inline void axg_drain(void)
{
    ax_msg_t m;

    while (axg_cli_port > 0 && axg_port_recv(axg_cli_port, &m) == 1)
        axg_ring_push(&m);
}

static inline int axg_connected(void)
{
    return axg_srv_port > 0 && axg_cli_port > 0;
}

static inline int axg_connect(void)
{
    if (axg_connected())
        return 0;

    if (axg_pid < 0)
        axg_pid = axg_getpid();

    for (int guard = 0; guard < 20000; guard++) {
        axg_srv_port = axg_port_lookup(AXG_PORT_NAME);
        if (axg_srv_port > 0)
            break;
        axg_yield();
    }
    if (axg_srv_port <= 0)
        return -1;

    axg_cli_port = axg_port_create(0);
    if (axg_cli_port <= 0) {
        axg_srv_port = -1;
        return -1;
    }

    return 0;
}

static inline int axg_wait_reply(uint32_t type, uint32_t win, ax_msg_t *out)
{
    if (axg_ring_find(type, win, out))
        return 0;

    for (long guard = 0; guard < 2000000; guard++) {
        ax_msg_t m;

        if (axg_port_recv(axg_cli_port, &m) == 1) {
            if (m.type == type && (win == 0 || m.win == win)) {
                if (out)
                    *out = m;
                return 0;
            }
            axg_ring_push(&m);
        }
        axg_yield();
    }

    return -1;
}

static inline uint32_t axg_token(const void *canvas)
{
    return (uint32_t)(uintptr_t)canvas;
}

/* ---------------- публичные обёртки ---------------- */

static inline uint32_t *axg_surface(const char *title, int w, int h,
                                    int *out_w, int *out_h)
{
    ax_msg_t m, r;

    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;

    if (axg_connect() != 0)
        return 0;

    axg_zero(&m, sizeof(m));
    m.type = MSG_REQ_SURFACE;
    m.from = (uint32_t)axg_cli_port;
    m.a    = (uint32_t)w;
    m.b    = (uint32_t)h;
    m.x    = (uint32_t)axg_pid;

    int k = 0;
    while (title && title[k] && k < 46) {
        m.data[k] = (uint8_t)title[k];
        k++;
    }
    m.data[k] = 0;
    m.len = (uint32_t)k + 1;

    if (axg_port_send(axg_srv_port, &m) != 0)
        return 0;

    if (axg_wait_reply(MSG_REPLY_SURFACE, 0, &r) != 0)
        return 0;

    uint64_t ptr = (uint64_t)r.a | ((uint64_t)r.b << 32);
    if (!ptr)
        return 0;

    if (out_w) *out_w = (int)r.x;
    if (out_h) *out_h = (int)r.y;

    return (uint32_t *)(uintptr_t)ptr;
}

static inline int axg_poll(uint32_t *canvas, ax_event *ev)
{
    ax_msg_t m;
    uint32_t tok;

    if (!axg_connected() || !canvas || !ev)
        return -1;

    tok = axg_token(canvas);
    axg_drain();

    if (!axg_ring_find(MSG_EVENT, tok, &m))
        return 0;

    ev->type = m.a;
    ev->key  = m.b;
    ev->mx   = (int32_t)m.x;
    ev->my   = (int32_t)m.y;

    if (m.len >= 12) {
        ev->buttons = (uint32_t)m.data[0] | ((uint32_t)m.data[1] << 8) |
                      ((uint32_t)m.data[2] << 16) | ((uint32_t)m.data[3] << 24);
        ev->w = (uint32_t)m.data[4] | ((uint32_t)m.data[5] << 8) |
                ((uint32_t)m.data[6] << 16) | ((uint32_t)m.data[7] << 24);
        ev->h = (uint32_t)m.data[8] | ((uint32_t)m.data[9] << 8) |
                ((uint32_t)m.data[10] << 16) | ((uint32_t)m.data[11] << 24);
    } else {
        ev->buttons = 0;
        ev->w = 0;
        ev->h = 0;
    }

    return 1;
}

static inline int axg_commit(uint32_t *canvas)
{
    ax_msg_t m;

    if (!axg_connected())
        return -1;

    axg_zero(&m, sizeof(m));
    m.type = MSG_REQ_COMMIT;
    m.win  = axg_token(canvas);
    m.from = (uint32_t)axg_cli_port;

    return axg_port_send(axg_srv_port, &m);
}

static inline int axg_win_focus(uint32_t *canvas)
{
    ax_msg_t m;

    if (!axg_connected())
        return -1;

    axg_zero(&m, sizeof(m));
    m.type = MSG_REQ_WIN_FOCUS;
    m.win  = axg_token(canvas);
    m.from = (uint32_t)axg_cli_port;
    m.x    = (uint32_t)axg_pid;

    return axg_port_send(axg_srv_port, &m);
}

#endif /* AXGLUE_H */
