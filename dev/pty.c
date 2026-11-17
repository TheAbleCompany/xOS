#include "pty.h"

#include "../lib/common.h"

#include <stddef.h>
#include <stdint.h>


#define MAX_PTYS 8


/*
 * Получение владельца fs_node_t.
 *
 * master_node и slave_node находятся непосредственно
 * внутри ax_pty_t, поэтому дополнительный указатель
 * owner в fs_node_t нам не нужен.
 */

#define PTY_FROM_MASTER(n) \
    ((ax_pty_t *)((char *)(n) - offsetof(ax_pty_t, master_node)))

#define PTY_FROM_SLAVE(n) \
    ((ax_pty_t *)((char *)(n) - offsetof(ax_pty_t, slave_node)))


/*
 * ============================================================
 * Ring buffer
 * ============================================================
 */

static void ring_init(pty_ring_t *r)
{
    r->head = 0;
    r->tail = 0;
    r->count = 0;
}

static u32int ring_free(const pty_ring_t *r)
{
    return PTY_RING_SIZE - r->count;
}

static int ring_push(pty_ring_t *r, u8int c)
{
    if (r->count >= PTY_RING_SIZE)
        return 0;

    r->buf[r->head] = c;

    r->head++;

    if (r->head >= PTY_RING_SIZE)
        r->head = 0;

    r->count++;

    return 1;
}

static int ring_pop(pty_ring_t *r, u8int *out)
{
    if (r->count == 0)
        return 0;

    *out = r->buf[r->tail];

    r->tail++;

    if (r->tail >= PTY_RING_SIZE)
        r->tail = 0;

    r->count--;

    return 1;
}

static u32int ring_write(
    pty_ring_t *r,
    const u8int *buf,
    u32int size
)
{
    u32int n = 0;

    while (n < size) {
        if (!ring_push(r, buf[n]))
            break;

        n++;
    }

    return n;
}

static u32int ring_read(
    pty_ring_t *r,
    u8int *buf,
    u32int size
)
{
    u32int n = 0;

    while (n < size) {
        if (!ring_pop(r, &buf[n]))
            break;

        n++;
    }

    return n;
}


/*
 * ============================================================
 * fs_node
 * ============================================================
 */

static void pty_node_init(
    fs_node_t *node,
    const char *name,
    read_type_t read,
    write_type_t write
)
{
    memset(node, 0, sizeof(*node));

    strcpy(node->name, name);

    node->mask   = 0;
    node->uid    = 0;
    node->gid    = 0;
    node->inode  = 0;
    node->length = 0;

    node->flags = FS_CHARDEVICE;

    node->read  = read;
    node->write = write;

    node->open    = 0;
    node->close   = 0;
    node->readdir = 0;
    node->finddir = 0;

    node->ptr  = 0;
    node->impl = 0;
}


/*
 * ============================================================
 * SLAVE
 *
 * Процесс видит slave.
 *
 * read(slave)
 *     <- m2s
 *
 * write(slave)
 *     -> s2m
 * ============================================================
 */

static u32int pty_slave_read(
    fs_node_t *node,
    u32int off,
    u32int size,
    u8int *buf
)
{
    ax_pty_t *pty;

    (void)off;

    if (!node || !buf || size == 0)
        return 0;

    pty = PTY_FROM_SLAVE(node);

    if (!pty->in_use)
        return 0;

    debug_puts("PTY SLAVE READ count=");

    debug_putnum(
        (uint64_t)pty->m2s.count
    );

    debug_putc('\n');

    return ring_read(
        &pty->m2s,
        buf,
        size
    );
}


static u32int pty_slave_write(
    fs_node_t *node,
    u32int off,
    u32int size,
    u8int *buf
)
{
    ax_pty_t *pty;

    (void)off;

    if (!node || !buf || size == 0)
        return 0;

    pty = PTY_FROM_SLAVE(node);

    if (!pty->in_use)
        return 0;

    /*
     * Вывод процесса:
     *
     * shell
     *   |
     *   v
     * slave_write()
     *   |
     *   v
     * s2m
     *   |
     *   v
     * master_read()
     */

    return ring_write(
        &pty->s2m,
        buf,
        size
    );
}


/*
 * ============================================================
 * MASTER
 *
 * Терминальный эмулятор видит master.
 *
 * read(master)
 *     <- s2m
 *
 * write(master)
 *     -> keyboard input -> m2s
 * ============================================================
 */

static u32int pty_master_read(
    fs_node_t *node,
    u32int off,
    u32int size,
    u8int *buf
)
{
    ax_pty_t *pty;

    (void)off;

    if (!node || !buf || size == 0)
        return 0;

    pty = PTY_FROM_MASTER(node);

    if (!pty->in_use)
        return 0;

    return ring_read(
        &pty->s2m,
        buf,
        size
    );
}


static u32int pty_master_write(
    fs_node_t *node,
    u32int off,
    u32int size,
    u8int *buf
)
{
    ax_pty_t *pty;

    (void)off;

    if (!node || !buf || size == 0)
        return 0;

    pty = PTY_FROM_MASTER(node);

    if (!pty->in_use)
        return 0;

    /*
     * Не помещаем данные напрямую в m2s.
     *
     * Они должны пройти через line discipline.
     */

    for (u32int i = 0; i < size; i++)
        pty_master_feed(pty, (char)buf[i]);

    return size;
}


/*
 * ============================================================
 * Keyboard -> PTY master
 * ============================================================
 */

void pty_master_feed(ax_pty_t *pty, char c)
{
     debug_puts("PTY FEED: ");

    debug_putnum((uint64_t)(u8int)c);

    debug_putc('\n');

    if (!pty || !pty->in_use)
        return;


    /*
     * --------------------------------------------------------
     * RAW MODE
     * --------------------------------------------------------
     */

    if (!pty->canonical) {

        if (ring_free(&pty->m2s) == 0)
            return;

        ring_push(
            &pty->m2s,
            (u8int)c
        );

        if (pty->echo) {
            ring_push(
                &pty->s2m,
                (u8int)c
            );
        }

        return;
    }


    /*
     * --------------------------------------------------------
     * CANONICAL MODE
     * --------------------------------------------------------
     */

    /*
     * ENTER
     */

    if (c == '\r' || c == '\n') {

        /*
         * Нормализуем CR и LF в LF.
         */

        if (pty->line_len < PTY_LINE_MAX)
            pty->line[pty->line_len++] = '\n';

        /*
         * Передаём строку процессу.
         */

        ring_write(
            &pty->m2s,
            (u8int *)pty->line,
            (u32int)pty->line_len
        );

        /*
         * Echo.
         *
         * Терминальный эмулятор получает CR/LF.
         */

        if (pty->echo) {
            ring_push(&pty->s2m, '\r');
            ring_push(&pty->s2m, '\n');
        }

        pty->line_len = 0;

        return;
    }


    /*
     * --------------------------------------------------------
     * BACKSPACE
     * --------------------------------------------------------
     */

    if (c == '\b' || (u8int)c == 127) {

        if (pty->line_len > 0) {

            pty->line_len--;

            if (pty->echo) {

                /*
                 * Удаление символа на терминале:
                 *
                 * BS
                 * SPACE
                 * BS
                 */

                ring_push(&pty->s2m, '\b');
                ring_push(&pty->s2m, ' ');
                ring_push(&pty->s2m, '\b');
            }
        }

        return;
    }


    /*
     * --------------------------------------------------------
     * CTRL-C
     * --------------------------------------------------------
     *
     * Пока у нас нет полноценной signal subsystem.
     *
     * Поэтому передаём 0x03 процессу.
     */

    if ((u8int)c == 3) {

        pty->line_len = 0;

        ring_push(
            &pty->m2s,
            3
        );

        if (pty->echo) {
            ring_push(&pty->s2m, '^');
            ring_push(&pty->s2m, 'C');
            ring_push(&pty->s2m, '\r');
            ring_push(&pty->s2m, '\n');
        }

        return;
    }


    /*
     * --------------------------------------------------------
     * Обычный символ
     * --------------------------------------------------------
     */

    if (pty->line_len >= PTY_LINE_MAX - 1)
        return;

    pty->line[pty->line_len++] = c;

    if (pty->echo) {
        ring_push(
            &pty->s2m,
            (u8int)c
        );
    }
}


/*
 * ============================================================
 * PTY pool
 * ============================================================
 */

static ax_pty_t pty_pool[MAX_PTYS];

static bool pty_subsystem_ready = false;


/*
 * ============================================================
 * Initialization
 * ============================================================
 */

void pty_subsystem_init(void)
{
    memset(
        (u8int *)pty_pool,
        0,
        sizeof(pty_pool)
    );

    for (int i = 0; i < MAX_PTYS; i++) {
        pty_pool[i].in_use = false;
        pty_pool[i].canonical = true;
        pty_pool[i].echo = true;
    }

    pty_subsystem_ready = true;
}


/*
 * ============================================================
 * Allocate
 * ============================================================
 */

ax_pty_t *pty_alloc(void)
{
    if (!pty_subsystem_ready)
        return 0;

    for (int i = 0; i < MAX_PTYS; i++) {

        if (!pty_pool[i].in_use) {

            ax_pty_t *pty = &pty_pool[i];

            memset(
                (u8int *)pty,
                0,
                sizeof(*pty)
            );

            ring_init(&pty->m2s);
            ring_init(&pty->s2m);

            pty->canonical = true;
            pty->echo = true;
            pty->line_len = 0;
            pty->in_use = true;

            pty_node_init(
                &pty->master_node,
                "ptmx",
                pty_master_read,
                pty_master_write
            );

            pty_node_init(
                &pty->slave_node,
                "tty",
                pty_slave_read,
                pty_slave_write
            );

            return pty;
        }
    }

    return 0;
}


/*
 * ============================================================
 * Free
 * ============================================================
 */

void pty_free(ax_pty_t *pty)
{
    if (!pty)
        return;

    pty->in_use = false;

    pty->line_len = 0;

    ring_init(&pty->m2s);
    ring_init(&pty->s2m);
}
void pty_flush_input(ax_pty_t *pty)
{
    if (!pty)
        return;

    ring_init(&pty->m2s);
    pty->line_len = 0;
}