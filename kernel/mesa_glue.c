#include <stdint.h>
#include <stddef.h>

#include <GL/gl.h>

#include "../lib/common.h"
#include "../libs/mesa/port/src/port_gl_api.h"
#include "mesa_glue.h"

extern void serial_puts(const char *s);

extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

static int ax_gl_ctors_done;

void ax_mesa_module_init(void)
{
    void (**fn)(void);

    if (ax_gl_ctors_done)
        return;

    ax_gl_ctors_done = 1;

    for (fn = __init_array_start; fn < __init_array_end; fn++)
        (*fn)();
}

#define AX_GL_TEST_W 64u
#define AX_GL_TEST_H 64u

static uint32_t ax_gl_test_canvas[AX_GL_TEST_W * AX_GL_TEST_H] __attribute__((aligned(64)));

static int ax_gl_expect(const char *what, uint32_t got, uint32_t want)
{
    if (got == want)
        return 0;

    serial_puts("[SELFTEST] mesa-");
    serial_puts(what);
    serial_puts(": FAIL (got 0x");
    {
        char buf[9];
        int i;

        for (i = 0; i < 8; i++) {
            unsigned nib = (unsigned)((got >> (28 - 4 * i)) & 0xFu);

            buf[i] = (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
        }

        buf[8] = 0;
        serial_puts(buf);
    }
    serial_puts(", want 0x");
    {
        char buf[9];
        int i;

        for (i = 0; i < 8; i++) {
            unsigned nib = (unsigned)((want >> (28 - 4 * i)) & 0xFu);

            buf[i] = (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
        }

        buf[8] = 0;
        serial_puts(buf);
    }
    serial_puts(")\n");

    return 1;
}

int ax_gl_selftest(void)
{
    void *ctx = NULL;
    const GLubyte *renderer;
    int failures = 0;
    unsigned i;

    ax_mesa_module_init();

    for (i = 0; i < AX_GL_TEST_W * AX_GL_TEST_H; i++)
        ax_gl_test_canvas[i] = 0xA0A0A0A0u;

    if (ax_gl_context_create_raw(ax_gl_test_canvas, AX_GL_TEST_W, AX_GL_TEST_H, &ctx) != 0 || !ctx) {
        serial_puts("[SELFTEST] mesa-context: FAIL (create)\n");
        return 1;
    }

    renderer = glGetString(GL_RENDERER);

    if (!renderer || !renderer[0]) {
        serial_puts("[SELFTEST] mesa-context: FAIL (no renderer)\n");
        ax_gl_context_destroy_raw(ctx);
        return 1;
    }

    serial_puts("[SELFTEST] mesa-renderer: ");
    serial_puts((const char *)renderer);
    serial_puts(" ver=");
    serial_puts((const char *)glGetString(GL_VERSION));
    serial_puts("\n");

    glViewport(0, 0, (GLsizei)AX_GL_TEST_W, (GLsizei)AX_GL_TEST_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBegin(GL_TRIANGLES);
    glColor4f(0.0f, 1.0f, 0.0f, 1.0f);
    glVertex2f(-0.9f, -0.9f);
    glVertex2f(0.9f, -0.9f);
    glVertex2f(0.0f, 0.9f);
    glEnd();

    glFinish();
    glFlush();

    if (ax_gl_context_flush(ctx) != 0) {
        serial_puts("[SELFTEST] mesa-flush: FAIL\n");
        ax_gl_context_destroy_raw(ctx);
        return 1;
    }

    failures += ax_gl_expect("clear",
                             ax_gl_test_canvas[2 * AX_GL_TEST_W + 2],
                             0xFF000000u);
    failures += ax_gl_expect("triangle",
                             ax_gl_test_canvas[(AX_GL_TEST_H / 2) * AX_GL_TEST_W + (AX_GL_TEST_W / 2)],
                             0xFF00FF00u);

    if (glGetError() != GL_NO_ERROR) {
        serial_puts("[SELFTEST] mesa-glerror: FAIL\n");
        failures++;
    }

    ax_gl_context_unbind(ctx);

    if (ax_gl_context_destroy_raw(ctx) != 0) {
        serial_puts("[SELFTEST] mesa-destroy: FAIL\n");
        failures++;
    }

    return failures;
}
