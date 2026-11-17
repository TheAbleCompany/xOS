# xHCI (USB 3) + USB-мышь

## Что было не так
- `drivers/usb/*` не линковался в `main.elf` (`#$(MAKE) -C drivers/usb`), а `usb_init()`
  нигде не вызывался -- USB в ядре фактически не существовало, мышь работала только по PS/2.
- Старые `drivers/usb/xhci.c` и `drivers/usb/xhci/*` в сборку не входят и не трогались.

## Что сделано
- `drivers/xhci.c`, `drivers/xhci.h` -- новый драйвер xHCI + HID-мышь (полностью опросный,
  без прерываний контроллера; ядро работает на legacy PIC без LAPIC/MSI).
- Подключение: `Makefile` (drivers/xhci.o), `drivers/Makefile`, `drivers/timer.c`
  (`xhci_timer_poll()` из IRQ0, 1 кГц), `kernel/main.c` (`xhci_init()` после `sti`, до `initTasking()`;
  `xhci_deferred()` в idle-цикле root-задачи), `drivers/mouse.c` (`handle_mouse()` вызывает
  `xhci_deferred()`; `mouse_dx/dy` забираются атомарно -- раньше движение между чтением и
  обнулением терялось).

## Возможности
- PCI-поиск (class 0C/03/30), bus master, размер BAR, **собственное UC-отображение MMIO**
  (64-битные BAR выше 4 ГиБ не покрыты HHDM Limine).
- BIOS->OS handoff, отключение SMI, Intel Panther/Lynx Point (EHCI->xHCI routing).
- HCRST, DCBAA, scratchpad, command/event ring, контексты 32/64 байта.
- Root-порты USB2 (port reset) и USB3 (link training), hot-plug/unplug, до 3 попыток.
- Хабы (USB2/USB3), route string, TT-информация для LS/FS за HS-хабом.
- Boot-мышь (SET_PROTOCOL/SET_IDLE) и **любой HID-указатель** по report descriptor
  (относительный и абсолютный: планшеты, тачпады, мыши без boot-протокола).
- Восстановление halted endpoint (Reset Endpoint + Set TR Dequeue).

## Ограничения
- USB-клавиатуры не обрабатываются (в системе клавиатура -- PS/2).
- Только control + interrupt IN; без isochronous/bulk/streams. Мыши за SS-хабом не проверялись в QEMU.
- Контроллеры, требующие загрузки прошивки (Renesas uPD720201 и т.п.), не поддерживаются.
- Подробный лог: serial COM1 (строки `XHCI: ...`); `xh_trace = 1` включает трассировку событий.

## Проверено (QEMU 8.2, PS/2 отключён через `-M q35,i8042=off`)
qemu-xhci и nec-usb-xhci (q35, i440fx); BIOS и UEFI; usb-mouse, usb-tablet,
мышь за usb-hub, hot-plug/unplug, клики, регресс PS/2 без xHCI.
