/**
 * sota_sys_esp32.c - sota_sys.h on FreeRTOS and esp_timer. The watchdog is
 * in sota_wdt_esp32.c.
 */

#include "sota_sys.h"

#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sota_engine.h"
#include "sota_time.h"

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static portMUX_TYPE s_lock_init = portMUX_INITIALIZER_UNLOCKED;

void sota_sys_sleep_ms(uint32_t ms)
{
    vTaskDelay(sota_ms_to_ticks(ms, configTICK_RATE_HZ));
}

/* Created on first use: an Arduino client object can be constructed
 * before the scheduler runs. */
void sota_sys_lock(void)
{
    if (!s_lock) {
        portENTER_CRITICAL(&s_lock_init);
        if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        portEXIT_CRITICAL(&s_lock_init);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void sota_sys_unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* The esp_timer task only notes the timeout: the rollback runs where the
 * application's wake-up sends it. */
static void on_timeout(void *arg)
{
    sota_engine_timeout_fired((sota_engine_t *)arg);
}

bool sota_sys_timer_start(void **timer, uint32_t seconds, struct sota_engine *e)
{
    sota_sys_timer_stop(timer);
    const esp_timer_create_args_t args = {
        .callback = on_timeout,
        .arg = e,
        .name = "sota_confirm",
    };
    esp_timer_handle_t h;
    if (esp_timer_create(&args, &h) != ESP_OK) return false;
    if (esp_timer_start_once(h, (uint64_t)seconds * 1000000u) != ESP_OK) {
        esp_timer_delete(h);
        return false;
    }
    *timer = h;
    return true;
}

void sota_sys_timer_stop(void **timer)
{
    if (!*timer) return;
    esp_timer_stop((esp_timer_handle_t)*timer);
    esp_timer_delete((esp_timer_handle_t)*timer);
    *timer = NULL;
}

void sota_sys_restart(void)
{
    esp_restart();
}
