#ifndef ABI_H
#define ABI_H

/*
 * ============================================================
 * kernel/abi.h -- маршрутизация системных вызовов по ABI задачи
 * ============================================================
 *
 * Задача (Task::abi) может быть не родной программой, а
 * двоичником Linux i386, Linux x86-64 или i386/FreeBSD.  Номера
 * вызовов у них свои, но обслуживаются те же примитивы ядра
 * (read/write/open/close/lseek/unlink/exec/exit), только
 * вызываются по широко распространённым Unix-номерам.
 *
 * Векторы:
 *   0x80 (int 0x80)     -- abi_int80_handler:
 *                          родные -> syscall_handler(),
 *                          Linux i386 и FreeBSD -> свой разбор.
 *   0x81 (syscall)      -- abi_syscall64_handler:
 *                          особый режим ABI для Linux x86-64.
 */

#include "isr.h"

void abi_init(void);

void abi_int80_handler(registers_t *regs);
void abi_syscall64_handler(registers_t *regs);

/*
 * Диспетчер ядра ArtyomX64 (lib/syscalls.c): привычные номера
 * родной системы.  Адресуется напрямую из обработчика 0x80.
 */
void syscall_native_handler(registers_t *regs);

#endif /* ABI_H */
