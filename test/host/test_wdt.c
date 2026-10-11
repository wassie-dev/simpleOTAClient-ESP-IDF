/**
 * Host checks for the trial watchdog deadline and the rolled_back cause.
 *
 *   cc -std=c99 -Wall -Wextra -Werror -I../../simpleota/src -o test_wdt test_wdt.c && ./test_wdt
 */

#include <stdio.h>
#include <string.h>

#include "sota_wdt.h"

static int g_failures = 0;

static void check(int cond, const char *msg)
{
    if (cond) {
        printf("  ok  %s\n", msg);
        return;
    }
    printf("  FAIL %s\n", msg);
    g_failures++;
}

int main(void)
{
    check(sota_wdt_margin_sec(300, 60) == 60u, "300 s margin stays at 60 s");
    check(sota_wdt_margin_sec(7200, 60) == 720u, "7200 s margin is 10 percent");
    check(sota_wdt_margin_sec(100, 60) == 60u, "short timeout keeps the 60 s floor");
    check(sota_wdt_margin_sec(86400, 60) == 8640u, "86400 s margin is 10 percent");

    const uint64_t us = 360ull * 1000000ull;
    const uint32_t cal = (uint32_t)(((uint64_t)1000000 << 19) / 150000u);
    uint64_t ticks = sota_slowclk_ticks(us, cal);
    check(ticks > 53000000ull && ticks < 55000000ull,
          "360 s at a 150 kHz calibration is about 54e6 ticks");
    check(sota_slowclk_ticks(us, 0) == 0, "a zero calibration asks for the nominal fallback");
    /* The shift guard is timeout_us > (UINT64_MAX >> 19). Just under it
     * still takes the multiply path; one past it saturates. */
    const uint64_t shift_limit = UINT64_MAX >> 19;
    const uint64_t just_under = sota_slowclk_ticks(shift_limit, cal);
    const uint64_t just_under_expect = (shift_limit << 19) / (uint64_t)cal;
    check(just_under == just_under_expect && just_under != UINT64_MAX,
          "a duration just under the shift limit is computed");
    check(sota_slowclk_ticks(shift_limit + 1, cal) == UINT64_MAX,
          "one past the shift limit saturates");
    check(sota_nominal_slow_ticks(360, 150000) == 54000000u, "nominal 360 s at 150 kHz");
    check(sota_nominal_slow_ticks(100000, 150000) == 0xFFFFFFFFu,
          "a deadline past the 32-bit stage clamps");
    check(sota_nominal_slow_ticks(0, 150000) == 2u, "a zero-length stage is at least 2 ticks");

    check(strcmp(sota_rollback_cause(true, SOTA_RST_WDT), "confirm_timeout") == 0,
          "an image this component invalidated stays confirm_timeout");
    check(strcmp(sota_rollback_cause(false, SOTA_RST_WDT), "watchdog") == 0,
          "a chip watchdog reset is reason watchdog");
    check(strcmp(sota_rollback_cause(false, 4), "boot_failed") == 0,
          "any other abandon stays boot_failed");
    check(strcmp(sota_rollback_cause(false, 5), "boot_failed") == 0,
          "an interrupt watchdog reset (code 5) stays boot_failed");
    check(strcmp(sota_rollback_cause(false, 6), "boot_failed") == 0,
          "a task watchdog reset (code 6) stays boot_failed");
    check(strcmp(sota_rollback_cause(true, 5), "confirm_timeout") == 0,
          "code 5 does not override an image this component invalidated");
    check(strcmp(sota_rollback_cause(true, 6), "confirm_timeout") == 0,
          "code 6 does not override an image this component invalidated");

    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("watchdog host checks passed\n");
    return 0;
}
