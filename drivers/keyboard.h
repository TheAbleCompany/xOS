#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "../dev/pty.h"

//void init_keyboard(void);
void init_keyboard(/*void*/size_t ioapicaddr);

void keyboard_handle_scancode(unsigned char keycode);

void keyboard_set_pty(ax_pty_t *pty);
ax_pty_t *keyboard_get_pty(void);

int is_buffer_empty(void);
int is_buffer_full(void);
int keyboard_buffer_available(void);

void keyboard_clear_buffer(void);
char keyboard_read(void);

int keyboard_pop_event(void);
extern int is_enter_pressed;
extern int is_caps_pressed;
extern int ax_ctrl_pressed;
extern int ax_shift_pressed;
extern char current_key;

#endif