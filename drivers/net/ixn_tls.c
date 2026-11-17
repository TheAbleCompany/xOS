#include "ixn_tls.h"
#include "ixn_compat.h"
#include "ixn_netdev.h"

#define IXSSL_IMPLEMENTATION
#include "ixssl.h"

static ixn_u32 g_ixn_tls_rand = 0xCAFEBABEu;

static void ixn_tls_rand_bytes(ixn_u8 *out, ixn_u32 n) {
    ixn_u32 i;
    for (i = 0u; i < n; i++) out[i] = (ixn_u8)ixn_rand_next(&g_ixn_tls_rand);
}

static ixn_u32 ixn_str_append(char *dst, ixn_u32 cap, ixn_u32 *len, const char *src) {
    ixn_u32 i = 0u;
    if (!dst || !len || !src || *len >= cap) return 0u;
    while (src[i] && *len + 1u < cap) { dst[*len] = src[i]; (*len)++; i++; }
    dst[*len] = 0;
    return i;
}

void ixn_tls_init(ixn_tls_t *t, ixn_socket_t *sock) {
    if (!t) return;
    ixn_memset(t, 0, sizeof(*t));
    t->sock = sock;
    t->state = IXN_TLS_STATE_IDLE;
    t->cipher = IXN_TLS_CIPHER_AES_128_GCM_SHA256;
    t->client_seq = 0u;
    t->server_seq = 0u;
}

static int ixn_tls_send_record(ixn_tls_t *t, ixn_u8 ct, const ixn_u8 *payload, ixn_u32 len) {
    ixn_u8 hdr[5];
    if (!t || !t->sock) return -1;
    hdr[0] = ct;
    hdr[1] = 0x03u; hdr[2] = 0x01u;
    hdr[3] = (ixn_u8)(len >> 8u);
    hdr[4] = (ixn_u8)(len & 0xFFu);
    if (ixn_tcp_send(t->sock, hdr, 5u) < 0) return -2;
    if (payload && len) {
        if (ixn_tcp_send(t->sock, payload, len) < 0) return -3;
    }
    return 0;
}

static int ixn_tls_recv_record(ixn_tls_t *t, ixn_u8 *out_ct, ixn_u8 *buf, ixn_u32 cap, ixn_u32 *out_len) {
    ixn_u8 hdr[5];
    int rlen;
    ixn_u32 spins = 3000u;
    if (!t || !out_ct || !buf || !out_len) return -1;
    while (spins--) {
        rlen = ixn_tcp_socket_recv(t->sock, hdr, 5u);
        if (rlen > 0) break;
        ixn_netdev_poll_all(32u);
        ixn_pause();
    }
    if (rlen <= 0) return -2;
    *out_ct = hdr[0];
    *out_len = ((ixn_u32)hdr[3] << 8u) | (ixn_u32)hdr[4];
    if (*out_len > cap) return -3;
    spins = 5000u;
    {
        ixn_u32 got = 0u;
        while (spins-- && got < *out_len) {
            rlen = ixn_tcp_socket_recv(t->sock, buf + got, *out_len - got);
            if (rlen > 0) got += (ixn_u32)rlen;
            else { ixn_netdev_poll_all(32u); ixn_pause(); }
        }
        if (got < *out_len) return -4;
        *out_len = got;
    }
    return 0;
}

static void ixn_tls_derive_keys(ixn_tls_t *t) {
    ixn_u8 early_secret[32], empty_hash[32], derived[32], handshake_secret[32];
    ixn_u8 hs_hash[32], master_secret[32];
    ixn_u8 salt[32];
    ixn_u8 zero[32];
    ixn_memset(zero, 0, 32u);
    ixssl_hkdf_extract(IXN_NULL, 0u, zero, 32u, early_secret);
    ixssl_sha256(IXN_NULL, 0u, empty_hash);
    {
        ixn_u8 info[8];
        ixn_u32 il = 0u;
        ixn_str_append((char *)info, sizeof(info), &il, "derived");
        info[il++] = 0u; info[il++] = 32u;
        ixssl_hkdf_expand(early_secret, 32u, info, il, derived, 32u);
    }
    ixssl_hkdf_extract(derived, 32u, t->shared, 32u, handshake_secret);
    ixssl_sha256(t->handshake_hash, 32u, hs_hash);
    {
        ixn_u8 info[16];
        ixn_u32 il = 0u;
        ixn_str_append((char *)info, sizeof(info), &il, "c hs traffic");
        info[il++] = 0u; info[il++] = 32u;
        ixssl_hkdf_expand(handshake_secret, 32u, info, il, t->client_write_key, 32u);
    }
    {
        ixn_u8 info[16];
        ixn_u32 il = 0u;
        ixn_str_append((char *)info, sizeof(info), &il, "s hs traffic");
        info[il++] = 0u; info[il++] = 32u;
        ixssl_hkdf_expand(handshake_secret, 32u, info, il, t->server_write_key, 32u);
    }
    {
        ixn_u8 info[8];
        ixn_u32 il = 0u;
        ixn_str_append((char *)info, sizeof(info), &il, "derived");
        info[il++] = 0u; info[il++] = 32u;
        ixssl_hkdf_expand(handshake_secret, 32u, info, il, derived, 32u);
    }
    ixssl_hkdf_extract(derived, 32u, zero, 32u, master_secret);
    (void)salt;
}

int ixn_tls_connect(ixn_tls_t *t) {
    ixn_u8 hello[256];
    ixn_u32 hlen = 0u;
    ixn_u8 ct;
    ixn_u32 rlen;
    if (!t || !t->sock) return -1;
    ixn_tls_rand_bytes(t->client_random, 32u);
    ixn_tls_rand_bytes(t->client_priv, 32u);
    ixssl_x25519_base(t->client_pub, t->client_priv);
    hello[hlen++] = IXN_TLS_HS_CLIENT_HELLO;
    hello[hlen++] = 0u; hello[hlen++] = 0u; hello[hlen++] = 0u;
    hello[hlen++] = 0x03u; hello[hlen++] = 0x01u;
    ixn_memcpy(hello + hlen, t->client_random, 32u); hlen += 32u;
    hello[hlen++] = 0u;
    hello[hlen++] = 0x00u; hello[hlen++] = 0x02u;
    hello[hlen++] = (ixn_u8)(IXN_TLS_CIPHER_AES_128_GCM_SHA256 >> 8u);
    hello[hlen++] = (ixn_u8)(IXN_TLS_CIPHER_AES_128_GCM_SHA256 & 0xFFu);
    hello[hlen++] = (ixn_u8)(IXN_TLS_CIPHER_CHACHA20_POLY1305 >> 8u);
    hello[hlen++] = (ixn_u8)(IXN_TLS_CIPHER_CHACHA20_POLY1305 & 0xFFu);
    hello[hlen++] = 0x01u; hello[hlen++] = 0x00u;
    hello[hlen++] = 0x00u; hello[hlen++] = 0x21u;
    hello[hlen++] = 0x00u; hello[hlen++] = 0x1Du;
    ixn_memcpy(hello + hlen, t->client_pub, 32u); hlen += 32u;
    hello[hlen++] = 0x00u; hello[hlen++] = 0x0Du;
    hello[hlen++] = 0x00u; hello[hlen++] = 0x09u;
    hello[hlen++] = 0x00u; hello[hlen++] = 0x05u;
    hello[hlen++] = 0x00u; hello[hlen++] = 0x03u;
    hello[hlen++] = 'h'; hello[hlen++] = 't'; hello[hlen++] = 't'; hello[hlen++] = 'p'; hello[hlen++] = '/';
    hello[hlen++] = '1'; hello[hlen++] = '.';
    hello[hlen++] = 0x00u; hello[hlen++] = 0x02u;
    hello[hlen++] = 0x00u; hello[hlen++] = 0x01u;
    {
        ixn_u32 hs_len = hlen - 4u;
        hello[1] = (ixn_u8)(hs_len >> 16u);
        hello[2] = (ixn_u8)(hs_len >> 8u);
        hello[3] = (ixn_u8)(hs_len & 0xFFu);
    }
    ixssl_sha256(hello, hlen, t->handshake_hash);
    if (ixn_tls_send_record(t, IXN_TLS_CT_HANDSHAKE, hello, hlen) != 0) return -2;
    t->state = IXN_TLS_STATE_HELLO_SENT;
    if (ixn_tls_recv_record(t, &ct, t->rx_buf, sizeof(t->rx_buf), &rlen) != 0) return -3;
    if (ct == IXN_TLS_CT_HANDSHAKE && rlen > 0u && t->rx_buf[0] == IXN_TLS_HS_SERVER_HELLO) {
        ixn_u32 off = 1u;
        ixn_u32 hs_len = ((ixn_u32)t->rx_buf[off] << 16u) | ((ixn_u32)t->rx_buf[off + 1u] << 8u) | t->rx_buf[off + 2u];
        off += 3u;
        off += 2u;
        ixn_memcpy(t->server_random, t->rx_buf + off, 32u); off += 32u;
        off += 1u;
        t->cipher = ((ixn_u16)t->rx_buf[off] << 8u) | t->rx_buf[off + 1u]; off += 2u;
        off += 1u;
        {
            ixn_u32 ext_len = ((ixn_u32)t->rx_buf[off] << 8u) | t->rx_buf[off + 1u]; off += 2u;
            while (off + 4u <= rlen && off < 6u + hs_len) {
                ixn_u16 etype = ((ixn_u16)t->rx_buf[off] << 8u) | t->rx_buf[off + 1u]; off += 2u;
                ixn_u16 elen = ((ixn_u16)t->rx_buf[off] << 8u) | t->rx_buf[off + 1u]; off += 2u;
                if (etype == 0x0033u && elen == 35u) {
                    off += 1u;
                    ixn_memcpy(t->server_pub, t->rx_buf + off, 32u); off += 32u;
                    ixssl_x25519(t->shared, t->client_priv, t->server_pub);
                    ixn_tls_derive_keys(t);
                    t->state = IXN_TLS_STATE_ESTABLISHED;
                    return 0;
                }
                off += elen;
            }
        }
    }
    return -4;
}

int ixn_tls_send(ixn_tls_t *t, const ixn_u8 *data, ixn_u32 len) {
    ixn_u8 ct[16];
    ixn_u8 nonce[12];
    ixn_u8 aad[5];
    if (!t || !data || t->state != IXN_TLS_STATE_ESTABLISHED) return -1;
    ixn_memcpy(nonce, t->client_write_iv, 12u);
    {
        ixn_u64 seq = t->client_seq;
        ixn_u32 i;
        for (i = 0u; i < 8u; i++) nonce[11u - i] ^= (ixn_u8)(seq >> (i * 8u));
    }
    aad[0] = IXN_TLS_CT_APP_DATA;
    aad[1] = 0x03u; aad[2] = 0x03u;
    aad[3] = (ixn_u8)((len + 16u) >> 8u);
    aad[4] = (ixn_u8)((len + 16u) & 0xFFu);
    if (t->cipher == IXN_TLS_CIPHER_CHACHA20_POLY1305) {
        ixssl_chacha20_poly1305_encrypt(t->client_write_key, nonce, aad, 5u, data, len, t->rx_buf, ct);
    } else {
        ixssl_aes_gcm_encrypt(t->client_write_key, 16u, nonce, 12u, aad, 5u, data, len, t->rx_buf, ct);
    }
    if (ixn_tcp_send(t->sock, aad, 5u) < 0) return -2;
    if (ixn_tcp_send(t->sock, t->rx_buf, len) < 0) return -3;
    if (ixn_tcp_send(t->sock, ct, 16u) < 0) return -4;
    t->client_seq++;
    return (int)len;
}

int ixn_tls_recv(ixn_tls_t *t, ixn_u8 *buf, ixn_u32 cap) {
    ixn_u8 hdr[5];
    ixn_u8 nonce[12];
    ixn_u8 aad[5];
    ixn_u8 tag[16];
    ixn_u16 reclen;
    int rlen;
    ixn_u32 spins = 3000u;
    if (!t || !buf || t->state != IXN_TLS_STATE_ESTABLISHED) return -1;
    while (spins--) {
        rlen = ixn_tcp_socket_recv(t->sock, hdr, 5u);
        if (rlen > 0) break;
        ixn_netdev_poll_all(32u);
        ixn_pause();
    }
    if (rlen <= 0) return -2;
    if (hdr[0] != IXN_TLS_CT_APP_DATA) return -3;
    reclen = ((ixn_u16)hdr[3] << 8u) | hdr[4];
    if (reclen < 16u || reclen > cap + 16u) return -4;
    {
        ixn_u32 plen = (ixn_u32)reclen - 16u;
        ixn_u32 got = 0u;
        spins = 5000u;
        while (spins-- && got < reclen) {
            rlen = ixn_tcp_socket_recv(t->sock, t->rx_buf + got, reclen - got);
            if (rlen > 0) got += (ixn_u32)rlen;
            else { ixn_netdev_poll_all(32u); ixn_pause(); }
        }
        if (got < reclen) return -5;
        ixn_memcpy(tag, t->rx_buf + plen, 16u);
        ixn_memcpy(nonce, t->server_write_iv, 12u);
        {
            ixn_u64 seq = t->server_seq;
            ixn_u32 i;
            for (i = 0u; i < 8u; i++) nonce[11u - i] ^= (ixn_u8)(seq >> (i * 8u));
        }
        ixn_memcpy(aad, hdr, 5u);
        if (t->cipher == IXN_TLS_CIPHER_CHACHA20_POLY1305) {
            if (!ixssl_chacha20_poly1305_decrypt(t->server_write_key, nonce, aad, 5u, t->rx_buf, plen, buf, tag)) return -6;
        } else {
            if (!ixssl_aes_gcm_decrypt(t->server_write_key, 16u, nonce, 12u, aad, 5u, t->rx_buf, plen, buf, tag)) return -7;
        }
        t->server_seq++;
        return (int)plen;
    }
}

void ixn_tls_close(ixn_tls_t *t) {
    if (!t) return;
    if (t->sock) {
        ixn_tcp_close(t->sock);
        ixn_socket_free(t->sock);
        t->sock = IXN_NULL;
    }
    t->state = IXN_TLS_STATE_CLOSED;
}

int ixn_https_get(ixn_netdev_t *dev, ixn_u32 dst_ip, ixn_u16 dst_port, const char *host, const char *path, ixn_u8 *out_body, ixn_u32 cap, ixn_u32 *out_len) {
    ixn_socket_t *s;
    ixn_tls_t tls;
    ixn_u8 req[512];
    ixn_u32 pos = 0u;
    int rc;
    ixn_u32 spins;
    if (!dev || !host || !path || !out_body) return -1;
    if (dst_port == 0u) dst_port = 443u;
    s = ixn_socket_alloc(IXN_SOCK_TYPE_TCP);
    if (!s) return -2;
    s->dev = dev;
    rc = ixn_tcp_connect(s, dst_ip, dst_port);
    if (rc != 0) { ixn_socket_free(s); return -3; }
    spins = 2000u;
    while (spins-- && s->tcp_state != IXN_TCP_ESTABLISHED) { ixn_netdev_poll_all(32u); ixn_pause(); }
    if (s->tcp_state != IXN_TCP_ESTABLISHED) { ixn_socket_free(s); return -4; }
    ixn_tls_init(&tls, s);
    rc = ixn_tls_connect(&tls);
    if (rc != 0) { ixn_socket_free(s); return -5; }
    pos += ixn_str_append((char *)req, sizeof(req), &pos, "GET ");
    pos += ixn_str_append((char *)req, sizeof(req), &pos, path);
    pos += ixn_str_append((char *)req, sizeof(req), &pos, " HTTP/1.1\r\nHost: ");
    pos += ixn_str_append((char *)req, sizeof(req), &pos, host);
    pos += ixn_str_append((char *)req, sizeof(req), &pos, "\r\nConnection: close\r\n\r\n");
    rc = ixn_tls_send(&tls, req, pos);
    if (rc < 0) { ixn_tls_close(&tls); return -6; }
    {
        ixn_u32 total = 0u;
        spins = 10000u;
        while (spins-- && total < cap) {
            rc = ixn_tls_recv(&tls, out_body + total, cap - total);
            if (rc > 0) total += (ixn_u32)rc;
            else if (rc < 0) break;
            else { ixn_netdev_poll_all(32u); ixn_pause(); }
        }
        if (out_len) *out_len = total;
        ixn_tls_close(&tls);
        return total > 0u ? 0 : -7;
    }
}
