/**
 * @file
 * @brief HTTP, implemented by the platform port (esp_http_client on ESP32,
 * for both clients) and by the host tests' fake.
 *
 * user is the sota_config_t's user pointer, where the port keeps its TLS
 * settings. TLS trust is entirely the port's.
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

/** A status sota_net_dl_open() returns for a redirect. */
static inline int sota_net_is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/** sota_net_dl_read() results besides a byte count. */
enum {
    SOTA_NET_ERROR = -1, /**< The connection failed. */
    SOTA_NET_AGAIN = -2, /**< No byte yet: the core calls again, and gives up
                              with download_stalled after http_timeout_ms. */
};

/** A Retry-After header's value in seconds, saturated; 0 when it is not a
 *  plain number of seconds (the HTTP-date form needs a clock). */
static inline uint32_t sota_net_parse_retry_after(const char *value)
{
    uint32_t s = 0;
    while (*value == ' ' || *value == '\t') ++value;
    if (*value < '0' || *value > '9') return 0;
    for (; *value >= '0' && *value <= '9'; ++value) {
        uint32_t d = (uint32_t)(*value - '0');
        s = s > (UINT32_MAX - d) / 10u ? UINT32_MAX : s * 10u + d;
    }
    while (*value == ' ' || *value == '\t') ++value;
    return *value ? 0 : s;
}

/**
 * POST body as JSON to url, with "Authorization: Bearer <token>". One
 * attempt: the engine retries. Returns the HTTP status, or -1 when no
 * response came back. When resp is not NULL it gets up to cap - 1 bytes of
 * the response body, NUL-terminated. When retry_after_s is not NULL it
 * gets the response's Retry-After header (sota_net_parse_retry_after()),
 * 0 when there is none.
 */
int sota_net_post(void *user, const char *url, const char *token, const char *body,
                  char *resp, size_t cap, uint32_t *retry_after_s);

/**
 * Start a firmware download: a GET without the Authorization header (the
 * pre-signed URL carries its own credentials). Returns the HTTP status, or
 * 0 when no connection was made. On 200, *len is the Content-Length or -1;
 * on anything else the download is already closed.
 *
 * Redirects are not followed here: the engine checks where each one leads.
 * On a redirect status, location gets the Location header, NUL-terminated,
 * or an empty string when there is none or it does not fit location_cap.
 */
int sota_net_dl_open(void *user, const char *url, int64_t *len, char *location,
                     size_t location_cap);

/** Read the next bytes of the body: a count, 0 once the whole body has
 *  arrived, SOTA_NET_AGAIN, or SOTA_NET_ERROR. The core times the stall, so
 *  an error after http_timeout_ms with no byte is reported as a stall. */
int sota_net_dl_read(void *user, uint8_t *buf, size_t cap);

/** End the download and free its connection. */
void sota_net_dl_close(void *user);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_NET_H */
