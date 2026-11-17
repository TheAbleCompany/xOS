#ifndef IXN_TLS_H
#define IXN_TLS_H

#include "ixn_types.h"
#include "ixn_l4.h"

#define IXN_TLS_VERSION_13  0x0304u
#define IXN_TLS_MAX_FRAG    16384u
#define IXN_TLS_MAX_RECORDS 32u

#define IXN_TLS_CT_INVALID      0u
#define IXN_TLS_CT_CHANGE_CIPHER 20u
#define IXN_TLS_CT_ALERT        21u
#define IXN_TLS_CT_HANDSHAKE    22u
#define IXN_TLS_CT_APP_DATA     23u

#define IXN_TLS_HS_CLIENT_HELLO 1u
#define IXN_TLS_HS_SERVER_HELLO 2u
#define IXN_TLS_HS_NEW_SESSION  4u
#define IXN_TLS_HS_ENCRYPTED_EXT 8u
#define IXN_TLS_HS_CERT         11u
#define IXN_TLS_HS_CERT_VERIFY  15u
#define IXN_TLS_HS_FINISHED     20u

#define IXN_TLS_CIPHER_AES_128_GCM_SHA256 0x1301u
#define IXN_TLS_CIPHER_AES_256_GCM_SHA384 0x1302u
#define IXN_TLS_CIPHER_CHACHA20_POLY1305 0x1303u

#define IXN_TLS_GROUP_X25519 0x001Du
#define IXN_TLS_SIG_ECDSA_SECP256 0x0403u

#define IXN_TLS_STATE_IDLE       0u
#define IXN_TLS_STATE_HELLO_SENT 1u
#define IXN_TLS_STATE_HANDSHAKE  2u
#define IXN_TLS_STATE_ESTABLISHED 3u
#define IXN_TLS_STATE_CLOSED     4u

typedef struct {
    ixn_u8 ct;
    ixn_u16 version;
    ixn_u16 length;
} ixn_tls_record_hdr_t;

typedef struct {
    ixn_socket_t *sock;
    ixn_u8 state;
    ixn_u16 cipher;
    ixn_u8 client_random[32];
    ixn_u8 server_random[32];
    ixn_u8 client_priv[32];
    ixn_u8 client_pub[32];
    ixn_u8 server_pub[32];
    ixn_u8 shared[32];
    ixn_u8 handshake_hash[32];
    ixn_u8 client_write_key[32];
    ixn_u8 server_write_key[32];
    ixn_u8 client_write_iv[12];
    ixn_u8 server_write_iv[12];
    ixn_u64 client_seq;
    ixn_u64 server_seq;
    ixn_u8 rx_buf[IXN_TLS_MAX_FRAG + 16];
    ixn_u32 rx_len;
} ixn_tls_t;

void ixn_tls_init(ixn_tls_t *t, ixn_socket_t *sock);
int ixn_tls_connect(ixn_tls_t *t);
int ixn_tls_send(ixn_tls_t *t, const ixn_u8 *data, ixn_u32 len);
int ixn_tls_recv(ixn_tls_t *t, ixn_u8 *buf, ixn_u32 cap);
void ixn_tls_close(ixn_tls_t *t);

int ixn_https_get(ixn_netdev_t *dev, ixn_u32 dst_ip, ixn_u16 dst_port, const char *host, const char *path, ixn_u8 *out_body, ixn_u32 cap, ixn_u32 *out_len);

#endif
