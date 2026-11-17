#ifndef AXPORT_H
#define AXPORT_H

/*
 * ============================================================
 * axport.h -- мёжпроцессный протокол ArtyomX64 (модель NeXT/macOS)
 * ============================================================
 *
 * Модель как в MacOS X / NeXTSTEP:
 *
 *   [ядро]   -- только ПРИМИТИВЫ: именованные порты сообщений
 *               (аналог Mach ports), разделяемая память, tty,
 *               ввод, информация о дисплее.  Ядро НЕ знает
 *               ничего о окнах, кнопках и композиторе.
 *
 *   [wserver] -- ОТДЕЛЬНЫЙ процесс (userspace ELF в initrd),
 *               владеет framebuffer'ом, окнами, событиями,
 *               меню, доком, обоями.  Аналог WindowServer.
 *
 *   [клиенты] -- приложения (notes, files, temu, ...).  Не трогают
 *               экран сами: рисуют в свой canvas (разделяемая
 *               память) и общаются с wserver ИСКЛЮЧИТЕЛЬНО
 *               сообщениями через порт "wserver".
 *
 * Этот заголовок -- общая "сетевая" часть протокола: структура
 * сообщения ax_msg_t и номера сообщений.  Ядро понимает ТОЛЬКО
 * конверт (копирует сообщения целиком), семантику полей знают
 * только wserver и клиенты (lib/axipc.h + lib/libax.h).
 *
 * Сисколлы-примитивы (kernel/ipc.c):
 *
 *   178 PORT_CREATE(name)         -> id порта (именованный) или анонимный
 *   179 PORT_LOOKUP(name)         -> id или -1
 *   180 PORT_SEND(port, msg)      -> 0 / -1 (очередь полна)
 *   181 PORT_RECV(port, msg)      -> 1 есть / 0 пусто
 *   182 GETPID()                  -> pid
 *   183 SHM_ALLOC(size)           -> ptr (HHDM, общий для всех)
 *   184 SHM_FREE(ptr)             -> 0
 *   185 TERM_OPEN()               -> id tty (pty_alloc; ctty вызывающего = slave)
 *   186 TERM_READ(id, buf, len)   -> байты
 *   187 TERM_WRITE(id, buf, len)  -> байты
 *   188 TERM_FOCUS(id)            -> 0  (клавиатура следует за фокусом)
 *   189 FB_INFO(out)              -> 0  (addr/w/h/pitch/bpp дисплея)
 *   190 INPUT_MOUSE(out)          -> 0  (x/y/buttons после handle_mouse())
 *   191 INPUT_KEY(out)            -> 1/0 (сканкод + shift/caps)
 *   192 PROC_NAME(pid, buf, len)  -> 0/-1
 *   193 GPU_SUBMIT_RT(canvas,w,h,verts,count) -> rc  (ixg, без понятия "окон")
 *   195 TERM_RELEASE(id)          -> 0
 */

#include <stdint.h>

/* Конверт сообщения. Ядро копирует его целиком, не заглядывая
 * в семантику. Размер данных ограничен AX_MSG_DATA_MAX. */
#define AX_MSG_DATA_MAX 980

typedef struct {
    uint32_t type;              /* MSG_* из этого заголовка            */
    uint32_t win;               /* токен окна = (uint32_t)canvas ptr   */
    uint32_t from;              /* порт отправителя (куда слать ответ) */
    uint32_t a, b;              /* обобщённые аргументы                */
    uint32_t x, y;              /* обобщённые аргументы                */
    uint32_t len;               /* полезная длина data[] в байтах      */
    uint8_t  data[AX_MSG_DATA_MAX];
} ax_msg_t;

/* ---- Клиент -> wserver ---- */
#define MSG_REQ_SURFACE     1   /* a=w b=h x=pid data:title  -> REPLY_SURFACE */
#define MSG_REQ_COMMIT      3   /* win: канвас изменился (dirty)             */
#define MSG_REQ_WIN_CLOSE   4   /* win: приложение закрыло своё окно         */
#define MSG_REQ_WIN_FOCUS   5   /* win: вывести окно на передний план        */
#define MSG_REQ_RESIZE      6   /* win a=w b=h -> REPLY_RESIZE (новый canvas)*/
#define MSG_REQ_MENU_SET    8   /* x=pid data: сер. ax_menubar_wire_t        */
#define MSG_REQ_SCREEN      10  /* -> REPLY_SCREEN (scr_w/scr_h сервера)     */
#define MSG_REQ_TIME        12  /* -> REPLY_TIME (RTC читает сервер)         */
#define MSG_REQ_TERM_ATTACH 14  /* win a=tty_id b=pid: окно владеет tty      */

/* ---- AxGL (3D через сервер) ---- */
#define MSG_REQ_GL_BEGIN    20  /* win a=w b=h: начать батч (сброс zbuf)     */
#define MSG_REQ_GL_TRI      21  /* win data: пачка axgl_hw_vertex_t          */
#define MSG_REQ_GL_END      22  /* win -> REPLY_GL (rc)                      */
#define MSG_REQ_GL_BACKEND  24  /* -> REPLY_GLSTR ("Software"/...)           */
#define MSG_REQ_GL_SETMODE  26  /* a=mode -> REPLY_GL (mode)                 */

/* ---- wserver -> клиент ---- */
#define MSG_EVENT           30  /* a=ev.type b=ev.key x=mx y=my
                                   data: buttons(4) w(4) h(4)                */
#define MSG_REPLY_SURFACE   2   /* a=ptr_lo b=ptr_hi x=w y=h                 */
#define MSG_REPLY_RESIZE    7   /* a=ptr_lo b=ptr_hi x=w y=h                 */
#define MSG_REPLY_OK        9   /* a=rc                                      */
#define MSG_REPLY_SCREEN    11  /* a=w b=h                                   */
#define MSG_REPLY_TIME      13  /* data: ax_time_t (28 байт)                 */
#define MSG_REPLY_GL        23  /* a=rc                                      */
#define MSG_REPLY_GLSTR     25  /* data: имя бэкенда, NUL-терминировано      */

/* Лимиты портов в ядре (см. kernel/ipc.c). */
#define AX_PORTS_MAX 16
#define AX_PORT_QLEN 32

/* ---- Структуры-примитивы (PLAIN DATA, для сисколлов 189-192) ---- */

/* 189 FB_INFO: параметры дисплея от Limine. */
typedef struct {
    uint64_t addr;      /* HHDM-виртуальный адрес framebuffer'а */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} ax_fb_info_t;

/* 190 INPUT_MOUSE: состояние мыши после handle_mouse(). */
typedef struct {
    int32_t  x, y;
    uint32_t buttons;   /* бит 0 = левая, бит 1 = правая */
} ax_mouse_info_t;

/* 191 INPUT_KEY: одно событие клавиатуры (сканкод + модификаторы). */
typedef struct {
    int32_t  key;       /* -1 нет; 0..127 сканкод; 0xF1/0xF4/0xF5 Ctrl+S/O/N */
    uint32_t shift;     /* Shift зажат                            */
    uint32_t caps;      /* CapsLock включён                       */
} ax_key_info_t;

#endif /* AXPORT_H */
