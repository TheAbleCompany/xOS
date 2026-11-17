#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "util/format/u_format.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "frontend/sw_winsys.h"
#include "pipe/p_state.h"

#include "port_gl_internal.h"

struct ax_sw_dt {
    enum pipe_format format;
    unsigned width;
    unsigned height;
    unsigned stride;
    unsigned size;
    unsigned map_flags;
    void *data;
    void *mapped;
    struct ax_gl_drawable *drawable;
    int unbacked;
};

struct ax_sw_ws {
    struct sw_winsys base;
};

static inline struct ax_sw_ws *ax_sw_ws(struct sw_winsys *ws)
{
    return (struct ax_sw_ws *)ws;
}

static inline struct ax_sw_dt *ax_sw_dt(struct sw_displaytarget *dt)
{
    return (struct ax_sw_dt *)dt;
}

static void ax_sw_destroy(struct sw_winsys *ws)
{
    FREE(ws);
}

static int ax_sw_get_fd(struct sw_winsys *ws)
{
    (void)ws;
    return -1;
}

static bool ax_sw_is_displaytarget_format_supported(struct sw_winsys *ws,
                                                    unsigned tex_usage,
                                                    enum pipe_format format)
{
    const struct util_format_description *desc;

    (void)ws;
    (void)tex_usage;

    if (format == PIPE_FORMAT_NONE)
        return false;

    desc = util_format_description(format);

    if (!desc)
        return false;

    if (desc->colorspace != UTIL_FORMAT_COLORSPACE_RGB)
        return false;

    return util_format_get_blocksize(format) == 4;
}

static struct sw_displaytarget *
ax_sw_displaytarget_create(struct sw_winsys *ws,
                           unsigned tex_usage,
                           enum pipe_format format,
                           unsigned width, unsigned height,
                           unsigned alignment,
                           const void *front_private,
                           unsigned *stride)
{
    struct ax_sw_dt *dt;
    unsigned format_stride;
    unsigned nblocksy;
    unsigned size;

    (void)ws;
    (void)tex_usage;

    if (alignment < 16)
        alignment = 16;

    dt = CALLOC_STRUCT(ax_sw_dt);
    if (!dt)
        return NULL;

    format_stride = util_format_get_stride(format, width);
    nblocksy = util_format_get_nblocksy(format, height);

    dt->stride = align(format_stride, alignment);
    dt->size = dt->stride * nblocksy;
    dt->format = format;
    dt->width = width;
    dt->height = height;
    dt->drawable = (struct ax_gl_drawable *)(uintptr_t)front_private;

    dt->data = align_malloc(dt->size, alignment);
    if (!dt->data) {
        FREE(dt);
        return NULL;
    }

    memset(dt->data, 0, dt->size);

    *stride = dt->stride;

    return (struct sw_displaytarget *)dt;
}

static struct sw_displaytarget *
ax_sw_displaytarget_create_mapped(struct sw_winsys *ws,
                                  unsigned tex_usage,
                                  enum pipe_format format,
                                  unsigned width, unsigned height,
                                  unsigned stride,
                                  void *data)
{
    struct ax_sw_dt *dt;

    (void)ws;
    (void)tex_usage;

    if (!data)
        return NULL;

    dt = CALLOC_STRUCT(ax_sw_dt);
    if (!dt)
        return NULL;

    dt->format = format;
    dt->width = width;
    dt->height = height;
    dt->stride = stride;
    dt->size = stride * util_format_get_nblocksy(format, height);
    dt->data = data;
    dt->mapped = data;
    dt->unbacked = 1;

    return (struct sw_displaytarget *)dt;
}

static struct sw_displaytarget *
ax_sw_displaytarget_from_handle(struct sw_winsys *ws,
                                const struct pipe_resource *templat,
                                struct winsys_handle *whandle,
                                unsigned *stride)
{
    (void)ws;
    (void)templat;
    (void)whandle;
    (void)stride;
    return NULL;
}

static bool ax_sw_displaytarget_get_handle(struct sw_winsys *ws,
                                           struct sw_displaytarget *dt,
                                           struct winsys_handle *whandle)
{
    (void)ws;
    (void)dt;
    (void)whandle;
    return false;
}

static void *ax_sw_displaytarget_map(struct sw_winsys *ws,
                                     struct sw_displaytarget *dt_st,
                                     unsigned flags)
{
    struct ax_sw_dt *dt = ax_sw_dt(dt_st);

    (void)ws;

    dt->map_flags = flags;
    dt->mapped = dt->data;

    return dt->mapped;
}

static void ax_sw_displaytarget_unmap(struct sw_winsys *ws,
                                      struct sw_displaytarget *dt_st)
{
    struct ax_sw_dt *dt = ax_sw_dt(dt_st);

    (void)ws;

    dt->map_flags = 0;
    dt->mapped = NULL;
}

static void ax_sw_dt_present(struct ax_sw_dt *dt, unsigned nboxes,
                             struct pipe_box *box)
{
    struct ax_gl_drawable *d = dt->drawable;
    unsigned bpp = util_format_get_blocksize(dt->format);
    unsigned rows;
    unsigned out_w;

    if (!d || !d->canvas)
        return;

    rows = dt->height < d->height ? dt->height : d->height;

    if (!nboxes || !box) {
        unsigned x, y, w;

        w = dt->width < d->width ? dt->width : d->width;

        for (y = 0; y < rows; y++) {
            uint32_t *src = (uint32_t *)((uint8_t *)dt->data + (size_t)y * dt->stride);
            uint32_t *dst = d->canvas + (size_t)y * d->width;

            for (x = 0; x < w; x++)
                dst[x] = src[x] | 0xFF000000u;
        }

        (void)bpp;
        (void)out_w;
        return;
    }

    for (unsigned i = 0; i < nboxes; i++) {
        int bx = box[i].x;
        int by = box[i].y;
        int bw = box[i].width;
        int bh = box[i].height;

        if (bx < 0) {
            bw += bx;
            bx = 0;
        }

        if (by < 0) {
            bh += by;
            by = 0;
        }

        if (bw <= 0 || bh <= 0)
            continue;

        if ((unsigned)bx >= d->width || (unsigned)by >= d->height)
            continue;

        if ((unsigned)(bx + bw) > d->width)
            bw = (int)d->width - bx;

        if ((unsigned)(by + bh) > d->height)
            bh = (int)d->height - by;

        if ((unsigned)(by + bh) > rows)
            bh = (int)rows - by;

        if (bw <= 0 || bh <= 0)
            continue;

        for (int y = 0; y < bh; y++) {
            uint32_t *src = (uint32_t *)((uint8_t *)dt->data +
                                         (size_t)(by + y) * dt->stride +
                                         (size_t)bx * bpp);
            uint32_t *dst = d->canvas + (size_t)(by + y) * d->width + bx;

            for (int x = 0; x < bw; x++)
                dst[x] = src[x] | 0xFF000000u;
        }
    }

    (void)out_w;
}

static void ax_sw_displaytarget_display(struct sw_winsys *ws,
                                        struct sw_displaytarget *dt_st,
                                        void *context_private,
                                        unsigned nboxes,
                                        struct pipe_box *box)
{
    struct ax_sw_dt *dt = ax_sw_dt(dt_st);

    (void)ws;

    if (context_private)
        dt->drawable = (struct ax_gl_drawable *)context_private;

    ax_sw_dt_present(dt, nboxes, box);
}

static void ax_sw_displaytarget_destroy(struct sw_winsys *ws,
                                        struct sw_displaytarget *dt_st)
{
    struct ax_sw_dt *dt = ax_sw_dt(dt_st);

    (void)ws;

    if (!dt->unbacked)
        align_free(dt->data);

    FREE(dt);
}

struct sw_winsys *ax_sw_winsys_create(void)
{
    struct ax_sw_ws *ws = CALLOC_STRUCT(ax_sw_ws);

    if (!ws)
        return NULL;

    ws->base.destroy = ax_sw_destroy;
    ws->base.get_fd = ax_sw_get_fd;
    ws->base.is_displaytarget_format_supported = ax_sw_is_displaytarget_format_supported;
    ws->base.displaytarget_create = ax_sw_displaytarget_create;
    ws->base.displaytarget_create_mapped = ax_sw_displaytarget_create_mapped;
    ws->base.displaytarget_from_handle = ax_sw_displaytarget_from_handle;
    ws->base.displaytarget_get_handle = ax_sw_displaytarget_get_handle;
    ws->base.displaytarget_map = ax_sw_displaytarget_map;
    ws->base.displaytarget_unmap = ax_sw_displaytarget_unmap;
    ws->base.displaytarget_display = ax_sw_displaytarget_display;
    ws->base.displaytarget_destroy = ax_sw_displaytarget_destroy;

    return &ws->base;
}
