#ifndef AX_PORT_GL_API_H
#define AX_PORT_GL_API_H

int ax_gl_context_create_raw(void *canvas, unsigned width, unsigned height,
                             void **ctx_out);
int ax_gl_context_destroy_raw(void *ctx);
int ax_gl_context_bind(void *ctx);
int ax_gl_context_unbind(void *ctx);
int ax_gl_context_resize(void *ctx, void *canvas, unsigned width, unsigned height);
int ax_gl_context_flush(void *ctx);

#endif
