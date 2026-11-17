// usb_core.c — перечисление USB-устройств поверх UHCI или EHCI.
//
// Вызовите usb_init() один раз из main(). Драйвер сам попробует найти
// сначала UHCI (проще, но встречается только на старом железе/в QEMU
// с явно добавленным piix3-usb-uhci), затем EHCI (High-Speed USB 2.0,
// есть почти на всех машинах 2005-2012 годов и позже как минимум для
// legacy-совместимости). Для чисто xHCI-only машин (большинство железа
// после ~2013 года) ни один из них не найдётся — там нужен отдельный
// драйвер xHCI.

#include "usb.h"
#include "uhci.h"
#include "ehci.h"
#include "../usb_hid_mouse.h"

typedef enum { USB_HC_NONE, USB_HC_UHCI, USB_HC_EHCI } usb_hc_kind_t;

static void dbg_putc(char c) { outb(0x3F8, (u8int)c); }
static void dbg_puts(const char *s) { while (*s) dbg_putc(*s++); }
static void dbg_hex16(u16int v)
{
    const char *hexd = "0123456789ABCDEF";
    dbg_puts("0x");
    for (int i = 12; i >= 0; i -= 4) dbg_putc(hexd[(v >> i) & 0xF]);
}
static void small_delay(void)
{
    for (volatile int i = 0; i < 2000000; i++)
        __asm__ volatile("pause");
}
usb_device_t usb_devices[USB_MAX_DEVICES];
static int next_free_address = 1;
static usb_hc_kind_t hc_kind = USB_HC_NONE;

int usb_control_transfer(u8int addr, usb_setup_packet_t *setup,
                          void *buffer, u16int length, int in_dir)
{
    if (hc_kind == USB_HC_UHCI) return uhci_control_transfer(addr, setup, buffer, length, in_dir);
    if (hc_kind == USB_HC_EHCI) return ehci_control_transfer(addr, setup, buffer, length, in_dir);
    return -1;
}

int usb_transfer(u8int addr, u8int endpoint, int in_dir,
                  void *buffer, u16int length, int *toggle,
                  u16int ep_max_packet)
{
    if (hc_kind == USB_HC_UHCI) return uhci_transfer(addr, endpoint, in_dir, buffer, length, toggle);
    if (hc_kind == USB_HC_EHCI) return ehci_transfer(addr, endpoint, in_dir, buffer, length, toggle, ep_max_packet);
    return -1;
}
static int hc_port_connected(int port)
{
    if (hc_kind == USB_HC_UHCI) return uhci_port_connected(port);
    if (hc_kind == USB_HC_EHCI) return ehci_port_connected(port);
    return 0;
}
static int hc_port_reset(int port)
{
    if (hc_kind == USB_HC_UHCI) return uhci_port_reset(port);
    if (hc_kind == USB_HC_EHCI) return ehci_port_reset(port);
    return 0;
}
static int hc_port_count(void)
{
    if (hc_kind == USB_HC_UHCI) return 2;   /* у UHCI всегда ровно 2 корневых порта */
    if (hc_kind == USB_HC_EHCI) return ehci_port_count();
    return 0;
}

static int enumerate_port(int port)
{
    if (!hc_port_connected(port))
        return 0;

    dbg_puts("USB: device detected on port ");
    dbg_putc((char)('0' + port));
    dbg_puts("\n");

    /*
     * USB reset.
     */
    if (!hc_port_reset(port)) {
        dbg_puts(
            "USB: port reset failed\n"
        );
        return 0;
    }

    usb_setup_packet_t setup;

    usb_device_descriptor_t desc;

    for (u16int i = 0;
         i < sizeof(desc);
         i++) {

        ((u8int *)&desc)[i] = 0;
    }

    /*
     * ------------------------------------------------------
     * 1. GET_DESCRIPTOR(Device, 8)
     * ------------------------------------------------------
     *
     * После reset устройство имеет адрес 0.
     *
     * На этом этапе EP0 MPS неизвестен,
     * поэтому UHCI использует стандартные 8 байт.
     */

    setup.bmRequestType = USB_DIR_IN;

    setup.bRequest =
        USB_REQ_GET_DESCRIPTOR;

    setup.wValue =
        (u16int)(USB_DESC_DEVICE << 8);

    setup.wIndex = 0;
    setup.wLength = 8;

    dbg_puts(
        "USB: GET_DESCRIPTOR(Device,8)\n"
    );

   if (usb_control_transfer(
        0,
        &setup,
        &desc,
        8,
        1) != 0) {

    dbg_puts(
        "USB: failed to read initial device descriptor\n"
    );

    return 0;
}

if (desc.bLength < 8 ||
    desc.bDescriptorType != USB_DESC_DEVICE) {

    dbg_puts(
        "USB: invalid initial device descriptor\n"
    );

    return 0;
}

uhci_set_ep0_max_packet(
    0,
    desc.bMaxPacketSize0
);
    /*
     * Проверяем, что устройство действительно
     * вернуло Device Descriptor.
     */
    if (desc.bLength < 8 ||
        desc.bDescriptorType != USB_DESC_DEVICE) {

        dbg_puts(
            "USB: invalid initial device descriptor\n"
        );

        return 0;
    }

    /*
     * Теперь мы знаем размер EP0.
     */
    u8int ep0_mps =
        desc.bMaxPacketSize0;

    if (ep0_mps != 8 &&
        ep0_mps != 16 &&
        ep0_mps != 32 &&
        ep0_mps != 64) {

        dbg_puts(
            "USB: invalid bMaxPacketSize0\n"
        );

        return 0;
    }

    /*
     * Только UHCI должен знать этот параметр.
     */
    if (hc_kind == USB_HC_UHCI) {

        uhci_set_ep0_max_packet(
            0,
            ep0_mps
        );
    }

    /*
     * ------------------------------------------------------
     * 2. SET_ADDRESS
     * ------------------------------------------------------
     */

    int addr =
        next_free_address++;

    if (addr > 127) {

        dbg_puts(
            "USB: out of device addresses\n"
        );

        return 0;
    }

    setup.bmRequestType =
        USB_DIR_OUT;

    setup.bRequest =
        USB_REQ_SET_ADDRESS;

    setup.wValue =
        (u16int)addr;

    setup.wIndex = 0;
    setup.wLength = 0;

    dbg_puts(
        "USB: SET_ADDRESS "
    );

    dbg_hex16((u16int)addr);
    dbg_puts("\n");

    if (usb_control_transfer(
            0,
            &setup,
            0,
            0,
            0) != 0) {

        dbg_puts(
            "USB: SET_ADDRESS failed\n"
        );

        return 0;
    }

    /*
     * USB требует небольшой интервал после
     * SET_ADDRESS перед использованием нового адреса.
     */
  small_delay();

uhci_set_ep0_max_packet(
    (u8int)addr,
    desc.bMaxPacketSize0
);

    /*
     * EP0 MPS сохраняется и для нового адреса.
     */
    if (hc_kind == USB_HC_UHCI) {

        uhci_set_ep0_max_packet(
            (u8int)addr,
            ep0_mps
        );
    }

    /*
     * ------------------------------------------------------
     * 3. Полный Device Descriptor
     * ------------------------------------------------------
     */

    setup.bmRequestType =
        USB_DIR_IN;

    setup.bRequest =
        USB_REQ_GET_DESCRIPTOR;

    setup.wValue =
        (u16int)(USB_DESC_DEVICE << 8);

    setup.wIndex = 0;

    setup.wLength =
        sizeof(usb_device_descriptor_t);

    dbg_puts(
        "USB: GET_DESCRIPTOR(Device,18)\n"
    );

    if (usb_control_transfer(
            (u8int)addr,
            &setup,
            &desc,
            sizeof(usb_device_descriptor_t),
            1) != 0) {

        dbg_puts(
            "USB: failed to read full device descriptor\n"
        );

        return 0;
    }

    if (desc.bLength != 18 ||
        desc.bDescriptorType != USB_DESC_DEVICE) {

        dbg_puts(
            "USB: invalid full device descriptor\n"
        );

        return 0;
    }

    /*
     * Debug.
     */
    dbg_puts("USB: VID=");
    dbg_hex16(desc.idVendor);

    dbg_puts(" PID=");
    dbg_hex16(desc.idProduct);

    dbg_puts(" class=");
    dbg_hex16(desc.bDeviceClass);

    dbg_puts(" EP0=");
    dbg_hex16(desc.bMaxPacketSize0);

    dbg_puts(" addr=");
    dbg_hex16((u16int)addr);

    dbg_puts("\n");

    /*
     * HID mouse.
     */
    if (usb_hid_mouse_try_attach(
            (u8int)addr)) {

        dbg_puts(
            "USB: attached as HID boot mouse\n"
        );
    }

    /*
     * Save device.
     */
    for (int i = 0;
         i < USB_MAX_DEVICES;
         i++) {

        if (!usb_devices[i].present) {

            usb_devices[i].present = 1;
            usb_devices[i].address =
                addr;

            usb_devices[i].port =
                (u8int)port;

            usb_devices[i].desc =
                desc;

            break;
        }
    }

    return 1;
}

void usb_init(void)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++) usb_devices[i].present = 0;
    next_free_address = 1;
    hc_kind = USB_HC_NONE;

    if (uhci_init()) {
        hc_kind = USB_HC_UHCI;
        dbg_puts("USB: using UHCI controller\n");
    } else if (ehci_init()) {
        hc_kind = USB_HC_EHCI;
        dbg_puts("USB: using EHCI controller\n");
    } else {
        dbg_puts("USB: no supported controller found (UHCI/EHCI), stack idle\n");
        return;
    }

    int ports = hc_port_count();
    for (int p = 0; p < ports; p++) enumerate_port(p);
}
