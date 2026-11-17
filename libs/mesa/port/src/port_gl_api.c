#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "util/u_inlines.h"
#include "pipe/p_state.h"
#include "frontend/api.h"
#include "st_context.h"

#include "port_gl_internal.h"

typedef struct {
    struct pipe_frontend_screen *fscreen;
    struct ax_gl_drawable *drawable;
    struct st_context *st;
    struct st_visual visual;
    int ready;
} ax_gl_ctx;

static ax_gl_ctx ax_gl_slots[2];

static ax_gl_ctx *ax_gl_pick_slot(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (!ax_gl_slots[i].ready)
            return &ax_gl_slots[i];
    }

    return NULL;
}

static void ax_gl_reset_slot(ax_gl_ctx *c)
{
    if (!c)
        return;

    c->fscreen = NULL;
    c->drawable = NULL;
    c->st = NULL;
    c->ready = 0;
    memset(&c->visual, 0, sizeof(c->visual));
}

static int ax_gl_make_current(ax_gl_ctx *c, int current)
{
    if (!c || !c->st)
        return -1;

    if (!st_api_make_current(c->st,
                             current ? &c->drawable->base : NULL,
                             current ? &c->drawable->base : NULL))
        return -1;

    return 0;
}

int ax_gl_context_create_raw(void *canvas, unsigned width, unsigned height,
                             void **ctx_out)
{
    struct st_context_attribs attribs;
    enum st_context_error error = ST_CONTEXT_SUCCESS;
    struct pipe_frontend_screen *fscreen;
    struct ax_gl_drawable *drawable;
    ax_gl_ctx *c;

    if (!ctx_out || !width || !height)
        return -1;

    *ctx_out = NULL;

    c = ax_gl_pick_slot();
    if (!c)
        return -1;

    fscreen = ax_gl_fscreen_create();
    if (!fscreen)
        return -1;

    memset(&c->visual, 0, sizeof(c->visual));
    c->visual.buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK;
    c->visual.color_format = PIPE_FORMAT_B8G8R8A8_UNORM;
    c->visual.depth_stencil_format = PIPE_FORMAT_Z24_UNORM_S8_UINT;
    c->visual.accum_format = PIPE_FORMAT_NONE;
    c->visual.samples = 0;

    drawable = ax_gl_drawable_create(fscreen, &c->visual, canvas, width, height);
    if (!drawable) {
        ax_gl_fscreen_destroy(fscreen);
        return -1;
    }

    memset(&attribs, 0, sizeof(attribs));
    attribs.profile = API_OPENGL_COMPAT;
    attribs.major = 1;
    attribs.minor = 0;
    attribs.flags = 0;
    attribs.context_flags = 0;
    attribs.visual = c->visual;

    c->fscreen = fscreen;
    c->drawable = drawable;

    c->st = st_api_create_context(fscreen, &attribs, &error, NULL);

    if (!c->st) {
        ax_gl_drawable_destroy(drawable);
        ax_gl_fscreen_destroy(fscreen);
        ax_gl_reset_slot(c);
        return -1;
    }

    if (ax_gl_make_current(c, 1) != 0) {
        st_destroy_context(c->st);
        ax_gl_drawable_destroy(drawable);
        ax_gl_fscreen_destroy(fscreen);
        ax_gl_reset_slot(c);
        return -1;
    }

    c->ready = 1;
    *ctx_out = c;

    return 0;
}

int ax_gl_context_destroy_raw(void *ctx)
{
    ax_gl_ctx *c = (ax_gl_ctx *)ctx;

    if (!c || !c->ready)
        return -1;

    st_api_make_current(c->st, NULL, NULL);

    st_destroy_context(c->st);
    c->st = NULL;

    ax_gl_drawable_destroy(c->drawable);
    c->drawable = NULL;

    ax_gl_fscreen_destroy(c->fscreen);
    c->fscreen = NULL;

    ax_gl_reset_slot(c);

    return 0;
}

int ax_gl_context_bind(void *ctx)
{
    ax_gl_ctx *c = (ax_gl_ctx *)ctx;

    if (!c || !c->ready)
        return -1;

    return ax_gl_make_current(c, 1);
}

int ax_gl_context_unbind(void *ctx)
{
    ax_gl_ctx *c = (ax_gl_ctx *)ctx;

    if (!c || !c->ready)
        return -1;

    st_api_make_current(c->st, NULL, NULL);

    return 0;
}

int ax_gl_context_resize(void *ctx, void *canvas, unsigned width, unsigned height)
{
    ax_gl_ctx *c = (ax_gl_ctx *)ctx;

    if (!c || !c->ready || !canvas || !width || !height)
        return -1;

    ax_gl_drawable_set_canvas(c->drawable, canvas, width, height);

    return ax_gl_make_current(c, 1);
}

int ax_gl_context_flush(void *ctx)
{
    ax_gl_ctx *c = (ax_gl_ctx *)ctx;

    if (!c || !c->ready)
        return -1;

    st_context_flush(c->st, ST_FLUSH_FRONT, NULL, NULL, NULL);

    return 0;
}
