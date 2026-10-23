/**
 * sota_net_idf.c - the core's HTTP transport on esp_http_client.
 */

#include "sota_net_idf.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_timer.h"
#include "sota_net.h"

static void client_config(const sota_net_idf_t *n, const char *url,
                          esp_http_client_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS;
    cfg->buffer_size = 2048;
    /* Pre-signed URLs run to 1.5 KB; the default 512 cannot hold the
     * request line. */
    cfg->buffer_size_tx = CONFIG_SIMPLEOTA_HTTP_TX_BUFFER;
    if (n->cert_pem) {
        cfg->cert_pem = n->cert_pem;
    } else {
        cfg->crt_bundle_attach = n->crt_bundle_attach;
    }
}

int sota_net_post(void *user, const char *url, const char *token, const char *body,
                  char *resp, size_t cap)
{
    /* Sized to the token: a fixed buffer would fail every request for a
     * long token as if the network were down. */
    size_t auth_len = strlen("Bearer ") + strlen(token) + 1;
    char *auth = malloc(auth_len);
    if (!auth) return -1;
    snprintf(auth, auth_len, "Bearer %s", token);

    esp_http_client_config_t cfg;
    client_config((const sota_net_idf_t *)user, url, &cfg);
    cfg.method = HTTP_METHOD_POST;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(auth);
        return -1;
    }
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "application/json");

    int status = -1;
    size_t len = strlen(body);
    if (esp_http_client_open(c, (int)len) == ESP_OK &&
        esp_http_client_write(c, body, (int)len) == (int)len &&
        esp_http_client_fetch_headers(c) >= 0) {
        status = esp_http_client_get_status_code(c);
        size_t total = 0;
        while (resp && total + 1 < cap) {
            int r = esp_http_client_read(c, resp + total, (int)(cap - 1 - total));
            if (r <= 0) break;
            total += (size_t)r;
        }
        if (resp && cap) resp[total] = '\0';
    }
    esp_http_client_cleanup(c);
    free(auth);
    return status;
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 ||
           status == 308;
}

/* No Authorization header: the pre-signed URL carries its own credentials,
 * and the token must not reach the storage host. The streaming API does
 * not follow redirects, so they are walked here. */
int sota_net_dl_open(void *user, const char *url, int64_t *len)
{
    sota_net_idf_t *n = (sota_net_idf_t *)user;
    esp_http_client_config_t cfg;
    client_config(n, url, &cfg);
    cfg.keep_alive_enable = true;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return 0;

    for (int redirects = 0;; ++redirects) {
        if (esp_http_client_open(c, 0) != ESP_OK) {
            esp_http_client_cleanup(c);
            return 0;
        }
        *len = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (is_redirect(status) && redirects < SOTA_NET_MAX_REDIRECTS) {
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            continue;
        }
        if (status != 200) {
            esp_http_client_cleanup(c);
            return status;
        }
        if (*len <= 0) *len = -1;
        n->dl = c;
        n->last_byte_us = esp_timer_get_time();
        return status;
    }
}

int sota_net_dl_read(void *user, uint8_t *buf, size_t cap)
{
    sota_net_idf_t *n = (sota_net_idf_t *)user;
    int r = esp_http_client_read(n->dl, (char *)buf, (int)cap);
    int64_t now = esp_timer_get_time();
    if (r > 0) {
        n->last_byte_us = now;
        return r;
    }
    if (r == 0 && esp_http_client_is_complete_data_received(n->dl)) return 0;
    bool stalled = now - n->last_byte_us >= (int64_t)CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS * 1000;
    return stalled ? SOTA_NET_STALLED : SOTA_NET_ERROR;
}

void sota_net_dl_close(void *user)
{
    sota_net_idf_t *n = (sota_net_idf_t *)user;
    if (n->dl) esp_http_client_cleanup(n->dl);
    n->dl = NULL;
}
