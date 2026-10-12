/**
 * Host checks for the trial watchdog stage value.
 *
 *   cc -std=c99 -Wall -Wextra -Werror -I../../simpleota/src -o test_wdt test_wdt.c && ./test_wdt
 */

#include <stdio.h>

#include "sota_wdt.h"

static int g_failures = 0;

static void check(int cond, const char *msg)
{
    printf("%s  %s\n", cond ? "  ok" : "FAIL", msg);
    if (!cond) g_failures++;
}

static uint32_t cal(uint32_t hz) { return (uint32_t)((1000000ull << 19) / hz); }

int main(void)
{
    bool clamped = false;
    check(sota_wdt_margin_sec(300, 60) == 60u, "300 s gets the 60 s floor");
    check(sota_wdt_margin_sec(7200, 60) == 720u, "7200 s gets 10 percent");
    check(sota_wdt_stage_ticks(300, 60, 0, 150000, &clamped) == 54000000u && !clamped,
          "300 s + 60 s at a nominal 150 kHz");
    uint32_t c = sota_wdt_stage_ticks(300, 60, cal(150000), 0, &clamped);
    check(c > 53990000u && c < 54010000u && !clamped,
          "the calibrated clock gives the same deadline");
    check(sota_wdt_stage_ticks(26030, 60, 0, 150000, &clamped) == 4294950000u && !clamped,
          "about 7.95 h at 150 kHz still fits");
    check(sota_wdt_stage_ticks(26031, 60, 0, 150000, &clamped) == UINT32_MAX && clamped,
          "one second more is clamped and reported");
    check(sota_wdt_stage_ticks(UINT32_MAX, 60, cal(150000), 0, &clamped) == UINT32_MAX &&
              clamped,
          "a huge timeout saturates instead of overflowing");
    check(sota_wdt_stage_ticks(0, 0, 0, 0, &clamped) == 2u, "never shorter than 2 ticks");
    check(sota_wdt_floor_sec(15000) == 70u,
          "the default 15 s HTTP timeout: one request takes up to 70 s");
    check(sota_wdt_floor_sec(5000) == SOTA_TRIAL_WDT_MARGIN_S,
          "a short HTTP timeout keeps the 60 s floor");
    check(sota_wdt_floor_sec(60000) == 250u, "a 60 s HTTP timeout");
    check(sota_wdt_floor_sec(15001) == 71u, "partial seconds round up");
    check(sota_wdt_floor_sec(UINT32_MAX) > SOTA_TRIAL_WDT_MARGIN_S,
          "the largest HTTP timeout does not overflow");
    check(sota_wdt_margin_sec(300, sota_wdt_floor_sec(15000)) == 70u &&
              sota_wdt_margin_sec(7200, sota_wdt_floor_sec(15000)) == 720u,
          "the floor applies until 10 percent is larger");

    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("watchdog host checks passed\n");
    return 0;
}
