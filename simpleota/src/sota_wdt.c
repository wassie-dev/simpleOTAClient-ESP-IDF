/*
 * Chip RTC/LP watchdog for a SimpleOTA trial.
 *
 * simpleota_init arms it while the running image is still pending
 * verification, for the confirm timeout plus a margin (the larger of
 * 60 seconds and 10 percent). The deadline uses the calibrated slow
 * clock. Confirm, the component's own rollback, and deep sleep disable
 * it. Other boots leave it alone, except to stop one left running by a
 * reset when the bootloader watchdog is off.
 */

#include "sdkconfig.h"

#include "sota_wdt.h"

#include <inttypes.h>

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && !CONFIG_SIMPLEOTA_DISABLE_TRIAL_WATCHDOG
#define SOTA_WDT_WANTED 1
#else
#define SOTA_WDT_WANTED 0
#endif

#if SOTA_WDT_WANTED && __has_include("hal/wdt_hal.h")
#define SOTA_WDT_HW 1
#include "hal/wdt_hal.h"
#include "soc/rtc.h"
#include "esp_sleep.h"
#if __has_include("esp_private/esp_clk.h")
#include "esp_private/esp_clk.h"
#define SOTA_HAVE_SLOWCLK_CAL 1
#else
#define SOTA_HAVE_SLOWCLK_CAL 0
#endif
#else
#define SOTA_WDT_HW 0
#endif

#if SOTA_WDT_WANTED
#include "esp_log.h"

static const char *TAG = "simpleota";
#endif

static bool s_armed;

#if SOTA_WDT_HW
/* Stage 0 value for the deadline plus margin. Sets *clamped when the
 * 32-bit stage cannot hold the whole deadline. */
static uint32_t sota_wdt_ticks(uint32_t confirm_sec, bool *clamped)
{
    uint32_t margin = sota_wdt_margin_sec(confirm_sec, SOTA_TRIAL_WDT_MARGIN_S);
    uint64_t total_sec = (uint64_t)confirm_sec + (uint64_t)margin;
    uint64_t ticks = 0;
#if SOTA_HAVE_SLOWCLK_CAL
    ticks = sota_slowclk_ticks(total_sec * 1000000ull, esp_clk_slowclk_cal_get());
#endif
    if (ticks == 0) {
        ticks = sota_nominal_slow_ticks(total_sec, rtc_clk_slow_freq_get_hz());
    }
    *clamped = ticks >= 0xFFFFFFFFu;
    if (*clamped) return 0xFFFFFFFFu;
    if (ticks < 2) return 2;
    return (uint32_t)ticks;
}

static void sota_wdt_hw_off(void)
{
    wdt_hal_context_t hal;
    wdt_hal_init(&hal, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&hal);
    wdt_hal_set_flashboot_en(&hal, false);
    wdt_hal_disable(&hal);
    wdt_hal_write_protect_enable(&hal);
    s_armed = false;
}

/* A deep-sleep wake is a new boot, and simpleota_init arms the watchdog
 * again if the image is still on trial. Stop it before sleeping so a long
 * sleep does not end in a watchdog reset. */
static void sota_wdt_before_deep_sleep(void)
{
    sota_wdt_disarm();
}
#endif

bool sota_wdt_arm(uint32_t confirm_sec)
{
#if SOTA_WDT_HW
    bool clamped = false;
    uint32_t ticks = sota_wdt_ticks(confirm_sec, &clamped);
    wdt_hal_context_t hal;
    wdt_hal_init(&hal, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&hal);
    wdt_hal_config_stage(&hal, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_set_flashboot_en(&hal, false);
    wdt_hal_enable(&hal);
    wdt_hal_write_protect_enable(&hal);
    s_armed = true;
    if (clamped) {
        ESP_LOGW(TAG,
                 "confirm timeout %" PRIu32 " s is longer than the chip "
                 "watchdog can wait; it resets the chip at its maximum",
                 confirm_sec);
    }
    /* Registering the same hook again is a no-op. If the hook table is
     * full, the README already asks for confirm before deep sleep. */
    (void)esp_deep_sleep_register_hook(&sota_wdt_before_deep_sleep);
    return true;
#else
    (void)confirm_sec;
#if SOTA_WDT_WANTED
    ESP_LOGW(TAG,
             "chip watchdog is not running; a hang during this trial "
             "is not reset by the component");
#endif
    s_armed = false;
    return false;
#endif
}

void sota_wdt_disarm(void)
{
#if SOTA_WDT_HW
    if (!s_armed) return;
    sota_wdt_hw_off();
#else
    s_armed = false;
#endif
}

void sota_wdt_quiesce(void)
{
    /* With the bootloader watchdog on, the bootloader reprograms the RTC
     * watchdog on every reset and the startup code or the application owns
     * it afterwards, so leave it alone. Without it, a watchdog armed before
     * a software or panic reset can still be counting. */
#if SOTA_WDT_HW && !defined(CONFIG_BOOTLOADER_WDT_ENABLE)
    sota_wdt_hw_off();
#endif
}
