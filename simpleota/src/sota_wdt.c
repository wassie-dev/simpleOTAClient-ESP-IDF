/*
 * Trial watchdog hardware calls. See sota_wdt.h.
 */

#include "sdkconfig.h"

#include "sota_wdt.h"

#include <inttypes.h>

/* The watchdog's reset is only undone by bootloader rollback. */
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && \
    !CONFIG_SIMPLEOTA_DISABLE_TRIAL_WATCHDOG && __has_include("hal/wdt_hal.h")
#define SOTA_WDT_HW 1
#include "hal/wdt_hal.h"
#include "soc/rtc.h"
#include "esp_log.h"
#include "esp_sleep.h"
#if __has_include("esp_private/esp_clk.h")
#include "esp_private/esp_clk.h"
#define SOTA_SLOWCLK_CAL() esp_clk_slowclk_cal_get()
#else
#define SOTA_SLOWCLK_CAL() 0u
#endif

static const char *TAG = "simpleota";
#else
#define SOTA_WDT_HW 0
#endif

#if SOTA_WDT_HW
static bool s_armed;

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

/* A deep-sleep wake is a new boot, and simpleota_init() arms the watchdog
 * again while the image is still on trial. Stop it before sleeping, so a
 * long sleep does not end in a watchdog reset. */
static void sota_wdt_before_deep_sleep(void)
{
    sota_wdt_disarm();
}
#endif

bool sota_wdt_arm(uint32_t confirm_sec)
{
#if SOTA_WDT_HW
    bool clamped = false;
    uint32_t ticks = sota_wdt_stage_ticks(confirm_sec,
                                          sota_wdt_floor_sec(CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS),
                                          SOTA_SLOWCLK_CAL(),
                                          rtc_clk_slow_freq_get_hz(), &clamped);
    wdt_hal_context_t hal;
    wdt_hal_init(&hal, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&hal);
    wdt_hal_config_stage(&hal, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_set_flashboot_en(&hal, false);
    wdt_hal_enable(&hal);
    wdt_hal_write_protect_enable(&hal);
    s_armed = true;
    if (clamped) {
        ESP_LOGW(TAG, "confirm timeout %" PRIu32 " s is longer than the chip "
                      "watchdog can wait; it resets the chip at its limit",
                 confirm_sec);
    }
    /* Registering the same hook again is a no-op. */
    (void)esp_deep_sleep_register_hook(&sota_wdt_before_deep_sleep);
    return true;
#else
    (void)confirm_sec;
    return false;
#endif
}

void sota_wdt_disarm(void)
{
#if SOTA_WDT_HW
    if (s_armed) sota_wdt_hw_off();
#endif
}

void sota_wdt_quiesce(void)
{
    /* With the bootloader watchdog on (the default), the bootloader
     * reprograms the RTC watchdog on every reset and the startup code or
     * the application owns it afterwards, so leave it alone. */
#if SOTA_WDT_HW && !defined(CONFIG_BOOTLOADER_WDT_ENABLE)
    sota_wdt_hw_off();
#endif
}
