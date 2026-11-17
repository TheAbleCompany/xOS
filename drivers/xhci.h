/*
 * drivers/xhci.h -- xHCI (USB 3.x) host controller + USB HID (мышь, клавиатура).
 *
 * Драйвер полностью опросный (polling): прерывания контроллера не
 * используются (ядро работает на legacy PIC без LAPIC/MSI).  Event ring
 * разбирается из IRQ0 (PIT, 1 кГц) -- xhci_timer_poll(); тяжёлые операции
 * (перечисление устройств, hot-plug, восстановление endpoint'ов)
 * выполняются в контексте процесса -- xhci_deferred().
 */
#ifndef XHCI_H
#define XHCI_H

/* Найти xHCI на PCI, перезапустить, перечислить устройства.
 * Вызывать ПОСЛЕ sti (нужен PIT) и ДО initTasking(). Возвращает число
 * найденных USB-HID устройств (мыши + клавиатуры; 0 -- нет контроллера
 * или HID). */
int  xhci_init(void);

/* Вызывается из обработчика IRQ0 каждую миллисекунду. Дёшево. */
void xhci_timer_poll(void);

/* Вызывать из обычного контекста (root-задача, handle_mouse):
 * hot-plug, повторная инициализация и т.п. */
void xhci_deferred(void);

int  xhci_present(void);
int  xhci_mouse_count(void);
int  xhci_kbd_count(void);

#endif
