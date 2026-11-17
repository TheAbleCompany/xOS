#ifndef FS_FILE_H
#define FS_FILE_H

#include <stdint.h>

#include "fs.h"

typedef struct file {
    fs_node_t *node;
    uint32_t pos;
    uint32_t flags;
} file_t;

#endif