#ifndef IXN_DHCP_DNS_H
#define IXN_DHCP_DNS_H

#include "ixn_types.h"
#include "ixn_netdev.h"

#define IXN_DHCP_CLIENT_PORT  68u
#define IXN_DHCP_SERVER_PORT  67u
#define IXN_DHCP_MAGIC_COOKIE 0x63825363u

#define IXN_DHCP_OP_BOOTREQUEST 1u
#define IXN_DHCP_OP_BOOTREPLY   2u

#define IXN_DHCP_OPT_PAD          0u
#define IXN_DHCP_OPT_SUBNET_MASK  1u
#define IXN_DHCP_OPT_ROUTER       3u
#define IXN_DHCP_OPT_DNS_SERVER   6u
#define IXN_DHCP_OPT_HOST_NAME   12u
#define IXN_DHCP_OPT_REQUESTED_IP 50u
#define IXN_DHCP_OPT_LEASE_TIME  51u
#define IXN_DHCP_OPT_MSG_TYPE    53u
#define IXN_DHCP_OPT_SERVER_ID   54u
#define IXN_DHCP_OPT_PARAM_LIST  55u
#define IXN_DHCP_OPT_END        255u

#define IXN_DHCP_MSG_DISCOVER 1u
#define IXN_DHCP_MSG_OFFER    2u
#define IXN_DHCP_MSG_REQUEST  3u
#define IXN_DHCP_MSG_DECLINE  4u
#define IXN_DHCP_MSG_ACK      5u
#define IXN_DHCP_MSG_NAK      6u
#define IXN_DHCP_MSG_RELEASE  7u

#define IXN_DHCP_MAX_OPTS 64u
#define IXN_DHCP_XID_DEFAULT 0x12345678u

#define IXN_DNS_PORT 53u
#define IXN_DNS_MAX_NAME 256u
#define IXN_DNS_MAX_ANSWERS 16u
#define IXN_DNS_TYPE_A     1u
#define IXN_DNS_TYPE_CNAME 5u
#define IXN_DNS_TYPE_AAAA  28u
#define IXN_DNS_CLASS_IN   1u

typedef struct {
    ixn_u8 op;
    ixn_u8 htype;
    ixn_u8 hlen;
    ixn_u8 hops;
    ixn_u32 xid;
    ixn_u16 secs;
    ixn_u16 flags;
    ixn_u32 ciaddr;
    ixn_u32 yiaddr;
    ixn_u32 siaddr;
    ixn_u32 giaddr;
    ixn_u8 chaddr[16];
    ixn_u8 sname[64];
    ixn_u8 file[128];
    ixn_u32 magic;
} ixn_dhcp_hdr_t;

typedef struct {
    ixn_u32 xid;
    ixn_u32 lease_time;
    ixn_u32 server_id;
    ixn_u32 offered_ip;
    ixn_u32 subnet_mask;
    ixn_u32 router;
    ixn_u32 dns_server;
    ixn_u8 state;
    ixn_u64 last_tick;
    ixn_netdev_t *dev;
} ixn_dhcp_client_t;

#define IXN_DHCP_STATE_INIT     0u
#define IXN_DHCP_STATE_SELECTING 1u
#define IXN_DHCP_STATE_REQUESTING 2u
#define IXN_DHCP_STATE_BOUND    3u
#define IXN_DHCP_STATE_RENEWING 4u

typedef struct {
    ixn_u16 id;
    ixn_u16 flags;
    ixn_u16 qdcount;
    ixn_u16 ancount;
    ixn_u16 nscount;
    ixn_u16 arcount;
} ixn_dns_hdr_t;

typedef struct {
    ixn_u32 ip;
    ixn_u32 ttl;
    ixn_u8 type;
} ixn_dns_answer_t;

void ixn_dhcp_init(ixn_dhcp_client_t *c, ixn_netdev_t *dev);
int ixn_dhcp_discover(ixn_dhcp_client_t *c);
int ixn_dhcp_request(ixn_dhcp_client_t *c);
int ixn_dhcp_rx(ixn_dhcp_client_t *c, ixn_netdev_t *dev, const ixn_u8 *data, ixn_u32 len);
int ixn_dhcp_run(ixn_dhcp_client_t *c);
void ixn_dhcp_set_hook(ixn_dhcp_client_t *c);
ixn_dhcp_client_t *ixn_dhcp_get_hook(void);
int ixn_dhcp_release(ixn_dhcp_client_t *c);
void ixn_dhcp_tick(ixn_dhcp_client_t *c);

int ixn_dns_resolve(ixn_netdev_t *dev, ixn_u32 dns_server, const char *name, ixn_u32 *out_ip);
int ixn_dns_resolve_default(const char *name, ixn_u32 *out_ip);
ixn_u32 ixn_dns_get_server(ixn_netdev_t *dev);

#endif
