#include "ixn_net.h"
#include "ixn_compat.h"
#include "ixn_skb.h"
#include "ixn_l2l3.h"
#include "ixn_l4.h"
#include "ixn_socket_api.h"
#include "ixn_http_ws.h"
#include "ixn_pci.h"
#include "ixn_e1000.h"
#include "ixn_rtl8139.h"
#include "ixn_eth_misc.h"
#include "ixn_usbnet.h"
#include "ixn_wifi.h"

static ixn_dhcp_client_t g_ixn_dhcp;
static ixn_u8 g_ixn_net_inited = 0u;

void ixn_net_init(void) {
    if (g_ixn_net_inited) return;
    ixn_skb_pool_init();
    ixn_netdev_init();
    ixn_l2l3_init();
    ixn_l4_init();
    ixn_socket_api_init();
    ixn_http_init();
    ixn_usb_init();
    ixn_wifi_init();
    ixn_e1000_probe();
    ixn_rtl8139_probe();
    ixn_eth_misc_probe();
    ixn_usb_probe();
    ixn_usbnet_probe();
    ixn_wifi_probe();
    (void)ixn_net_up();
    /*
     * Loopback-устройство lo (127.0.0.1/8).  Регистрируем после
     * ixn_net_up(): get_first_up() и get_default() не должны выбирать lo,
     * иначе DHCP уйдёт не в тот интерфейс.  Благодаря нему ping 127.0.0.1
     * не вылетает в физическую сеть.
     */
    {
        ixn_netdev_t *lo = ixn_netdev_alloc("lo", IXN_NETDEV_TYPE_LOOPBACK);
        if (lo && ixn_netdev_register(lo) == 0) {
            ixn_netdev_set_ipv4(lo, 0x7F000001u, 0xFF000000u, 0u);
            (void)ixn_netdev_open(lo);
        }
    }
    g_ixn_net_inited = 1u;
}

void ixn_net_shutdown(void) {
    if (!g_ixn_net_inited) return;
    ixn_http_server_shutdown();
    ixn_netdev_shutdown_all();
    ixn_usb_shutdown();
    ixn_skb_pool_shutdown();
    g_ixn_net_inited = 0u;
}

int ixn_net_up(void) {
    ixn_netdev_t *dev = ixn_netdev_get_first_up();
    if (dev) return 0;
    dev = ixn_netdev_get_default();
    if (!dev) return -1;
    return ixn_netdev_open(dev);
}

void ixn_net_poll(void) {
    ixn_netdev_poll_all(IXN_NETDEV_RX_BUDGET);
    ixn_usb_poll();
}

int ixn_net_dhcp(ixn_dhcp_client_t *out_client) {
    ixn_netdev_t *dev = ixn_netdev_get_first_up();
    if (!dev) return -1;
    if (!out_client) return -2;
    ixn_dhcp_init(out_client, dev);
    if (ixn_dhcp_discover(out_client) != 0) return -3;
    {
        ixn_u64 deadline = ixn_ticks_ms() + 15000u;
        while (ixn_ticks_ms() < deadline && out_client->state != IXN_DHCP_STATE_BOUND) {
            ixn_netdev_poll_all(IXN_NETDEV_RX_BUDGET);
            ixn_dhcp_tick(out_client);
            ixn_pause();
        }
    }
    return out_client->state == IXN_DHCP_STATE_BOUND ? 0 : -4;
}

int ixn_net_auto_configure(void) {
    int rc = ixn_net_up();
    if (rc != 0) return rc;
    return ixn_net_dhcp(&g_ixn_dhcp);
}

ixn_u32 ixn_net_device_count(void) {
    return ixn_netdev_count();
}

const char *ixn_net_default_name(void) {
    ixn_netdev_t *dev = ixn_netdev_get_default();
    return dev ? dev->name : "none";
}

static ixn_dhcp_client_t g_ixn_boot_dhcp;
static ixn_u8 g_ixn_boot_dhcp_done = 0u;

int ixn_net_dhcp_blocking(ixn_u32 timeout_ms) {
    ixn_netdev_t *dev;
    ixn_u64 deadline;

    if (g_ixn_boot_dhcp_done) return 0;

    dev = ixn_netdev_get_default();
    if (!dev) return -1;
    if (ixn_netdev_open(dev) != 0) return -2;

    ixn_dhcp_init(&g_ixn_boot_dhcp, dev);
    ixn_dhcp_set_hook(&g_ixn_boot_dhcp);

    if (ixn_dhcp_discover(&g_ixn_boot_dhcp) != 0) return -3;

    deadline = ixn_ticks_ms() + (ixn_u64)(timeout_ms ? timeout_ms : 8000u);

    while (ixn_ticks_ms() < deadline &&
           g_ixn_boot_dhcp.state != IXN_DHCP_STATE_BOUND) {
        ixn_netdev_poll_all(IXN_NETDEV_RX_BUDGET);
        ixn_dhcp_tick(&g_ixn_boot_dhcp);
        ixn_pause();
    }

    if (g_ixn_boot_dhcp.state != IXN_DHCP_STATE_BOUND) {
        ixn_dhcp_set_hook(0);
        return -4;
    }

    g_ixn_boot_dhcp_done = 1u;
    return 0;
}

int ixn_net_dhcp_configured(ixn_u32 *out_ip, ixn_u32 *out_mask, ixn_u32 *out_gw, ixn_u32 *out_dns) {
    if (!g_ixn_boot_dhcp_done) return 0;
    if (out_ip)   *out_ip   = g_ixn_boot_dhcp.offered_ip;
    if (out_mask) *out_mask = g_ixn_boot_dhcp.subnet_mask;
    if (out_gw)   *out_gw   = g_ixn_boot_dhcp.router;
    if (out_dns)  *out_dns  = g_ixn_boot_dhcp.dns_server;
    return 1;
}
