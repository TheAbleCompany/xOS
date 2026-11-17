#ifndef USB_H
#define USB_H

#include <stdint.h>

void usb_init(void);
void usb_poll(void);
uint32_t usb_controller_count(void);
uint32_t usb_hid_device_count(void);

#endif
