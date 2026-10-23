/**
 * simpleota.c - the ESP-IDF client: the public API, the managed task, and
 * the core's application interface. The update logic is the shared core's
 * (src/core, vendored from SimpleOTAClient-core), so the protocol, the
 * trial record in NVS and the rollback rules match the Arduino client.
 *
 * Threading model: one background task ("simpleota") owns all network and
 * flash activity. Public API calls only notify it, except
 * simpleota_confirm(), which records the confirm on the caller's task, and
 * simpleota_reboot_for_update(), which stops the background task and posts
 * its status from a short-lived task of the same stack size.
 */

#include "simpleota.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "sota_app.h"
#include "sota_engine.h"
#include "sota_net_idf.h"
#include "sota_time.h"

_Static_assert((int)SOTA_EVENT_CHECK_DONE == (int)SIMPLEOTA_EVENT_CHECK_DONE &&
                   (int)SOTA_EVENT_UPDATE_AVAILABLE == (int)SIMPLEOTA_EVENT_UPDATE_AVAILABLE &&
                   (int)SOTA_EVENT_DOWNLOAD_PROGRESS == (int)SIMPLEOTA_EVENT_DOWNLOAD_PROGRESS &&
                   (int)SOTA_EVENT_UPDATE_FAILED == (int)SIMPLEOTA_EVENT_UPDATE_FAILED &&
                   (int)SOTA_EVENT_BEFORE_REBOOT == (int)SIMPLEOTA_EVENT_BEFORE_REBOOT &&
                   (int)SOTA_EVENT_TRIAL_BOOT == (int)SIMPLEOTA_EVENT_TRIAL_BOOT &&
                   (int)SOTA_EVENT_CONFIRMED == (int)SIMPLEOTA_EVENT_CONFIRMED &&
                   (int)SOTA_EVENT_ROLLING_BACK == (int)SIMPLEOTA_EVENT_ROLLING_BACK,
               "sota_app.h events match simpleota_event_id_t");

static const char *TAG = "simpleota";

/* Without bootloader rollback, the engine switches back itself. */
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE || CONFIG_SIMPLEOTA_SOFTWARE_ROLLBACK
#define SOTA_ROLLBACK 1
#else
#define SOTA_ROLLBACK 0
#endif

/* The config's strings, copied at init: the engine borrows them. */
enum {
    STR_TOKEN,
    STR_BASE_URL,
    STR_DEVICE_ID,
    STR_CHIP_FAMILY,
    STR_BOARD_ID,
    STR_HARDWARE_REVISION,
    STR_PARTITION_PROFILE,
    STR_CHANNEL,
    STR_LABELS_JSON,
    STR_SECURITY_MODE,
    STR_VERSION_LABEL,
    STR_CERT_PEM,
    STR_COUNT,
};

typedef struct {
    sota_net_idf_t net; /* First: the engine's user pointer is this struct. */
    sota_engine_t engine;
    char *str[STR_COUNT];
    bool initialized;
    uint32_t initial_delay_ms;
    bool (*is_connected)(void *user_ctx);
    void (*event_cb)(const simpleota_event_t *event, void *user_ctx);
    void *user_ctx;
    TaskHandle_t task;
    SemaphoreHandle_t stopped_sem;
    volatile bool stop_requested;
} simpleota_t;

static simpleota_t s;

/* ---------------------------------------------------------------------------
 * sota_app.h
 * ------------------------------------------------------------------------- */

void sota_app_event(void *user, const sota_event_t *ev)
{
    simpleota_t *c = (simpleota_t *)user;
    if (!c->event_cb) return;
    simpleota_event_t out = {
        .id = (simpleota_event_id_t)ev->id,
        .reason = ev->reason,
        .build_number = ev->build,
        .bytes_done = ev->done,
        .bytes_total = ev->total,
    };
    c->event_cb(&out, c->user_ctx);
}

void sota_app_wake(void *user)
{
    simpleota_t *c = (simpleota_t *)user;
    if (c->task) xTaskNotifyGive(c->task);
}

/* The core's levels are esp_log_level_t's. */
void sota_app_log(int level, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ESP_LOG_LEVEL((esp_log_level_t)level, TAG, "%s", line);
}

/* ---------------------------------------------------------------------------
 * Managed task
 * ------------------------------------------------------------------------- */

static void sota_task(void *arg)
{
    (void)arg;
    /* An overdue confirm timeout outranks the initial delay. */
    if (s.initial_delay_ms && !s.engine.timeout_pending) {
        ulTaskNotifyTake(pdTRUE, sota_ms_to_ticks(s.initial_delay_ms, configTICK_RATE_HZ));
    }
    while (!s.stop_requested) {
        bool connected = !s.is_connected || s.is_connected(s.user_ctx);
        uint32_t wait_s = sota_engine_step(&s.engine, connected, NULL);
        ulTaskNotifyTake(pdTRUE, sota_ms_to_ticks((uint64_t)wait_s * 1000u, configTICK_RATE_HZ));
    }
    s.task = NULL;
    xSemaphoreGive(s.stopped_sem);
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Byte-identical to the Arduino client's, so a device keeps its identity
 * across a framework migration. */
static void default_device_id(char out[18])
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
}

static bool copy_strings(const simpleota_config_t *cfg)
{
    char mac[18];
    if (!cfg->device_id) default_device_id(mac);
    const char *src[STR_COUNT] = {
        [STR_TOKEN] = cfg->token,
        [STR_BASE_URL] = cfg->base_url,
        [STR_DEVICE_ID] = cfg->device_id ? cfg->device_id : mac,
        [STR_CHIP_FAMILY] = cfg->chip_family ? cfg->chip_family : CONFIG_IDF_TARGET,
        [STR_BOARD_ID] = cfg->board_id,
        [STR_HARDWARE_REVISION] = cfg->hardware_revision,
        [STR_PARTITION_PROFILE] = cfg->partition_profile,
        [STR_CHANNEL] = cfg->channel,
        [STR_LABELS_JSON] = cfg->labels_json,
        [STR_SECURITY_MODE] = cfg->security_mode,
        [STR_VERSION_LABEL] = cfg->version_label,
        [STR_CERT_PEM] = cfg->cert_pem,
    };
    for (int i = 0; i < STR_COUNT; ++i) {
        if (src[i] && !(s.str[i] = strdup(src[i]))) return false;
    }
    return true;
}

static esp_err_t init_engine(const simpleota_config_t *config)
{
    sota_config_t cfg = {
        .token = s.str[STR_TOKEN],
        .server_url = s.str[STR_BASE_URL],
        .device =
            {
                .device_id = s.str[STR_DEVICE_ID],
                .framework = "esp_idf",
                .chip_family = s.str[STR_CHIP_FAMILY],
                .board_id = s.str[STR_BOARD_ID],
                .hardware_revision = s.str[STR_HARDWARE_REVISION],
                .partition_profile = s.str[STR_PARTITION_PROFILE],
                .channel = s.str[STR_CHANNEL],
                .security_mode = s.str[STR_SECURITY_MODE],
                .labels_json = s.str[STR_LABELS_JSON],
                .nvs_schema_version = config->nvs_schema_version ? config->nvs_schema_version : 1,
            },
        .version_label = s.str[STR_VERSION_LABEL],
        .check_interval_s = config->check_interval_s,
        .confirm_timeout_s = config->confirm_timeout_s,
        .http_timeout_ms = CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS,
        .auto_confirm = !config->manual_confirm,
        .auto_reboot = !config->disable_auto_reboot,
        .rollback = SOTA_ROLLBACK,
        .user = &s,
    };
    if (!sota_engine_init(&s.engine, &cfg)) return ESP_ERR_INVALID_ARG;
    /* An unparseable key fails init: never continue unpinned. */
    for (uint8_t i = 0; i < config->num_signing_keys; ++i) {
        const simpleota_signing_key_t *k = &config->signing_keys[i];
        if (!k->pem || !sota_engine_add_key(&s.engine, k->key_id, k->pem)) {
            ESP_LOGE(TAG, "signing key %u: invalid Ed25519 public key PEM", i);
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t simpleota_init(const simpleota_config_t *config)
{
    if (s.initialized) return ESP_ERR_INVALID_STATE;
    if (!config || config->num_signing_keys > SOTA_VERIFIER_MAX_KEYS) return ESP_ERR_INVALID_ARG;
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (!config->cert_pem) {
        ESP_LOGE(TAG, "no TLS trust anchor: enable CONFIG_MBEDTLS_CERTIFICATE_BUNDLE or set cert_pem");
        return ESP_ERR_INVALID_STATE;
    }
#endif

    memset(&s, 0, sizeof(s));
    s.stopped_sem = xSemaphoreCreateBinary();
    if (!s.stopped_sem || !copy_strings(config)) {
        simpleota_deinit();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = init_engine(config);
    if (err != ESP_OK) {
        simpleota_deinit();
        return err;
    }

    s.net.cert_pem = s.str[STR_CERT_PEM];
    s.net.crt_bundle_attach = config->crt_bundle_attach;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (!s.net.cert_pem && !s.net.crt_bundle_attach) s.net.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    s.initial_delay_ms = config->initial_delay_ms;
    s.is_connected = config->is_connected;
    s.event_cb = config->event_cb;
    s.user_ctx = config->user_ctx;
    s.initialized = true;

    sota_engine_validate_boot(&s.engine);
    ESP_LOGI(TAG, "init: device=%s chip=%s build=%" PRIu32 " keys=%u rollback=%d",
             s.str[STR_DEVICE_ID], s.str[STR_CHIP_FAMILY], s.engine.st.live.build,
             s.engine.num_keys, SOTA_ROLLBACK);
    return ESP_OK;
}

esp_err_t simpleota_start(void)
{
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    if (s.task) return ESP_OK;
    s.stop_requested = false;
    BaseType_t ok = xTaskCreate(sota_task, "simpleota", CONFIG_SIMPLEOTA_TASK_STACK_SIZE, NULL,
                                CONFIG_SIMPLEOTA_TASK_PRIORITY, &s.task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

/* The task stops between steps, so a flash write is never interrupted. */
esp_err_t simpleota_stop(void)
{
    if (!s.initialized || !s.task) return ESP_OK;
    s.stop_requested = true;
    xTaskNotifyGive(s.task);
    if (xSemaphoreTake(s.stopped_sem, sota_ms_to_ticks(60000, configTICK_RATE_HZ)) != pdTRUE) {
        ESP_LOGW(TAG, "stop: task did not exit within 60 s");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t simpleota_check_now(void)
{
    if (!s.initialized || !s.task) return ESP_ERR_INVALID_STATE;
    xTaskNotifyGive(s.task);
    return ESP_OK;
}

esp_err_t simpleota_confirm(void)
{
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    if (sota_engine_confirm(&s.engine) == SOTA_CONFIRM_FAILED) return ESP_FAIL;
    /* The task posts the confirmed report. */
    if (s.task) xTaskNotifyGive(s.task);
    return ESP_OK;
}

/* The status post needs a TLS handshake, which does not fit a typical
 * caller's stack (the main task's default is 3584 bytes). */
static void reboot_task(void *arg)
{
    (void)arg;
    sota_engine_reboot_for_update(&s.engine);
    vTaskDelete(NULL);
}

esp_err_t simpleota_reboot_for_update(void)
{
    if (!s.initialized || !sota_engine_update_waiting()) return ESP_ERR_INVALID_STATE;
    /* From event_cb, on the component task: its stack is big enough. */
    if (s.task && xTaskGetCurrentTaskHandle() == s.task) sota_engine_reboot_for_update(&s.engine);
    /* Otherwise stop the task first, so only one TLS session is open. */
    simpleota_stop();
    if (xTaskCreate(reboot_task, "sota_reboot", CONFIG_SIMPLEOTA_TASK_STACK_SIZE, NULL,
                    CONFIG_SIMPLEOTA_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the reboot task; restarting without the status post");
        esp_restart();
    }
    for (;;) vTaskDelay(portMAX_DELAY);
}

bool simpleota_is_trial_boot(void) { return s.engine.in_trial; }

uint32_t simpleota_current_build(void) { return s.engine.st.live.build; }

esp_err_t simpleota_deinit(void)
{
    if (s.task) simpleota_stop();
    if (s.initialized) sota_engine_deinit(&s.engine);
    for (int i = 0; i < STR_COUNT; ++i) free(s.str[i]);
    if (s.stopped_sem) vSemaphoreDelete(s.stopped_sem);
    memset(&s, 0, sizeof(s));
    return ESP_OK;
}
