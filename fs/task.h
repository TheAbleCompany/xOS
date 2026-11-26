#ifndef AX_FS_TASK_H
#define AX_FS_TASK_H

/*
 * Устаревшая копия fs/task.h удалена: единственное определение
 * Task/Registers живёт в multitask/task.h.  Дубликат с тем же
 * include-guard (TASK_H) давал другой layout в файлах, которые
 * включали его первым (kernel/abi.c), из-за чего Task::abi
 * читался по неверному смещению и native-сисколлы после
 * TERM_OPEN уходили в чужой ABI.
 */
#include "../multitask/task.h"

#endif
