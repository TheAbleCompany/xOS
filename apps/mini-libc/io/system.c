// SPDX-License-Identifier: BSD-3-Clause
//
// system() для ArtyomX64.
//
// fork/wait в ядре нет, поэтому классический semantics недостижим.
// Реализация: берём первый токен команды как имя программы и запускаем
// её через syscall exec(11). Возврат: 0 — запуск состоялся, -1 — нет.
//
// lua-1.0 в io_execute() всё равно игнорирует результат system().

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <internal/syscall.h>
#include <internal/arch/x86_64/syscall_list.h>

int system(const char *command)
{
	char prog[256];
	size_t i = 0;

	if (command == NULL)
		return -1;	/* "shell отсутствует" */

	/* Пропускаем ведущие пробелы. */
	while (command[i] == ' ' || command[i] == '\t')
		i++;

	/* Первый токен — имя исполняемого файла. */
	{
		size_t j = 0;

		while (command[i] && command[i] != ' ' && command[i] != '\t' &&
		       j < sizeof(prog) - 1)
			prog[j++] = command[i++];

		prog[j] = '\0';
	}

	if (prog[0] == '\0') {
		errno = EINVAL;
		return -1;
	}

	{
		long ret = syscall(__NR_exec, prog, 0, (long)NULL);

		if (ret < 0) {
			errno = (int)(-ret);
			return -1;
		}
	}

	return 0;
}

/* abort перенесён в stdlib/abort.c */

/* getenv перенесён в stdlib/env.c */
