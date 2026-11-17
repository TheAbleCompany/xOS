/*
 * ============================================================
 * axgl.h -- AxGL kernel side (mini-OpenGL driver interface)
 * ============================================================
 *
 * AxGL -- простой OpenGL-подобный 3D API для ArtyomX OS.
 *
 * Слой пользователя (apps/axgl.h) делает всю математику:
 * матрицы MODELVIEW/PROJECTION, клиппинг, перспективное
 * деление -- и получает экранные вершины в формате
 * ixg_hw_vertex_t (x, y, z, rhw, u, v, color -- тот же формат,
 * что ждут GPU-бэкенды ixg: NVIDIA / AMD / Intel).
 *
 * Дальше вершины уходят в syscall AX_SYS_GPU_TRIANGLES, и ядро:
 *
 *   1. Если драйвер ixg нашёл поддерживаемый GPU и его 3D
 *      бэкенд умеет hw_draw_triangles -- вершины уходят прямо
 *      в GPU-конвейер (растеризация на видеокарте, НЕ на CPU).
 *
 *   2. Если GPU не найден (QEMU, виртуалка, неподдерживаемое
 *      железо) -- работает встроенный программный растеризатор
 *      ядра (аналог llvmpipe у Mesa): треугольники с Gouraud-
 *      интерполяцией цвета и Z-буфером рисуются в canvas окна.
 *
 * Приложение одно и то же; выбор бэкенда прозрачен, как у
 * настоящего OpenGL (ICD + программный fallback).
 */

#ifndef AXGL_KERNEL_H
#define AXGL_KERNEL_H

#include "../lib/common.h"

/*
 * AX_SYS_GPU_TRIANGLES:
 *   rdi = canvas (указатель окна, полученный от ax_surface)
 *   rsi = указатель на массив вершин axgl_user_vertex_t
 *   rdx = число вершин (кратно 3; каждый триплет -- треугольник)
 *
 * Возвращает:
 *   > 0 -- вершины отрендерены GPU-бэкендом ixg (не CPU)
 *   = 0 -- вершины отрендерены программным растеризатором ядра
 *   < 0 -- ошибка (нет такого окна, плохой счётчик вершин, ...)
 */
#define AX_SYS_GPU_TRIANGLES 175

/*
 * AX_SYS_GPU_BACKEND:
 *   rdi = canvas
 *
 * Возвращает указатель (только чтение) на имя активного
 * бэкенда: "NVIDIA", "AMD", "Intel" или "Software".
 */
#define AX_SYS_GPU_BACKEND   176

/*
 * AX_SYS_GPU_SETMODE:
 *   rdi = режим рендеринга:
 *
 *     AXGL_MODE_AUTO      (0) -- по умолчанию.  Если ixg нашёл
 *         GPU с 3D-бэкендом -- батч уходит в GPU (драйверы
 *         пользователя работают).  Далее два случая:
 *           - бэкенд умеет CPU-visible render target (Intel,
 *             NVIDIA): после flush кадр readback'ом копируется
 *             в canvas окна -- на экране пиксели видеокарты;
 *           - readback недоступен (AMD/экзотика/QEMU): canvas
 *             окна заполняет программный растеризатор (SW-зеркало),
 *             чтобы окно гарантированно не было пустым.
 *
 *     AXGL_MODE_FORCE_SW  (1) -- только программный растеризатор
 *         в canvas (GPU не трогаем совсем).
 *
 *     AXGL_MODE_GPU_ONLY  (2) -- только GPU-сабмит (диагностика
 *         настоящего GPU-пути на реальном железе; canvas окна
 *         ядром не заполняется).
 *
 * Возвращает установленный режим (или -1 при неверном аргументе).
 */
#define AX_SYS_GPU_SETMODE   177

#define AXGL_MODE_AUTO     0
#define AXGL_MODE_FORCE_SW 1
#define AXGL_MODE_GPU_ONLY 2

/* Максимальное число вершин за один syscall. */
#define AXGL_MAX_VERTS 32767u

/*
 * Экранная вершина, которую шлёт пользовательский режим.
 * Layout ПОЛНОСТЬЮ совпадает с ixg_hw_vertex_t (drivers/gpu),
 * но ядро не тянет заголовки GPU-драйвера в UI-слой:
 * формат зафиксирован здесь и проверяется статическим
 * ассертом размерности в axgl.c.
 */
typedef struct {
    float   x, y;      /* экранные координаты, пиксели          */
    float   z;         /* NDC z в [-1..1] (для Z-теста/GPU)     */
    float   rhw;       /* 1/w (для перспективно-корректного GPU */
    float   u, v;      /* UV (зарезервировано, = 0)             */
    uint32_t color;    /* 0xAARRGGBB                            */
} axgl_user_vertex_t;

int  ax_syscall_gpu_triangles(uint32_t canvas_ptr,
                              const axgl_user_vertex_t *uverts,
                              uint32_t vert_count);

const char *ax_syscall_gpu_backend(uint32_t canvas_ptr);

int  ax_syscall_gpu_set_mode(uint32_t mode);
int  ax_syscall_gpu_get_mode(void);

#endif
