#include <stdint.h>

#include "../kernel/isr.h"
#include "../lib/common.h"
#include "../drivers/framebuffer.h"
#include "../flanterm/flanterm.h"

extern struct flanterm_context *ft_ctx;
volatile int mouse_dx = 0;
volatile int mouse_dy = 0;
volatile int mouse_buttons = 0;

int m_cursor_x = 320;
int m_cursor_y = 240;

static int mouse_present = 0;


/*
 * ============================================
 * 8042 / PS2
 * ============================================
 */

#define PS2_STATUS       0x64
#define PS2_COMMAND      0x64
#define PS2_DATA         0x60

#define PS2_STATUS_OBF   0x01
#define PS2_STATUS_IBF   0x02

#define PS2_CMD_ENABLE_AUX       0xA8
#define PS2_CMD_READ_CONFIG      0x20
#define PS2_CMD_WRITE_CONFIG     0x60
#define PS2_CMD_WRITE_AUX        0xD4

#define MOUSE_CMD_RESET          0xFF
#define MOUSE_CMD_DEFAULTS       0xF6
#define MOUSE_CMD_ENABLE_REPORT  0xF4

#define MOUSE_ACK                0xFA
#define MOUSE_SELF_TEST_OK       0xAA


/*
 * ============================================
 * Debug
 * ============================================
 */

static void mouse_debug(const char *s)
{
    /*
     * r12-bootfix: раньше после цикла указатель s уже стоял на
     * '\0', и flanterm_write(ft_ctx, s, strlen(s)) писал ноль
     * байт (сообщение видел только COM1).  Плюс не было проверки
     * ft_ctx на NULL.
     */
    const char *p = s;

    while (*p)
        outb(0x3F8, *p++);

    if (ft_ctx)
        flanterm_write(ft_ctx, s, (size_t)(p - s));
}

static void mouse_debug_hex(uint8_t x)
{
    const char hex[] = "0123456789ABCDEF";

    outb(0x3F8, hex[(x >> 4) & 0xF]);
    outb(0x3F8, hex[x & 0xF]);
}


/*
 * ============================================
 * Wait helpers
 * ============================================
 */

static int mouse_wait_read(unsigned int timeout)
{
    while (timeout--) {

        if (inb(PS2_STATUS) & PS2_STATUS_OBF)
            return 1;

        __asm__ volatile("pause");
    }

    return 0;
}


static int mouse_wait_write(unsigned int timeout)
{
    while (timeout--) {

        if (!(inb(PS2_STATUS) & PS2_STATUS_IBF))
            return 1;

        __asm__ volatile("pause");
    }

    return 0;
}


/*
 * ============================================
 * Read controller data
 * ============================================
 */

static int mouse_read_wait(
    unsigned int timeout,
    uint8_t *out
)
{
    if (!mouse_wait_read(timeout))
        return 0;

    *out = inb(PS2_DATA);

    return 1;
}


/*
 * ============================================
 * Send command to mouse
 * ============================================
 */

static int mouse_write_wait(
    unsigned int timeout,
    uint8_t data
)
{
    /*
     * Wait until 8042 input buffer is free.
     */

    if (!mouse_wait_write(timeout))
        return 0;


    /*
     * Tell 8042 that the next byte
     * goes to the auxiliary device.
     */

    outb(PS2_COMMAND, PS2_CMD_WRITE_AUX);


    if (!mouse_wait_write(timeout))
        return 0;


    outb(PS2_DATA, data);

    return 1;
}


/*
 * ============================================
 * Mouse packet processing
 * ============================================
 */

void mouse_handle_packet(
    int dx,
    int dy,
    unsigned int buttons
)
{
    mouse_dx += dx;

    /*
     * PS/2 Y axis is inverted.
     */

    mouse_dy -= dy;

    mouse_buttons =
        (int)(buttons & 0x07u);
}


/*
 * ============================================
 * IRQ12
 * ============================================
 */

void mouse_handler(registers_t *r)
{
    //flanterm_write(ft_ctx, "MOV\r\n", 5);
    (void)r;

    static uint8_t packet[3];
    static uint8_t cycle = 0;

    uint8_t status;


    /*
     * Check output buffer.
     */

    status = inb(PS2_STATUS);

    if (!(status & PS2_STATUS_OBF)) {

        /*
         * Still acknowledge IRQ12.
         */

        outb(0xA0, 0x20);
        outb(0x20, 0x20);

        return;
    }


    /*
     * Bit 5:
     *
     * 1 = auxiliary/mouse data
     * 0 = keyboard data
     */

    if (!(status & 0x20)) {

        /*
         * This is not mouse data.
         *
         * Do not consume it here.
         */

        outb(0xA0, 0x20);
        outb(0x20, 0x20);

        return;
    }


    uint8_t data = inb(PS2_DATA);


    /*
     * ========================================
     * Packet synchronization
     * ========================================
     *
     * First byte always has bit 3 set.
     */

    if (cycle == 0) {

        if (!(data & 0x08)) {

            /*
             * Lost synchronization.
             */

            outb(0xA0, 0x20);
            outb(0x20, 0x20);

            return;
        }

        packet[0] = data;
        cycle = 1;

    } else if (cycle == 1) {

        packet[1] = data;
        cycle = 2;

    } else {

        packet[2] = data;
        cycle = 0;


        /*
         * ====================================
         * Decode packet
         * ====================================
         */

        int dx = (int8_t)packet[1];
        int dy = (int8_t)packet[2];

        unsigned int buttons =
            packet[0] & 0x07;


        /*
         * Overflow bits:
         *
         * bit 6 = X overflow
         * bit 7 = Y overflow
         *
         * Ignore corrupted packet.
         */

        if (packet[0] & 0xC0) {

            outb(0xA0, 0x20);
            outb(0x20, 0x20);

            return;
        }


        mouse_handle_packet(
            dx,
            dy,
            buttons
        );
    }


    /*
     * ========================================
     * EOI
     * ========================================
     *
     * IRQ12:
     *
     * slave PIC first
     * master PIC second
     */

    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}


/*
 * ============================================
 * Install / initialize mouse
 * ============================================
 */

void mouse_install(void)
{
    const unsigned int timeout = 100000;

    uint8_t config;
    uint8_t ack;

    mouse_debug("[MOUSE] init\n");


    /*
     * ========================================
     * 1. Очистить pending output
     * ========================================
     */

    for (int i = 0; i < 32; i++) {

        if (!(inb(0x64) & 1))
            break;

        (void)inb(0x60);
    }


    /*
     * ========================================
     * 2. Enable second PS/2 port
     * ========================================
     */

    if (!mouse_wait_write(timeout))
        goto fail;

    outb(0x64, 0xA8);


    /*
     * ========================================
     * 3. Read controller config
     * ========================================
     */

    if (!mouse_wait_write(timeout))
        goto fail;

    outb(0x64, 0x20);

    if (!mouse_read_wait(timeout, &config))
        goto fail;


    /*
     * ========================================
     * 4. Enable IRQ12
     *
     * bit 1 = IRQ12 enable
     *
     * bit 5 = mouse clock disable
     *
     * We need:
     *
     * bit 1 = 1
     * bit 5 = 0
     * ========================================
     */

    config |=  (1 << 1);
    config &= ~(1 << 5);


    /*
     * ========================================
     * 5. Write config back
     * ========================================
     */

    if (!mouse_wait_write(timeout))
        goto fail;

    outb(0x64, 0x60);

    if (!mouse_wait_write(timeout))
        goto fail;

    outb(0x60, config);


    /*
     * ========================================
     * 6. Register IRQ12 handler
     * ========================================
     */

    register_interrupt_handler(
        IRQ12,
        mouse_handler
    );


    /*
     * ========================================
     * 7. Send defaults to mouse
     * ========================================
     */

    if (!mouse_write_wait(
            timeout,
            0xF6))
        goto fail;

    if (!mouse_read_wait(
            timeout,
            &ack))
        goto fail;

    if (ack != 0xFA)
        goto fail;


    /*
     * ========================================
     * 8. Enable reporting
     * ========================================
     */

    if (!mouse_write_wait(
            timeout,
            0xF4))
        goto fail;

    if (!mouse_read_wait(
            timeout,
            &ack))
        goto fail;

    if (ack != 0xFA)
        goto fail;


    mouse_present = 1;

    mouse_debug(
        "[MOUSE] initialized\n"
    );

    return;


fail:

    mouse_present = 0;

    mouse_debug(
        "[MOUSE] initialization failed\n"
    );
}
/*
 * ============================================
 * GUI handling
 * ============================================
 */

extern void xhci_deferred(void);

void handle_mouse(void)
{
    xhci_deferred();   /* USB hot-plug / восстановление endpoint'ов */

    if (!mouse_present &&
        mouse_dx == 0 &&
        mouse_dy == 0)
        return;


    if (mouse_dx != 0 ||
        mouse_dy != 0) {

        int adx = __atomic_exchange_n(&mouse_dx, 0, __ATOMIC_SEQ_CST);
        int ady = __atomic_exchange_n(&mouse_dy, 0, __ATOMIC_SEQ_CST);
        m_cursor_x += adx;
        m_cursor_y += ady;


        /*
         * Clamp X/Y к размеру дисплея.
         *
         * Раньше здесь вызывались gfx_width()/gfx_height() из
         * lib/gfxlib.c (компонента GUI).  После выноса GUI в
         * отдельный процесс wserver ядро больше не линкует
         * gfxlib -- драйвер берёт размеры напрямую у Limine
         * framebuffer (drivers/framebuffer.h), как и положено
         * драйверу.
         */

        if (m_cursor_x < 0)
            m_cursor_x = 0;

        if (m_cursor_x >= (int)framebuffer_width)
            m_cursor_x =
                (int)framebuffer_width - 1;


        /*
         * Clamp Y.
         */

        if (m_cursor_y < 0)
            m_cursor_y = 0;

        if (m_cursor_y >= (int)framebuffer_height)
            m_cursor_y =
                (int)framebuffer_height - 1;


        /*
         * Atomically-ish clear the
         * accumulated movement.
         *
         * Interrupts should ideally be
         * disabled around this section
         * if this is called from normal
         * kernel context.
         */

        /* mouse_dx/dy уже обнулены атомарно выше */
    }
}