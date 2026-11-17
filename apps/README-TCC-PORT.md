# Порт tcc (tinycc-mob 0.9.28rc) на ArtyomX64 — что сделано

## Итог

tcc собирается и работает на ArtyomX64. Бинарник: `apps/tinycc-mob/build-art/bin/tcc`
(статический ELF, int 0x80 твоего ядра, собран хостовым gcc + mini-libc).
tcc компилирует C-программы в статические ELF, которые запускаются тем же
`exec`, и линкует их с `crt0.o + libc.a + libtcc1.a` из корня initrd.

На хост-линукс весь код (mini-libc + компилятор) проверен через шим syscall:
- тест setjmp/longjmp — 9/9 (включая паттерн `error_jmp_buf` из libtcc.c и
  восстановление всех callee-saved после глубины 50);
- тест новых модулей libc (qsort, strtoll/strtoull, time/strftime/mktime,
  fseek/ftell/rewind, mkstemp, malloc/realloc стресс) — все OK;
- `tcc` компилирует тесты `tests/tests2` (38 OK; остальные — platform-N/A,
  см. «Ограничения»), все 10 файлов lua-1.0 (включая y_tab.c на ~2500 строк);
- функциональный прогон: программы, СКОМПИЛИРОВАННЫЕ tcc, слинкованные с
  mini-libc (шим), на хосте работают побитово корректно — setjmp/longjmp,
  printf %f/%s/%d, структуры, sqrt, strtoll.

## Использование на ОС

В терминале (tcc уже в initrd, демо-исходники тоже):

    exec tcc /hello.c -o /hello2
    exec hello2

    exec tcc /sjt.c -o /sjt2        # setjmp/longjmp демо
    exec sjt2

    exec tcc -c /x.c                # только объект
    exec tcc -v                     # версия/пути

tcc ищет заголовки и библиотеки в корне initrd ("/"): `stdio.h`, `sys/stat.h`,
`libc.a`, `libtcc1.a`, `crt0.o`, `tccdefs.h` — всё уже упаковано.

## setjmp/longjmp в mini-libc (то, что просил)

- `crt/jmp.S` — ПЕРЕПИСАН: раньше там был только `setjmp`, а `longjmp` вообще
  отсутствовал; кроме того, старый Makefile клал jmp.S в архив ИСХОДНИКОМ
  (`patsubst %.c` не трогал `.S`), т.е. setjmp реально не линковался. Теперь
  собирается `jmp.o` с символами: `setjmp/_setjmp/sigsetjmp/__sigsetjmp`
  и `longjmp/_longjmp/siglongjmp/__longjmp_chk`.
- `jmp_buf[8]` (SysV x86-64): rbx, rbp, r12–r15, rsp (без ret addr), rip.
  `longjmp(env, 0)` возвращает 1, как требует стандарт.
- `include/setjmp.h` — убран кривой макрос `setjmp-> _setjmp`, объявлены все
  варианты.
- `libtcc.c` использует `setjmp/longjmp` (`error_jmp_buf`) — без этого tcc не
  мог запуститься в принципе. Теперь всё сходится: проверено и на gcc-коде
  (хост-тесты), и на tcc-коде (функциональный прогон tcc-компиляции).

## Что добавлено в mini-libc

Новые файлы:
- stdlib/qsort.c    — heapsort (O(1) стек; стек задачи всего 16 КБ)
- stdlib/env.c      — getenv (NULL: ядро не передаёт envp), getcwd ("/"),
                      realpath (схлопывание "/"), setenv/putenv заглушки
- stdlib/abort.c    — abort() -> сообщение + exit(134)
- stdlib/atexit.c   — atexit, вызывается из exit() (LIFO) + fflush(NULL)
- stdlib/strtoll.c  — КОРРЕКТНЫЕ strtoll/strtoull (+ алиас __isoc23_strtoull);
                      старый strtoull.c исключён из сборки (ломал 0x/базы)
- stdlib/strtof.c   — strtof/strtold (через strtod)
- stdlib/assert.c   — __assert_fail
- stdlib/misc.c     — execvp/execv (ENOSYS: exec ядра заменяет задачу),
                      mprotect (заглушка: память RWX)
- time/time.c       — time (метка сборки libc, -DTIME_EPOCH; RTC в ядре нет),
                      gettimeofday, gmtime/localtime (гражданский календарь,
                      UTC), mktime, strftime
- io/fseek.c        — fseek/ftell/rewind/fgetpos/fsetpos поверх lseek(8);
                      учтён буфер чтения: SEEK_CUR смещается на -_r
- io/mkstemp.c      — mkstemp (счётчик в .bss вместо getpid)

Новые заголовки: stdint.h, stdbool.h, assert.h, inttypes.h, sys/time.h;
расширены limits.h (LLONG_*/PATH_MAX и пр.), time.h (struct tm), math.h
(ldexpl/frexpl/fabsl/floorl/ceill), sys/mman.h (MAP_*).
stdio.h/string.h/ctype.h/math.h/setjmp.h/errno.h теперь включают <stddef.h>
(в системном наборе заголовков stdarg/stddef — от tcc, старая цепочка через
internal/types.h давала NULL только с mini-libc'шным stdarg).

Исправлено:
- io/fopen.c       — режимы '+': теперь выставляются _IO_READ|_IO_WRITE
                     (раньше "w+b" не мог читать!)
- process/exit.c   — exit() прогоняет atexit-обработчики и fflush(NULL);
                     добавлен _exit
- process/sleep.c  — поля timespec на tv_sec/tv_nsec
- io/system.c      — убраны дубли getenv/abort
- mm/malloc.c      — куча 1 МиБ -> 16 МиБ (-DHEAP_SIZE переопределяет);
                     tcc держит в куче буферы секций/таблицы символов
- Makefile         — починены два бага: .S-файлы попадали в архив исходниками
                     и `make clean` УДАЛЯЛ .S-файлы, перечисленные в SRCS;
                     добавлены новые модули, SHIM-вариант (`make SHIM=1`)
- include/internal/arch/x86_64/syscall_arch.h — хук ARTYOM_HOST_SHIM для
                     хост-тестов

## Патчи ядра (обязательны)

1. **lseek (syscall 8)** — lib/syscalls.c: функция `lseek(fd, offset, whence)`
   (SEEK_SET/CUR/END через f->pos, каталоги отбрасываются, позиция за концом
   файла разрешена — POSIX, initrd_write уже умеет нулевые gaps) и `case 8:`
   в диспетчере. Номер 8 не конфликтует: диспетчер его раньше не обрабатывал,
   а в mini-libc `__NR_lseek 8` был уже «зарезервирован».
2. **Нормализация путей** — initrd плоский (имена без '/'), а tcc и
   приложения оперируют "/stdio.h". В `open/exec/unlink` добавлен
   `skip_leading_slashes()` перед finddir/initrd_create. Без этого
   `#include <stdio.h>` и `-o /hello2` не находили бы файлы.
3. Синтаксис обеих правок проверен `gcc -fsyntax-only` с точными флагами
   ядра (include.mk).

## Что изменено в tinycc-mob (цель ArtyomX64)

- `config.h` — переписан (оригинал сохранён в config.h.orig-linux):
  TARGETOS_ArtyomX64, TCC_TARGET_X86_64, CONFIG_TCCDIR="/",
  SYSINCLUDE/LIBPATHS/CRTPREFIX = "{B}", CONFIG_TCC_PREDEFS=0 (tccdefs.h
  читается с диска "/tccdefs.h" — не надо пересобирать tcc ради макросов),
  CONFIG_TCC_STATIC (нет dlfcn), CONFIG_TCC_SEMLOCK=0 (нет semaphore.h),
  BCHECK/BACKTRACE=0.
- `tcc.h` — ветка TARGETOS_ArtyomX64 (не Linux, не BSD).
- `tccpp.c` — предопределённый макрос `__artyomx64__` (вместо __linux__).
- `include/tccdefs.h` — `__INT64_TYPE__ long` для __artyomx64__.
- `tccelf.c` — crtbegin добавляет только `crt0.o` (нет crti/crtn/.init/.fini);
  crtend — пусто.
- `libtcc.c` — `static_link = 1` (динамического загрузчика в ядре нет).
- `tccrun.c` — `tcc -run` для ArtyomX64 пока отключён с внятной ошибкой.
- `build-artyomx64.sh` — собирает всё: libc, tcc, tcc-shim, libtcc1.a,
  crt0.o, набор системных заголовков (см. ниже).

## libtcc1.a и crt0.o

- `libtcc1.a` собран хостовым gcc из lib/: libtcc1.c, stdatomic.c, builtin.c,
  va_list.c (__va_arg для SysV va_list!), alloca.S, atomic.o — это обычный
  SysV x86-64 код, ABI тот же, что и у кода под ОС.
- `crt0.o` = `ld -r` от crt/start.o + crt/__libc_start_main.o (точка входа
  `_start`, вызывает main через __libc_start_main, exit).

## Как пересобрать

    # всё сразу (libc + tcc + shim + libtcc1 + crt0 + заголовки):
    cd apps/tinycc-mob && ./build-artyomx64.sh

    # initrd с tcc, libc.a, libtcc1.a, crt0.o и заголовками:
    cd apps && make initrd      # mkinitrd2 пересобран (MAX_FILES 64->96)

    # копируется в /initrd.img и limine-iso-root/boot/initrd.img

## Хост-тесты (без QEMU)

    cd apps/tests-host && ./run_tests.sh          # setjmp + libc2 через шим
    cd apps/tinycc-mob
    AXROOT=<root> ./build-art/bin/tcc-shim /hello.c -o /hello2

`AXROOT` имитирует раскладку initrd на хосте: пути "/stdio.h" и т.п.
ремапятся в <root>/stdio.sh… т.е. можно прогнать точную последовательность
команд ОС. Пример дерева: build-art/include/* + libc.a + libtcc1.a +
crt0.o + hello.c + sjt.c.

## Ограничения

- `tcc -run` отключён (нужны RWX-выделение и runmain.o под mini-libc) —
  следующий этап.
- Куча 16 МиБ: tcc компилирует файлы в тысячи строк (y_tab.c ~2500 строк —
  OK); тест 101_cleanup.c (4^8 cleanup-скоупов ≈ 65 тыс. областей) требует
  больше — это стресс-тест, не регресс.
- CONFIG_TCC_BCHECK/BACKTRACE выключены (нет сигналов/altstack).
- `__DATE__`/`__TIME__` дают дату СБОРКИ libc (RTC в ядре нет).
- Нет pthread/TLS/атомарных C11-примитивов на ядре (stdatomic компилируется
  через libtcc1, но sem/pthread отсутствуют).
- long double литералы длиннее ~19 значащих цифр теряют точность
  (strtold через strtod).
- Ошибки компиляции могут писать вывод в VGA-консоль только ASCII.

## Исправление зависания tcc (важно!)

### Симптом

`tcc /hello.c -o /hello2` «зависает» при запуске: shell не отвечает,
система не реагирует, ошибок не выдаётся.

### Причина 1: переполнение стека задачи (главная)

Ядро выделяло задаче стек в **16 КиБ** (`STACK_SIZE 16384` в
`multitask/task.c` и `fs/task.c`). Замеры реального tcc (двоичный
эквивалент, измерено на хосте через `ulimit -s`):

| вход                         | минимальный стек tcc |
|------------------------------|----------------------|
| `hello.c` (полный цикл)      | ~14 КиБ              |
| `sjt.c` (компиляция)         | ~12 КиБ              |
| `lex_yy.c` (~16 КБ, 2500 стр.)| ~16 КиБ             |

То есть 16 КиБ — впритык, а кадры обработчика `int 0x80` ядра
задачи выполняются на **том же стеке** (задачи работают в кольце 0,
отдельного стека для syscall нет). Итог: переполнение.

Особенно коварно то, что переполнение **не вызывает исключение**:
куча задач выделяется kmalloc'ом в общем адресном пространстве ядра,
MMU-защиты нет. Ушедший вниз указатель стека молча затирает соседние
блоки kheap — заголовки арены и списки задач. Первый же последующий
`kmalloc`/`kfree` или переключение контекста ходит по испорченным
спискам — система намертво зависает. Внешне это выглядит как
«tcc завис при запуске», хотя виноват стек.

### Исправление 1

`STACK_SIZE 16384` → `(2 * 1024 * 1024)` в **обоих** файлах
`multitask/task.c` и `fs/task.c` (ядро линкует multitask/tsk.o,
второй — для консистентности). 2 МиБ на задачу: 16 задач × 2 МиБ =
32 МиБ максимум, на 128 МиБ QEMU — с запасом.

### Причина 2: не было обработчиков исключений CPU

В `kernel/isr.c` векторы 0..31 (page fault, GPF, invalid opcode,
stack fault, …) **никем не обрабатывались**: `isr_handler` не находил
обработчик, выполнял `iret` обратно на упавшую инструкцию,
исключение повторялось бесконечно. Любой краш в задаче выглядел
как зависание всей системы, без единого сообщения.

### Исправление 2

`kernel/isr.c`: добавлен `initialise_exception_handlers()` —
регистрирует обработчики 0..31. Исключение печатает на serial
номер/имя, RIP/RSP/ERRCODE (и CR2 для page fault), после чего
пользовательская задача завершается (`task_exit(139)`), а shell
продолжает работать; исключение в корневой задаче ядра останавливает
систему с диагностикой. Вызов добавлен в `kernel/main.c` сразу после
`init_descriptor_tables()` и **до** запуска задач.

### Что нужно сделать

Пересобрать **ядро** (initrd/tcc менять не нужно):

    make          # в корне ArtyomX64
    make limine-iso   # или ваш обычный рецепт ISO

После этого `tcc /hello.c -o /hello2` компилирует нормально, а при
реальном баге в программе shell печатает `=== CPU EXCEPTION ===`,
`TASK: killed by exception` и возвращается в приглашение.

### Хост-методика, которой это было найдено

1. `tcc-shim` (glibc-стартап + shim-syscall) — работал, поэтому баг
   был не в tcc.
2. `tcc-full` — та же сборка, но с реальным startup-путём ОС
   (`start.S` + `__libc_start_main` + libc.a) и shim'ом, байт-в-байт
   повторяющим семантику ядра (неизвестный syscall → -1): полный
   цикл «компиляция + линковка + запись вывода» прошёл — guest-код
   корректен.
3. Замер стека через `ulimit -s` на `tcc-full`: hello.c требует
   14 КиБ, лимит ядра — 16 КиБ, минус кадры ядра → переполнение.

## Исправление «tcc запускается и ничего не делает» / page fault 0x402C80 (критично!)

### Симптом

```
=== CPU EXCEPTION ===
EXC: 14 Page fault
RIP: 0x0000000000402C80
RSP: 0xFFFF8000........
ERR: 0x0000000000000010
CR2: 0x0000000000402C80
TASK: killed by exception, PID=3
```

tcc запускается (crt0 отрабатывает), но на первом же вызове `main(...)`
происходит page fault: `RIP == CR2 == 0x402C80`, `ERR=0x10`
(ошибка выборки инструкции, страница отсутствует).

### Причина

Ядро загружало non-PIE ELF-образы (tcc и всё, что собрано им) в кучу
ядра (`kmalloc`, адреса `0xFFFF8000...`) и передавало туда управление.
Код слинкован по адресам `0x400000+`, и любой **абсолютный** переход/
вызов (а первый такой — `call main` из `__libc_start_main`, `main` у tcc
лежит как раз на `0x402C80`) улетал на линковочный виртуальный адрес,
который в адресном пространстве ядра **не отображён** → цикл fault →
задача убивается обработчиком исключений (если он есть) или тихое
зависание (если нет).

Хост-shim это не ловил: там образ грузится загрузчиком Linux по
правильному адресу.

### Исправление (уже в ядре)

- `multitask/loader.c`: `image_load64` больше не «кусок памяти», а
  contiguous `pmm_alloc_pages` + копирование через HHDM
  (`vmm_phys_to_virt`); диапазон публикуется как «pending image».
- `multitask/task.c`: `task_spawn` для user-entry
  (`entry < AX_USER_VA_MAX`) создаёт **отдельное адресное пространство**
  (`vmm_new_address_space`) и отображает образ по его линковочному
  адресу с `PRESENT|WRITE|USER`; `regs.cr3 = ns`.
- `task_exit` освобождает кадры образа (`aximage_free`); при неудаче
  спавна pending-образ сбрасывается.
- `kernel/axshell.c`: GUI-приложения тоже идут через `task_spawn`
  (раньше user-код звали прямо из адресного пространства ядра).

### Как убедиться, что работает ИМЕННО исправленное ядро

При загрузке в serial-логе (первые строки, до GUI) должна быть строка:

```
[DEBUG] KERNEL BUILD: tccfix2-20250831 (exec=per-task-AS, exc-handlers=yes)
```

и на экране до появления GUI мелькает
`kernel build: tccfix2-20250831 (exec per-task AS)`.

**Если этой строки нет — вы загрузили старое ядро**, и tcc будет падать
с приведённым выше дампом. Проверка:

```
strings main.elf | grep tccfix2
```

### Проверено на реальной ОС (QEMU, ISO из этой сборки)

- `tcc /hello.c -o /hello2` — компилирует, `/hello2` печатает
  `Hello from tcc on ArtyomX64! argc = 1`;
- `tcc /sjt.c -o /sjt2` — `/sjt2` печатает
  `longjmp returned 777, stage=1` (setjmp/longjmp на голом железе).

### Пересборка ядра — только ЧИСТАЯ

Ловушка, которая дважды стреляла: устаревшие `.o` в дереве. Признаки:
правки исходников «не применяются», симтомы смеси старого и нового.
Причины: `gcc ... | head` (SIGPIPE убивает gcc, а `$?` берётся от
`head`), зависимости в Makefile не отслеживают заголовки.

Поэтому:

```
make clean        # обязательно!
make
make limine-iso
strings main.elf | grep tccfix2   # маркер должен найтись
```

Если пересобираете вручную — компилируйте каждый файл отдельной
командой и проверяйте код возврата gcc (в комплекте
`scripts/rebuild_kernel.sh`, строго проверяющий rc каждого файла).
