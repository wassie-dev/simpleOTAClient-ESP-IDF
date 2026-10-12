/**
 * sota_report.h - delivery and polling policy for deferred status reports.
 *
 * "confirmed" and "rolled_back" are telemetry. What the server says about
 * them never changes what the device does. A report is delivered once the
 * server returns any HTTP response. Without a response it stays pending
 * and is sent again on later checks: a few quick retries, then at the
 * normal check interval.
 *
 * No ESP-IDF types, so the host tests can include this header.
 */

#ifndef SOTA_REPORT_H
#define SOTA_REPORT_H

#include <stdbool.h>
#include <stdint.h>

/** Quick retries for a pending report before it waits for the check interval. */
#define SOTA_REPORT_QUICK_RETRIES 3u

/** Seconds between is_connected() polls while the network is down. */
#define SOTA_CONNECT_POLL_S 1u

/** A report is delivered once the server returns any HTTP response. */
static inline bool sota_report_delivered(int http_status)
{
    return http_status > 0;
}

/**
 * Seconds until the next check.
 *
 * connected:       is_connected() allowed the check that just ran. While it
 *                  does not, it is polled every SOTA_CONNECT_POLL_S, so the
 *                  check runs as soon as the network is up instead of a
 *                  whole check interval later.
 * in_trial:        a trial is waiting for its first successful check-in.
 * report_pending:  a confirmed or rolled_back report is waiting to be sent.
 * report_failures: sends of that report that got no response this boot.
 */
static inline uint32_t sota_poll_interval_s(bool connected, bool in_trial,
                                            bool report_pending,
                                            uint32_t report_failures,
                                            uint32_t quick_s,
                                            uint32_t check_interval_s)
{
    if (!connected) return SOTA_CONNECT_POLL_S;
    if (in_trial) return quick_s;
    if (report_pending && report_failures < SOTA_REPORT_QUICK_RETRIES) return quick_s;
    return check_interval_s;
}

#endif /* SOTA_REPORT_H */
