# Порт run.c (llama2.c) на ArtyomX64 — адаптация mini-libc

## Итог

`run.c` (миниатюрный инференс LLM Llama-2 на чистом C, ~1000 строк)
**компилируется и работает** с mini-libc:

- хост-валидация (gcc + libc-shim): компиляция без ошибок, полный цикл
  генерации токенов, `printf("%f")`, top-p семплирование (qsort),
  кодирование промпта (bsearch), chat-режим — всё работает;
- tcc-валидация: `tcc-shim` (точный аналог `tcc` на ОС) компилирует
  и линкует `run.c` без единой ошибки; машинный код, сгенерированный
  tcc, проверен на хосте с реальным startup-путём ОС
  (`_start -> __libc_start_main -> main`) — работает;
- регрессии mini-libc (`tests-host/run_tests.sh`) — все тесты зелёные;
- реальная `libc.a` (int 0x80, цель ОС) собирается со всеми новыми
  символами: `sqrtf/expf/powf`, `clock_gettime`, `mmap/munmap`,
  `bsearch`.

## Что добавлено/исправлено в mini-libc

### 1. io/printf.c — КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: зависание printf("%f")

`decimal_exponent(0.0)` уходил в бесконечный цикл:

    while (x < 1.0) { x *= 10.0; E--; }   /* 0.0 * 10 == 0.0 навсегда! */

Любой `printf("%f", 0.0)` (и `%e`/`%g` с нулём) зависал. В run.c это
строка `achieved tok/s: %f`. Исправление: ранний `return 0` при
`x == 0.0` + предохранитель `E < -400` для субнормалей.

### 2. math/math.c + include/math.h — float-варианты (C99)

Добавлены `sqrtf, expf, powf, logf, log2f, log10f, fabsf, floorf,
ceilf, roundf, truncf, fmodf, modff, frexpf, ldexpf, sinf, cosf, tanf,
asinf, acosf, atanf, atan2f, sinhf, coshf, tanhf` — обёртки над
double-версиями. run.c использует `sqrtf` (rmsnorm), `expf` (softmax),
`powf`.

### 3. time/time.c + include/time.h — clock_gettime

run.c меряет скорость генерации через `clock_gettime(CLOCK_REALTIME)`.
Добавлены: `clockid_t`, константы `CLOCK_*` (номера Linux — для шима и
будущего ядра), `clock_gettime`, `clock_getres`.

Цепочка источников времени (от лучшего к запасному):
1. syscall `clock_gettime` (228) — работает на хост-шиме, заработает в ядре;
2. syscall `gettimeofday` (96) — хост-шим;
3. syscall AX time (163) — CMOS RTC ядра ArtyomX64 (гражданский
   календарь -> epoch через days_from_civil);
4. `TIME_EPOCH` — метка сборки libc.

`time()` и `gettimeofday()` переведены на ту же цепочку: на реальной
ОС `time(NULL)` теперь даёт настоящее время RTC (для default-seed
сэмплера), а не константу сборки.

### 4. mm/mmap.c — рабочие mmap/munmap (эмуляция)

run.c грузит чекпоинт модели файловым mmap (PROT_READ|MAP_PRIVATE),
но ядро ArtyomX64 mmap НЕ реализует. Реализовано в libc:

- сначала настоящий syscall mmap(9) — заработает автоматически,
  когда ядро его добавит (и уже доступен любым хост-шимам с пробросом);
- при отказе — эмуляция:
  * anonymous (MAP_ANONYMOUS / fd < 0) -> `calloc` из кучи;
  * file-backed -> `malloc` + чтение `read()` с сохранением/восстановлением
    файловой позиции fd (POSIX: mmap не двигает offset);
- эмулированные отображения регистрируются в статической таблице
  (128 слотов, без кучи), `munmap()` на зарегистрированном адресе
  делает `free()`;
- **ВАЖНО (исправлена мина):** старый `munmap()` падал бы в syscall 11,
  а номер 11 в ArtyomX64 занят `exec()`! Fallback убран, неизвестные
  отображения возвращают EINVAL. Ошибка нумерации `__NR_munmap 11`
  задокументирована в syscall_list.h — когда ядро получит mmap,
  номера нужно развести.

Ограничение эмуляции: модель целиком лежит в куче приложения
(HEAP_SIZE = 16 МиБ по умолчанию). Модель `tiny-llm.bin` (~160 КиБ)
и `stories260K` (~1 МиБ) помещаются; stories15M (60 МБ) — нет
(можно поднять: `-DHEAP_SIZE=(80*1024*1024)`, но нужен запас RAM).

### 5. stdlib/bsearch.c (новый файл) + stdlib.h

`bsearch()` для `Tokenizer::str_lookup()` (бинарный поиск по словарю).
Классический вариант на делении пополам.

### 6. include/unistd.h — тянет stdint.h

Как в glibc: типы фиксированной ширины (`int8_t` и др.) приходят в
пользовательский код через `<unistd.h>`. У run.c `int8_t` в сигнатуре
`encode(...)` именно так.

### 7. include/sys/mman.h — чистка

Убраны дубли определений MAP_SHARED/MAP_PRIVATE/MAP_ANONYMOUS/MAP_FAILED
(устраняли предупреждения «redefined»), добавлен прототип mprotect.

### 8. tests-host/syscall_shim.c — проброс тайм-вызовов

Номера совпадают с Linux, поэтому добавлен прямой проброс:
`35` (nanosleep), `96` (gettimeofday), `228` (clock_gettime).
mmap(9) в шим НЕ пробрасывается сознательно — эмуляция в libc даёт
поведение, идентичное реальной ОС (у которой mmap нет), включая
цикл mmap/munmap.

## Хост-валидация (методика)

    cd apps/tests-host
    make -C ../mini-libc SHIM=1
    gcc -nostdinc -I../mini-libc/include -fno-stack-protector -fno-builtin \
        -fPIE -O2 -DARTYOM_HOST_SHIM=1 -Wno-unused-result \
        -c ../coreutils/run.c -o bin/run.o
    gcc -c syscall_shim.c -o bin/syscall_shim.o
    gcc -no-pie bin/run.o bin/syscall_shim.o ../mini-libc/libc-shim.a -o bin/run

Тестовая модель генерируется `make_tiny_llm.py` (формат чекпоинта и
tokenizer.bin — точно как читает run.c). Модель спроектирована так,
что жадная генерация детерминированно циклит по printable-ASCII:

    $ ./bin/run bin/tiny-llm.bin -z bin/tokenizer.bin -t 0 -n 70
    "#$%&'()*+,-./0123456789:;<=>? !"#$%&'()*+,-./0123456789:;<=>? !"#$%&'
    achieved tok/s: 17250.000000

Это числовая валидация всего forward-прохода: embedding -> rmsnorm
(sqrtf) -> attention (softmax/expf) -> FFN (silu) -> финальный rmsnorm
-> классификатор -> сэмплирование -> декодер. Любая поломка чисел
ломает цикл. Проверены также: `-t 1.0 -p 0.9` (top-p: qsort),
`-i "hello"` (bsearch), `-m chat` (fgets/sprintf), EOF-поток stdin.

### tcc-валидация (как на ОС)

    cd apps/tinycc-mob && ./build-artyomx64.sh
    # AXROOT имитирует раскладку initrd: заголовки + libc.a + crt0.o
    AXROOT=<root> ./build-art/bin/tcc-shim /run.c -o /run_tcc -I/
    # подстройка: crt0.o/libc.a/libtcc1.a tcc ищет рядом с собой ({B})

и проверка кода tcc на хосте (схема «tcc-full» из README-TCC-PORT):
объект `run_tcc.o` линкуется с `crt/start.S`-цепочкой (`host_start.S`
— хост-вариант точки входа) и libc-shim.a — сгенерированный tcc код
работает идентично gcc-сборке.

## Как запустить на ОС

1. Пересобрать libc и tcc: `cd apps/tinycc-mob && ./build-artyomx64.sh`
   (пересобирает mini-libc + tcc + системные заголовки build-art/include).
2. Положить в initrd (apps/Makefile, цель initrd, в список mkinitrd2):

       mini-libc/libc.a libc.a none        (уже есть)
       tiny-llm.bin tiny-llm.bin none      ; модель
       tokenizer.bin tokenizer.bin none    ; токенизатор
       run.c run.c none                    ; исходник

   mkinitrd2 скопирует заголовки из tinycc-mob/build-art/include
   (обновлённые math.h/time.h/unistd.h/stdlib.h/sys/mman.h попадут
   в initrd автоматически).
3. Пересобрать образ: `cd apps && make initrd`, затем ядро и ISO
   по обычной схеме (см. README-TCC-PORT про чистую пересборку ядра).
4. В терминале ОС:

       exec tcc /run.c -o /run
       exec run /tiny-llm.bin -z /tokenizer.bin -t 0 -n 40

## Ограничения

- Размер модели ограничен кучей 16 МиБ (см. выше).
- CLOCK_MONOTONIC разделяет источник с REALTIME (в ядре нет
  uptime-счётчика; когда появится — clock_gettime уже готов).
- RTC ядра даёт секунды: tok/s на коротких прогонах на ОС будет
  грубым (на хост-шиме — точные миллисекунды).
- В run.c не изменено ни одной строки — вся адаптация в libc.
