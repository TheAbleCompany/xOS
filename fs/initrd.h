// initrd.h -- Defines the interface for and structures relating to the initial ramdisk.
//             Written for JamesM's kernel development tutorials.
//
// Extended with runtime-write support:
//
//   initrd_ram_file_t  -- heap-backed file storage a node points
//                         to via node->ptr once the file is
//                         writable (created at runtime or after
//                         the first write to an image file).
//   initrd_write()     -- write handler installed on writable nodes.
//   initrd_truncate()  -- correct O_TRUNC implementation.
//   initrd_create()    -- create a new empty file at runtime.

#ifndef INITRD_H
#define INITRD_H

#include "../lib/common.h"
#include "fs.h"

typedef struct
{
    u32int nfiles; // The number of files in the ramdisk.
} initrd_header_t;

/*
 * On-disk file header written by tools/mkinitrd2.
 *
 * Layout (76 bytes with the tail padding):
 *
 *   0        u8  magic   (0xBF)
 *   1        u8  type    (FS_FILE)
 *   2        u8  app_type (PROGRAM_NONE / CONSOLE / GUI, see fs.h)
 *   3..66    char name[64]
 *   67       pad byte (struct alignment)
 *   68..71   u32 offset  (from the start of the initrd image)
 *   72..75   u32 length
 */
typedef struct
{
    u8int magic;     // Magic number, for error checking.
    u8int type;      // FS object type (FS_FILE).
    u8int app_type;  // PROGRAM_NONE / PROGRAM_CONSOLE / PROGRAM_GUI.
    s8int name[64];  // Filename.
    u32int offset;   // Offset in the initrd that the file starts.
    u32int length;   // Length of the file.
} initrd_file_header_t;

/*
 * RAM copy of a writable file.  fs_node_t::ptr points at one of
 * these (24 bytes) once the file can be written.
 */
typedef struct
{
    u8int  *data;   // Heap buffer (may be 0 while empty).
    u64int  len;    // Current file length in bytes.
    u64int  cap;    // Allocated capacity of data.
} initrd_ram_file_t;

// Initialises the initial ramdisk. It gets passed the address of the multiboot module,
// and returns a completed filesystem node.
fs_node_t *initialise_initrd(u64int location);
fs_node_t *initrd_remove(char *name);
fs_node_t *initrd_move(char *src, char *dst);

/* rename(2) с заменой приёмника (initrd -- RAM-ФС). */
int initrd_rename(const char *from, const char *to);

/* mkdir(2): плоское дерево, всегда успех. */
int initrd_mkdir(const char *path);

/*
 * Write handler.  Installed automatically on promoted nodes;
 * exported so fs/open.c can reference the writable model.
 */
u32int initrd_write(fs_node_t *node, u32int offset, u32int size, u8int *buffer);

/*
 * Reset a file to zero length (O_TRUNC).  For image files the
 * file is first promoted to an empty RAM copy.  Never frees the
 * initrd_ram_file_t itself.
 */
int initrd_truncate(fs_node_t *node);

/*
 * Create a new empty writable file in the ramdisk root.
 * Returns the new node or 0 on failure.  If the name already
 * exists, the existing node is returned.
 */
fs_node_t *initrd_create(const char *name);

#endif
