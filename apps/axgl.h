/*
 * ============================================================
 * axgl.h -- AxGL: простой OpenGL-подобный 3D API (user side)
 * ============================================================
 *
 * Mini-OpenGL для ArtyomX OS в духе OpenGL 1.x fixed function:
 *
 *   axgl_perspective / axgl_rotatef / axgl_translatef  -- матрицы
 *   axgl_begin(AXGL_TRIANGLES) ... axgl_end()          -- immediate mode
 *   axgl_vertex3f / axgl_color3f                       -- атрибуты
 *
 * Как у настоящего OpenGL, вершинный конвейер двухступенчатый:
 *
 *   1. Пользовательский слой (этот файл, собран с -msse2):
 *      MVP-трансформация, клиппинг по near-плоскости,
 *      перспективное деление, viewport-маппинг, backface culling.
 *      Результат -- экранные вершины формата ixg_hw_vertex_t
 *      (тот же формат, что едят GPU-бэкенды ixg).
 *
 *   2. Ядро (syscall 175):
 *      - если найден GPU (NVIDIA/AMD/Intel) -- вершины уходят
 *        прямо в 3D-движок видеокарты (растеризация на GPU);
 *      - иначе -- программный растеризатор ядра (fallback).
 *
 * Приложение НЕ знает, какой путь активен (как ICD + llvmpipe);
 * имя активного бэкенда можно показать через axgl_backend().
 *
 * Использование:
 *
 *     #include "../apps/axgl.h"
 *
 *     uint32_t *cv = ax_surface("GL Demo", W, H);
 *     axgl_init(cv, W, H);
 *
 *     ... в кадре:
 *     axgl_clear(AXGL_RGB(20, 22, 34));
 *     axgl_matrix_mode(AXGL_PROJECTION);
 *     axgl_load_identity();
 *     axgl_perspective(60.0f, (float)W / H, 0.1f, 100.0f);
 *     axgl_matrix_mode(AXGL_MODELVIEW);
 *     axgl_load_identity();
 *     axgl_translatef(0, 0, -3.5f);
 *     axgl_rotatef(angle, 1, 1, 0);
 *
 *     axgl_begin(AXGL_TRIANGLES);
 *       axgl_color3f(1,0,0); axgl_vertex3f(-1,-1, 1); ...
 *     axgl_end();                    <-- отправляет кадр в GPU/ядро
 *
 *     ax_commit(cv);
 *
 * Сборка приложения: -msse2 -mfpmath=sse (как raycaster).
 */

#ifndef AXGL_USER_H
#define AXGL_USER_H

#include <stdint.h>
#include "../lib/axipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Константы (в стиле GL)
 * ============================================================ */

#define AXGL_TRIANGLES   0
#define AXGL_QUADS       1

#define AXGL_PROJECTION  0
#define AXGL_MODELVIEW   1

/* Режимы рендеринга (раньше -- kernel/axgl.h; теперь сервер). */
#define AXGL_MODE_AUTO       0
#define AXGL_MODE_FORCE_SW   1
#define AXGL_MODE_GPU_ONLY   2

/* Формат пикселя canvas: 0xAARRGGBB (совместим с AX_ARGB). */
#define AXGL_ARGB(a,r,g,b) (((uint32_t)(a)<<24)|((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))
#define AXGL_RGB(r,g,b)    AXGL_ARGB(0xFF,r,g,b)

/* Лимиты. */
#define AXGL_MAX_IMMEDIATE 4096   /* вершин за один begin/end   */
#define AXGL_MAX_SUBMIT    30000  /* вершин за один syscall     */
#define AXGL_NEAR_EPS      0.001f

/* ============================================================
 * Syscall-обвязка
 * ============================================================ */

/* Экранная вершина -- раскладка обязана совпадать с
 * ixg_hw_vertex_t из drivers/gpu/ixg_driver.h. */
typedef struct {
    float    x, y;    /* пиксели экрана                    */
    float    z;       /* NDC z [-1..1] (глубина)           */
    float    rhw;     /* 1/w для перспективно-корректного GPU */
    float    u, v;    /* UV (зарезервировано = 0)          */
    uint32_t color;   /* 0xAARRGGBB                        */
} axgl_hw_vertex_t;

/*
 * Точка расширения: если приложение (или host-тест) определило
 * AXGL_CUSTOM_SUBMIT до включения заголовка, оно обязано само
 * предоставить ax_gpu_triangles() -- например, для прогонов
 * конвейера на host-системе.
 */
#ifndef AXGL_CUSTOM_SUBMIT

/*
 * ============================================================
 * AxGL через оконный сервер (модель NeXT/macOS)
 * ============================================================
 *
 * Прежний сисколл AX_SYS_GPU_TRIANGLES (175) УДАЛЁН из ядра:
 * GL-пайплайн теперь живёт в wserver (аналог GL-движка
 * WindowServer'а).  Приложение шлёт вершины СООБЩЕНИЯМИ:
 *
 *   MSG_REQ_GL_BEGIN (сброс состояния кадра на сервере)
 *   MSG_REQ_GL_TRI   ...  пачки вершин (кратно 3)
 *   MSG_REQ_GL_END   -> REPLY_GL (итоговый rc)
 *
 * Вершин axgl_hw_vertex_t -- 28 байт; в сообщение помещается
 * (AX_MSG_DATA_MAX / 28) = 35 вершин, поэтому большой батч
 * автоматически режется на пачки.  Каждый треугольник целиком
 * попадает в одну пачку (растеризатор сервера рисует их
 * независимо, Z-буфер персистентный между пачками батча).
 */

#include "../lib/libax.h"

#define AXGL_VERTS_PER_MSG  ((int)(AX_MSG_DATA_MAX / sizeof(axgl_hw_vertex_t)))

static inline int ax_gpu_triangles(const void *canvas,
                                   const axgl_hw_vertex_t *verts,
                                   uint32_t count)
{
    if (!ax_connected())
        return -1;

    uint32_t tok = (uint32_t)(uintptr_t)canvas;

    /* BEGIN: сервер запомнит канвас/размеры окна и сбросит флаги. */
    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_REQ_GL_BEGIN;
    m.win  = tok;
    m.from = (uint32_t)g_ax_cli_port;
    if (ax_port_send(g_ax_srv_port, &m) != 0)
        return -1;

    /* Пачки вершин. */
    uint32_t sent = 0;
    while (sent < count) {
        uint32_t n = count - sent;
        if (n > (uint32_t)AXGL_VERTS_PER_MSG)
            n = (uint32_t)AXGL_VERTS_PER_MSG;

        memset((u8int *)&m, 0, sizeof(m));
        m.type = MSG_REQ_GL_TRI;
        m.win  = tok;
        m.from = (uint32_t)g_ax_cli_port;
        m.len  = n * (uint32_t)sizeof(axgl_hw_vertex_t);
        memcpy((u8int *)m.data, (u8int *)(verts + sent), m.len);

        if (ax_port_send(g_ax_srv_port, &m) != 0)
            return -1;

        sent += n;
    }

    /* END: сервер пометит окно грязным и вернёт итог. */
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_REQ_GL_END;
    m.win  = tok;
    m.from = (uint32_t)g_ax_cli_port;
    if (ax_port_send(g_ax_srv_port, &m) != 0)
        return -1;

    ax_msg_t r;
    if (ax_wait_reply(MSG_REPLY_GL, tok, &r) != 0)
        return -1;

    return (int32_t)r.a;
}

static inline const char *ax_gpu_backend_name(const void *canvas)
{
    static char axgl_backend_buf[24];

    if (!ax_connected())
        return "Software";

    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_REQ_GL_BACKEND;
    m.win  = (uint32_t)(uintptr_t)canvas;
    m.from = (uint32_t)g_ax_cli_port;

    if (ax_port_send(g_ax_srv_port, &m) != 0)
        return "Software";

    ax_msg_t r;
    if (ax_wait_reply(MSG_REPLY_GLSTR, 0, &r) != 0)
        return "Software";

    int k = 0;
    while (k < 23 && k < (int)r.len && r.data[k]) {
        axgl_backend_buf[k] = (char)r.data[k];
        k++;
    }
    axgl_backend_buf[k] = 0;

    return axgl_backend_buf;
}

#endif /* AXGL_CUSTOM_SUBMIT */

/* ============================================================
 * Маленькая float-математика (freestanding, SSE2)
 * ============================================================ */

static float axgl__sqrtf(float x)
{
    float r = x;
    if (x <= 0.0f) return 0.0f;
    for (int i = 0; i < 12; i++)          /* Ньютон, ~float-точность */
        r = 0.5f * (r + x / r);
    return r;
}

static float axgl__sinf(float x)
{
    /* приведение к [-pi, pi] + полином -- как в raycaster */
    const float PI = 3.14159265358979f;
    while (x >  PI) x -= 2.0f * PI;
    while (x < -PI) x += 2.0f * PI;
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f
                     + x2 * x2 / 120.0f
                     - x2 * x2 * x2 / 5040.0f);
}

static float axgl__cosf(float x) { return axgl__sinf(x + 1.5707963267948f); }

/* ============================================================
 * Состояние контекста (один на процесс -- как старый GL)
 * ============================================================ */

typedef struct {
    /* матрицы, row-major 4x4 */
    float proj[16];
    float model[16];
    int   cur_matrix;

    /* immediate mode */
    int    prim;
    int    prim_active;
    struct {
        float x, y, z;
        float r, g, b, a;
    } imm[AXGL_MAX_IMMEDIATE];
    int    imm_n;
    float  cr, cg, cb, ca;

    /* viewport / цель */
    uint32_t *canvas;
    int       vw, vh;
    float     znear;    /* дистанция near-плоскости (для клипа) */

    /* настройки */
    int cull_enabled;
} axgl_ctx_t;

static axgl_ctx_t axgl__c;

/* ============================================================
 * Матрицы
 * ============================================================ */

static void axgl__mat_identity(float *m)
{
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* out = a * b -- раскладка column-major, как в OpenGL
 * (индекс = column * 4 + row; семантика: b применяется к
 * вектору первой, затем a -- GL-постфиксное умножение). */
static void axgl__mat_mul(float *out, const float *a, const float *b)
{
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = s;
        }
    for (int i = 0; i < 16; i++) out[i] = t[i];
}

/* v' = m * v (vec4) -- column-major: столбец k матрицы
 * умножается на v[k] и складывается. */
static void axgl__mat_vec4(const float *m, const float *v, float *out)
{
    for (int r = 0; r < 4; r++) {
        out[r] = m[0 * 4 + r] * v[0] +
                 m[1 * 4 + r] * v[1] +
                 m[2 * 4 + r] * v[2] +
                 m[3 * 4 + r] * v[3];
    }
}

static float *axgl__current_matrix(void)
{
    return (axgl__c.cur_matrix == AXGL_PROJECTION)
         ? axgl__c.proj : axgl__c.model;
}

/* ============================================================
 * Публичный API
 * ============================================================ */

/* Привязать canvas окна (из ax_surface) и задать viewport. */
static void axgl_init(uint32_t *canvas, int w, int h)
{
    for (int i = 0; i < (int)sizeof(axgl__c); i++)
        ((uint8_t *)&axgl__c)[i] = 0;

    axgl__c.canvas = canvas;
    axgl__c.vw = w;
    axgl__c.vh = h;
    axgl__c.cr = axgl__c.cg = axgl__c.cb = 1.0f;
    axgl__c.ca = 1.0f;
    axgl__c.znear = 0.1f;
    axgl__c.cull_enabled = 1;
    axgl__mat_identity(axgl__c.proj);
    axgl__mat_identity(axgl__c.model);
}

static void axgl_viewport(int w, int h)
{
    axgl__c.vw = w;
    axgl__c.vh = h;
}

static void axgl_cull_face(int enable) { axgl__c.cull_enabled = enable; }

static void axgl_matrix_mode(int mode) { axgl__c.cur_matrix = mode; }

static void axgl_load_identity(void)
{
    axgl__mat_identity(axgl__current_matrix());
}

/* GL-совместимая перспективная матрица. */
static void axgl_perspective(float fovy_deg, float aspect,
                             float znear, float zfar)
{
    float *m = axgl__c.proj;
    /* cot(fovy/2): через sin/cos, чтобы обойтись без libm */
    float half = fovy_deg * 0.5f * 0.0174532925f;
    float s    = axgl__sinf(half);
    float c    = axgl__cosf(half);
    float cot  = (s != 0.0f) ? (c / s) : 1.0f;
    float zn_n = znear, zf_n = zfar;

    for (int i = 0; i < 16; i++) m[i] = 0.0f;

    /* Запоминаем near-плоскость: клиппинг ведётся по
     * w >= znear (w_clip = -z_view = дистанция до точки). */
    axgl__c.znear = (zn_n > 0.0f) ? zn_n : 0.1f;

    /* GL-учебник (column-major): w_clip = -z_view. */
    m[0]  = cot / aspect;
    m[5]  = cot;
    m[10] = (zf_n + zn_n) / (zn_n - zf_n);
    m[11] = -1.0f;
    m[14] = (2.0f * zf_n * zn_n) / (zn_n - zf_n);
    m[15] = 0.0f;
}

static void axgl_translatef(float x, float y, float z)
{
    float t[16], out[16];
    axgl__mat_identity(t);
    t[12] = x; t[13] = y; t[14] = z;
    float *m = axgl__current_matrix();
    axgl__mat_mul(out, m, t);
    for (int i = 0; i < 16; i++) m[i] = out[i];
}

static void axgl_scalef(float x, float y, float z)
{
    float t[16], out[16];
    axgl__mat_identity(t);
    t[0] = x; t[5] = y; t[10] = z;
    float *m = axgl__current_matrix();
    axgl__mat_mul(out, m, t);
    for (int i = 0; i < 16; i++) m[i] = out[i];
}

/* Поворот на angle градусов вокруг оси (x,y,z) -- Rodrigues. */
static void axgl_rotatef(float angle_deg, float x, float y, float z)
{
    float a = angle_deg * 0.0174532925f;
    float s = axgl__sinf(a), c = axgl__cosf(a);
    float len = axgl__sqrtf(x * x + y * y + z * z);
    if (len < 1e-8f) return;
    x /= len; y /= len; z /= len;

    float t[16], out[16];
    float ic = 1.0f - c;

    /* Rodrigues, column-major (как glRotate в спецификации GL). */
    t[0]  = c + x * x * ic;      t[1]  = y * x * ic + z * s; t[2]  = z * x * ic - y * s; t[3]  = 0.0f;
    t[4]  = x * y * ic - z * s;  t[5]  = c + y * y * ic;    t[6]  = z * y * ic + x * s; t[7]  = 0.0f;
    t[8]  = x * z * ic + y * s;  t[9]  = y * z * ic - x * s; t[10] = c + z * z * ic;   t[11] = 0.0f;
    t[12] = 0.0f;                t[13] = 0.0f;              t[14] = 0.0f;              t[15] = 1.0f;

    float *m = axgl__current_matrix();
    axgl__mat_mul(out, m, t);
    for (int i = 0; i < 16; i++) m[i] = out[i];
}

/* ============================================================
 * Immediate mode
 * ============================================================ */

static void axgl_color3f(float r, float g, float b)
{
    axgl__c.cr = r; axgl__c.cg = g; axgl__c.cb = b; axgl__c.ca = 1.0f;
}

static void axgl_color4f(float r, float g, float b, float a)
{
    axgl__c.cr = r; axgl__c.cg = g; axgl__c.cb = b; axgl__c.ca = a;
}

static void axgl_begin(int prim)
{
    axgl__c.prim = prim;
    axgl__c.prim_active = 1;
    axgl__c.imm_n = 0;
}

static void axgl_vertex3f(float x, float y, float z)
{
    if (!axgl__c.prim_active) return;
    if (axgl__c.imm_n >= AXGL_MAX_IMMEDIATE) return;

    axgl__c.imm[axgl__c.imm_n].x = x;
    axgl__c.imm[axgl__c.imm_n].y = y;
    axgl__c.imm[axgl__c.imm_n].z = z;
    axgl__c.imm[axgl__c.imm_n].r = axgl__c.cr;
    axgl__c.imm[axgl__c.imm_n].g = axgl__c.cg;
    axgl__c.imm[axgl__c.imm_n].b = axgl__c.cb;
    axgl__c.imm[axgl__c.imm_n].a = axgl__c.ca;
    axgl__c.imm_n++;
}

/* Очистка canvas цветом (аналог glClear -- прямой fill буфера). */
static void axgl_clear(uint32_t color)
{
    if (!axgl__c.canvas) return;
    uint32_t n = (uint32_t)axgl__c.vw * (uint32_t)axgl__c.vh;
    for (uint32_t i = 0; i < n; i++) axgl__c.canvas[i] = color;
}

static uint32_t axgl__pack_color(float r, float g, float b, float a)
{
    int R = (int)(r * 255.0f + 0.5f);
    int G = (int)(g * 255.0f + 0.5f);
    int B = (int)(b * 255.0f + 0.5f);
    int A = (int)(a * 255.0f + 0.5f);
    if (R < 0) R = 0;
    if (R > 255) R = 255;
    if (G < 0) G = 0;
    if (G > 255) G = 255;
    if (B < 0) B = 0;
    if (B > 255) B = 255;
    if (A < 0) A = 0;
    if (A > 255) A = 255;
    return ((uint32_t)A << 24) | ((uint32_t)R << 16) |
           ((uint32_t)G << 8)  | (uint32_t)B;
}

/* Клиппинг полигона по near-плоскости w >= znear
 * (Sutherland-Hodgman).  w_clip = -z_view -- дистанция от
 * камеры, поэтому near-плоскость -- это w = znear.
 * Возвращает число вершин результата (0 -- целиком за near). */
typedef struct { float x, y, z, w; float r, g, b, a; } axgl__cv_t;

static int axgl__clip_near(axgl__cv_t *poly, int n)
{
    axgl__cv_t out[8];
    int m = 0;

    for (int i = 0; i < n; i++) {
        axgl__cv_t *cur = &poly[i];
        axgl__cv_t *nxt = &poly[(i + 1) % n];
        float wmin = (axgl__c.znear > AXGL_NEAR_EPS)
                   ? axgl__c.znear : AXGL_NEAR_EPS;
        float dc = cur->w - wmin;
        float dn = nxt->w - wmin;

        if (dc >= 0.0f) out[m++] = *cur;          /* cur внутри   */
        if ((dc >= 0.0f) != (dn >= 0.0f)) {       /* пересечение  */
            float t = dc / (dc - dn);
            axgl__cv_t p;
            p.x = cur->x + t * (nxt->x - cur->x);
            p.y = cur->y + t * (nxt->y - cur->y);
            p.z = cur->z + t * (nxt->z - cur->z);
            p.w = cur->w + t * (nxt->w - cur->w);
            p.r = cur->r + t * (nxt->r - cur->r);
            p.g = cur->g + t * (nxt->g - cur->g);
            p.b = cur->b + t * (nxt->b - cur->b);
            p.a = cur->a + t * (nxt->a - cur->a);
            if (m < 8) out[m++] = p;
        }
        if (m >= 7) break;                        /* защита буфера */
    }

    for (int i = 0; i < m; i++) poly[i] = out[i];
    return m;
}

/* Преобразовать один треугольник clip-space -> экран и добавить
 * в батчик отправки. Возвращает число добавленных вершин. */
static int axgl__emit_tri(axgl_hw_vertex_t *out, int out_n, int out_cap,
                          const axgl__cv_t *ct)
{
    float sx[3], sy[3], sz[3], rhw[3];
    uint32_t col[3];
    float area;

    for (int i = 0; i < 3; i++) {
        float inv_w = 1.0f / ct[i].w;
        float ndc_x = ct[i].x * inv_w;
        float ndc_y = ct[i].y * inv_w;
        float ndc_z = ct[i].z * inv_w;

        sx[i]  = (ndc_x + 1.0f) * 0.5f * (float)axgl__c.vw;
        sy[i]  = (1.0f - ndc_y) * 0.5f * (float)axgl__c.vh;
        /* Глубина в диапазоне [0..1] (как glDepthRange по
         * умолчанию): near -> 0, far -> 1.  Меньше = ближе --
        * ровно то, что ждёт Z-тест GL_LESS в ядре. */
        sz[i]  = ndc_z * 0.5f + 0.5f;
        rhw[i] = inv_w;
        col[i] = axgl__pack_color(ct[i].r, ct[i].g, ct[i].b, ct[i].a);

        /* Клэмп в безопасный для int32-растеризатора диапазон
         * (canvas всегда внутри, форма видимой части не меняется).
         * Заодно гарантирует санитарный диапазон ядра. */
        if (sx[i] < -4096.0f)  sx[i] = -4096.0f;
        if (sx[i] >  20480.0f) sx[i] = 20480.0f;
        if (sy[i] < -4096.0f)  sy[i] = -4096.0f;
        if (sy[i] >  20480.0f) sy[i] = 20480.0f;
    }

    /* Backface culling (GL: front = CCW; экран с Y вниз -->
     * front-грани имеют отрицательную screen-area). */
    if (axgl__c.cull_enabled) {
        area = (sx[1] - sx[0]) * (sy[2] - sy[0])
             - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (area >= 0.0f) return out_n;   /* back -- отсекаем */
    }

    /* Полностью за экраном -- не тратим слоты. */
    if ((sx[0] < 0 && sx[1] < 0 && sx[2] < 0) ||
        (sy[0] < 0 && sy[1] < 0 && sy[2] < 0) ||
        (sx[0] > axgl__c.vw && sx[1] > axgl__c.vw && sx[2] > axgl__c.vw) ||
        (sy[0] > axgl__c.vh && sy[1] > axgl__c.vh && sy[2] > axgl__c.vh))
        return out_n;

    if (out_n + 3 > out_cap) return out_n;

    for (int i = 0; i < 3; i++) {
        out[out_n].x  = sx[i];
        out[out_n].y  = sy[i];
        out[out_n].z  = sz[i];
        out[out_n].rhw = rhw[i];
        out[out_n].u  = 0.0f;
        out[out_n].v  = 0.0f;
        out[out_n].color = col[i];
        out_n++;
    }
    return out_n;
}

/* Завершить примитив: трансформировать, клипповать, отправить.
 * Возврат: >0 GPU, 0 software, <0 ошибка syscall. */
static int axgl_end(void)
{
    axgl_hw_vertex_t batch[AXGL_MAX_IMMEDIATE * 2];
    int out_n = 0;
    float mvp[16];
    int ret = 0;

    axgl__c.prim_active = 0;

    if (axgl__c.imm_n < 3 || !axgl__c.canvas) {
        axgl__c.imm_n = 0;
        return 0;
    }

    axgl__mat_mul(mvp, axgl__c.proj, axgl__c.model);

    /* Сборка примитивов -> треугольники в clip space.
     * ВАЖНО: буфер клипа -- 4 вершины (треугольник, пересекающий
     * near-плоскость, превращается в квад из 4 вершин). */
    if (axgl__c.prim == AXGL_QUADS) {
        int q = 0;
        while (q + 3 < axgl__c.imm_n) {
            int idx[6] = { q, q + 1, q + 2, q, q + 2, q + 3 };
            for (int ti = 0; ti < 2; ti++) {
                axgl__cv_t ct[4];
                int poly_n;

                for (int k = 0; k < 3; k++) {
                    const int vi = idx[ti * 3 + k];
                    float v[4] = { axgl__c.imm[vi].x,
                                   axgl__c.imm[vi].y,
                                   axgl__c.imm[vi].z,
                                   1.0f };
                    float o[4];
                    axgl__mat_vec4(mvp, v, o);
                    ct[k].x = o[0]; ct[k].y = o[1];
                    ct[k].z = o[2]; ct[k].w = o[3];
                    ct[k].r = axgl__c.imm[vi].r;
                    ct[k].g = axgl__c.imm[vi].g;
                    ct[k].b = axgl__c.imm[vi].b;
                    ct[k].a = axgl__c.imm[vi].a;
                }

                poly_n = axgl__clip_near(ct, 3);
                if (poly_n >= 3) {
                    /* fan-тригуляция клипнутого полигона */
                    for (int fan = 1; fan + 1 < poly_n; fan++) {
                        axgl__cv_t tri[3] = { ct[0], ct[fan], ct[fan + 1] };
                        out_n = axgl__emit_tri(batch, out_n,
                                               (int)(sizeof(batch) /
                                                     sizeof(batch[0])),
                                               tri);
                    }
                }
            }
            q += 4;
        }
    } else { /* AXGL_TRIANGLES */
        int t = 0;
        while (t + 2 < axgl__c.imm_n) {
            axgl__cv_t ct[4];   /* до 4 вершин после клипа! */
            int poly_n;

            for (int k = 0; k < 3; k++) {
                const int vi = t + k;
                float v[4] = { axgl__c.imm[vi].x,
                               axgl__c.imm[vi].y,
                               axgl__c.imm[vi].z,
                               1.0f };
                float o[4];
                axgl__mat_vec4(mvp, v, o);
                ct[k].x = o[0]; ct[k].y = o[1];
                ct[k].z = o[2]; ct[k].w = o[3];
                ct[k].r = axgl__c.imm[vi].r;
                ct[k].g = axgl__c.imm[vi].g;
                ct[k].b = axgl__c.imm[vi].b;
                ct[k].a = axgl__c.imm[vi].a;
            }

            poly_n = axgl__clip_near(ct, 3);
            if (poly_n >= 3) {
                for (int fan = 1; fan + 1 < poly_n; fan++) {
                    axgl__cv_t tri[3] = { ct[0], ct[fan], ct[fan + 1] };
                    out_n = axgl__emit_tri(batch, out_n,
                                           (int)(sizeof(batch) /
                                                 sizeof(batch[0])),
                                           tri);
                }
            }
            t += 3;
        }
    }

    axgl__c.imm_n = 0;

    if (out_n >= 3)
        ret = ax_gpu_triangles(axgl__c.canvas, batch, (uint32_t)out_n);

    return ret;
}

/* Имя активного 3D-бэкенда ("NVIDIA"/"AMD"/"Intel"/"Software"). */
static const char *axgl_backend(void)
{
    if (!axgl__c.canvas) return "Software";
    return ax_gpu_backend_name(axgl__c.canvas);
}

/* Выбор пути рендеринга (см. lib/axipc.h):
 *   AXGL_MODE_AUTO     -- GPU-сабмит (если есть) + видимый SW-canvas;
 *   AXGL_MODE_FORCE_SW -- только программный растеризатор;
 *   AXGL_MODE_GPU_ONLY -- только GPU-сабмит (окно не обновляется).
 * Возвращает установленный режим или -1. */
static int axgl_set_render_mode(int mode)
{
    if (!ax_connected())
        return -1;

    ax_msg_t m;
    memset((u8int *)&m, 0, sizeof(m));
    m.type = MSG_REQ_GL_SETMODE;
    m.from = (uint32_t)g_ax_cli_port;
    m.a    = (uint32_t)mode;

    if (ax_port_send(g_ax_srv_port, &m) != 0)
        return -1;

    ax_msg_t r;
    if (ax_wait_reply(MSG_REPLY_GL, 0, &r) != 0)
        return -1;

    return (int32_t)r.a;
}

#ifdef __cplusplus
}
#endif

#endif /* AXGL_USER_H */
