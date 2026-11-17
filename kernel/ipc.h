#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

/*
 * ============================================================
 * ipc.h -- межпроцессные примитивы ядра ArtyomX64 (модель Mach)
 * ============================================================
 *
 * Здесь живёт ВСЁ, что ядро даёт процессам для общения:
 *
 *   - именованные порты сообщений (PORT_CREATE/LOOKUP/SEND/RECV);
 *   - разделяемая память (SHM_ALLOC/SHM_FREE: блоки kernel heap,
 *     которые через общий HHDM видны всем адресным пространствам);
 *   - tty-сервис (TERM_*: pty как объект драйвера, как BSD tty
 *     в macOS -- окнами ядро не занимается);
 *   - ввод (INPUT_MOUSE/INPUT_KEY: сырые HID-примитивы);
 *   - информация о дисплее (FB_INFO) и о процессах (PROC_NAME);
 *   - сырой GPU-сабмит ixg (GPU_SUBMIT_RT: буфер + размер, без окон).
 *
 * Сисколлы: 178-193, 195.  Обработчики регистрирует
 * lib/syscalls.c (initialise_syscalls вызывает ipc_init()).
 */

#include "../lib/axport.h"
#include "../lib/common.h"

/* ---- Инициализация (очистка таблицы портов). ---- */
void ipc_init(void);

/* ---- Порты сообщений ---- */
int  ipc_port_create(const char *name /* NULL = анонимный */);
int  ipc_port_lookup(const char *name);
int  ipc_port_send(int port, const ax_msg_t *msg);
int  ipc_port_recv(int port, ax_msg_t *out);
void ipc_ports_reap_dead(void);   /* снять порты умерших владельцев */

/* ---- Разделяемая память ---- */
void *ipc_shm_alloc(uint64_t size);
void  ipc_shm_free(void *ptr);

/* ---- tty-сервис (pty как BSD tty) ---- */
int ipc_term_open(void);
int ipc_term_read(int id, uint8_t *buf, uint32_t maxlen);
int ipc_term_write(int id, const uint8_t *buf, uint32_t len);
int ipc_term_focus(int id);
int ipc_term_release(int id);

/* ---- Дисплей / ввод / процессы ---- */
int ipc_fb_info(ax_fb_info_t *out);
int ipc_input_mouse(ax_mouse_info_t *out);
int ipc_input_key(ax_key_info_t *out);
int ipc_proc_name(int pid, char *buf, uint32_t len);
int ipc_getpid(void);

/* ---- Сырой GPU-сабмит ixg (без понятия "окон") ---- */
int ipc_gpu_submit_rt(uint64_t canvas, uint32_t w, uint32_t h,
                      const void *verts, uint32_t count);

#endif /* KERNEL_IPC_H */
