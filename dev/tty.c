#include "../dev/tty.h"
#include "../mm/kheap.h"
#include "../lib/common.h"
#include "../drivers/keyboard.h" 
#include "../fs/fs.h"

// --- Объявления ваших глобальных данных из initrd.c ---
// Убедитесь, что они видны линковщику (не static)
extern fs_node_t *root_nodes;
extern int nroot_nodes;
extern fs_node_t *initrd_dev; // Узел /dev для возврата при навигации ".."

static tty_device_t **tty_table;
static int tty_count = 0;
static const char *pts_prefix = "pts/";

// --- Вспомогательные функции ---
static void itoa_simple(int v, char *out) {
    if (v == 0) { 
        out[0] = '0'; 
        out[1] = 0; 
        return; 
    }
    
    char tmp[12]; 
    int t = 0;
    int neg = 0;
    
    if (v < 0) { 
        neg = 1; 
        v = -v; 
    }
    
    while (v) { 
        tmp[t++] = '0' + (v % 10); 
        v /= 10; 
    }
    
    int p = 0;
    if (neg) out[p++] = '-';
    
    while (t) out[p++] = tmp[--t];
    out[p] = 0;
}

// --- Утилиты кольцевого буфера ---
void pty_buffer_write(pty_buffer_t *buf, char c) {
    buf->data[buf->head] = c;
    buf->head = (buf->head + 1) % PTY_BUFFER_SIZE;
    if (buf->head == buf->tail) {
        buf->tail = (buf->tail + 1) % PTY_BUFFER_SIZE;
    }
}

static int pty_buffer_read(pty_buffer_t *buf, char *out) {
    if (buf->head == buf->tail) return 0;
    *out = buf->data[buf->tail];
    buf->tail = (buf->tail + 1) % PTY_BUFFER_SIZE;
    return 1;
}

// --- Колбэки VFS ---
static u32int tty_read(fs_node_t *node, u32int offset, u32int size, u8int *buffer) {
    (void)offset;
    tty_device_t *tty = (tty_device_t*)(uintptr_t)node->impl; 
    
    if (tty->stopped) return 0;

    u32int read_bytes = 0;
    while (read_bytes < size) {
        char c;
        int got = pty_buffer_read(&tty->buffer, &c);
        
        if (!got) {
            if ((tty->termios_c_lflag & ICANON)) break;
            return read_bytes;
        }

        buffer[read_bytes++] = (u8int)c;
        tty->has_data = false;

        if ((tty->termios_c_lflag & ICANON) && c == '\n') {
            break;
        }
    }
    return read_bytes;
}

static u32int tty_write(fs_node_t *node, u32int offset, u32int size, u8int *buffer) {
    (void)offset;
    tty_device_t *tty = (tty_device_t*)(uintptr_t)node->impl;

    for (u32int i = 0; i < size; i++) {
        char c = buffer[i];
        
        if ((tty->termios_c_lflag & IXON)) {
            if (c == 0x13) { tty->stopped = true; continue; } 
            if (c == 0x11) { tty->stopped = false; }
        }

        if ((tty->termios_c_lflag & ECHO)) {
            pty_buffer_write(&tty->buffer, c);
            if (c == '\n' && (tty->termios_c_lflag & ICRNL)) {
                pty_buffer_write(&tty->buffer, '\r');
            }
        }

        if ((tty->termios_c_lflag & OPOST) && c == '\n') {
            pty_buffer_write(&tty->buffer, '\r');
        }
        
        pty_buffer_write(&tty->buffer, c);
        tty->has_data = true;
    }
    return size;
}

// Упрощенный ioctl
int tty_ioctl(fs_node_t *node, unsigned long com, char *data) {
    tty_device_t *tty = (tty_device_t*)(uintptr_t)node->impl;
    
    switch(com) {
        case TCGETS:
            memcpy((u8int*)data, &tty->termios_c_lflag, sizeof(uint32_t));
            break;
        case TCSETS:
            memcpy(&tty->termios_c_lflag, (u8int*)data, sizeof(uint32_t));
            break;
        case TIOCGWINSZ:
            memcpy((u8int*)data, &tty->winsize, sizeof(struct winsize));
            break;
        case TIOCSWINSZ:
            memcpy(&tty->winsize, (u8int*)data, sizeof(struct winsize));
            break;
        default:
            return -1;
    }
    return 0;
}

// Поиск внутри /dev/pts/
static fs_node_t* tty_finddir(fs_node_t *node, char *name) {
    tty_device_t *tty = (tty_device_t*)(uintptr_t)node->impl;
    if (!tty->master && !strcmp(name, "..")) return initrd_dev; 
    return NULL; 
}

// --- Аллокация пары устройств ---
tty_device_t* pty_allocate(void) {
    tty_device_t *master = (tty_device_t*)kmalloc(sizeof(tty_device_t));
    tty_device_t *slave = (tty_device_t*)kmalloc(sizeof(tty_device_t));
    memset(master, 0, sizeof(tty_device_t));
    memset(slave, 0, sizeof(tty_device_t));

    master->master = true;
    master->slave_number = -1;
    master->termios_c_lflag = ICANON | ECHO | ICRNL | IXON | OPOST;
    master->winsize.ws_row = 25;
    master->winsize.ws_col = 80;
    
    slave->master = false;
    slave->slave_number = tty_count;
    slave->termios_c_lflag = ICANON | ECHO | ICRNL | IXON | OPOST;
    slave->winsize.ws_row = 25;
    slave->winsize.ws_col = 80;

    master->buffer = slave->buffer;

    // Настройка Slave узла (/dev/pts/N)
    char name[16];
    strcpy(name, pts_prefix);
    char num[4];
    itoa_simple(slave->slave_number, num);
    strcat(name, num);
    
    strcpy(slave->node.name, name);
    slave->node.mask = slave->node.uid = slave->node.gid = 0;
    slave->node.flags = FS_CHARDEVICE;
    slave->node.read = &tty_read;
    slave->node.write = &tty_write;
    slave->node.readdir = 0;
    slave->node.finddir = &tty_finddir;
    //slave->node.ioctl = &tty_ioctl; // Раскомментируйте, если добавили ioctl в fs_node_t
    slave->node.impl = (u32int)(uintptr_t)slave; 
    slave->node.length = 0;

    // Добавляем слейва в корень ФС (в список файлов RAM-диска)
    root_nodes[nroot_nodes++] = slave->node; 

    // Настройка Master узла (/dev/ptmx)
    strcpy(master->node.name, "ptmx");
    master->node.mask = master->node.uid = master->node.gid = 0;
    master->node.flags = FS_CHARDEVICE;
    master->node.read = &tty_read;
    master->node.write = &tty_write;
    master->node.readdir = 0;
    master->node.finddir = 0;
    //master->node.ioctl = &tty_ioctl;
    master->node.impl = (u32int)(uintptr_t)master;
    master->node.length = 0;

    tty_table[tty_count++] = master;
    tty_table[tty_count++] = slave;
    
    return master;
}

// --- Инициализация подсистемы терминалов ---
void tty_install(void) {
    tty_table = (tty_device_t**)kmalloc(sizeof(tty_device_t*) * 64);
    memset(tty_table, 0, sizeof(tty_device_t*) * 64);
    
    // Создаем директорию /dev/pts вручную в списке файлов RAM-диска
    fs_node_t *pts_dir = (fs_node_t*)kmalloc(sizeof(fs_node_t));
    strcpy(pts_dir->name, "pts");
    pts_dir->mask = pts_dir->uid = pts_dir->gid = 0;
    pts_dir->flags = FS_DIRECTORY;
    pts_dir->read = 0;
    pts_dir->write = 0;
    pts_dir->open = 0;
    pts_dir->close = 0;
    pts_dir->readdir = 0; 
    pts_dir->finddir = &tty_finddir;
    pts_dir->impl = 0;
    pts_dir->length = 0;

    // Находим 'dev' и добавляем туда 'pts'
    for (int i = 0; i < nroot_nodes; i++) {
        if (!strcmp(root_nodes[i].name, "dev")) {
            // Здесь логика зависит от вашего дерева каталогов.
            // Если у вас плоская структура root_nodes, файл "pts" уже создан выше,
            // или он будет найден finddir_fs("/dev", "pts").
            break; 
        }
    }
    
    // Выделяем первый терминал при старте системы
    pty_allocate(); 
}