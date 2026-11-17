#ifndef DEV_CONSOLE_H
#define DEV_CONSOLE_H

#include "../fs/fs.h"

/* Простой символьный псевдотерминал.
 * Ввод берётся из уже существующего кольцевого буфера клавиатуры
 * (drivers/keyboard.c), вывод идёт в serial (COM1) — то есть виден
 * в хосте через `qemu ... -serial stdio`.
 *
 * console_node — единственный узел, который exec() открывает как
 * stdin=stdout=stderr для запускаемых процессов. */
extern fs_node_t console_node;

/* Один раз инициализировать console_node. Вызывать после того, как
 * fs_root/initrd готовы, и до первого exec(). */
void console_install(void);

/* Есть ли непрочитанные байты ввода */
int console_input_available(void);

#endif