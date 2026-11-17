#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_atomic.h"
#include "util/u_memory.h"
#include "pipe/p_state.h"
#include "pipe/p_screen.h"
#include "frontend/api.h"
#include "frontend/sw_winsys.h"
#include "sp_public.h"
#include "sp_screen.h"
#include "st_context.h"

#include "port_gl_internal.h"

static unsigned ax_gl_drawable_id_seq = 1;

static void ax_drawable_get_format(struct ax_gl_drawable *d,
                                   enum st_attachment_type statt,
                                   enum pipe_format *format,
                                   unsigned *bind)
{
    switch (statt) {
    case ST_ATTACHMENT_FRONT_LEFT:
    case ST_ATTACHMENT_BACK_LEFT:
    case ST_ATTACHMENT_FRONT_RIGHT:
    case ST_ATTACHMENT_BACK_RIGHT:
        *format = util_format_linear(d->base.visual->color_format);
        *bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
        break;
    case ST_ATTACHMENT_DEPTH_STENCIL:
        *format = d->base.visual->depth_stencil_format;
        *bind = PIPE_BIND_DEPTH_STENCIL;
        break;
    default:
        *format = PIPE_FORMAT_NONE;
        *bind = 0;
        break;
    }
}

static bool ax_drawable_validate(struct st_context *st,
                                 struct pipe_frontend_drawable *pd,
                                 const enum st_attachment_type *statts,
                                 unsigned count,
                                 struct pipe_resource **out,
                                 struct pipe_resource **resolve)
{
    struct ax_gl_drawable *d = (struct ax_gl_drawable *)pd;
    struct pipe_screen *screen = d->base.fscreen->screen;
    struct pipe_resource templ;
    unsigned i;

    (void)st;
    (void)resolve;

    memset(&templ, 0, sizeof(templ));
    templ.target = PIPE_TEXTURE_2D;
    templ.width0 = d->width;
    templ.height0 = d->height;
    templ.depth0 = 1;
    templ.array_size = 1;
    templ.last_level = 0;
    templ.nr_samples = 0;
    templ.nr_storage_samples = 0;

    for (i = 0; i < count; i++) {
        enum pipe_format format;
        unsigned bind;
        struct pipe_resource **slot;

        if (statts[i] < 0 || statts[i] >= ST_ATTACHMENT_COUNT)
            continue;

        ax_drawable_get_format(d, statts[i], &format, &bind);

        if (format == PIPE_FORMAT_NONE)
            continue;

        if (statts[i] != ST_ATTACHMENT_DEPTH_STENCIL)
            bind |= PIPE_BIND_DISPLAY_TARGET;

        slot = &d->textures[statts[i]];

        if (*slot && (*slot)->width0 == d->width && (*slot)->height0 == d->height)
            continue;

        pipe_resource_reference(slot, NULL);

        templ.format = format;
        templ.bind = bind;

        if (statts[i] == ST_ATTACHMENT_FRONT_LEFT && screen->resource_create_front)
            *slot = screen->resource_create_front(screen, &templ, (const void *)d);
        else
            *slot = screen->resource_create(screen, &templ);

        if (!*slot)
            return false;
    }

    if (out) {
        for (i = 0; i < count; i++) {
            if (statts[i] < 0 || statts[i] >= ST_ATTACHMENT_COUNT)
                continue;

            pipe_resource_reference(&out[i], d->textures[statts[i]]);
        }
    }

    return true;
}

static bool ax_drawable_flush_front(struct st_context *st,
                                    struct pipe_frontend_drawable *pd,
                                    enum st_attachment_type statt)
{
    struct ax_gl_drawable *d = (struct ax_gl_drawable *)pd;
    struct pipe_screen *screen = d->base.fscreen->screen;
    struct pipe_resource *ptex = d->textures[statt];

    if (!ptex)
        return false;

    screen->flush_frontbuffer(screen, st ? st->pipe : NULL, ptex, 0, 0,
                              (void *)d, 0, NULL);

    p_atomic_inc(&d->base.stamp);

    return true;
}

static bool ax_drawable_flush_swapbuffers(struct st_context *st,
                                          struct pipe_frontend_drawable *pd)
{
    (void)st;
    (void)pd;
    return false;
}

static int ax_screen_get_param(struct pipe_frontend_screen *fscreen,
                               enum st_manager_param param)
{
    (void)fscreen;

    switch (param) {
    case ST_MANAGER_BROKEN_INVALIDATE:
        return 0;
    }

    return 0;
}

static void ax_screen_set_background_context(struct st_context *st,
                                             struct util_queue_monitoring *queue_info)
{
    (void)st;
    (void)queue_info;
}

struct pipe_frontend_screen *ax_gl_fscreen_create(void)
{
    struct pipe_frontend_screen *fscreen;
    struct sw_winsys *winsys;
    struct pipe_screen *screen;

    fscreen = CALLOC_STRUCT(pipe_frontend_screen);
    if (!fscreen)
        return NULL;

    winsys = ax_sw_winsys_create();
    if (!winsys) {
        FREE(fscreen);
        return NULL;
    }

    screen = softpipe_create_screen(winsys);
    if (!screen) {
        winsys->destroy(winsys);
        FREE(fscreen);
        return NULL;
    }

    fscreen->screen = screen;
    fscreen->get_egl_image = NULL;
    fscreen->validate_egl_image = NULL;
    fscreen->get_param = ax_screen_get_param;
    fscreen->set_background_context = ax_screen_set_background_context;
    fscreen->st_screen = NULL;

    return fscreen;
}

void ax_gl_fscreen_destroy(struct pipe_frontend_screen *fscreen)
{
    struct sw_winsys *winsys;
    struct pipe_screen *screen;

    if (!fscreen)
        return;

    screen = fscreen->screen;

    if (screen) {
        winsys = softpipe_screen(screen)->winsys;
        screen->destroy(screen);
        if (winsys)
            winsys->destroy(winsys);
    }

    FREE(fscreen);
}

struct ax_gl_drawable *ax_gl_drawable_create(struct pipe_frontend_screen *fscreen,
                                             const struct st_visual *visual,
                                             void *canvas,
                                             unsigned width, unsigned height)
{
    struct ax_gl_drawable *d;

    if (!fscreen || !visual || width == 0 || height == 0)
        return NULL;

    d = CALLOC_STRUCT(ax_gl_drawable);
    if (!d)
        return NULL;

    d->fscreen = fscreen;
    d->canvas = (uint32_t *)canvas;
    d->width = width;
    d->height = height;

    d->base.stamp = 1;
    d->base.ID = p_atomic_inc_return(&ax_gl_drawable_id_seq);
    d->base.fscreen = fscreen;
    d->base.visual = visual;
    d->base.flush_front = ax_drawable_flush_front;
    d->base.validate = ax_drawable_validate;
    d->base.flush_swapbuffers = ax_drawable_flush_swapbuffers;

    return d;
}

void ax_gl_drawable_destroy(struct ax_gl_drawable *d)
{
    unsigned i;

    if (!d)
        return;

    st_api_destroy_drawable(&d->base);

    for (i = 0; i < ST_ATTACHMENT_COUNT; i++)
        pipe_resource_reference(&d->textures[i], NULL);

    FREE(d);
}

void ax_gl_drawable_set_canvas(struct ax_gl_drawable *d, void *canvas,
                               unsigned width, unsigned height)
{
    unsigned i;

    if (!d || !canvas || !width || !height)
        return;

    if (d->width == width && d->height == height && d->canvas == canvas)
        return;

    for (i = 0; i < ST_ATTACHMENT_COUNT; i++)
        pipe_resource_reference(&d->textures[i], NULL);

    d->canvas = (uint32_t *)canvas;
    d->width = width;
    d->height = height;

    p_atomic_inc(&d->base.stamp);
}
