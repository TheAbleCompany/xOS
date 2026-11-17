#ifndef IXN_NETDEV_H
#define IXN_NETDEV_H

#include "ixn_types.h"

#define IXN_NETDEV_NAME_LEN     16u
#define IXN_NETDEV_MAX          16u
#define IXN_NETDEV_MTU_DEFAULT  1500u
#define IXN_NETDEV_MTU_JUMBO    9000u
#define IXN_NETDEV_MAX_QUEUES   8u
#define IXN_NETDEV_RX_BUDGET    64u

#define IXN_NETDEV_FLAG_UP          (1u << 0)
#define IXN_NETDEV_FLAG_BROADCAST   (1u << 1)
#define IXN_NETDEV_FLAG_LOOPBACK    (1u << 2)
#define IXN_NETDEV_FLAG_P2P         (1u << 3)
#define IXN_NETDEV_FLAG_RUNNING     (1u << 4)
#define IXN_NETDEV_FLAG_NOARP       (1u << 5)
#define IXN_NETDEV_FLAG_PROMISC     (1u << 6)
#define IXN_NETDEV_FLAG_MULTICAST   (1u << 7)
#define IXN_NETDEV_FLAG_WIRELESS    (1u << 8)
#define IXN_NETDEV_FLAG_ETHER       (1u << 9)

#define IXN_NETDEV_TYPE_UNKNOWN  0u
#define IXN_NETDEV_TYPE_ETHER    1u
#define IXN_NETDEV_TYPE_LOOPBACK 2u
#define IXN_NETDEV_TYPE_WIRELESS 3u
#define IXN_NETDEV_TYPE_USBNET   4u

#define IXN_NETDEV_STATS_FIELDS \
    ixn_u64 rx_packets; ixn_u64 tx_packets; \
    ixn_u64 rx_bytes;   ixn_u64 tx_bytes; \
    ixn_u64 rx_errors;  ixn_u64 tx_errors; \
    ixn_u64 rx_dropped; ixn_u64 tx_dropped; \
    ixn_u64 rx_crc_errors; ixn_u64 rx_frame_errors; \
    ixn_u64 rx_length_errors; ixn_u64 rx_over_errors; \
    ixn_u64 tx_carrier_errors; ixn_u64 tx_aborted_errors; \
    ixn_u64 multicast; ixn_u64 collisions

struct ixn_skb;
struct ixn_netdev;

typedef struct ixn_skb ixn_skb_t;
typedef struct ixn_netdev ixn_netdev_t;

typedef int (*ixn_netdev_open_fn)(ixn_netdev_t *dev);
typedef int (*ixn_netdev_stop_fn)(ixn_netdev_t *dev);
typedef int (*ixn_netdev_start_xmit_fn)(ixn_netdev_t *dev, ixn_skb_t *skb);
typedef void (*ixn_netdev_rx_fn)(ixn_netdev_t *dev, ixn_skb_t *skb);
typedef int (*ixn_netdev_set_mac_fn)(ixn_netdev_t *dev, const ixn_u8 *mac);
typedef int (*ixn_netdev_set_mtu_fn)(ixn_netdev_t *dev, ixn_u32 mtu);
typedef void (*ixn_netdev_poll_fn)(ixn_netdev_t *dev, ixn_u32 budget);
typedef void (*ixn_netdev_irq_fn)(ixn_netdev_t *dev);
typedef void (*ixn_netdev_shutdown_fn)(ixn_netdev_t *dev);

typedef struct {
    IXN_NETDEV_STATS_FIELDS;
} ixn_netdev_stats_t;

typedef struct {
    ixn_u8 bus;
    ixn_u8 slot;
    ixn_u8 func;
    ixn_u16 vendor;
    ixn_u16 device;
    ixn_u32 class_code;
    ixn_u8 irq;
    ixn_u64 bar[6];
    ixn_u64 bar_size[6];
} ixn_netdev_pci_info_t;

struct ixn_netdev {
    char name[IXN_NETDEV_NAME_LEN];
    ixn_u32 index;
    ixn_u32 type;
    ixn_u32 flags;
    ixn_u32 mtu;
    ixn_u32 tx_queue_len;
    ixn_u8 hw_addr[IXN_ETH_ALEN];
    ixn_u8 broadcast[IXN_ETH_ALEN];
    ixn_u8 dev_addr[IXN_ETH_ALEN];
    ixn_u32 ifindex;

    ixn_u32 ipv4_addr;
    ixn_u32 ipv4_netmask;
    ixn_u32 ipv4_gateway;
    ixn_u32 ipv4_broadcast;
    ixn_u8 ipv4_configured;

    ixn_netdev_stats_t stats;
    ixn_netdev_pci_info_t pci;

    void *priv;
    volatile ixn_u32 state_lock;

    ixn_netdev_open_fn       open;
    ixn_netdev_stop_fn       stop;
    ixn_netdev_start_xmit_fn start_xmit;
    ixn_netdev_rx_fn         rx_handler;
    ixn_netdev_set_mac_fn    set_mac_address;
    ixn_netdev_set_mtu_fn    set_mtu;
    ixn_netdev_poll_fn       poll;
    ixn_netdev_irq_fn        irq_handler;
    ixn_netdev_shutdown_fn   shutdown;
};

void ixn_netdev_init(void);
ixn_netdev_t *ixn_netdev_alloc(const char *name, ixn_u32 type);
int ixn_netdev_register(ixn_netdev_t *dev);
int ixn_netdev_unregister(ixn_netdev_t *dev);
int ixn_netdev_open(ixn_netdev_t *dev);
int ixn_netdev_stop(ixn_netdev_t *dev);
int ixn_netdev_up(ixn_netdev_t *dev);
int ixn_netdev_down(ixn_netdev_t *dev);
int ixn_netdev_xmit(ixn_netdev_t *dev, ixn_skb_t *skb);
ixn_netdev_t *ixn_netdev_get_by_index(ixn_u32 index);
ixn_netdev_t *ixn_netdev_get_by_name(const char *name);
ixn_netdev_t *ixn_netdev_get_default(void);
ixn_netdev_t *ixn_netdev_get_loopback(void);
ixn_netdev_t *ixn_netdev_get_first_up(void);
ixn_u32 ixn_netdev_count(void);
void ixn_netdev_set_default(ixn_netdev_t *dev);
void ixn_netdev_set_rx_handler(ixn_netdev_t *dev, ixn_netdev_rx_fn fn);
int ixn_netdev_set_mac(ixn_netdev_t *dev, const ixn_u8 *mac);
int ixn_netdev_set_mtu(ixn_netdev_t *dev, ixn_u32 mtu);
void ixn_netdev_set_ipv4(ixn_netdev_t *dev, ixn_u32 addr, ixn_u32 mask, ixn_u32 gw);
void ixn_netdev_poll_all(ixn_u32 budget);
void ixn_netdev_shutdown_all(void);
void ixn_netdev_stats_get(ixn_netdev_t *dev, ixn_netdev_stats_t *out);

static inline ixn_u8 ixn_netdev_is_up(ixn_netdev_t *dev) {
    return (dev && (dev->flags & IXN_NETDEV_FLAG_UP)) ? 1u : 0u;
}
static inline ixn_u8 ixn_netdev_is_running(ixn_netdev_t *dev) {
    return (dev && (dev->flags & IXN_NETDEV_FLAG_RUNNING)) ? 1u : 0u;
}
static inline ixn_u8 ixn_netdev_is_loopback(ixn_netdev_t *dev) {
    return (dev && (dev->type == IXN_NETDEV_TYPE_LOOPBACK)) ? 1u : 0u;
}
static inline ixn_u8 ixn_netdev_is_wireless(ixn_netdev_t *dev) {
    return (dev && (dev->type == IXN_NETDEV_TYPE_WIRELESS)) ? 1u : 0u;
}

#endif
