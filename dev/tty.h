#ifndef TTY_H
#define TTY_H

#include "../fs/fs.h"
#include <stdint.h>
#include <stdbool.h>


// IOCTL команды
#define TCGETS          0x5401
#define TCSETS          0x5402
#define TIOCGWINSZ      0x5413
#define TIOCSWINSZ      0x5414

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

// Флаги termios
#define ECHO    0x00000008
#define ICANON  0x00000100
#define ISIG    0x00000001
#define ICRNL   0x00000100
#define IXON    0x00020000
#define OPOST   0x00000001

#define PTY_BUFFER_SIZE 8192

typedef struct pty_buffer {
    char data[PTY_BUFFER_SIZE];
    volatile u32int head;
    volatile u32int tail;
} pty_buffer_t;

// ОСНОВНАЯ СТРУКТУРА УСТРОЙСТВА
typedef struct tty_device {
    fs_node_t node;
    bool master;
    int slave_number;
    pty_buffer_t buffer;
    
    uint32_t termios_c_lflag;
    struct winsize winsize;
    bool stopped;
    bool has_data;
} tty_device_t;

void tty_install(void);
tty_device_t* pty_allocate(void);

#endif // Конец защиты макроса