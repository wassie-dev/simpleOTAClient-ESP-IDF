/**
 * sota_net.h - HTTP, implemented by each client's adapter (HTTPClient on
 * Arduino, esp_http_client on ESP-IDF) and by the host tests' fake.
 *
 * user is the sota_config_t's user pointer, so an adapter can keep its TLS
 * settings per engine. TLS trust is entirely the adapter's.
 */

#ifndef SOTA_NET_H
#define SOTA_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Redirects a download follows before it fails with their status. */
#define SOTA_NET_MAX_REDIRECTS 3

/** sota_net_dl_read() results besides a byte count. */
enum {
    SOTA_NET_ERROR = -1,   /**< The connection failed. */
    SOTA_NET_STALLED = -2, /**< No byte arrived within the transport timeout. */
};

/**
 * POST body as JSON to url, with "Authorization: Bearer <token>". One
 * attempt: the engine retries. Returns the HTTP status, or -1 when no
 * response came back. When resp is not NULL it gets up to cap - 1 bytes of
 * the response body, NUL-terminated.
 */
int sota_net_post(void *user, const char *url, const char *token, const char *body,
                  char *resp, size_t cap);

/**
 * Start a firmware download: a GET without the Authorization header (the
 * pre-signed URL carries its own credentials), following up to
 * SOTA_NET_MAX_REDIRECTS redirects. Returns the final HTTP status, or 0
 * when no connection was made. On 200, *len is the Content-Length or -1;
 * on anything else the download is already closed.
 */
int sota_net_dl_open(void *user, const char *url, int64_t *len);

/** Read the next bytes of the body: a count, 0 once the whole body has
 *  arrived, or SOTA_NET_ERROR / SOTA_NET_STALLED. */
int sota_net_dl_read(void *user, uint8_t *buf, size_t cap);

/** End the download and free its connection. */
void sota_net_dl_close(void *user);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_NET_H */
