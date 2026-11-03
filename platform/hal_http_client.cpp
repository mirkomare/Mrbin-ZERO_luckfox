#include "hal_http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

typedef struct {
    char url[256];
    int method;
    int timeout_ms;
    char headers[16][160];
    int n_hdr;
    char *body;
    size_t body_len;
    char *resp;
    size_t resp_len;
    int status;
    esp_http_client_event_handle_t evt;
    void *user;
    int opened;
} cli_t;

#ifdef HAVE_LIBCURL
static size_t curl_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    cli_t *c = (cli_t *)ud;
    size_t n = size * nmemb;
    char *p = (char *)realloc(c->resp, c->resp_len + n + 1);
    if (!p) return 0;
    c->resp = p;
    memcpy(c->resp + c->resp_len, ptr, n);
    c->resp_len += n;
    c->resp[c->resp_len] = 0;
    if (c->evt) {
        esp_http_client_event_t e = {};
        e.event_id = HTTP_EVENT_ON_DATA;
        e.data = ptr;
        e.data_len = (int)n;
        e.user_data = c->user;
        c->evt(&e);
    }
    return n;
}
#endif

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    cli_t *c = (cli_t *)calloc(1, sizeof(*c));
    if (cfg) {
        snprintf(c->url, sizeof(c->url), "%s", cfg->url ? cfg->url : "");
        c->method = cfg->method;
        c->timeout_ms = cfg->timeout_ms ? cfg->timeout_ms : 20000;
        c->evt = cfg->event_handler;
        c->user = cfg->user_data;
    }
    return c;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t h, const char *key, const char *val) {
    cli_t *c = (cli_t *)h;
    if (!c || c->n_hdr >= 16) return ESP_FAIL;
    snprintf(c->headers[c->n_hdr++], 160, "%s: %s", key, val);
    return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t h, int write_len) {
    cli_t *c = (cli_t *)h;
    if (!c) return ESP_FAIL;
    if (write_len > 0) {
        free(c->body);
        c->body = (char *)calloc(1, (size_t)write_len + 1);
        c->body_len = 0;
    }
    c->opened = 1;
    return ESP_OK;
}

int esp_http_client_write(esp_http_client_handle_t h, const char *buf, int len) {
    cli_t *c = (cli_t *)h;
    if (!c || !c->body) return -1;
    memcpy(c->body + c->body_len, buf, (size_t)len);
    c->body_len += (size_t)len;
    return len;
}

int esp_http_client_fetch_headers(esp_http_client_handle_t h) {
    cli_t *c = (cli_t *)h;
    if (!c) return -1;
#ifdef HAVE_LIBCURL
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    curl_easy_setopt(curl, CURLOPT_URL, c->url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)c->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, c);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    struct curl_slist *hdrs = nullptr;
    for (int i = 0; i < c->n_hdr; ++i) hdrs = curl_slist_append(hdrs, c->headers[i]);
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    if (c->method == HTTP_METHOD_POST) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, c->body ? c->body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)c->body_len);
    }
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    c->status = (rc == CURLE_OK) ? (int)code : -1;
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return c->status;
#else
    (void)c;
    return -1;
#endif
}

int esp_http_client_get_status_code(esp_http_client_handle_t h) {
    cli_t *c = (cli_t *)h;
    return c ? c->status : -1;
}
int esp_http_client_get_content_length(esp_http_client_handle_t h) {
    cli_t *c = (cli_t *)h;
    return c ? (int)c->resp_len : 0;
}
int esp_http_client_read(esp_http_client_handle_t h, char *buf, int len) {
    cli_t *c = (cli_t *)h;
    if (!c || !buf || !c->resp) return 0;
    int n = (int)c->resp_len;
    if (n > len) n = len;
    memcpy(buf, c->resp, (size_t)n);
    return n;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t) { return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h) {
    cli_t *c = (cli_t *)h;
    if (!c) return ESP_OK;
    free(c->body);
    free(c->resp);
    free(c);
    return ESP_OK;
}
