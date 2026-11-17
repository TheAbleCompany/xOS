#include "console.h"
#include "../drivers/keyboard.h"
#include "../lib/common.h"

fs_node_t console_node;

static void serial_out(char c)
{
    outb(0x3F8, (u8int)c);
}

/* read: неблокирующий — отдаёт всё, что уже накопилось в буфере
 * клавиатуры, и ни байтом больше. Блокировку "до Enter" делает уже
 * пользовательский read() в lib/unistd.h через yield(). */
static u32int console_read(fs_node_t *node, u32int offset, u32int size, u8int *buffer)
{
    (void)node; (void)offset;
    u32int n = 0;
    while (n < size && !is_buffer_empty()) {
        char c = keyboard_read();
        if (c == (char)-1) break;
        buffer[n++] = (u8int)c;
    }
    return n;
}

/* write: льём в serial. Видно в хосте (qemu -serial stdio / COM1). */
static u32int console_write(fs_node_t *node, u32int offset, u32int size, u8int *buffer)
{
    (void)node; (void)offset;
    for (u32int i = 0; i < size; i++) {
        char c = (char)buffer[i];
        serial_out(c);
        if (c == '\n') serial_out('\r'); /* чтобы терминал хоста не "лесенкой" писал */
    }
    return size;
}

void console_install(void)
{
    strcpy(console_node.name, "console");
    console_node.mask   = 0;
    console_node.uid    = 0;
    console_node.gid    = 0;
    console_node.inode  = 0;
    console_node.length = 0;
    console_node.flags  = FS_CHARDEVICE;
    console_node.read   = &console_read;
    console_node.write  = &console_write;
    console_node.open   = 0;
    console_node.close  = 0;
    console_node.readdir = 0;
    console_node.finddir = 0;
    console_node.ptr    = 0;
    console_node.impl   = 0;
}

int console_input_available(void)
{
    return !is_buffer_empty();
}