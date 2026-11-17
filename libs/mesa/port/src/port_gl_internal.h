#ifndef AX_PORT_GL_INTERNAL_H
#define AX_PORT_GL_INTERNAL_H

#include <stdint.h>

#include "pipe/p_state.h"
#include "frontend/api.h"

struct sw_winsys;

struct ax_gl_drawable {
    struct pipe_frontend_drawable base;
    struct pipe_frontend_screen *fscreen;
    uint32_t *canvas;
    unsigned width;
    unsigned height;
    struct pipe_resource *textures[ST_ATTACHMENT_COUNT];
};

struct sw_winsys *ax_sw_winsys_create(void);

struct pipe_frontend_screen *ax_gl_fscreen_create(void);
void ax_gl_fscreen_destroy(struct pipe_frontend_screen *fscreen);

struct ax_gl_drawable *ax_gl_drawable_create(struct pipe_frontend_screen *fscreen,
                                             const struct st_visual *visual,
                                             void *canvas,
                                             unsigned width, unsigned height);
void ax_gl_drawable_destroy(struct ax_gl_drawable *d);
void ax_gl_drawable_set_canvas(struct ax_gl_drawable *d, void *canvas,
                               unsigned width, unsigned height);

#endif
