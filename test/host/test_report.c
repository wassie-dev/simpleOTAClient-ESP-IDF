/**
 * Host checks for tick conversion and the deferred-report policy.
 *
 *   cc -std=c99 -Wall -Wextra -Werror -I../../simpleota/src -o test_report test_report.c && ./test_report
 */

#include <stdio.h>

#include "sota_report.h"
#include "sota_time.h"

static int g_failures = 0;

static void check(int cond, const char *msg)
{
    printf("%s  %s\n", cond ? "  ok" : "FAIL", msg);
    if (!cond) g_failures++;
}

int main(void)
{
    check(sota_ms_to_ticks(3600000u, 1000) == 3600000u, "1 h at 1000 Hz");
    check(sota_ms_to_ticks(21600000u, 1000) == 21600000u,
          "6 h at 1000 Hz does not wrap");
    check(sota_ms_to_ticks(86400000u, 100) == 8640000u, "24 h at 100 Hz");
    check(sota_ms_to_ticks(0xFFFFFFFFull * 1000u, 1000) == SOTA_MAX_WAIT_TICKS,
          "a duration past the tick range saturates");
    check(SOTA_MAX_WAIT_TICKS != UINT32_MAX,
          "a saturated wait is never portMAX_DELAY, which waits forever");
    check(sota_ms_to_ticks(4294967294ull, 1000) == 4294967294u &&
              sota_ms_to_ticks(4294967295ull, 1000) == SOTA_MAX_WAIT_TICKS,
          "saturation boundary at 1000 Hz");
    check(sota_ms_to_ticks(42949672940ull, 100) == 4294967294u &&
              sota_ms_to_ticks(42949672960ull, 100) == SOTA_MAX_WAIT_TICKS,
          "saturation boundary at 100 Hz");
    check(sota_ms_to_ticks(15, 100) == 1u, "partial ticks round down");

    check(sota_report_delivered(200), "a 2xx delivers the report");
    check(sota_report_delivered(404), "a 4xx delivers the report");
    check(sota_report_delivered(503), "a 5xx delivers the report");
    check(!sota_report_delivered(-1), "no response keeps the report");
    check(!sota_report_delivered(0), "status 0 keeps the report");

    check(sota_poll_interval_s(true, true, false, 0, 10, 3600) == 10,
          "a trial polls fast");
    check(sota_poll_interval_s(true, false, true, 0, 10, 3600) == 10,
          "a new pending report polls fast");
    check(sota_poll_interval_s(true, false, true, SOTA_REPORT_QUICK_RETRIES - 1, 10, 3600) == 10,
          "the last quick retry is still fast");
    check(sota_poll_interval_s(true, false, true, SOTA_REPORT_QUICK_RETRIES, 10, 3600) == 3600,
          "after the quick retries, a pending report waits the check interval");
    check(sota_poll_interval_s(true, false, false, 0, 10, 3600) == 3600,
          "steady state waits the check interval");
    check(sota_poll_interval_s(false, false, false, 0, 10, 21600) == SOTA_CONNECT_POLL_S,
          "no network: poll again shortly, not a whole interval later");
    check(sota_poll_interval_s(false, true, false, 0, 10, 3600) == SOTA_CONNECT_POLL_S &&
              sota_poll_interval_s(false, false, true, 5, 10, 3600) == SOTA_CONNECT_POLL_S,
          "no network: shortly, also during a trial or with a report pending");

    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("report and tick host checks passed\n");
    return 0;
}
