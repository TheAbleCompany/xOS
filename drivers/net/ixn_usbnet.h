#ifndef IXN_USBNET_H
#define IXN_USBNET_H

#include "ixn_types.h"
#include "ixn_netdev.h"
#include "ixn_pci.h"

#define IXN_USB_MAX_DEVS    16u
#define IXN_USB_MAX_ENDPOINTS 16u
#define IXN_USB_MAX_PACKET   4096u

#define IXN_USB_DIR_OUT 0u
#define IXN_USB_DIR_IN  1u

#define IXN_USB_TRANSFER_CTRL  0u
#define IXN_USB_TRANSFER_BULK  1u
#define IXN_USB_TRANSFER_INT   2u
#define IXN_USB_TRANSFER_ISO   3u

#define IXN_USB_REQ_GET_STATUS   0u
#define IXN_USB_REQ_CLEAR_FEATURE 1u
#define IXN_USB_REQ_SET_FEATURE   3u
#define IXN_USB_REQ_SET_ADDRESS   5u
#define IXN_USB_REQ_GET_DESC      6u
#define IXN_USB_REQ_SET_DESC      7u
#define IXN_USB_REQ_SET_CONFIG    9u
#define IXN_USB_REQ_SET_INTERFACE 11u

#define IXN_USB_DESC_DEVICE     1u
#define IXN_USB_DESC_CONFIG     2u
#define IXN_USB_DESC_STRING     3u
#define IXN_USB_DESC_INTERFACE  4u
#define IXN_USB_DESC_ENDPOINT   5u

#define IXN_USB_CLASS_PER_INTERFACE 0u
#define IXN_USB_CLASS_COMM          0x02u
#define IXN_USB_CLASS_CDC_DATA      0x0Au
#define IXN_USB_CLASS_VENDOR        0xFFu

#define IXN_USB_SUBCLASS_CDC_ECM    0x06u
#define IXN_USB_SUBCLASS_CDC_NCM    0x0Du

#define IXN_USBNET_TYPE_CDC_ECM  1u
#define IXN_USBNET_TYPE_RNDIS    2u
#define IXN_USBNET_TYPE_AX88772  3u
#define IXN_USBNET_TYPE_AX88179  4u
#define IXN_USBNET_TYPE_MCS7830  5u

typedef struct {
    ixn_u8 addr;
    ixn_u8 attrib;
    ixn_u16 max_packet;
    ixn_u8 interval;
} ixn_usb_endpoint_t;

typedef struct {
    ixn_u8 bLength;
    ixn_u8 bDescriptorType;
    ixn_u16 bcdUSB;
    ixn_u8 bDeviceClass;
    ixn_u8 bDeviceSubClass;
    ixn_u8 bDeviceProtocol;
    ixn_u8 bMaxPacketSize0;
    ixn_u16 idVendor;
    ixn_u16 idProduct;
    ixn_u16 bcdDevice;
    ixn_u8 iManufacturer;
    ixn_u8 iProduct;
    ixn_u8 iSerialNumber;
    ixn_u8 bNumConfigurations;
} ixn_usb_device_desc_t;

typedef struct {
    ixn_u8 bus;
    ixn_u8 slot;
    ixn_u8 func;
    ixn_u8 addr;
    ixn_u8 speed;
    ixn_u16 vendor;
    ixn_u16 product;
    ixn_u8 configured;
    ixn_usb_endpoint_t endpoints[IXN_USB_MAX_ENDPOINTS];
    ixn_u32 ep_count;
    ixn_u8 ep_in_bulk;
    ixn_u8 ep_out_bulk;
    ixn_u8 ep_int;
    void *hc_priv;
} ixn_usb_device_t;

typedef int (*ixn_usb_hc_init_fn)(ixn_u8 bus, ixn_u8 slot, ixn_u8 func, ixn_u64 bar, ixn_u64 bar_size, void **out_priv);
typedef void (*ixn_usb_hc_shutdown_fn)(void *priv);
typedef int (*ixn_usb_hc_control_fn)(void *priv, ixn_usb_device_t *dev, ixn_u8 request_type, ixn_u8 request, ixn_u16 value, ixn_u16 index, ixn_u8 *buf, ixn_u16 len);
typedef int (*ixn_usb_hc_bulk_fn)(void *priv, ixn_usb_device_t *dev, ixn_u8 ep, ixn_u8 dir, ixn_u8 *buf, ixn_u32 len);
typedef void (*ixn_usb_hc_poll_fn)(void *priv);

typedef struct {
    ixn_usb_hc_init_fn init;
    ixn_usb_hc_shutdown_fn shutdown;
    ixn_usb_hc_control_fn control;
    ixn_usb_hc_bulk_fn bulk;
    ixn_usb_hc_poll_fn poll;
    ixn_u16 vendor;
    ixn_u16 device;
} ixn_usb_hc_driver_t;

typedef struct {
    ixn_netdev_t *netdev;
    ixn_usb_device_t *usbdev;
    ixn_u8 type;
    ixn_u8 mac[IXN_ETH_ALEN];
    ixn_u8 *rx_buf;
    ixn_u32 rx_len;
    ixn_u8 *tx_buf;
} ixn_usbnet_t;

void ixn_usb_init(void);
void ixn_usb_register_hc(const ixn_usb_hc_driver_t *drv);
int ixn_usb_probe(void);
void ixn_usb_shutdown(void);
void ixn_usb_poll(void);

int ixn_usb_control_msg(ixn_usb_device_t *dev, ixn_u8 rt, ixn_u8 req, ixn_u16 val, ixn_u16 idx, ixn_u8 *buf, ixn_u16 len);
int ixn_usb_bulk_in(ixn_usb_device_t *dev, ixn_u8 ep, ixn_u8 *buf, ixn_u32 len);
int ixn_usb_bulk_out(ixn_usb_device_t *dev, ixn_u8 ep, ixn_u8 *buf, ixn_u32 len);

int ixn_usbnet_probe(void);
int ixn_usbnet_init_dev(ixn_usbnet_t *u, ixn_usb_device_t *usbdev, ixn_u8 type);
void ixn_usbnet_shutdown(ixn_usbnet_t *u);
void ixn_usbnet_poll(ixn_netdev_t *dev, ixn_u32 budget);

#endif
