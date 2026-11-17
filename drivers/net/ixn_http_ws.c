#include "ixn_http_ws.h"
#include "ixn_l2l3.h"
#include "ixn_compat.h"

static ixn_http_conn_t g_ixn_http_conns[IXN_HTTP_MAX_REQUESTS];
static ixn_u32 g_ixn_http_conn_count = 0u;
static ixn_u16 g_ixn_http_listen_port = 0u;
static ixn_socket_t *g_ixn_http_listen_sock = IXN_NULL;
static ixn_http_handler_fn g_ixn_http_handler = IXN_NULL;
static void *g_ixn_http_handler_user = IXN_NULL;
static ixn_netdev_t *g_ixn_http_dev = IXN_NULL;
static ixn_u32 g_ixn_ws_rand = 0xDEADBEEFu;

static const char *ixn_http_method_str(ixn_u32 m) {
    switch (m) {
        case IXN_HTTP_METHOD_GET: return "GET";
        case IXN_HTTP_METHOD_POST: return "POST";
        case IXN_HTTP_METHOD_PUT: return "PUT";
        case IXN_HTTP_METHOD_DELETE: return "DELETE";
        case IXN_HTTP_METHOD_HEAD: return "HEAD";
        default: return "GET";
    }
}

static ixn_u32 ixn_str_copy(char *dst, const char *src, ixn_u32 cap) {
    ixn_u32 i = 0u;
    if (!dst || cap == 0u) return 0u;
    if (!src) { dst[0] = 0; return 0u; }
    while (src[i] && i + 1u < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return i;
}

static ixn_u32 ixn_str_append(char *dst, ixn_u32 cap, ixn_u32 *len, const char *src) {
    ixn_u32 i = 0u;
    if (!dst || !len || !src || *len >= cap) return 0u;
    while (src[i] && *len + 1u < cap) { dst[*len] = src[i]; (*len)++; i++; }
    dst[*len] = 0;
    return i;
}

void ixn_http_init(void) {
    ixn_memset(g_ixn_http_conns, 0, sizeof(g_ixn_http_conns));
    g_ixn_http_conn_count = 0u;
    g_ixn_http_listen_sock = IXN_NULL;
    g_ixn_http_handler = IXN_NULL;
}

int ixn_http_response_init(ixn_http_response_t *r, ixn_u32 status, const char *status_text) {
    if (!r) return -1;
    ixn_memset(r, 0, sizeof(*r));
    r->status_code = status;
    ixn_str_copy(r->status_text, status_text ? status_text : "OK", sizeof(r->status_text));
    return 0;
}

int ixn_http_response_set_header(ixn_http_response_t *r, const char *name, const char *value) {
    ixn_u32 n;
    if (!r || !name || !value || r->header_count >= IXN_HTTP_MAX_HEADERS) return -1;
    n = r->header_count;
    {
        ixn_u32 i = 0u, pos = 0u;
        while (name[i] && pos + 1u < IXN_HTTP_MAX_HEADER_VAL) { r->headers[n][pos++] = name[i++]; }
        if (pos + 1u < IXN_HTTP_MAX_HEADER_VAL) r->headers[n][pos++] = ':';
        if (pos + 1u < IXN_HTTP_MAX_HEADER_VAL) r->headers[n][pos++] = ' ';
        i = 0u;
        while (value[i] && pos + 1u < IXN_HTTP_MAX_HEADER_VAL) { r->headers[n][pos++] = value[i++]; }
        r->headers[n][pos] = 0;
    }
    r->header_count++;
    return 0;
}

int ixn_http_response_set_body(ixn_http_response_t *r, const ixn_u8 *body, ixn_u32 len) {
    if (!r) return -1;
    r->body = (ixn_u8 *)body;
    r->body_len = len;
    r->content_length = len;
    return 0;
}

int ixn_http_response_serialize(ixn_http_response_t *r, ixn_u8 *buf, ixn_u32 cap) {
    ixn_u32 pos = 0u, i;
    char len_str[16];
    if (!r || !buf || cap < 64u) return -1;
    pos += ixn_str_append((char *)buf, cap, &pos, "HTTP/1.1 ");
    {
        char code[8];
        ixn_u32 c = r->status_code, idx = 0u;
        if (c == 0u) { code[idx++] = '0'; }
        else { char tmp[8]; ixn_u32 ti = 0u; while (c) { tmp[ti++] = '0' + (c % 10u); c /= 10u; } while (ti) code[idx++] = tmp[--ti]; }
        code[idx] = 0;
        pos += ixn_str_append((char *)buf, cap, &pos, code);
    }
    pos += ixn_str_append((char *)buf, cap, &pos, " ");
    pos += ixn_str_append((char *)buf, cap, &pos, r->status_text);
    pos += ixn_str_append((char *)buf, cap, &pos, "\r\n");
    {
        ixn_u32 n = r->content_length, idx = 0u;
        if (n == 0u) len_str[idx++] = '0';
        else { char tmp[8]; ixn_u32 ti = 0u; while (n) { tmp[ti++] = '0' + (n % 10u); n /= 10u; } while (ti) len_str[idx++] = tmp[--ti]; }
        len_str[idx] = 0;
    }
    pos += ixn_str_append((char *)buf, cap, &pos, "Content-Length: ");
    pos += ixn_str_append((char *)buf, cap, &pos, len_str);
    pos += ixn_str_append((char *)buf, cap, &pos, "\r\n");
    for (i = 0u; i < r->header_count; i++) {
        pos += ixn_str_append((char *)buf, cap, &pos, r->headers[i]);
        pos += ixn_str_append((char *)buf, cap, &pos, "\r\n");
    }
    pos += ixn_str_append((char *)buf, cap, &pos, "\r\n");
    if (r->body && r->body_len && pos + r->body_len < cap) {
        ixn_memcpy(buf + pos, r->body, r->body_len);
        pos += r->body_len;
    }
    return (int)pos;
}

static int ixn_http_parse_response(ixn_http_response_t *r, const ixn_u8 *data, ixn_u32 len) {
    ixn_u32 pos = 0u, line_start = 0u, i;
    if (!r || !data || len < 16u) return -1;
    ixn_memset(r, 0, sizeof(*r));
    if (ixn_memcmp(data, "HTTP/", 5u) != 0) return -2;
    for (i = 0u; i < len; i++) {
        if (data[i] == ' ' && pos == 0u) { pos = i + 1u; break; }
    }
    if (pos == 0u) return -3;
    r->status_code = 0u;
    while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
        r->status_code = r->status_code * 10u + (ixn_u32)(data[pos] - '0');
        pos++;
    }
    while (pos < len && data[pos] == ' ') pos++;
    {
        ixn_u32 st = 0u;
        while (pos < len && data[pos] != '\r' && st + 1u < sizeof(r->status_text)) r->status_text[st++] = data[pos++];
        r->status_text[st] = 0;
    }
    while (pos < len && (data[pos] == '\r' || data[pos] == '\n')) pos++;
    line_start = pos;
    while (pos < len) {
        if (data[pos] == '\r' && pos + 1u < len && data[pos + 1u] == '\n') {
            if (pos == line_start) {
                pos += 2u;
                break;
            }
            if (r->header_count < IXN_HTTP_MAX_HEADERS) {
                ixn_u32 hl = pos - line_start;
                if (hl >= IXN_HTTP_MAX_HEADER_VAL) hl = IXN_HTTP_MAX_HEADER_VAL - 1u;
                ixn_memcpy(r->headers[r->header_count], data + line_start, hl);
                r->headers[r->header_count][hl] = 0;
                if (ixn_memcmp(r->headers[r->header_count], "Content-Length:", 15u) == 0) {
                    ixn_u32 k = 15u, v = 0u;
                    while (r->headers[r->header_count][k] == ' ') k++;
                    while (r->headers[r->header_count][k] >= '0' && r->headers[r->header_count][k] <= '9') {
                        v = v * 10u + (ixn_u32)(r->headers[r->header_count][k] - '0');
                        k++;
                    }
                    r->content_length = v;
                }
                r->header_count++;
            }
            pos += 2u;
            line_start = pos;
        } else {
            pos++;
        }
    }
    if (pos < len) {
        r->body_len = len - pos;
        r->body = (ixn_u8 *)(data + pos);
    }
    r->complete = 1u;
    return 0;
}

int ixn_http_request(ixn_netdev_t *dev, ixn_u32 dst_ip, ixn_u16 dst_port, ixn_u32 method,
                     const char *path, const char *host, const ixn_u8 *body, ixn_u32 body_len,
                     ixn_http_response_t *out_resp) {
    ixn_socket_t *s;
    ixn_u8 req[IXN_HTTP_MAX_BODY + 1024u];
    ixn_u32 pos = 0u;
    int rc, rlen;
    ixn_u32 spins;
    if (!dev || !path || !out_resp) return -1;
    if (dst_port == 0u) dst_port = IXN_HTTP_PORT_DEFAULT;
    s = ixn_socket_alloc(IXN_SOCK_TYPE_TCP);
    if (!s) return -2;
    s->dev = dev;
    rc = ixn_tcp_connect(s, dst_ip, dst_port);
    if (rc != 0) { ixn_socket_free(s); return -3; }
    spins = 2000u;
    while (spins-- && s->tcp_state != IXN_TCP_ESTABLISHED) { ixn_netdev_poll_all(32u); ixn_pause(); }
    if (s->tcp_state != IXN_TCP_ESTABLISHED) { ixn_socket_free(s); return -4; }
    pos += ixn_str_append((char *)req, sizeof(req), &pos, ixn_http_method_str(method));
    pos += ixn_str_append((char *)req, sizeof(req), &pos, " ");
    pos += ixn_str_append((char *)req, sizeof(req), &pos, path);
    pos += ixn_str_append((char *)req, sizeof(req), &pos, " HTTP/1.1\r\n");
    if (host) {
        pos += ixn_str_append((char *)req, sizeof(req), &pos, "Host: ");
        pos += ixn_str_append((char *)req, sizeof(req), &pos, host);
        pos += ixn_str_append((char *)req, sizeof(req), &pos, "\r\n");
    }
    pos += ixn_str_append((char *)req, sizeof(req), &pos, "Connection: close\r\n");
    if (body && body_len) {
        char len_str[16];
        ixn_u32 n = body_len, idx = 0u;
        if (n == 0u) len_str[idx++] = '0';
        else { char tmp[8]; ixn_u32 ti = 0u; while (n) { tmp[ti++] = '0' + (n % 10u); n /= 10u; } while (ti) len_str[idx++] = tmp[--ti]; }
        len_str[idx] = 0;
        pos += ixn_str_append((char *)req, sizeof(req), &pos, "Content-Length: ");
        pos += ixn_str_append((char *)req, sizeof(req), &pos, len_str);
        pos += ixn_str_append((char *)req, sizeof(req), &pos, "\r\n");
    }
    pos += ixn_str_append((char *)req, sizeof(req), &pos, "\r\n");
    if (body && body_len && pos + body_len < sizeof(req)) {
        ixn_memcpy(req + pos, body, body_len);
        pos += body_len;
    }
    rc = ixn_tcp_send(s, req, pos);
    if (rc < 0) { ixn_socket_free(s); return -5; }
    {
        ixn_u8 rbuf[IXN_HTTP_MAX_BODY + 1024u];
        ixn_u32 total = 0u;
        spins = 5000u;
        while (spins--) {
            rlen = ixn_tcp_socket_recv(s, rbuf + total, sizeof(rbuf) - total);
            if (rlen > 0) {
                total += (ixn_u32)rlen;
                if (total >= sizeof(rbuf) - 1u) break;
            } else if (s->tcp_state != IXN_TCP_ESTABLISHED) {
                break;
            }
            ixn_netdev_poll_all(32u);
            ixn_pause();
        }
        if (total == 0u) { ixn_socket_free(s); return -6; }
        rc = ixn_http_parse_response(out_resp, rbuf, total);
        ixn_tcp_close(s);
        ixn_socket_free(s);
        return rc;
    }
}

int ixn_http_get(ixn_netdev_t *dev, ixn_u32 dst_ip, ixn_u16 dst_port, const char *path,
                 const char *host, ixn_http_response_t *out_resp) {
    return ixn_http_request(dev, dst_ip, dst_port, IXN_HTTP_METHOD_GET, path, host, IXN_NULL, 0u, out_resp);
}

int ixn_http_server_init(ixn_netdev_t *dev, ixn_u16 port, ixn_http_handler_fn handler, void *user) {
    if (!dev || !handler) return -1;
    g_ixn_http_dev = dev;
    g_ixn_http_listen_port = port;
    g_ixn_http_handler = handler;
    g_ixn_http_handler_user = user;
    g_ixn_http_listen_sock = ixn_socket_alloc(IXN_SOCK_TYPE_TCP);
    if (!g_ixn_http_listen_sock) return -2;
    g_ixn_http_listen_sock->dev = dev;
    g_ixn_http_listen_sock->local_ip = dev->ipv4_addr;
    g_ixn_http_listen_sock->local_port = IXN_htons(port);
    return ixn_tcp_listen(g_ixn_http_listen_sock, 8u);
}

static void ixn_http_conn_process(ixn_http_conn_t *c) {
    ixn_u32 pos = 0u, line_start = 0u;
    ixn_http_request_t *req;
    ixn_http_response_t resp;
    if (!c || c->state != IXN_HTTP_STATE_IDLE) return;
    req = &c->req;
    ixn_memset(req, 0, sizeof(*req));
    {
        ixn_u32 sp = 0u;
        while (sp < c->buf_len && c->buf[sp] != ' ') sp++;
        if (sp >= c->buf_len) return;
        if (ixn_memcmp(c->buf, "GET", 3u) == 0) req->method = IXN_HTTP_METHOD_GET;
        else if (ixn_memcmp(c->buf, "POST", 4u) == 0) req->method = IXN_HTTP_METHOD_POST;
        else if (ixn_memcmp(c->buf, "PUT", 3u) == 0) req->method = IXN_HTTP_METHOD_PUT;
        else if (ixn_memcmp(c->buf, "DELETE", 6u) == 0) req->method = IXN_HTTP_METHOD_DELETE;
        else if (ixn_memcmp(c->buf, "HEAD", 4u) == 0) req->method = IXN_HTTP_METHOD_HEAD;
        pos = sp + 1u;
        {
            ixn_u32 pp = 0u;
            while (pos < c->buf_len && c->buf[pos] != ' ' && pp + 1u < IXN_HTTP_MAX_URL) req->path[pp++] = c->buf[pos++];
            req->path[pp] = 0;
        }
        while (pos < c->buf_len && c->buf[pos] != '\n') pos++;
        pos++;
        line_start = pos;
    }
    while (pos < c->buf_len) {
        if (c->buf[pos] == '\r' && pos + 1u < c->buf_len && c->buf[pos + 1u] == '\n') {
            if (pos == line_start) { pos += 2u; break; }
            if (req->header_count < IXN_HTTP_MAX_HEADERS) {
                ixn_u32 hl = pos - line_start;
                if (hl >= IXN_HTTP_MAX_HEADER_VAL) hl = IXN_HTTP_MAX_HEADER_VAL - 1u;
                ixn_memcpy(req->headers[req->header_count], c->buf + line_start, hl);
                req->headers[req->header_count][hl] = 0;
                if (ixn_memcmp(req->headers[req->header_count], "Host:", 5u) == 0) {
                    ixn_u32 k = 5u;
                    while (req->headers[req->header_count][k] == ' ') k++;
                    ixn_str_copy(req->host, req->headers[req->header_count] + k, sizeof(req->host));
                }
                if (ixn_memcmp(req->headers[req->header_count], "Content-Length:", 15u) == 0) {
                    ixn_u32 k = 15u, v = 0u;
                    while (req->headers[req->header_count][k] == ' ') k++;
                    while (req->headers[req->header_count][k] >= '0' && req->headers[req->header_count][k] <= '9') {
                        v = v * 10u + (ixn_u32)(req->headers[req->header_count][k] - '0'); k++;
                    }
                    req->content_length = v;
                }
                req->header_count++;
            }
            pos += 2u;
            line_start = pos;
        } else pos++;
    }
    if (pos < c->buf_len) {
        req->body = c->buf + pos;
        req->body_len = c->buf_len - pos;
    }
    ixn_http_response_init(&resp, 200u, "OK");
    if (g_ixn_http_handler) g_ixn_http_handler(req, &resp, g_ixn_http_handler_user);
    {
        ixn_u8 rbuf[IXN_HTTP_MAX_BODY + 1024u];
        int rlen = ixn_http_response_serialize(&resp, rbuf, sizeof(rbuf));
        if (rlen > 0) ixn_tcp_send(c->sock, rbuf, (ixn_u32)rlen);
    }
    ixn_tcp_close(c->sock);
}

void ixn_http_server_poll(void) {
    ixn_u32 i;
    if (!g_ixn_http_listen_sock) return;
    for (;;) {
        ixn_socket_t *child = ixn_tcp_accept(g_ixn_http_listen_sock);
        if (!child) break;
        for (i = 0u; i < IXN_HTTP_MAX_REQUESTS; i++) {
            if (!g_ixn_http_conns[i].sock) {
                g_ixn_http_conns[i].sock = child;
                g_ixn_http_conns[i].state = IXN_HTTP_STATE_IDLE;
                g_ixn_http_conns[i].buf_len = 0u;
                break;
            }
        }
    }
    for (i = 0u; i < IXN_HTTP_MAX_REQUESTS; i++) {
        ixn_http_conn_t *c = &g_ixn_http_conns[i];
        if (!c->sock) continue;
        {
            int rlen = ixn_tcp_socket_recv(c->sock, c->buf + c->buf_len, sizeof(c->buf) - c->buf_len);
            if (rlen > 0) {
                c->buf_len += (ixn_u32)rlen;
                if (c->buf_len >= 4u) {
                    ixn_u32 j;
                    for (j = 0u; j + 3u < c->buf_len; j++) {
                        if (c->buf[j] == '\r' && c->buf[j + 1u] == '\n' && c->buf[j + 2u] == '\r' && c->buf[j + 3u] == '\n') {
                            ixn_http_conn_process(c);
                            ixn_socket_free(c->sock);
                            c->sock = IXN_NULL;
                            c->state = IXN_HTTP_STATE_IDLE;
                            c->buf_len = 0u;
                            break;
                        }
                    }
                }
            }
            if (c->sock && c->sock->tcp_state != IXN_TCP_ESTABLISHED && c->sock->tcp_state != IXN_TCP_LISTEN) {
                ixn_socket_free(c->sock);
                c->sock = IXN_NULL;
                c->buf_len = 0u;
            }
        }
    }
}

void ixn_http_server_shutdown(void) {
    ixn_u32 i;
    for (i = 0u; i < IXN_HTTP_MAX_REQUESTS; i++) {
        if (g_ixn_http_conns[i].sock) {
            ixn_tcp_close(g_ixn_http_conns[i].sock);
            ixn_socket_free(g_ixn_http_conns[i].sock);
            g_ixn_http_conns[i].sock = IXN_NULL;
        }
    }
    if (g_ixn_http_listen_sock) {
        ixn_tcp_close(g_ixn_http_listen_sock);
        ixn_socket_free(g_ixn_http_listen_sock);
        g_ixn_http_listen_sock = IXN_NULL;
    }
}

static void ixn_ws_gen_key(ixn_u8 *out, ixn_u32 cap) {
    static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    ixn_u8 raw[16];
    ixn_u32 i;
    if (!out || cap < 25u) return;
    for (i = 0u; i < 16u; i++) raw[i] = (ixn_u8)ixn_rand_next(&g_ixn_ws_rand);
    for (i = 0u; i < 16u; i += 3u) {
        ixn_u32 v = ((ixn_u32)raw[i] << 16u) | ((ixn_u32)raw[i + 1u] << 8u) | (ixn_u32)raw[i + 2u];
        out[(i / 3u) * 4u + 0u] = b64[(v >> 18u) & 0x3Fu];
        out[(i / 3u) * 4u + 1u] = b64[(v >> 12u) & 0x3Fu];
        out[(i / 3u) * 4u + 2u] = b64[(v >> 6u) & 0x3Fu];
        out[(i / 3u) * 4u + 3u] = b64[v & 0x3Fu];
    }
    out[24u] = 0;
}

int ixn_ws_connect(ixn_netdev_t *dev, ixn_u32 dst_ip, ixn_u16 dst_port, const char *host, const char *path, ixn_socket_t **out_sock) {
    ixn_socket_t *s;
    ixn_u8 req[1024];
    ixn_u32 pos = 0u;
    ixn_u8 key[25];
    int rc, rlen;
    ixn_u32 spins;
    if (!dev || !host || !path || !out_sock) return -1;
    if (dst_port == 0u) dst_port = IXN_WS_PORT_DEFAULT;
    s = ixn_socket_alloc(IXN_SOCK_TYPE_TCP);
    if (!s) return -2;
    s->dev = dev;
    rc = ixn_tcp_connect(s, dst_ip, dst_port);
    if (rc != 0) { ixn_socket_free(s); return -3; }
    spins = 2000u;
    while (spins-- && s->tcp_state != IXN_TCP_ESTABLISHED) { ixn_netdev_poll_all(32u); ixn_pause(); }
    if (s->tcp_state != IXN_TCP_ESTABLISHED) { ixn_socket_free(s); return -4; }
    ixn_ws_gen_key(key, sizeof(key));
    pos += ixn_str_append((char *)req, sizeof(req), &pos, "GET ");
    pos += ixn_str_append((char *)req, sizeof(req), &pos, path);
    pos += ixn_str_append((char *)req, sizeof(req), &pos, " HTTP/1.1\r\n");
    pos += ixn_str_append((char *)req, sizeof(req), &pos, "Host: ");
    pos += ixn_str_append((char *)req, sizeof(req), &pos, host);
    pos += ixn_str_append((char *)req, sizeof(req), &pos, "\r\n");
    pos += ixn_str_append((char *)req, sizeof(req), &pos, "Upgrade: websocket\r\n");
    pos += ixn_str_append((char *)req, sizeof(req), &pos, "Connection: Upgrade\r\n");
    pos += ixn_str_append((char *)req, sizeof(req), &pos, "Sec-WebSocket-Key: ");
    pos += ixn_str_append((char *)req, sizeof(req), &pos, (char *)key);
    pos += ixn_str_append((char *)req, sizeof(req), &pos, "\r\n");
    pos += ixn_str_append((char *)req, sizeof(req), &pos, "Sec-WebSocket-Version: 13\r\n\r\n");
    rc = ixn_tcp_send(s, req, pos);
    if (rc < 0) { ixn_socket_free(s); return -5; }
    {
        ixn_u8 rbuf[1024];
        spins = 3000u;
        while (spins--) {
            rlen = ixn_tcp_socket_recv(s, rbuf, sizeof(rbuf));
            if (rlen > 0) break;
            ixn_netdev_poll_all(32u);
            ixn_pause();
        }
        if (rlen <= 0) { ixn_socket_free(s); return -6; }
        if (ixn_memcmp(rbuf, "HTTP/1.1 101", 12u) != 0) { ixn_socket_free(s); return -7; }
    }
    *out_sock = s;
    return 0;
}

int ixn_ws_send_frame(ixn_socket_t *sock, ixn_u8 opcode, const ixn_u8 *data, ixn_u32 len) {
    ixn_u8 hdr[14];
    ixn_u32 hlen = 2u;
    if (!sock) return -1;
    hdr[0] = 0x80u | (opcode & 0x0Fu);
    if (len < 126u) {
        hdr[1] = (ixn_u8)len;
    } else if (len < 65536u) {
        hdr[1] = 126u;
        ixn_be16_store(hdr + 2u, (ixn_u16)len);
        hlen += 2u;
    } else {
        hdr[1] = 127u;
        ixn_be32_store(hdr + 6u, 0u);
        ixn_be32_store(hdr + 2u, len);
        hlen += 8u;
    }
    if (ixn_tcp_send(sock, hdr, hlen) < 0) return -2;
    if (data && len) {
        if (ixn_tcp_send(sock, data, len) < 0) return -3;
    }
    return 0;
}

int ixn_ws_recv_frame(ixn_socket_t *sock, ixn_ws_frame_t *frame) {
    ixn_u8 hdr[14];
    ixn_u32 need = 2u;
    int rlen;
    ixn_u32 spins;
    if (!sock || !frame) return -1;
    ixn_memset(frame, 0, sizeof(*frame));
    spins = 3000u;
    while (spins--) {
        rlen = ixn_tcp_socket_recv(sock, hdr, need);
        if (rlen > 0) break;
        ixn_netdev_poll_all(32u);
        ixn_pause();
    }
    if (rlen <= 0) return -2;
    frame->fin = (hdr[0] >> 7u) & 1u;
    frame->opcode = hdr[0] & 0x0Fu;
    frame->masked = (hdr[1] >> 7u) & 1u;
    {
        ixn_u8 plen = hdr[1] & 0x7Fu;
        if (plen < 126u) frame->payload_len = plen;
        else if (plen == 126u) {
            ixn_u8 ext[2];
            spins = 1000u;
            while (spins--) { rlen = ixn_tcp_socket_recv(sock, ext, 2u); if (rlen > 0) break; ixn_netdev_poll_all(16u); ixn_pause(); }
            if (rlen <= 0) return -3;
            frame->payload_len = ixn_be16_load(ext);
        } else {
            ixn_u8 ext[8];
            spins = 1000u;
            while (spins--) { rlen = ixn_tcp_socket_recv(sock, ext, 8u); if (rlen > 0) break; ixn_netdev_poll_all(16u); ixn_pause(); }
            if (rlen <= 0) return -4;
            frame->payload_len = ixn_be32_load(ext + 4u);
        }
    }
    if (frame->masked) {
        spins = 1000u;
        while (spins--) { rlen = ixn_tcp_socket_recv(sock, frame->mask, 4u); if (rlen > 0) break; ixn_netdev_poll_all(16u); ixn_pause(); }
        if (rlen <= 0) return -5;
    }
    if (frame->payload_len > IXN_WS_MAX_FRAME) return -6;
    if (frame->payload_len > 0u) {
        spins = 5000u;
        ixn_u32 got = 0u;
        while (spins-- && got < frame->payload_len) {
            rlen = ixn_tcp_socket_recv(sock, frame->payload + got, (ixn_u32)(frame->payload_len - got));
            if (rlen > 0) got += (ixn_u32)rlen;
            else { ixn_netdev_poll_all(32u); ixn_pause(); }
        }
        if (got < frame->payload_len) return -7;
        if (frame->masked) {
            ixn_u32 i;
            for (i = 0u; i < frame->payload_len; i++) frame->payload[i] ^= frame->mask[i & 3u];
        }
    }
    return 0;
}

int ixn_ws_close(ixn_socket_t *sock) {
    if (!sock) return -1;
    ixn_ws_send_frame(sock, IXN_WS_OPCODE_CLOSE, IXN_NULL, 0u);
    ixn_tcp_close(sock);
    ixn_socket_free(sock);
    return 0;
}
