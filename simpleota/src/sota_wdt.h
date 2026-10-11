/**
 * sota_wdt.h - trial watchdog deadline and rolled_back cause.
 */

#ifndef SOTA_WDT_H
#define SOTA_WDT_H

#include <stdbool.h>
#include <stdint.h>

/** Smallest extra seconds added to the confirm timeout when arming. */
#define SOTA_TRIAL_WDT_MARGIN_S 60u

/** Numeric value of ESP_RST_WDT in esp_reset_reason_t. */
#define SOTA_RST_WDT 7

/** Extra watchdog time: the larger of floor_sec and 10 percent of confirm_sec. */
static inline uint32_t sota_wdt_margin_sec(uint32_t confirm_sec, uint32_t floor_sec)
{
    uint32_t tenth = confirm_sec / 10u;
    return tenth > floor_sec ? tenth : floor_sec;
}

/**
 * Slow-clock ticks for a duration. cal_q13_19 is the cached calibration
 * (microseconds per tick, Q13.19). Returns 0 when cal is 0 so the caller
 * can fall back to the nominal frequency, and saturates instead of
 * overflowing.
 */
static inline uint64_t sota_slowclk_ticks(uint64_t timeout_us, uint32_t cal_q13_19)
{
    if (cal_q13_19 == 0) return 0;
    if (timeout_us > (UINT64_MAX >> 19)) return UINT64_MAX;
    return (timeout_us << 19) / (uint64_t)cal_q13_19;
}

/**
 * Nominal slow-clock ticks, used when the calibration value is still 0.
 * The result is clamped to the 32-bit stage and is at least 2 ticks.
 */
static inline uint32_t sota_nominal_slow_ticks(uint64_t seconds, uint32_t hz)
{
    if (hz == 0) hz = 1;
    uint64_t ticks = seconds * (uint64_t)hz;
    if (ticks > 0xFFFFFFFFu) return 0xFFFFFFFFu;
    if (ticks < 2) return 2;
    return (uint32_t)ticks;
}

/**
 * Cause stored with a rolled_back report when this boot is running a
 * different image than the one the trial flashed.
 *
 * image_invalid is true when the new slot is in the INVALID state, which
 * only the component's confirm-timeout path sets: "confirm_timeout".
 * A chip-watchdog reset (ESP_RST_WDT, code 7): "watchdog". Interrupt and
 * task watchdog resets (ESP_RST_INT_WDT code 5, ESP_RST_TASK_WDT code 6)
 * stay "boot_failed", as does any other abandon.
 */
static inline const char *sota_rollback_cause(bool image_invalid, int reset_code)
{
    if (image_invalid) return "confirm_timeout";
    if (reset_code == (int)SOTA_RST_WDT) return "watchdog";
    return "boot_failed";
}

/**
 * Arm the RTC/LP watchdog for confirm_sec plus the margin, and stop it
 * again before deep sleep. Logs a warning when the deadline is longer
 * than the watchdog can wait. Returns true when the hardware is running.
 * Returns false when the watchdog is opted out or this target has no
 * wdt_hal.
 */
bool sota_wdt_arm(uint32_t confirm_sec);

/** Disable the watchdog if this component armed it. */
void sota_wdt_disarm(void);

/**
 * Stop a watchdog left running across a reset, on a build where the
 * bootloader does not reprogram it (CONFIG_BOOTLOADER_WDT_ENABLE off).
 * Does nothing otherwise.
 */
void sota_wdt_quiesce(void);

#endif /* SOTA_WDT_H */
