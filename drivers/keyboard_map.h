/* The base array is taken from
    http://www.osdever.net/bkerndev/Docs/keyboard.htm
   All credits where due.

   Extended with the shift and caps-lock variants used by the
   keyboard driver (keyboard.c) and axshell (axshell_input.c):

     keyboard_map[]       -- plain scancode -> ASCII
     keyboard_map_shift[] -- Shift held
     keyboard_map_caps[]  -- CapsLock held (letters uppercased)

   Shift+CapsLock is handled by the caller: for letters it falls
   back to the plain map (lowercase), PC-standard behaviour.
*/

unsigned char keyboard_map[128] =
{
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',     /* 9 */
  '9', '0', '-', '=', '\b',     /* Backspace */
  '\t',                 /* Tab */
  'q', 'w', 'e', 'r',   /* 19 */
  't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', /* Enter key */
    0,                  /* 29   - Control */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',     /* 39 */
 '\'', '`',   0,                /* Left shift */
 '\\', 'z', 'x', 'c', 'v', 'b', 'n',                    /* 49 */
  'm', ',', '.', '/',   0,                              /* Right shift */
  '*',
    0,  /* Alt */
  ' ',  /* Space bar */
    0,  /* Caps lock */
    0,  /* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,  /* < ... F10 */
    0,  /* 69 - Num lock*/
    0,  /* Scroll Lock */
    0,  /* Home key */
    0,  /* Up Arrow */
    0,  /* Page Up */
  '-',
    0,  /* Left Arrow */
    0,
    0,  /* Right Arrow */
  '+',
    0,  /* 79 - End key*/
    0,  /* Down Arrow */
    0,  /* Page Down */
    0,  /* Insert Key */
    0,  /* Delete Key */
    0,   0,   0,
    0,  /* F11 Key */
    0,  /* F12 Key */
    0,  /* All other keys are undefined */
};

/* Shift held: uppercase letters + shifted punctuation row. */
unsigned char keyboard_map_shift[128] =
{
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*',     /* 9 */
  '(', ')', '_', '+', '\b',     /* Backspace */
  '\t',                 /* Tab */
  'Q', 'W', 'E', 'R',   /* 19 */
  'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', /* Enter key */
    0,                  /* 29   - Control */
  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',     /* 39 */
 '"', '~',   0,                /* Left shift */
 '|', 'Z', 'X', 'C', 'V', 'B', 'N',                     /* 49 */
  'M', '<', '>', '?',   0,                              /* Right shift */
  '*',
    0,  /* Alt */
  ' ',  /* Space bar */
    0,  /* Caps lock */
    0,  /* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,  /* < ... F10 */
    0,  /* 69 - Num lock*/
    0,  /* Scroll Lock */
    0,  /* Home key */
    0,  /* Up Arrow */
    0,  /* Page Up */
  '-',
    0,  /* Left Arrow */
    0,
    0,  /* Right Arrow */
  '+',
    0,  /* 79 - End key*/
    0,  /* Down Arrow */
    0,  /* Page Down */
    0,  /* Insert Key */
    0,  /* Delete Key */
    0,   0,   0,
    0,  /* F11 Key */
    0,  /* F12 Key */
    0,  /* All other keys are undefined */
};

/* CapsLock held: letters uppercased, everything else unchanged. */
unsigned char keyboard_map_caps[128] =
{
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',     /* 9 */
  '9', '0', '-', '=', '\b',     /* Backspace */
  '\t',                 /* Tab */
  'Q', 'W', 'E', 'R',   /* 19 */
  'T', 'Y', 'U', 'I', 'O', 'P', '[', ']', '\n', /* Enter key */
    0,                  /* 29   - Control */
  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ';',     /* 39 */
 '\'', '`',   0,                /* Left shift */
 '\\', 'Z', 'X', 'C', 'V', 'B', 'N',                    /* 49 */
  'M', ',', '.', '/',   0,                              /* Right shift */
  '*',
    0,  /* Alt */
  ' ',  /* Space bar */
    0,  /* Caps lock */
    0,  /* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,  /* < ... F10 */
    0,  /* 69 - Num lock*/
    0,  /* Scroll Lock */
    0,  /* Home key */
    0,  /* Up Arrow */
    0,  /* Page Up */
  '-',
    0,  /* Left Arrow */
    0,
    0,  /* Right Arrow */
  '+',
    0,  /* 79 - End key*/
    0,  /* Down Arrow */
    0,  /* Page Down */
    0,  /* Insert Key */
    0,  /* Delete Key */
    0,   0,   0,
    0,  /* F11 Key */
    0,  /* F12 Key */
    0,  /* All other keys are undefined */
};

/*
 * ============================================================
 * Кодировка событий клавиатуры (keyboard_pop_event / INPUT_KEY)
 * ============================================================
 *
 * Обычное нажатие обычной клавиши -- по-прежнему 0x00..0x7F
 * (сканкод set 1), поэтому существующие клиенты не меняются.
 * Старшие биты добавлены для игр (DOOM), которым нужно знать
 * об отпускании клавиш и о стрелках:
 *
 *   KBE_RELEASE  0x8000 -- клавиша отпущена (break-код)
 *   KBE_EXT      0x4000 -- клавиша расширенная (префикс 0xE0:
 *                         стрелки, правый Ctrl/Alt, keypad)
 *   KBE_RAWONLY  0x2000 -- доставлять только как AX_EV_RAWKEY,
 *                         не транслировать в ASCII (модификаторы,
 *                         Ctrl+<буква>)
 *
 * Младшие 8 бит -- всегда сканкод set 1.
 *
 * Используется драйвером (drivers/keyboard.c) и WindowServer'ом
 * (server/wserver_input.c), который превращает события в
 * AX_EV_RAWKEY для сфокусированного окна.
 */
#define KBE_RELEASE  0x8000u
#define KBE_EXT      0x4000u
#define KBE_RAWONLY  0x2000u
#define KBE_SC_MASK  0x00FFu
