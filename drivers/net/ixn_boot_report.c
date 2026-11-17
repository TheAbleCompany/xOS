#include "ixn_net.h"
#include "ixn_netdev.h"
#include "ixn_compat.h"
#include "ixn_l2l3.h"

#include "../../lib/common.h"

extern void serial_puts_ax(const char *s);

static void net_put_u32(ixn_u32 v)
{
    char buf[16];
    int i = 0;

    if (v == 0u) { serial_puts_ax("0"); return; }
    while (v) { buf[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (i) { char c[2]; c[0] = buf[--i]; c[1] = 0; serial_puts_ax(c); }
}

static void net_put_ip(ixn_u32 ip)
{
    char seg[8];
    int shift;

    for (shift = 24; shift >= 0; shift -= 8) {
        ixn_u32 v = (ip >> shift) & 0xFFu;
        int i = 0;
        if (v == 0u) { seg[i++] = '0'; }
        else { while (v) { seg[i++] = (char)('0' + (v % 10u)); v /= 10u; } }
        while (i) { char c[2]; c[0] = seg[--i]; c[1] = 0; serial_puts_ax(c); }
        if (shift) serial_puts_ax(".");
    }
}

static void net_put_mac(const ixn_u8 *mac)
{
    static const char hx[] = "0123456789ABCDEF";
    char out[4];

    for (int i = 0; i < 6; i++) {
        out[0] = hx[(mac[i] >> 4) & 0xF];
        out[1] = hx[mac[i] & 0xF];
        out[2] = (i == 5) ? 0 : ':';
        out[3] = 0;
        serial_puts_ax(out);
    }
}

void ax_net_boot_report(void)
{
    ixn_u32 count = ixn_netdev_count();

    serial_puts_ax("[NET] devices: ");
    net_put_u32(count);
    serial_puts_ax("\n");

    for (ixn_u32 i = 0; i < count; i++) {
        ixn_netdev_t *dev = ixn_netdev_get_by_index(i);
        if (!dev) continue;

        serial_puts_ax("[NET] ");
        serial_puts_ax(dev->name);
        serial_puts_ax(" mac=");
        net_put_mac(dev->dev_addr);
        serial_puts_ax(" pci=");
        net_put_u32(dev->pci.vendor);
        serial_puts_ax(":");
        net_put_u32(dev->pci.device);
        serial_puts_ax(" ifindex=");
        net_put_u32(dev->ifindex);
        serial_puts_ax(dev->ipv4_configured ? " ip=" : " ip=unconfigured");
        if (dev->ipv4_configured) net_put_ip(dev->ipv4_addr);
        serial_puts_ax(" up=");
        net_put_u32(ixn_netdev_is_up(dev) ? 1u : 0u);
        serial_puts_ax(" run=");
        net_put_u32(ixn_netdev_is_running(dev) ? 1u : 0u);
        serial_puts_ax(" tx=");
        net_put_u32((ixn_u32)dev->stats.tx_packets);
        serial_puts_ax(" rx=");
        net_put_u32((ixn_u32)dev->stats.rx_packets);
        serial_puts_ax("\n");
    }

    if (count == 0u)
        serial_puts_ax("[NET] no supported NIC found\n");
}

void ax_net_dhcp_try(ixn_u32 timeout_ms)
{
    int rc = ixn_net_dhcp_blocking(timeout_ms);

    if (rc == 0) {
        ixn_u32 ip = 0, mask = 0, gw = 0, dns = 0;
        ixn_net_dhcp_configured(&ip, &mask, &gw, &dns);
        serial_puts_ax("[NET] dhcp bound ip=");
        net_put_ip(ip);
        serial_puts_ax(" mask=");
        net_put_ip(mask);
        serial_puts_ax(" gw=");
        net_put_ip(gw);
        serial_puts_ax(" dns=");
        net_put_ip(dns);
        serial_puts_ax("\n");
    } else {
        serial_puts_ax("[NET] dhcp unavailable (no offer)\n");
    }
}
