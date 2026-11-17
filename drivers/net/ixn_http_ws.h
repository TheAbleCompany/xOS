#ifndef IXN_HTTP_WS_H
#define IXN_HTTP_WS_H

#include "ixn_types.h"
#include "ixn_netdev.h"
#include "ixn_l4.h"

#define IXN_HTTP_PORT_DEFAULT 80u
#define IXN_HTTPS_PORT_DEFAULT 443u
#define IXN_HTTP_MAX_URL 512u
#define IXN_HTTP_MAX_HEADERS 32u
#define IXN_HTTP_MAX_HEADER_VAL 256u
#define IXN_HTTP_MAX_BODY (64u * 1024u)
#define IXN_HTTP_MAX_REQUESTS 16u

#define IXN_WS_PORT_DEFAULT 80u
#define IXN_WS_MAX_FRAME (64u * 1024u)

#define IXN_HTTP_METHOD_GET     1u
#define IXN_HTTP_METHOD_POST    2u
#define IXN_HTTP_METHOD_PUT     3u
#define IXN_HTTP_METHOD_DELETE  4u
#define IXN_HTTP_METHOD_HEAD    5u

#define IXN_HTTP_STATE_IDLE     0u
#define IXN_HTTP_STATE_REQ_LINE 1u
#define IXN_HTTP_STATE_HEADERS  2u
#define IXN_HTTP_STATE_BODY     3u
#define IXN_HTTP_STATE_DONE     4u

typedef struct {
    ixn_u32 status_code;
    char status_text[64];
    char headers[IXN_HTTP_MAX_HEADERS][IXN_HTTP_MAX_HEADER_VAL];
    ixn_u32 header_count;
    ixn_u8 *body;
    ixn_u32 body_len;
    ixn_u32 content_length;
    ixn_u8 chunked;
    ixn_u8 complete;
} ixn_http_response_t;

typedef struct {
    ixn_u32 method;
    char path[IXN_HTTP_MAX_URL];
    char host[IXN_HTTP_MAX_URL];
    char headers[IXN_HTTP_MAX_HEADERS][IXN_HTTP_MAX_HEADER_VAL];
    ixn_u32 header_count;
    ixn_u8 *body;
    ixn_u32 body_len;
    ixn_u32 content_length;
} ixn_http_request_t;

typedef int (*ixn_http_handler_fn)(ixn_http_request_t *req, ixn_http_response_t *resp, void *user);

typedef struct {
    ixn_socket_t *sock;
    ixn_u8 state;
    ixn_u8 buf[IXN_HTTP_MAX_BODY + 1024u];
    ixn_u32 buf_len;
    ixn_http_request_t req;
    ixn_http_handler_fn handler;
    void *user;
} ixn_http_conn_t;

#define IXN_WS_OPCODE_CONT  0x0u
#define IXN_WS_OPCODE_TEXT  0x1u
#define IXN_WS_OPCODE_BIN   0x2u
#define IXN_WS_OPCODE_CLOSE 0x8u
#define IXN_WS_OPCODE_PING  0x9u
#define IXN_WS_OPCODE_PONG  0xAu

typedef struct {
    ixn_socket_t *sock;
    ixn_u8 fin;
    ixn_u8 opcode;
    ixn_u8 masked;
    ixn_u8 mask[4];
    ixn_u64 payload_len;
    ixn_u8 payload[IXN_WS_MAX_FRAME];
    ixn_u32 payload_off;
} ixn_ws_frame_t;

void ixn_http_init(void);

int ixn_http_get(ixn_netdev_t *dev, ixn_u32 dst_ip, ixn_u16 dst_port, const char *path,
                 const char *host, ixn_http_response_t *out_resp);
int ixn_http_request(ixn_netdev_t *dev, ixn_u32 dst_ip, ixn_u16 dst_port, ixn_u32 method,
                     const char *path, const char *host, const ixn_u8 *body, ixn_u32 body_len,
                     ixn_http_response_t *out_resp);

int ixn_http_server_init(ixn_netdev_t *dev, ixn_u16 port, ixn_http_handler_fn handler, void *user);
void ixn_http_server_poll(void);
void ixn_http_server_shutdown(void);

int ixn_http_response_init(ixn_http_response_t *r, ixn_u32 status, const char *status_text);
int ixn_http_response_set_header(ixn_http_response_t *r, const char *name, const char *value);
int ixn_http_response_set_body(ixn_http_response_t *r, const ixn_u8 *body, ixn_u32 len);
int ixn_http_response_serialize(ixn_http_response_t *r, ixn_u8 *buf, ixn_u32 cap);

int ixn_ws_connect(ixn_netdev_t *dev, ixn_u32 dst_ip, ixn_u16 dst_port, const char *host, const char *path, ixn_socket_t **out_sock);
int ixn_ws_send_frame(ixn_socket_t *sock, ixn_u8 opcode, const ixn_u8 *data, ixn_u32 len);
int ixn_ws_recv_frame(ixn_socket_t *sock, ixn_ws_frame_t *frame);
int ixn_ws_close(ixn_socket_t *sock);

#endif
