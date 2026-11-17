# HoverTank3DClone → ArtyomX64: порт игры + userspace-загрузчик изображений

Порт игры **HoverTank3DClone** (SFML/C++) на **ArtyomX64** и **полноценный
загрузчик изображений для user space** — `apps/imgload`. Ядро НЕ тронуто:
ни одного патча kernel не требуется, всё работает через существующие
сисколлы (`int 0x80`: open/read/close + AX surface API).

```
 HoverTank3DClone (SFML)              ArtyomX64 (этот порт)
 ───────────────────────              ─────────────────────
 sf::RenderWindow 800x600      →      ax_surface() 640x400 (адаптивно)
 sf::Texture / loadFromFile    →      imgload (PNG/BMP, user space)
 createMaskFromColor(Black)    →      альфа запечена в PNG (hov_assets.py)
 sf::Font TTF                  →      системный шрифт 8x8 (ax_text)
 sf::Clock                     →      TSC, калиброванный по RTC (ax_time)
 std::vector / классы          →      статические буферы, чистый C
```

## Состав поставки

| Файл | Что это |
|------|---------|
| `apps/imgload/` | **userspace-загрузчик изображений** (5 файлов, ~1300 строк) |
| `apps/hovertank.c` | игра — полный порт `main.cpp` на C без зависимостей |
| `apps/sprites/` | спрайты, сжатые под окно ОС (см. «Ассеты») |
| `apps/Makefile` | ваш Makefile + цели `hovertank.bin`, imgload, initrd-записи |
| `apps/tests-host/` | хост-шим `ax_*` — игра и imgload тестируются на Linux |
| `tools/hov_assets.py` | конвейер ассетов: оригинальные PNG/JPEG → компактные PNG |
| `build/hovertank.bin` | уже собранный бинарник игры (ELF PIE, 46 КБ) |
| `build/initrd.img` | готовый initrd с игрой и спрайтами (59 файлов, 5.3 МБ) |

## Быстрый старт

Вариант 1 — взять готовое: скопировать `build/initrd.img` в корень ОС
(и/или в `limine-iso-root/boot/initrd.img`), пересобрать ISO как обычно:

```bash
cp build/initrd.img /путь/к/artyomx64/initrd.img
cd /путь/к/artyomx64 && bash build.sh        # ваш GRUB-вариант
# или ваш limine-сборочный скрипт
```

Вариант 2 — интеграция исходников в ваше дерево:

```bash
# 1. файлы
cp -r apps/imgload apps/sprites  apps/            # новые каталоги
cp    apps/hovertank.c apps/Makefile  apps/       # игра + Makefile
# (apps/tests-host и tools — опционально, для тестов на хосте)

# 2. сборка и initrd
cd apps
make                # соберёт все приложения, включая hovertank.bin
make initrd         # пересоберёт initrd.img (там уже есть hovertank + *.png)
cp initrd.img ../initrd.img
cp initrd.img ../limine-iso-root/boot/initrd.img

# 3. ISO
cd .. && bash build.sh
```

В QEMU: `qemu-system-x86_64 -cdrom artyomx.iso -serial stdio -m 512`.
Игра запускается из Files (файл `hovertank`) или из терминала.

## Управление

| Клавиша | Действие |
|---------|----------|
| `W` / `S` | вперёд / назад |
| `A` / `D` | поворот влево / вправо |
| `Q` / `E` | стрейф влево / вправо |
| `ESC` | закрыть окно |
| меню **Game → New Game** | заново |
| меню **Game → Close Window** | закрыть окно |

Цель: собрать всех 3 «chan» до конца «Dream time» (30 сек, каждый chan +5 сек).
Солдаты патрулируют — касание = game over. Собрал всех — победа
(«Return to the real world!»).

Почему не стрелки: в `drivers/keyboard_map.h` сканкоды стрелок отображаются
в 0, и драйвер не шлёт событий отпускания/автоповтора — поэтому движение
пошаговое, по нажатию (как в вашем raycaster.c).

## Userspace-загрузчик изображений (apps/imgload)

Никакого malloc: вся черновая память — статические буферы
(`IMG_FILE_MAX` 1 МиБ под сжатый файл + `IMG_RAW_MAX` 4 МиБ под
распакованные сканлайны + ~1 КиБ на CRC/окно inflate).

Поддерживаемые форматы:

* **PNG** (RFC 2083): глубины 1/2/4/8/16 бит; цветовые типы
  0 (gray), 2 (RGB), 3 (палитра), 4 (gray+A), 6 (RGBA); tRNS
  (палитровая альфа + цветовой ключ); 16-бит сэмплы срезаются до
  старшего байта. Interlace Adam7 → `IMG_ERR_UNSUPPORTED`.
* **BMP**: BITMAPCOREHEADER и BITMAPINFOHEADER/V4/V5; без сжатия
  (BI_RGB): 1/4/8 бит (палитра) и 24/32 бита; bottom-up и top-down
  (отрицательная высота). RLE/BI_BITFIELDS → `IMG_ERR_UNSUPPORTED`.

Собственный декомпрессор DEFLATE (RFC 1951) + zlib-обёртка (RFC 1950)
с проверкой Adler-32; CRC32 каждого чанка PNG проверяется.

API (`imgload.h`):

```c
typedef struct { int w, h; uint32_t *pix; } img_image_t;

/* декодировать файл в буфер вызывающего (ARGB8888) */
int img_load_file(const char *path, uint32_t *dst, int dst_max_px,
                  img_image_t *out);

/* то же из памяти; только размеры; текст ошибки */
int img_load_memory(const uint8_t *data, int len, ...);
int img_query_file(const char *path, int *w, int *h);
const char *img_strerror(int err);
```

Коды ошибок: `IMG_OK` / `IMG_ERR_OPEN` / `IMG_ERR_READ` /
`IMG_ERR_TOO_BIG` / `IMG_ERR_FORMAT` / `IMG_ERR_UNSUPPORTED` /
`IMG_ERR_CORRUPT` / `IMG_ERR_NODATA`.

Линкуется к любому приложению: `ld ... app.o imgload.o inflate.o
img_png.o img_bmp.o libc.o`. Зависимостей от libc нет — свои
`img_memcpy/img_memset/...` (не конфликтуют с libc.o).

Корректность подтверждена на хосте побитовым сравнением с PIL
(`tests-host/test_imgload.c`): RGB/RGBA/палитра/gray/LA/16-бит PNG и
24/32/8-бит + top-down BMP — 0 расхождений; Adam7 корректно отклоняется.

## Игра (apps/hovertank.c)

* Карта 9x10 и коды стен (4 бита, HOV-палитра) — 1:1 с оригиналом.
* Рейкаст: 200 лучей, шаг 0.02, коррекция «рыбьего глаза»
  (`cos(angleDifference)`), затухание яркости `0.25 + 0.75*(1-d/15)`.
* Спрайты: солдат (8 кадров спереди + 8 сзади, выбор вида по позиции
  игрока), chan 15 кадров; патруль, z-буфер по центральному лучу
  (как в оригинале), сортировка дальние→ближние; альфа-блендинг
  вместо жёсткого цветового ключа (края спрайтов мягче).
* Тайминг: `rdtsc()` калибруется по RTC через `ax_time()` при старте
  (2 секунды, чисто в user space, без новых сисколлов). Если TSC
  недоступен — резервный режим с фиксированным dt. Игровой цикл ~20 FPS,
  `ax_commit()` только ставит dirty-флаг — композитор ядра сам подхватывает
  кадр, так что непрерывный рендер анимаций ничего не фликает.
* Окно: `ax_screen()` → 640x400, сжатие под маленькие экраны (запас
  130px покрывает menubar 24 + dock 56 + заголовок 28).
* Если какой-то спрайт не загрузился — игра продолжает работать на
  плоских цветных болванках (проверено на хосте).

Память: .bss ≈ 6.5 МиБ (буферы спрайтов 384+240+640 КиБ + 5 МиБ черновой
памяти imgload) — стек задачи 2 МиБ не пересекается, ELF-загрузчик ОС
обнуляет .bss, размер файла всего 46 КиБ.

## Ассеты

Оригиналы слишком велики для декода в ОС (chan.png 2490x1992 RGBA
≈ 20 МиБ после распаковки), поэтому `tools/hov_assets.py` (PIL) делает
компактные версии — уже в поставке (`apps/sprites/`):

| Ассет | Было | Стало | Что сделано |
|-------|------|-------|-------------|
| soldier.png | 1536x1024 RGB, 1.27 МБ | 512x192 RGBA, 169 КБ | вырезаны 16 чистых кадров 64x96 из дизайн-листа (без надписей и артов), чёрный ключ → альфа, залиты «дырки» |
| chan.png | 2490x1992 RGBA, 2.65 МБ | 320x192 RGBA, 73 КБ | только 3 используемых ряда, кадры 64x64 |
| game_over.jpeg | 1000x1000 progressive JPEG | 512x320 RGB PNG, 87 КБ | JPEG в ОС не декодируется (progressive) — конвертирован в PNG |

Перегенерировать: `python3 tools/hov_assets.py <каталог оригинальных
спрайтов> apps/sprites`. Файлы кладутся в initrd плоскими:
`soldier.png`, `chan.png`, `gameover.png` (записи уже добавлены
в цель `initrd` Makefile).

## Тесты на хосте (Linux)

Тот же код собирается на Linux с `-DARTYOM_HOST_SHIM`: imgload читает
файлы через stdio, а `tests-host/shim_ax.[ch]` эмулирует `ax_*`:
канвас в malloc, скриптовые события (`HOV_SCRIPT`), фейковые RTC
(1/20 сек за кадр) и дампы кадров в PPM.

```bash
cd apps
# юнит-тест imgload (побитовое сравнение с эталонами PIL)
gcc -O2 -DARTYOM_HOST_SHIM -I. -o /tmp/test_imgload \
    tests-host/test_imgload.c imgload/*.c
# сама игра
gcc -O2 -DARTYOM_HOST_SHIM -I. -I../lib -o /tmp/hov_host \
    hovertank.c imgload/*.c tests-host/shim_ax.c
HOV_SCRIPT=сценарий.txt /tmp/hov_host   # key w / menu 0 0 / dump tag / quit
```

Прогнанные сценарии: стартовый кадр, подход к солдату (перед/спина,
масштабирование), столкновение → game over, Game→New Game,
таймер-gameover на 600-м кадре, подбор chan (+5 сек на HUD).

## Скриншоты (host-тесты, docs/frames/)

| Файл | Кадр |
|------|------|
| `corridor_soldier.png` | коридор, солдат (спрайт из PNG, альфа) |
| `wall_closeup.png` | приближение, масштабирование спрайта |
| `chan_sprite.png` | chan-спрайт среди цветных HOV-стен |
| `chan_picked_plus5.png` | после подбора: Dream time +5 (27→31) |
| `game_over.png` | экран Game Over (gameover.png + надпись) |

## Замечания

* `exec()` в вашей `fs/exec.c` уже читает ELF через `malloc(sz)` —
  46 КиБ бинарника проходят без патчей.
* Спрайты в initrd занимают ~330 КиБ; initrd вырос с 4.9 до 5.3 МиБ.
* Если захотите поднять лимиты imgload — переопределите
  `IMG_FILE_MAX`/`IMG_RAW_MAX` в CFLAGS (`-DIMG_RAW_MAX=...`).
* Шрифт ОС — ASCII 8x8, поэтому HUD на английском (как в оригинале).
