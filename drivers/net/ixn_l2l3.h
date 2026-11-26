#ifndef IXN_L2L3_H
#define IXN_L2L3_H

#include "ixn_types.h"
#include "ixn_skb.h"
#include "ixn_netdev.h"

#define IXN_ETH_TYPE_IPV4   0x0800u
#define IXN_ETH_TYPE_ARP    0x0806u
#define IXN_ETH_TYPE_IPV6   0x86DDu
#define IXN_ETH_TYPE_VLAN   0x8100u
#define IXN_ETH_TYPE_PAE    0x888Eu

#define IXN_ARP_HW_ETHER    1u
#define IXN_ARP_OP_REQUEST  1u
#define IXN_ARP_OP_REPLY    2u

#define IXN_IPV4_VERSION    4u
#define IXN_IPV4_IHL_MIN    5u
#define IXN_IPV4_TTL_DEFAULT 64u
#define IXN_IPV4_FLAG_DF    0x4000u
#define IXN_IPV4_FLAG_MF    0x2000u
#define IXN_IPV4_OFFSET_MASK 0x1FFFu

#define IXN_IP_PROTO_ICMP   1u
#define IXN_IP_PROTO_IGMP   2u
#define IXN_IP_PROTO_TCP    6u
#define IXN_IP_PROTO_UDP    17u
#define IXN_IP_PROTO_IPV6   41u
#define IXN_IP_PROTO_OSPF   89u

#define IXN_ICMP_ECHO_REPLY   0u
#define IXN_ICMP_DEST_UNREACH 3u
#define IXN_ICMP_ECHO_REQUEST 8u
#define IXN_ICMP_TIME_EXCEED  11u

#define IXN_ARP_TABLE_SIZE   64u
#define IXN_ARP_CACHE_TTL_MS (300000ull)

typedef struct {
    ixn_u8 dst[IXN_ETH_ALEN];
    ixn_u8 src[IXN_ETH_ALEN];
    ixn_u16 type;
} ixn_eth_hdr_t;

typedef struct {
    ixn_u16 htype;
    ixn_u16 ptype;
    ixn_u8 hlen;
    ixn_u8 plen;
    ixn_u16 oper;
    ixn_u8 sha[IXN_ETH_ALEN];
    ixn_u8 spa[IXN_IPV4_ALEN];
    ixn_u8 tha[IXN_ETH_ALEN];
    ixn_u8 tpa[IXN_IPV4_ALEN];
} ixn_arp_pkt_t;

typedef struct {
    ixn_u8 ver_ihl;
    ixn_u8 tos;
    ixn_u16 tot_len;
    ixn_u16 id;
    ixn_u16 flags_frag;
    ixn_u8 ttl;
    ixn_u8 protocol;
    ixn_u16 checksum;
    ixn_u32 src;
    ixn_u32 dst;
} ixn_ipv4_hdr_t;

typedef struct {
    ixn_u8 type;
    ixn_u8 code;
    ixn_u16 checksum;
    ixn_u16 id;
    ixn_u16 seq;
} ixn_icmp_hdr_t;

typedef struct {
    ixn_u32 ip;
    ixn_u8 mac[IXN_ETH_ALEN];
    ixn_netdev_t *dev;
    ixn_u64 timestamp;
    ixn_u8 state;
    ixn_u8 reserved[3];
} ixn_arp_entry_t;

#define IXN_ARP_STATE_FREE     0u
#define IXN_ARP_STATE_RESOLVED 1u
#define IXN_ARP_STATE_STALE    2u

void ixn_l2l3_init(void);

ixn_u16 ixn_checksum16(const ixn_u8 *data, ixn_u32 len, ixn_u32 init);
ixn_u16 ixn_checksum16_combine(ixn_u16 c1, ixn_u16 c2, ixn_u32 len2);

void ixn_eth_build_header(ixn_skb_t *skb, ixn_netdev_t *dev, const ixn_u8 *dst_mac, ixn_u16 eth_type);
int ixn_eth_rx(ixn_netdev_t *dev, ixn_skb_t *skb);
int ixn_eth_tx(ixn_netdev_t *dev, ixn_skb_t *skb, const ixn_u8 *dst_mac, ixn_u16 eth_type);

/* arp_resolve: MAC разрешён, out_mac заполнен */
#define IXN_ARP_RESOLVE_OK       0
/* arp_resolve: записи в кэше нет, ARP-запрос отправлен, MAC пока неизвестен */
#define IXN_ARP_RESOLVE_SENT   (-3)

int ixn_arp_resolve(ixn_netdev_t *dev, ixn_u32 ip, ixn_u8 *out_mac);
int ixn_arp_ensure(ixn_netdev_t *dev, ixn_u32 ip, ixn_u32 timeout_ms);
int ixn_arp_rx(ixn_netdev_t *dev, ixn_skb_t *skb);
void ixn_arp_update(ixn_netdev_t *dev, ixn_u32 ip, const ixn_u8 *mac);
ixn_arp_entry_t *ixn_arp_lookup(ixn_u32 ip);
void ixn_arp_flush(ixn_netdev_t *dev);

int ixn_ipv4_rx(ixn_netdev_t *dev, ixn_skb_t *skb);
int ixn_ipv4_tx(ixn_netdev_t *dev, ixn_skb_t *skb, ixn_u32 dst_ip, ixn_u8 protocol);
ixn_ipv4_hdr_t *ixn_ipv4_hdr(ixn_skb_t *skb);
int ixn_is_local_ip(ixn_netdev_t *dev, ixn_u32 dst_ip);

int ixn_icmp_rx(ixn_netdev_t *dev, ixn_skb_t *skb);
int ixn_icmp_send_echo(ixn_netdev_t *dev, ixn_u32 dst_ip, ixn_u16 id, ixn_u16 seq, const ixn_u8 *data, ixn_u32 data_len);
int ixn_icmp_send(ixn_netdev_t *dev, ixn_u32 dst_ip, ixn_u8 type, ixn_u8 code, const ixn_u8 *payload, ixn_u32 payload_len);

/* --- ping: приём эхо-ответов, как ядро Linux отдаёт их raw-сокету --- */
#define IXN_PING_QUEUE_SIZE 16u

typedef struct {
    ixn_u8  in_use;
    ixn_u8  code;
    ixn_u8  _pad[2];
    ixn_u32 src_ip;
    ixn_u32 seq;
    ixn_u32 ttl;
    ixn_u32 size;
    ixn_u16 id;     /* ICMP id в host order -- матчинг id+seq, как в Linux */
    ixn_u16 _pad2;
    ixn_u64 ts_ms;
} ixn_ping_reply_t;

int ixn_ping_get_reply(ixn_ping_reply_t *out);
/* Забрать из очереди ответ, совпадающий по id И seq, не трогая чужие
 * ответы (параллельные ping-процессы не крадут пакеты друг у друга). */
int ixn_ping_get_reply_match(ixn_u16 id, ixn_u32 seq, ixn_ping_reply_t *out);
void ixn_ping_flush(void);

ixn_u32 ixn_ipv4_parse_str(const char *s);
int ixn_ipv4_format_str(ixn_u32 ip, char *out, ixn_u32 cap);

static inline ixn_u8 ixn_ipv4_version(ixn_ipv4_hdr_t *h) { return (h->ver_ihl >> 4u) & 0xFu; }
static inline ixn_u8 ixn_ipv4_ihl(ixn_ipv4_hdr_t *h) { return h->ver_ihl & 0xFu; }
static inline ixn_u32 ixn_ipv4_hdr_len(ixn_ipv4_hdr_t *h) { return (ixn_u32)(h->ver_ihl & 0xFu) * 4u; }

#endif
