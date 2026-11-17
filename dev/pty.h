#ifndef DEV_PTY_H
#define DEV_PTY_H
#include "../fs/fs.h"
#include <stdbool.h>

#define PTY_RING_SIZE 2048
#define PTY_LINE_MAX  256

typedef struct { u8int buf[PTY_RING_SIZE]; u32int head, tail, count; } pty_ring_t;

typedef struct ax_pty {
    pty_ring_t m2s;
    pty_ring_t s2m;
    fs_node_t  master_node;
    fs_node_t  slave_node;
    bool  canonical, echo;
    char  line[PTY_LINE_MAX];
    int   line_len;
    bool  in_use;
} ax_pty_t;

/* Обязательно вызвать один раз при старте ядра, до первого pty_alloc(). */
void      pty_subsystem_init(void);

ax_pty_t *pty_alloc(void);
void      pty_free(ax_pty_t *pty);
void      pty_master_feed(ax_pty_t *pty, char c);
void      pty_flush_input(ax_pty_t *pty);
#define PTY_FROM_MASTER(n) \
    ((ax_pty_t *)((char *)(n) - offsetof(ax_pty_t, master_node)))

#define PTY_FROM_SLAVE(n) \
    ((ax_pty_t *)((char *)(n) - offsetof(ax_pty_t, slave_node)))


#endif