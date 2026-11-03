#pragma once

#include "hal_log.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_METHOD_GET  0
#define HTTP_METHOD_POST 1
#define HTTP_EVENT_ON_DATA 1

typedef struct {
    int event_id;
    void *data;
    int data_len;
    void *user_data;
} esp_http_client_event_t;

typedef esp_err_t (*esp_http_client_event_handle_t)(esp_http_client_event_t *evt);

typedef struct {
    const char *url;
    int method;
    int timeout_ms;
    void *crt_bundle_attach;
    esp_http_client_event_handle_t event_handler;
    void *user_data;
    int buffer_size_tx;
    int disable_auto_redirect;
} esp_http_client_config_t;

typedef void *esp_http_client_handle_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *val);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len);
int esp_http_client_write(esp_http_client_handle_t c, const char *buf, int len);
int esp_http_client_fetch_headers(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
int esp_http_client_get_content_length(esp_http_client_handle_t c);
int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);

#ifdef __cplusplus
}
#endif
