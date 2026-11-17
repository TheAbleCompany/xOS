# Порт lua-1.0 на ArtyomX64 — что сделано

## Итог

lua-1.0 собирается и работает на ArtyomX64. Бинарник: `apps/lua-1.0-master/lua`
(статический ELF, -no-pie, только syscalls int 0x80 твоего ядра).
На хост-линукс тот же код (с шимом syscall) проходит тесты:
libc-тест — 71/72 строк побитово совпадают с glibc; все lua-скрипты
(test_simple, comprehensive, lib/save.lua, lib/globals.lua, formatted write)
выполняются корректно.

Запуск в ОС: положи `lua` в initrd (уже добавлен в `apps/Makefile`,
цель `initrd`: строка `lua lua console`), затем в терминале:

    exec lua /test_simple.lua

(пути — по структуре твоего initrd; имя файла — аргумент argv[1],
lua.c его читает через exec_args).

## Что добавлено/исправлено в mini-libc (всё по твоему API int 0x80)

Номера syscall исправлены по диспетчеру ядра (lib/syscalls.c):
close = 6 (было 3 — конфликтовало с read!), unlink = 10, exec = 11.

Новые файлы:
- io/std.c        — stdin/stdout/stderr (буферы в .bss, stdout/stderr
                    небуферизованные — промпт "> " виден до ввода)
- io/ungetc.c     — ungetc (нужен лексеру и io_read lua)
- io/fileops.c    — fwrite/fread/fgets/getchar/getc
- io/scanf.c      — vfscanf/fscanf/scanf/vsscanf/sscanf: %d %i %u %o %x,
                    %ld, %f (float*), %lf, %s с шириной, %c, %[скансеты]
                    с ^ и диапазонами, подавление *
- io/unlink.c     — unlink (syscall 10) + remove + rename-заглушка
- io/system.c     — system() через exec(11) (fork/wait в ядре нет,
                    lua результат system() всё равно игнорирует)
- stdlib/strtod.c — strtod/atof/strtol/strtoul/atoi/atol/abs/labs
- ctype/ctype.c   — isspace/isdigit/isalpha/.../toupper/tolower
- math/math.c     — sin cos tan asin acos atan atan2 sinh cosh tanh,
                    ceil floor round trunc fabs fmod modf frexp ldexp,
                    sqrt (sqrtsd), fmod (fprem), exp, log, log10, log2, pow
- string/strdup.c — strdup
- include/ctype.h, include/math.h

Исправлено в существующем:
- mm/malloc.c     — ПОЛНОСТЬЮ переписан: раньше вызывал mmap (syscall 9),
                    которого в ядре нет → malloc всегда падал. Теперь
                    статическая куча 1 МиБ в .bss + first-fit со списком
                    свободных блоков, слиянием, realloc/calloc.
- io/printf.c     — ПОЛНОСТЬЮ переписан: настоящий vsnprintf-движок
                    (флаги -0+ #, ширина, точность, l/ll/z, %d %i %u %o
                    %x %X %c %s %p %f %e %g %G %%). %g с округлением
                    round-half-even и срезанием нулей, точные десятичные
                    цифры для N/10^k (нет дрейфа на 7-й цифре).
                    printf/fprintf/sprintf/snprintf + v-варианты.
- io/fopen.c      — fopen теперь выделяет буфер (fgetc раньше читал в
                    NULL!), добавлен fdopen, режимы r/w/a/+ и b.
- io/fflush.c     — не считает ошибкой сброс читающего/небуферизованного
                    потока.
- io/fputc.c      — поддержка _IO_UNBUFFERED (иначе stdout не печатал).
- string/string.c — memmove (был TODO-стаб с копией вперёд — ломал
                    перекрывающиеся области и писал за границу буфера!),
                    memcmp (сравнивал до NUL, а не num байт),
                    strchr/strrchr (strchr(s,0) обязан вернуть указатель
                    на NUL — lua buildformat() так добавляет в конец
                    строки).
- include/*.h     — stdio.h (va_list, новые прототипы), stdlib.h,
                    unistd.h (unlink), string.h (strdup), новый ctype.h,
                    math.h.

## Что изменено в lua-1.0-master

- Makefile: добавлен mathlib.o (раньше mathlib_open не линковался),
  убран -lm (математика теперь в mini-libc), -Os вместо -O0 -g
  (компактнее: суммарно загружаемые сегменты ~90 КБ).
- strlib.c: раскомментирован `#include <ctype.h>` (tolower/toupper).
- y_tab.c: code_number() — целые > 65535 больше не усекаются до
  unsigned short (print(1000000) печатал 16960); теперь уходят в
  PUSHFLOAT.

## Патч ядра (обязателен для загрузки lua!)

Без него lua не запустится: exec() в lib/syscalls.c читал ELF в
статический буфер 64 КБ, а у lua текст+rodata+data ≈ 90 КБ (даже с -Os).
Изменение в lib/syscalls.c, функция exec():

БЫЛО:
    static char buf[65536];
    memset(buf, 0, sizeof(buf));
    u32int sz = read_fs(fsnode, 0, sizeof(buf), (u8int *)buf);
    ...
    if (sz == 0) { exec_free_args(args); return -1; }

СТАЛО:
    u32int sz = fsnode->length;
    char *buf = (char *)malloc(sz ? sz : 1);
    if (!buf) { exec_free_args(args); return -1; }
    memset(buf, 0, sz);
    sz = read_fs(fsnode, 0, sz, (u8int *)buf);
    ...
    if (sz == 0) { free(buf); exec_free_args(args); return -1; }

и после image_load():
    free(buf);          /* образ уже скопирован в память задачи */

(malloc/free в этом файле — это kmalloc/kfree, макросы уже есть.)
Патч уже применён в приложенном lib/syscalls.c. Синтаксис проверен
gcc -fsyntax-only с флагами ядра.

## Известные ограничения

- %.2f для значений в ~1-2 ulp от границы x.xx5 может округляться иначе,
  чем glibc (2.345 → "2.34" вместо "2.35"): точное знание 18-й десятичной
  цифры требует длинной арифметики. На %g (которым пользуется lua) не
  влияет.
- sin/cos при |x| > ~1e8 теряют точность (редукция по 2π в double);
  sin(1e10) верен до ~4 знака.
- lseek/mmap/brk/fork/wait в ядре отсутствуют — fseek в lua 1.0 не
  используется, system() не ждёт завершения.
- lua 1.0 не понимает UTF-8 в исходниках (лексер ASCII) и CRLF/^Z в
  файлах тестов; `==` в lua 1.0 — это `=` (ср. lib/*.lua).
- Куча lua — 1 МиБ в .bss; стек задачи — 16 КБ (ядро), глубокая рекурсия
  парсера может упереться.

## Как пересобрать

    cd apps/mini-libc && make clean && make
    cd ../lua-1.0-master && make clean && make
    cd .. && make initrd     # соберёт бинарники и initrd.img с lua

Примечание: crt/start.asm собирается nasm'ом (в Makefile mini-libc);
crt/start.o в архиве уже собран.
