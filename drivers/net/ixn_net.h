#ifndef IXN_NET_H
#define IXN_NET_H

#include "ixn_types.h"
#include "ixn_netdev.h"
#include "ixn_dhcp_dns.h"

void ixn_net_init(void);
void ixn_net_shutdown(void);
int ixn_net_up(void);
void ixn_net_poll(void);
int ixn_net_dhcp(ixn_dhcp_client_t *out_client);
int ixn_net_auto_configure(void);
ixn_u32 ixn_net_device_count(void);
const char *ixn_net_default_name(void);
void ax_net_boot_report(void);
void ax_net_dhcp_try(ixn_u32 timeout_ms);
int  ixn_net_dhcp_blocking(ixn_u32 timeout_ms);
int  ixn_net_dhcp_configured(ixn_u32 *out_ip, ixn_u32 *out_mask,
                            ixn_u32 *out_gw, ixn_u32 *out_dns);

#endif
