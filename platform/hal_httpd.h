#pragma once

#include "hal_log.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_GET  1
#define HTTP_POST 2

typedef struct httpd_req {
    void *sess;
    char uri[256];
    char method[8];
    char query[512];
    char *body;
    size_t body_len;
    int content_len;
    int status;
    char content_type[80];
    char extra_hdr[16][160];
    int extra_hdr_n;
    char *resp;
    size_t resp_len;
    size_t resp_cap;
    int chunked;
    int keep_sock;
    void *user_ctx;
} httpd_req_t;

typedef esp_err_t (*httpd_uri_handler_t)(httpd_req_t *req);

typedef struct {
    const char *uri;
    int method;
    httpd_uri_handler_t handler;
    void *user_ctx;
} httpd_uri_t;

typedef struct {
    uint16_t server_port;
    uint16_t max_open_sockets;
    uint16_t max_uri_handlers;
    uint16_t stack_size;
    int recv_wait_timeout;
    int send_wait_timeout;
    int lru_purge_enable;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG() { 80, 7, 16, 8192, 15, 5, 0 }

typedef void *httpd_handle_t;

esp_err_t httpd_start(httpd_handle_t *out, const httpd_config_t *cfg);
esp_err_t httpd_stop(httpd_handle_t h);
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *uri);

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *str);

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size);
size_t httpd_req_get_url_query_len(httpd_req_t *r);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
int httpd_req_to_sockfd(httpd_req_t *r);
int httpd_send(httpd_req_t *r, const char *buf, size_t buf_len);
esp_err_t httpd_req_async_handler_begin(httpd_req_t *r, httpd_req_t **out);
esp_err_t httpd_req_async_handler_complete(httpd_req_t *r);

#define HTTPD_RESP_USE_STRLEN (-1)
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
esp_err_t httpd_resp_send_err(httpd_req_t *r, int code, const char *msg);

#ifdef __cplusplus
}
#endif
