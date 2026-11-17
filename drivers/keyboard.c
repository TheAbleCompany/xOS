#include "keyboard.h"
#include "monitor.h"
#include "keyboard_map.h"
#include "../kernel/isr.h"
#include "../dev/pty.h"
/*#include "ioapic.h"
#include "pic.h"
#include "apic.h"
*/
#define KEYBOARD_DATA_PORT    0x60
#define KEYBOARD_STATUS_PORT  0x64

#define ENTER_KEY_CODE        0x1C
#define BACKSPACE_KEY_CODE    0x0E
#define CAPS_KEY_CODE 0x3A

#define KEY_EVENT_QUEUE_SIZE 256

/*
 * Кодировка события клавиатуры -- KBE_* в keyboard_map.h
 * (общая с wserver_input.c).
 */
static volatile unsigned int key_event_queue[KEY_EVENT_QUEUE_SIZE];
static volatile unsigned int key_event_head = 0;
static volatile unsigned int key_event_tail = 0;

/*
 * PTY, в который сейчас направляется клавиатурный ввод.
 *
 * Обычно это slave/master PTY терминала, который сейчас
 * является активным.
 */
static ax_pty_t *keyboard_pty = 0;

/*
 * Оставляем current_key, потому что axshell.c использует
 * его для GUI-событий.
 */
char current_key = 0;

/*
 * ============================================================
 * Ctrl modifier state
 * ============================================================
 *
 * Set to 1 while the Ctrl key (Left Ctrl, scancode 0x1D) is held.
 *
 * While Ctrl is down, letter presses are translated into
 * special GUI keycodes instead of plain ASCII:
 *
 *     Ctrl + S  ->  0xF1  (axshell maps it to AX_KEY_CTRL_S)
 *     Ctrl + O  ->  0xF4  (axshell maps it to AX_KEY_CTRL_O)
 *     Ctrl + N  ->  0xF5  (axshell maps it to AX_KEY_CTRL_N)
 */
int ax_ctrl_pressed = 0;

/* Special GUI keycodes consumed by axshell's process_input(). */
#define KB_SPECIAL_CTRL_S   0xF1
#define KB_SPECIAL_CTRL_O   0xF4
#define KB_SPECIAL_CTRL_N   0xF5

/*
 * ============================================================
 * Shift modifier state
 * ============================================================
 *
 * Set to 1 while either Shift key is held:
 *
 *     Left Shift  : make 0x2A / break 0xAA
 *     Right Shift : make 0x36 / break 0xB6
 *
 * Используется и самим драйвером, и axshell (process_input),
 * чтобы scancode транслировался в "shift'нутый" ASCII-символ
 * ('!', '@', заглавные буквы и т.д.).
 */
int ax_shift_pressed = 0;
/*
 * Старые переменные оставляем для совместимости
 * со старым кодом syscall/keyboard.
 *
 * Но новый терминальный read() НЕ должен пользоваться
 * этим буфером.
 */
#define BUFFER_SIZE 256

static char keyboard_buffer[BUFFER_SIZE];
static int buffer_head = 0;
static int buffer_tail = 0;
static int buffer_count = 0;

int is_enter_pressed = 0;
int is_caps_pressed = 0;

static void keyboard_push_event(unsigned int keycode)
{
    unsigned int next =
        (key_event_head + 1) % KEY_EVENT_QUEUE_SIZE;

    if (next == key_event_tail) {
        return; /* очередь заполнена */
    }

    key_event_queue[key_event_head] = keycode;
    key_event_head = next;
}

int keyboard_pop_event(void)
{
    unsigned int keycode;

    if (key_event_tail == key_event_head)
        return -1;

    keycode = key_event_queue[key_event_tail];

    key_event_tail =
        (key_event_tail + 1) % KEY_EVENT_QUEUE_SIZE;

    return keycode;
}

/*
 * ============================================================
 * PTY target
 * ============================================================
 */

void keyboard_set_pty(ax_pty_t *pty)
{
    keyboard_pty = pty;
}

ax_pty_t *keyboard_get_pty(void)
{
    return keyboard_pty;
}


/*
 * ============================================================
 * Старый keyboard buffer
 * ============================================================
 *
 * Оставлен для совместимости.
 * PTY read() его больше не использует.
 */

int is_buffer_empty(void)
{
    return buffer_count == 0;
}

int is_buffer_full(void)
{
    return buffer_count >= BUFFER_SIZE - 1;
}

int keyboard_buffer_available(void)
{
    return buffer_count > 0;
}

void keyboard_clear_buffer(void)
{
    buffer_head = 0;
    buffer_tail = 0;
    buffer_count = 0;
    is_enter_pressed = 0;
}

char keyboard_read(void)
{
    char c;

    if (is_buffer_empty())
        return -1;

    c = keyboard_buffer[buffer_tail];

    buffer_tail =
        (buffer_tail + 1) % BUFFER_SIZE;

    buffer_count--;

    return c;
}


/*
 * ============================================================
 * Отправка символа в PTY
 * ============================================================
 */

static void keyboard_send_char(char c)
{
    /*
     * Если активного PTY нет — символ просто теряется.
     */
    if (!keyboard_pty)
        return;

    /*
     * ВАЖНО:
     *
     * Здесь НЕ надо делать echo через monitor_put().
     *
     * Echo теперь полностью выполняет PTY:
     *
     * keyboard
     *     ↓
     * pty_master_feed()
     *     ↓
     *     ├── m2s
     *     └── s2m (echo)
     */
    //pty_master_feed(keyboard_pty, c);
}


/*
 * ============================================================
 * Обработка одного scancode
 * ============================================================
 */

void keyboard_handle_scancode(unsigned char keycode)
{
    /*
     * ========================================================
     * 0xE0 prefix (extended keys: arrows, right Ctrl/Alt,
     * F11/F12, keypad / Enter ...).  Scancode set 1 marks
     * them with a leading 0xE0 byte; track it so the next
     * byte is delivered as an "extended" event.
     * ========================================================
     */
    static int e0_prefix = 0;

    if (keycode == 0xE0) {
        e0_prefix = 1;
        return;
    }

    if (keycode == 0xE1) {          /* Pause: multi-byte, not decoded */
        e0_prefix = 0;
        return;
    }

    int is_ext   = e0_prefix;
    int is_break = (keycode & 0x80u) != 0;
    int sc       = (int)(keycode & 0x7Fu);

    e0_prefix = 0;

    /*
     * ========================================================
     * Modifier tracking (make/break, BEFORE any filtering).
     * ========================================================
     */
    if (!is_ext && (sc == 0x2A || sc == 0x36))
        ax_shift_pressed = !is_break;

    if (!is_ext && sc == 0x1D)
        ax_ctrl_pressed = !is_break;

    /*
     * ========================================================
     * RAW key event -- every key, including releases and
     * modifiers.  High bits are described in keyboard_map.h
     * (KBE_*):
     *
     *   0x8000 release
     *   0x4000 extended (E0 prefix)
     *   0x2000 raw-only: не транслировать в ASCII
     *   0x00FF scancode
     *
     * Старое поведение сохранено: обычное нажатие обычной
     * клавиши по-прежнему приходит в очередь как 0x00..0x7F,
     * поэтому существующие приложения (wserver/редакторы)
     * ничего не замечают.
     * ========================================================
     */
    {
        unsigned int raw = (unsigned int)sc;

        if (is_ext)   raw |= KBE_EXT;
        if (is_break) raw |= KBE_RELEASE;

        /*
         * Модификаторы и Ctrl+<letter> получают KBE_RAWONLY:
         * игровые окна видят их как AX_EV_RAWKEY, но текстовые
         * приложения не должны печатать символы от Shift/Ctrl.
         */
        if (!is_ext && (sc == 0x2A || sc == 0x36 || sc == 0x1D ||
                        sc == 0x38 || sc == 0x3A))
            raw |= KBE_RAWONLY;

        if (ax_ctrl_pressed && !is_ext && !is_break &&
            sc != 0x1D && keyboard_map[sc] != 0)
            raw |= KBE_RAWONLY;

        keyboard_push_event(raw);
    }

    current_key = is_break ? 0 : (char)sc;

    /*
     * ========================================================
     * Ниже -- только "make" обычных клавиш: трансляция в
     * ASCII/спецкоды для окон и PTY.
     * ========================================================
     */
    if (is_break || is_ext) {
        current_key = 0;
        return;
    }

    /*
     * ========================================================
     * Ctrl + letter combinations.
     *
     * While Ctrl is held, these keys produce special GUI
     * keycodes instead of plain ASCII, so applications can
     * implement Save (Ctrl+S), Open (Ctrl+O), New (Ctrl+N).
     * ========================================================
     */
    if (ax_ctrl_pressed) {
        unsigned char special = 0;

        if (sc == 0x1F)            /* S */
            special = KB_SPECIAL_CTRL_S;
        else if (sc == 0x18)       /* O */
            special = KB_SPECIAL_CTRL_O;
        else if (sc == 0x31)       /* N */
            special = KB_SPECIAL_CTRL_N;

        if (special) {
            keyboard_push_event(special);
            return;
        }

        /*
         * Other Ctrl combinations are suppressed:
         * they must not type letters into documents.
         */
        if (keyboard_map[sc])
            return;
    }

    /*
     * PTY-часть (старое поведение).
     */
    if (sc == ENTER_KEY_CODE) {
        keyboard_send_char('\n');
        is_enter_pressed = 1;
        return;
    }

    if (sc == BACKSPACE_KEY_CODE) {
        keyboard_send_char('\b');
        return;
    }

    if (sc == CAPS_KEY_CODE) {
        is_caps_pressed = !is_caps_pressed;
        return;
    }

    {
        char c;
        if (ax_shift_pressed)
            c = keyboard_map_shift[(unsigned int)sc];
        else if (is_caps_pressed)
            c = keyboard_map_caps[(unsigned int)sc];
        else
            c = keyboard_map[(unsigned int)sc];

        if (c == 0)
            return;

        keyboard_send_char(c);
    }
}


/*
 * ============================================================
 * IRQ1
 * ============================================================
 */

static void keyboard_callback(registers_t regs)
{
     /*#include "../flanterm/flanterm.h"

extern struct flanterm_context *ft_ctx;
         flanterm_write(ft_ctx, "KEY\r\n", 5);*/
    
    unsigned char status;
    unsigned char keycode;

    (void)regs;

    /*
     * Проверяем наличие данных.
     */
    status = inb(KEYBOARD_STATUS_PORT);

    if (status & 0x01u) {

        keycode = inb(KEYBOARD_DATA_PORT);

        /*
         * Вся обработка клавиши теперь здесь.
         */
        keyboard_handle_scancode(keycode);

    }

    /*
     * EOI для master PIC.
     */
    outb(0x20, 0x20);
     //writeAPICRegister(APIC_EOI_REGISTER, 0);
}


/*
 * ============================================================
 * Initialization
 * ============================================================
 */

/*
 * ============================================================
 * r12-hwdiag: диагностика PS/2 клавиатуры на экран + COM1
 * ============================================================
 *
 * В QEMU BIOS сам включает нужные биты i8042 (IRQ1, трансляция
 * сканкодов).  На реальном железе после самотеста контроллера
 * (0xAA) конфигурационный байт сбрасывается, поэтому его нужно
 * восстановить.  Без экранной диагностики состояние клавиатуры
 * было видно только в COM1.
 */

#include "../flanterm/flanterm.h"
extern struct flanterm_context *ft_ctx;

static void kbd_debug(const char *s)
{
    const char *p = s;

    while (*p)
        outb(0x3F8, *p++);

    if (ft_ctx)
        flanterm_write(ft_ctx, s, (size_t)(p - s));
}

static void kbd_debug_hex(unsigned char x)
{
    const char hex[] = "0123456789ABCDEF";

    outb(0x3F8, hex[(x >> 4) & 0xF]);
    outb(0x3F8, hex[x & 0xF]);

    {
        char b[3];
        b[0] = hex[(x >> 4) & 0xF];
        b[1] = hex[x & 0xF];
        b[2] = 0;
        if (ft_ctx)
            flanterm_write(ft_ctx, b, 2);
    }
}

static int kbd_wait(unsigned int timeout, int for_write)
{
    while (timeout--) {
        unsigned char st = inb(KEYBOARD_STATUS_PORT);

        if (for_write ? !(st & 0x02u) : (st & 0x01u))
            return 1;

        __asm__ volatile("pause");
    }

    return 0;
}

void init_keyboard(/*void*/size_t ioapicaddr)
{
    const unsigned int timeout = 2000000;
    unsigned char resp;

    (void)ioapicaddr;

    keyboard_clear_buffer();

    current_key = 0;

    keyboard_pty = 0;

    kbd_debug("[KBD] init\n");

    /*
     * 1. Очистить pending output.
     */

    for (int i = 0; i < 32; i++) {

        if (!(inb(KEYBOARD_STATUS_PORT) & 0x01u))
            break;

        (void)inb(KEYBOARD_DATA_PORT);
    }

    /*
     * 2. Самотест контроллера 8042 (0xAA -> 0x55).
     *    ВАЖНО: он сбрасывает конфигурационный байт!
     */

    if (!kbd_wait(timeout, 1)) goto kbd_fail;

    outb(KEYBOARD_STATUS_PORT, 0xAA);

    if (!kbd_wait(timeout, 0)) goto kbd_fail;

    resp = inb(KEYBOARD_DATA_PORT);

    if (resp != 0x55) {
        kbd_debug("[KBD] controller self-test: 0x");
        kbd_debug_hex(resp);
        kbd_debug("\n");
        goto kbd_fail;
    }

    /*
     * 3. Восстановить конфигурацию:
     *    bit 0 = IRQ1 enable, bit 1 = IRQ12 enable,
     *    bit 6 = трансляция сканкодов (set 2 -> set 1).
     */

    {
        unsigned char cfg;

        if (!kbd_wait(timeout, 1)) goto kbd_fail;

        outb(KEYBOARD_STATUS_PORT, 0x20);

        if (!kbd_wait(timeout, 0)) goto kbd_fail;

        cfg = inb(KEYBOARD_DATA_PORT);

        cfg |=  (1u << 0);
        cfg |=  (1u << 1);
        cfg |=  (1u << 6);
        cfg &= ~(1u << 5);

        if (!kbd_wait(timeout, 1)) goto kbd_fail;

        outb(KEYBOARD_STATUS_PORT, 0x60);

        if (!kbd_wait(timeout, 1)) goto kbd_fail;

        outb(KEYBOARD_DATA_PORT, cfg);
    }

    /*
     * 4. Тест клавиатурного интерфейса (0xAB -> 0x00).
     */

    if (!kbd_wait(timeout, 1)) goto kbd_fail;

    outb(KEYBOARD_STATUS_PORT, 0xAB);

    if (!kbd_wait(timeout, 0)) goto kbd_fail;

    resp = inb(KEYBOARD_DATA_PORT);

    if (resp != 0x00) {
        kbd_debug("[KBD] interface test: 0x");
        kbd_debug_hex(resp);
        kbd_debug("\n");
        goto kbd_fail;
    }

    /*
     * 5. Включить первый порт (0xAE).
     */

    if (!kbd_wait(timeout, 1)) goto kbd_fail;

    outb(KEYBOARD_STATUS_PORT, 0xAE);

    /*
     * 6. Включить сканирование (0xF4 -> ACK 0xFA).
     *    Байты клавиатуре пишутся напрямую в порт 0x60.
     */

    if (!kbd_wait(timeout, 1)) goto kbd_fail;

    outb(KEYBOARD_DATA_PORT, 0xF4);

    if (!kbd_wait(timeout, 0)) goto kbd_fail;

    resp = inb(KEYBOARD_DATA_PORT);

    if (resp != 0xFA) {
        kbd_debug("[KBD] enable scanning: 0x");
        kbd_debug_hex(resp);
        kbd_debug("\n");
        goto kbd_fail;
    }

    kbd_debug("[KBD] ready\n");

    register_interrupt_handler(
        IRQ1,
        &keyboard_callback
    );

    return;

kbd_fail:
    kbd_debug("[KBD] init failed (нет отклика i8042)\n");

    register_interrupt_handler(
        IRQ1,
        &keyboard_callback
    );
}