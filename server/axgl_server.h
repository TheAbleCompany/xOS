/*
 * ============================================================
 * axgl_server.h -- AxGL на стороне WindowServer
 * ============================================================
 *
 * AxGL -- простой OpenGL-подобный 3D API для ArtyomX64.
 *
 * МОДЕЛЬ NeXT/macOS: после выноса GUI из ядра весь GL-пайплайн
 * живёт в ПРОЦЕССЕ wserver (как GL-движок WindowServer'а):
 *
 *   app (apps/axgl.h: матрицы, клиппинг, вершины)
 *        -> сообщения MSG_REQ_GL_BEGIN / _TRI / _END через порт
 *        -> wserver_axgl.c
 *              |-- GPU найден ixg (kernel GPU_SUBMIT_RT)?
 *              |     да -> сабмит в видеокарту + readback в canvas
 *              |     нет -> программный растеризатор сервера
 *              |            (Z-буфер + Gouraud, fixed-point, -mno-sse)
 *              v
 *        пиксели в canvas окна -> commit -> композитинг
 *
 * Формат вершины СОВПАДАЕТ с apps/axgl.h (axgl_hw_vertex_t) и с
 * ixg_hw_vertex_t ядра: 7 x 4 байта = 28 байт.
 */

#ifndef AXGL_SERVER_H
#define AXGL_SERVER_H

#include "../lib/common.h"

#define AXGL_MAX_VERTS 32767u

/* Режимы рендеринга (были AXGL_MODE_* в kernel/axgl.h). */
#define AXGL_MODE_AUTO       0
#define AXGL_MODE_FORCE_SW   1
#define AXGL_MODE_GPU_ONLY   2

typedef struct {
    float    x, y;
    float    z;
    float    rhw;
    float    u, v;
    uint32_t color;
} axgl_user_vertex_t;

#endif /* AXGL_SERVER_H */
