/**
 * simpleota.c - SimpleOTA managed OTA client for ESP-IDF.
 *
 * Behavioral reference: SimpleOTAClient-Arduino v0.4.0. The protocol
 * sequence, status event vocabulary, NVS semantics, signed-firmware gate,
 * and rollback bookkeeping mirror that (hardware-verified) implementation;
 * where ESP-IDF offers a native mechanism (bootloader app rollback, the
 * certificate bundle) it is used instead of the Arduino DIY equivalent.
 *
 * Threading model: one background task ("simpleota") owns all network and
 * flash activity. Public API calls only set flags / send notifications,
 * except simpleota_confirm(), which touches NVS on the caller's task, and
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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "sota_json.h"
#include "sota_nvs.h"
#include "sota_report.h"
#include "sota_sha256.h"
#include "sota_signing.h"
#include "sota_time.h"
#include "sota_trial.h"

_Static_assert((int)SOTA_RST_POWERON == (int)ESP_RST_POWERON &&
                   (int)SOTA_RST_SW == (int)ESP_RST_SW &&
                   (int)SOTA_RST_PANIC == (int)ESP_RST_PANIC &&
                   (int)SOTA_RST_INT_WDT == (int)ESP_RST_INT_WDT &&
                   (int)SOTA_RST_TASK_WDT == (int)ESP_RST_TASK_WDT &&
                   (int)SOTA_RST_WDT == (int)ESP_RST_WDT &&
                   (int)SOTA_RST_DEEPSLEEP == (int)ESP_RST_DEEPSLEEP &&
                   (int)SOTA_RST_BROWNOUT == (int)ESP_RST_BROWNOUT,
               "sota_trial.h reset codes match esp_reset_reason_t");

static const char *TAG = "simpleota";

#define SOTA_DEFAULT_BASE_URL "https://simpleota.com"
#define SOTA_DEFAULT_CHECK_INTERVAL_S 3600
#define SOTA_DEFAULT_CONFIRM_TIMEOUT_S 300
#define SOTA_TRIAL_RETRY_INTERVAL_S 10
#define SOTA_MAX_REDIRECTS 3
#define SOTA_RESP_BUF_SIZE 4096

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

typedef struct {
    bool has;
    char *url; /* heap: pre-signed URLs are long and unbounded */
    char checksum[80]; /* sha256 hex, lowercased */
    char deployment_id[80];
    char version[80];
    uint32_t build_number;
    uint32_t size;
    bool offer_signed;
    bool offer_has_signature;
    uint8_t signature[64];
    char sig_key_id[64];
} sota_offer_t;

typedef struct {
    bool initialized;

    /* Config (owned copies). */
    char *token;
    char *base_url;
    char *device_id;
    char *chip_family;
    char *board_id;
    char *hardware_revision;
    char *partition_profile;
    char *channel;
    char *labels_json;
    char *security_mode;
    char *version_label; /* explicit config override; NULL = use NVS */
    uint8_t nvs_schema_version;
    uint32_t check_interval_s;
    uint32_t initial_delay_ms;
    uint32_t confirm_timeout_s;
    bool manual_confirm;
    bool disable_auto_reboot;
    uint8_t keys[SOTA_VERIFIER_MAX_KEYS][32];
    char key_ids[SOTA_VERIFIER_MAX_KEYS][64];
    uint8_t num_keys;
    bool (*is_connected)(void *user_ctx);
    void (*event_cb)(const simpleota_event_t *event, void *user_ctx);
    void *user_ctx;
    char *cert_pem; /* owned copy (header promises all strings are copied) */
    esp_err_t (*crt_bundle_attach)(void *conf);

    /* Persisted state, loaded at init and kept current. */
    sota_nvs_state_t nvs;

    /* Runtime. */
    sota_offer_t offer;
    TaskHandle_t task;
    SemaphoreHandle_t stopped_sem;
    volatile bool stop_requested;
    volatile bool in_trial;
    volatile bool confirm_timeout_fired;
    sota_outbox_t outbox;         /* reports waiting to be sent */
    StaticSemaphore_t outbox_lock_buf;
    SemaphoreHandle_t outbox_lock; /* guards outbox: the app task and the component task both use it */
    uint32_t report_failures;     /* report sends with no response this boot */
    esp_timer_handle_t confirm_timer;
    bool auth_warned;
} sota_state_t;

static sota_state_t s;

/* A flashed update waits for its reboot: the boot slot already points at
 * it. The next update slot holds it, so nothing may be downloaded until the
 * reboot. Kept outside s because it describes this boot: simpleota_deinit()
 * must not clear it, and a later simpleota_init() must not read the waiting
 * update's trial record as a rollback. */
static volatile bool s_update_waiting;

/* Guards the in_trial claim in claim_trial_end(). */
static portMUX_TYPE s_trial_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------- */

static char *dup_or_null(const char *str) {
    if (!str) return NULL;
    char *copy = strdup(str);
    return copy;
}

static void emit_event(simpleota_event_id_t id, const char *reason,
                       uint32_t build, size_t done, size_t total) {
    if (!s.event_cb) return;
    simpleota_event_t evt = {
        .id = id,
        .reason = reason,
        .build_number = build,
        .bytes_done = done,
        .bytes_total = total,
    };
    s.event_cb(&evt, s.user_ctx);
}

/* Reject tokens containing CR/LF or other control bytes; defends against
 * header injection if a caller ever sourced the token from untrusted input. */
static bool token_looks_safe(const char *t) {
    if (!t || !t[0]) return false;
    for (const char *p = t; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

static void clear_offer(void) {
    free(s.offer.url);
    memset(&s.offer, 0, sizeof(s.offer));
}

static void lowercase(char *str) {
    for (; *str; ++str) {
        if (*str >= 'A' && *str <= 'Z') *str += 'a' - 'A';
    }
}

static const char *effective_version_label(void) {
    if (s.version_label && s.version_label[0]) return s.version_label;
    if (s.nvs.version[0]) return s.nvs.version;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * HTTP helpers
 * ------------------------------------------------------------------------- */

static void apply_tls_config(esp_http_client_config_t *cfg) {
    if (s.cert_pem) {
        cfg->cert_pem = s.cert_pem;
    } else {
        cfg->crt_bundle_attach = s.crt_bundle_attach;
    }
}

/*
 * POST a JSON body to base_url+path with the bearer token. Used for /check/
 * and /status/ only, never the firmware download.
 *
 * Retry policy (mirrors Arduino postJson): on transport failure, wait 2 s
 * and retry once. Any received HTTP response is returned immediately; a 4xx
 * retry would be pointless and a 5xx retry is left to the next cycle.
 *
 * Returns the HTTP status code, or -1 on transport failure. On success the
 * body (up to resp_cap-1 bytes) is copied into resp, null-terminated.
 */
static int http_post_json(const char *path, const char *body, char *resp,
                          size_t resp_cap) {
    char url[160];
    int n = snprintf(url, sizeof(url), "%s%s", s.base_url, path);
    if (n <= 0 || (size_t)n >= sizeof(url)) return -1;

    char auth[192];
    n = snprintf(auth, sizeof(auth), "Bearer %s", s.token);
    if (n <= 0 || (size_t)n >= sizeof(auth)) return -1;

    size_t body_len = strlen(body);

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt > 0) vTaskDelay(sota_ms_to_ticks(2000, configTICK_RATE_HZ));

        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS,
            .buffer_size = 2048,
            .buffer_size_tx = CONFIG_SIMPLEOTA_HTTP_TX_BUFFER,
        };
        apply_tls_config(&cfg);

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) continue;

        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_header(client, "Content-Type", "application/json");

        esp_err_t err = esp_http_client_open(client, body_len);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "POST %s open failed: %s (attempt %d)", path,
                     esp_err_to_name(err), attempt);
            esp_http_client_cleanup(client);
            continue;
        }
        int written = esp_http_client_write(client, body, body_len);
        if (written < 0 || (size_t)written != body_len) {
            esp_http_client_cleanup(client);
            continue;
        }
        if (esp_http_client_fetch_headers(client) < 0) {
            esp_http_client_cleanup(client);
            continue;
        }
        int status = esp_http_client_get_status_code(client);

        size_t total = 0;
        if (resp && resp_cap > 1) {
            while (total < resp_cap - 1) {
                int r = esp_http_client_read(client, resp + total,
                                             resp_cap - 1 - total);
                if (r <= 0) break;
                total += (size_t)r;
            }
        }
        if (resp && resp_cap > 0) resp[total] = '\0';

        esp_http_client_cleanup(client);

        ESP_LOGD(TAG, "POST %s -> %d", path, status);
        if ((status == 401 || status == 403) && !s.auth_warned) {
            s.auth_warned = true;
            ESP_LOGW(TAG,
                     "server rejected the token (HTTP %d). Check your project "
                     "token.",
                     status);
        }
        return status; /* any received response: no retry */
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * JSON request bodies. String values are escaped, so a value from the server
 * (a version label, a deployment id) or the integrator can never break the
 * body the server has to parse.
 * ------------------------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ok; /* false once something did not fit */
} json_buf_t;

static void jb_raw(json_buf_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void jb_raw(json_buf_t *b, const char *fmt, ...) {
    if (!b->ok) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->ok = false;
        return;
    }
    b->len += (size_t)n;
}

/* Append "key":"value", after a comma unless it is the first member. */
static void jb_str(json_buf_t *b, const char *key, const char *val) {
    jb_raw(b, "%s\"%s\":\"", b->len > 1 ? "," : "", key);
    if (!b->ok) return;
    if (!sota_json_escape(val, b->buf + b->len, b->cap - b->len)) {
        b->ok = false;
        return;
    }
    b->len += strlen(b->buf + b->len);
    jb_raw(b, "\"");
}

/* ---------------------------------------------------------------------------
 * /api/v1/ota/status/
 * ------------------------------------------------------------------------- */

/*
 * Post one lifecycle event. Returns the HTTP status, or -1 when nothing was
 * sent or no response came back. The response body is not interpreted:
 * status events are telemetry and never change what the device does.
 */
static int post_status_for(const char *event, const char *reason,
                           const char *deployment_id, uint32_t build_number) {
    if (!deployment_id || !deployment_id[0]) {
        ESP_LOGD(TAG, "status: no deployment context; skipping %s", event);
        return -1;
    }
    ESP_LOGI(TAG, "status: event=%s build=%" PRIu32 " deployment=%s%s%s", event,
             build_number, deployment_id, reason ? " reason=" : "",
             reason ? reason : "");

    char body[384];
    json_buf_t b = {body, sizeof(body), 0, true};
    jb_raw(&b, "{");
    jb_str(&b, "device_id", s.device_id);
    jb_str(&b, "deployment_id", deployment_id);
    jb_str(&b, "event", event);
    jb_raw(&b, ",\"build_number\":%" PRIu32, build_number);
    if (reason) jb_str(&b, "reason", reason);
    jb_raw(&b, "}");
    if (!b.ok) return -1;

    char resp[256];
    return http_post_json("/api/v1/ota/status/", body, resp, sizeof(resp));
}

/* Post a status event for the CURRENT offer. */
static int post_status(const char *event, const char *reason) {
    return post_status_for(event, reason, s.offer.deployment_id,
                           s.offer.build_number);
}

/* Back on the previous image: its identity becomes the live one again. */
static void restore_previous_identity(void) {
    sota_nvs_set_identity(s.nvs.prev_build, s.nvs.prev_hash, s.nvs.prev_version);
    s.nvs.build = s.nvs.prev_build;
    strlcpy(s.nvs.hash, s.nvs.prev_hash, sizeof(s.nvs.hash));
    strlcpy(s.nvs.version, s.nvs.prev_version, sizeof(s.nvs.version));
}

/* A staged record whose slot switch will not happen. Staging may already
 * have written the new identity, so the identity of the image that keeps
 * running, still in RAM, goes back to NVS. */
static void abandon_staged_record(void) {
    sota_nvs_set_identity(s.nvs.build, s.nvs.hash, s.nvs.version);
    sota_nvs_drop_trial();
}

/*
 * Deferred terminal reports (see sota_report.h). Once a boot has worked out
 * what happened to a trial, the report is queued in the outbox and sent at
 * the start of each check. Each is done once the server returns any HTTP
 * response; without one it stays queued, in NVS, across reboots.
 */
static void queue_report(const char *event, const char *reason,
                         const char *deployment_id, uint32_t build) {
    xSemaphoreTake(s.outbox_lock, portMAX_DELAY);
    bool queued = sota_outbox_push(&s.outbox, event, reason, deployment_id, build);
    if (queued) sota_nvs_save_outbox(&s.outbox);
    xSemaphoreGive(s.outbox_lock);
    if (!queued) {
        ESP_LOGW(TAG, "%s report has no usable deployment context; dropped", event);
        return;
    }
    ESP_LOGI(TAG, "%s report queued for build %" PRIu32, event, build);
}

/* The lock is not held while posting: the report is copied out, and popped
 * only if a report queued meanwhile has not pushed it out already. */
static void send_pending_reports(void) {
    for (;;) {
        sota_report_t r;
        xSemaphoreTake(s.outbox_lock, portMAX_DELAY);
        bool have = s.outbox.count > 0;
        if (have) r = s.outbox.reports[0];
        xSemaphoreGive(s.outbox_lock);
        if (!have) return;

        int status = post_status_for(r.event, r.reason[0] ? r.reason : NULL,
                                     r.deployment, r.build);
        if (!sota_report_delivered(status)) {
            s.report_failures++;
            return;
        }
        s.report_failures = 0;

        xSemaphoreTake(s.outbox_lock, portMAX_DELAY);
        if (s.outbox.count > 0 && memcmp(&s.outbox.reports[0], &r, sizeof(r)) == 0) {
            sota_outbox_pop(&s.outbox);
            sota_nvs_save_outbox(&s.outbox);
        }
        xSemaphoreGive(s.outbox_lock);
        if (strcmp(r.event, "confirmed") == 0) {
            emit_event(SIMPLEOTA_EVENT_CONFIRMED, NULL, r.build, 0, 0);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Confirm / rollback
 * ------------------------------------------------------------------------- */

static void stop_confirm_timer(void) {
    if (s.confirm_timer) {
        esp_timer_stop(s.confirm_timer); /* ok if not running */
        esp_timer_delete(s.confirm_timer);
        s.confirm_timer = NULL;
    }
}

/* A confirm (from the component task or the app task) and the timeout
 * rollback can race. Only the first to get here ends the trial. */
static bool claim_trial_end(void) {
    portENTER_CRITICAL(&s_trial_mux);
    bool claimed = s.in_trial;
    s.in_trial = false;
    portEXIT_CRITICAL(&s_trial_mux);
    return claimed;
}

/* Confirm the running trial image. Called from the component task
 * (auto-confirm) or the app task (simpleota_confirm). Returns an error,
 * with the trial still running, when the bootloader cannot record the
 * image as good. */
static esp_err_t do_confirm(void) {
    /* Claimed first: a timeout that fires meanwhile is skipped by the task. */
    if (!claim_trial_end()) return ESP_OK;

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mark_app_valid failed: %s; trial continues",
                 esp_err_to_name(err));
        portENTER_CRITICAL(&s_trial_mux);
        s.in_trial = true;
        portEXIT_CRITICAL(&s_trial_mux);
        /* A timeout that fired while in_trial was false was skipped by the
         * task. Hand it back so the rollback still happens. */
        if (s.confirm_timer && !esp_timer_is_active(s.confirm_timer)) {
            s.confirm_timeout_fired = true;
            if (s.task) xTaskNotifyGive(s.task);
        }
        return err;
    }
#endif
    stop_confirm_timer();
    queue_report("confirmed", NULL, s.nvs.fail_dep, s.nvs.fail_build);
    sota_nvs_drop_trial();
    s.nvs.trial = 0;
    s.nvs.conf_pend = 0;
    ESP_LOGI(TAG, "trial image confirmed (build %" PRIu32 ")", s.nvs.build);
    return ESP_OK;
}

/* Roll the trial back; reason goes in the rolled_back report. The confirm
 * timeout runs this in the component task: the esp_timer callback only
 * notifies, because NVS and mark-invalid work must not run on the shared
 * esp_timer task stack. */
static void do_rollback(const char *reason) {
    if (!claim_trial_end()) return; /* confirmed, or being confirmed */

    ESP_LOGW(TAG, "%s: rolling back to build %" PRIu32, reason,
             s.nvs.prev_build);
    emit_event(SIMPLEOTA_EVENT_ROLLING_BACK, reason, s.nvs.prev_build, 0, 0);

    /* Restore identity BEFORE rebooting so the next boot reports the old
     * build (unconditionally: prev_build==0 is valid fresh-device state). */
    sota_nvs_rollback_restore(&s.nvs, reason);
    stop_confirm_timer();

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    /* Marks the running app INVALID in otadata and reboots into the last
     * bootable image. Returns only on error. */
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(TAG, "rollback reboot failed: %s (staying up)",
             esp_err_to_name(err));
#else
    /* Should be unreachable: the timer is only armed with rollback support. */
    esp_restart();
#endif
}

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
static void confirm_timer_cb(void *arg) {
    (void)arg;
    /* esp_timer task context: keep it tiny. The component task does the work. */
    s.confirm_timeout_fired = true;
    if (s.task) xTaskNotifyGive(s.task);
}

static esp_err_t arm_confirm_timer(void) {
    const esp_timer_create_args_t args = {
        .callback = confirm_timer_cb,
        .name = "sota_confirm",
    };
    esp_err_t err = esp_timer_create(&args, &s.confirm_timer);
    if (err != ESP_OK) return err;
    return esp_timer_start_once(s.confirm_timer,
                                (uint64_t)s.confirm_timeout_s * 1000000ULL);
}
#endif /* CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE */

/* ---------------------------------------------------------------------------
 * Boot-time trial validation
 * ------------------------------------------------------------------------- */

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
/* Map a stored flash address back to its app partition (for otadata state). */
static const esp_partition_t *find_app_partition_by_addr(uint32_t addr) {
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p && p->address == addr) {
            esp_partition_iterator_release(it);
            return p;
        }
    }
    return NULL;
}
#endif

/* First boot of the new image. */
static void start_trial(bool staged) {
    /* Power was lost after the slot switch, before sota_staged was removed. */
    if (staged) sota_nvs_unstage_trial();
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(running, &state);
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG,
                 "trial boot but app state is %d (expected "
                 "PENDING_VERIFY); bootloader rollback may be off",
                 (int)state);
    }
    s.in_trial = true;
    if (arm_confirm_timer() != ESP_OK) {
        /* Cannot supervise the trial: fail safe by rolling back now
         * rather than running unconfirmed forever. */
        ESP_LOGE(TAG, "confirm timer unavailable; rolling back");
        do_rollback("confirm_timeout");
        return;
    }
    ESP_LOGI(TAG, "trial boot of build %" PRIu32 "; confirm within %" PRIu32 " s",
             s.nvs.build, s.confirm_timeout_s);
    emit_event(SIMPLEOTA_EVENT_TRIAL_BOOT, NULL, s.nvs.build, 0, 0);
#else
    /* Without bootloader rollback nothing can switch back: accept the image
     * and report it confirmed. */
    ESP_LOGW(TAG,
             "running new image without CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE: "
             "no automatic rollback; the image is accepted.");
    queue_report("confirmed", NULL, s.nvs.fail_dep, s.nvs.fail_build);
    sota_nvs_drop_trial();
    s.nvs.trial = 0;
    s.nvs.conf_pend = 0;
#endif
}

/* Rollback cause for a record that is not on its new image: only the
 * component's own confirm-timeout path marks that slot INVALID. */
static const char *rollback_cause(void) {
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *new_part = find_app_partition_by_addr(s.nvs.new_part);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (new_part && esp_ota_get_state_partition(new_part, &st) == ESP_OK &&
        st == ESP_OTA_IMG_INVALID) {
        return "confirm_timeout";
    }
#endif
    /* Otherwise the reset that ended the trial says why. */
    return sota_rollback_reason((int)esp_reset_reason());
}

static void process_boot_validation(void) {
    if (s_update_waiting) {
        ESP_LOGI(TAG, "build %" PRIu32 " is flashed and waiting for reboot",
                 s.nvs.build);
        return;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    sota_boot_facts_t facts = {
        .trial = s.nvs.trial,
        .staged = s.nvs.staged,
        .new_part = s.nvs.new_part,
        .running_part = running ? (uint32_t)running->address : 0,
        .boot_part = boot ? (uint32_t)boot->address : 0,
        .deep_sleep_wake = esp_reset_reason() == ESP_RST_DEEPSLEEP,
    };

    switch (sota_classify_boot(&facts)) {
    case SOTA_BOOT_NONE:
        /* An earlier release clears sota_trial on confirm and keeps the
         * report context until the report is sent: that is all that is left. */
        if (s.nvs.conf_pend) {
            queue_report("confirmed", NULL, s.nvs.fail_dep, s.nvs.fail_build);
            sota_nvs_drop_trial();
        }
        break;
    case SOTA_BOOT_REPORT_PENDING:
        queue_report("rolled_back",
                     s.nvs.rb_reason[0] ? s.nvs.rb_reason : "confirm_timeout",
                     s.nvs.fail_dep, s.nvs.fail_build);
        sota_nvs_drop_trial();
        break;
    case SOTA_BOOT_ROLLED_BACK: {
        const char *cause = rollback_cause();
        ESP_LOGW(TAG, "rollback detected (%s): running 0x%" PRIx32
                      " but new image was at 0x%" PRIx32,
                 cause, facts.running_part, s.nvs.new_part);
        restore_previous_identity();
        queue_report("rolled_back", cause, s.nvs.fail_dep, s.nvs.fail_build);
        sota_nvs_drop_trial();
        break;
    }
    case SOTA_BOOT_WAITING:
        ESP_LOGI(TAG, "build %" PRIu32 " is flashed and waiting for reboot",
                 s.nvs.fail_build);
        s_update_waiting = true;
        return; /* the record stays for the boot that runs the new image */
    case SOTA_BOOT_ABANDONED:
        /* The record went in with the new identity before the slot switch
         * that never happened. */
        ESP_LOGW(TAG, "update was cut short before its slot switch; dropping it");
        if (sota_abandon_restores_identity(s.nvs.staged)) restore_previous_identity();
        sota_nvs_drop_trial();
        break;
    case SOTA_BOOT_INCOMPLETE:
        ESP_LOGW(TAG, "dropping a trial record that cannot be checked");
        sota_nvs_drop_trial();
        break;
    case SOTA_BOOT_SLEPT_IN_TRIAL:
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
        s.in_trial = true;
        do_rollback("deep_sleep"); /* usually does not return */
        return;
#else
        start_trial(facts.staged); /* accepts the image */
        return;
#endif
    case SOTA_BOOT_TRIAL:
        start_trial(facts.staged);
        return;
    }
    s.nvs.trial = 0;
    s.nvs.conf_pend = 0;
}

/* ---------------------------------------------------------------------------
 * /api/v1/ota/check/
 * ------------------------------------------------------------------------- */

/* Build the check-in body. String values are escaped; labels_json is
 * embedded verbatim. */
static char *build_check_body(void) {
    size_t cap = 1024;
    if (s.labels_json) cap += strlen(s.labels_json);
    char *body = malloc(cap);
    if (!body) return NULL;

    json_buf_t b = {body, cap, 0, true};
    jb_raw(&b, "{");
    jb_str(&b, "device_id", s.device_id);
    jb_raw(&b, ",\"framework\":\"esp_idf\"");
    jb_str(&b, "chip_family", s.chip_family);
    jb_raw(&b, ",\"current_build_number\":%" PRIu32 ",\"nvs_schema_version\":%u",
           s.nvs.build, (unsigned)s.nvs_schema_version);
    if (s.board_id) jb_str(&b, "board_id", s.board_id);
    if (s.hardware_revision) jb_str(&b, "hardware_revision", s.hardware_revision);
    if (s.partition_profile) jb_str(&b, "partition_profile", s.partition_profile);
    if (s.security_mode) jb_str(&b, "security_mode", s.security_mode);
    if (s.channel) jb_str(&b, "channel", s.channel);
    const char *ver = effective_version_label();
    if (ver) jb_str(&b, "version_label", ver);
    if (s.nvs.hash[0]) jb_str(&b, "current_hash", s.nvs.hash);
    if (s.labels_json) jb_raw(&b, ",\"labels\":%s", s.labels_json);
    jb_raw(&b, "}");
    if (!b.ok) {
        free(body);
        return NULL;
    }
    return body;
}

/* Returns true when an update offer was stored. */
static bool do_check(void) {
    /* Queued reports go first, as their own status POSTs, so a degraded
     * /check/ endpoint does not delay them. */
    send_pending_reports();

    char *body = build_check_body();
    if (!body) return false;

    char *resp = malloc(SOTA_RESP_BUF_SIZE);
    if (!resp) {
        free(body);
        return false;
    }

    int status = http_post_json("/api/v1/ota/check/", body, resp,
                                SOTA_RESP_BUF_SIZE);
    free(body);

    if (status < 200 || status >= 300) {
        ESP_LOGD(TAG, "check: HTTP %d", status);
        free(resp);
        return false;
    }

    /* Server confirmed reachable: auto-confirm a running trial and send the
     * confirmed report it queues. When the confirm fails, the trial goes on
     * and the next trial poll tries again. */
    if (s.in_trial && !s.manual_confirm && do_confirm() == ESP_OK && !s.in_trial) {
        send_pending_reports();
    }

    bool got_offer = false;
    bool available = false;
    if (!sota_json_get_bool(resp, "update_available", &available) || !available) {
        char status_str[48];
        bool has_status =
            sota_json_get_string(resp, "status", status_str, sizeof(status_str));
        ESP_LOGI(TAG, "check: no update (%s)",
                 has_status ? status_str : "up_to_date");
        emit_event(SIMPLEOTA_EVENT_CHECK_DONE, has_status ? status_str : NULL,
                   s.nvs.build, 0, 0);
        free(resp);
        return false;
    }

    char *url = malloc(2048); /* pre-signed URLs are long */
    char checksum[80];
    char mode[24], algo[24], key_id[64], sig_b64[128];
    uint32_t build = 0;
    if (!url) {
        free(resp);
        return false;
    }

    if (!sota_json_get_string(resp, "url", url, 2048) ||
        !sota_json_get_string(resp, "checksum", checksum, sizeof(checksum)) ||
        !sota_json_get_u32(resp, "build_number", &build)) {
        ESP_LOGW(TAG, "check: malformed offer payload");
        free(url);
        free(resp);
        return false;
    }

    clear_offer();
    s.offer.url = url; /* ownership moves to the offer */
    strlcpy(s.offer.checksum, checksum, sizeof(s.offer.checksum));
    sota_json_get_string(resp, "deployment_id", s.offer.deployment_id,
                         sizeof(s.offer.deployment_id));
    sota_json_get_string(resp, "version", s.offer.version,
                         sizeof(s.offer.version));
    s.offer.build_number = build;
    sota_json_get_u32(resp, "size", &s.offer.size);

    /* Signed-firmware fields (only meaningful for signed offers). */
    s.offer.offer_signed =
        sota_json_get_string(resp, "security_mode", mode, sizeof(mode)) &&
        strcmp(mode, SIMPLEOTA_SECURITY_SIGNED) == 0;
    if (sota_json_get_string(resp, "signing_key_id", key_id, sizeof(key_id)))
        strlcpy(s.offer.sig_key_id, key_id, sizeof(s.offer.sig_key_id));
    if (s.offer.offer_signed &&
        sota_json_get_string(resp, "signature", sig_b64, sizeof(sig_b64))) {
        /* An algorithm other than ed25519 leaves the signature unusable:
         * verification then fails closed when keys are pinned. */
        bool has_algo =
            sota_json_get_string(resp, "signature_algorithm", algo, sizeof(algo));
        bool algo_ok = !has_algo || !algo[0] || strcmp(algo, "ed25519") == 0;
        if (algo_ok && sota_decode_signature_b64(sig_b64, s.offer.signature)) {
            s.offer.offer_has_signature = true;
        } else {
            ESP_LOGW(TAG, "check: signed offer carries unusable signature (algo=%s)",
                     has_algo ? algo : "?");
        }
    }
    free(resp);

    lowercase(s.offer.checksum);
    s.offer.has = true;
    got_offer = true;
    ESP_LOGI(TAG, "check: offer build=%" PRIu32 " version=%s size=%" PRIu32,
             build, s.offer.version[0] ? s.offer.version : "?", s.offer.size);
    emit_event(SIMPLEOTA_EVENT_UPDATE_AVAILABLE, NULL, build, 0, s.offer.size);
    return got_offer;
}

/* ---------------------------------------------------------------------------
 * Apply (download + verify + flash)
 * ------------------------------------------------------------------------- */

/* Shared failure tail: report + event + drop the offer. Assumes any OTA
 * handle and TLS session have already been released. */
static void finish_failed(const char *reason) {
    ESP_LOGE(TAG, "update failed: %s", reason);
    post_status("failed", reason);
    emit_event(SIMPLEOTA_EVENT_UPDATE_FAILED, reason, s.offer.build_number, 0, 0);
    clear_offer();
}

/* Convenience: abort the OTA handle, free resources, post failed, clear. */
static void fail_update(esp_ota_handle_t handle, bool ota_started,
                        esp_http_client_handle_t client,
                        sota_sig_verifier_t *verifier, uint8_t *chunk,
                        const char *reason) {
    if (ota_started) esp_ota_abort(handle);
    /* Free the download TLS session BEFORE the status POST (each mbedTLS
     * session costs tens of KB of heap; two at once can exhaust an ESP32.
     * This exact ordering bug lost 'failed' events in the field on the
     * Arduino client). */
    if (client) esp_http_client_cleanup(client);
    free(verifier);
    free(chunk);
    finish_failed(reason);
}

/* Open the download URL, following up to SOTA_MAX_REDIRECTS redirects.
 * Returns NULL after posting the failure. On success the response headers
 * have been fetched and *content_length is set. */
static esp_http_client_handle_t open_download(int64_t *content_length,
                                              char reason_buf[32]) {
    esp_http_client_config_t cfg = {
        .url = s.offer.url,
        .timeout_ms = CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        /* Pre-signed URLs are routinely 500-1500+ chars; the default TX
         * buffer (512) cannot even fit the request line. */
        .buffer_size_tx = CONFIG_SIMPLEOTA_HTTP_TX_BUFFER,
        .keep_alive_enable = true,
    };
    apply_tls_config(&cfg);
    /* Note: NO Authorization header here. The pre-signed URL embeds its own
     * credentials, and the bearer token must not leak to the storage host. */

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        strlcpy(reason_buf, "https_begin_failed", 32);
        return NULL;
    }

    for (int redirects = 0;; ++redirects) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            strlcpy(reason_buf, "https_begin_failed", 32);
            return NULL;
        }
        *content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 301 || status == 302 || status == 303 || status == 307 ||
            status == 308) {
            /* The open/read streaming API does NOT auto-follow redirects
             * (only perform() does); walk them manually, bounded. */
            if (redirects >= SOTA_MAX_REDIRECTS) {
                esp_http_client_cleanup(client);
                snprintf(reason_buf, 32, "http_status_%d", status);
                return NULL;
            }
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }
        if (status != 200) {
            esp_http_client_cleanup(client);
            snprintf(reason_buf, 32, "http_status_%d", status);
            return NULL;
        }
        return client;
    }
}

static void do_apply(void) {
    if (!s.offer.has) return;

    /* The next update slot is the one after the running image. While a
     * flashed update waits for simpleota_reboot_for_update(), that slot
     * holds it, and another download would erase it. The offer is fetched
     * again on a later check. */
    if (s_update_waiting) {
        ESP_LOGI(TAG, "update waiting for reboot; not downloading build %" PRIu32,
                 s.offer.build_number);
        clear_offer();
        return;
    }
    /* The other slot holds the previous image, which a running trial may
     * still need to return to. */
    if (s.in_trial) {
        ESP_LOGI(TAG, "running image not confirmed yet; not downloading build %" PRIu32,
                 s.offer.build_number);
        clear_offer();
        return;
    }

    /* ---- Transport guard first (Arduino ordering: a non-HTTPS URL is
     * insecure_url regardless of what the signed gate would say) ---------- */
    if (strncmp(s.offer.url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "refusing non-HTTPS download URL");
        post_status("failed", "insecure_url");
        emit_event(SIMPLEOTA_EVENT_UPDATE_FAILED, "insecure_url",
                   s.offer.build_number, 0, 0);
        clear_offer();
        return;
    }

    /* ---- Signed-firmware gate (pre-download; fail-closed) --------------- */
    bool device_signed =
        s.security_mode && strcmp(s.security_mode, SIMPLEOTA_SECURITY_SIGNED) == 0;
    sota_signed_gate_t gate =
        sota_signed_gate(device_signed, s.num_keys, s.offer.offer_signed,
                         s.offer.offer_has_signature);

    if (gate == SOTA_GATE_FAIL_CLOSED) {
        ESP_LOGE(TAG,
                 "signed firmware required but the offer carries no usable "
                 "signature; rejecting before download");
        post_status("failed", "signature_invalid");
        emit_event(SIMPLEOTA_EVENT_UPDATE_FAILED, "signature_invalid",
                   s.offer.build_number, 0, 0);
        clear_offer();
        return;
    }
    if (gate == SOTA_GATE_WARN_UNVERIFIED) {
        ESP_LOGW(TAG,
                 "applying a SIGNED artifact WITHOUT verification: no public "
                 "key is pinned. Pin the project key via "
                 "simpleota_config_t.signing_keys.");
    }

    sota_sig_verifier_t *verifier = NULL;
    if (gate == SOTA_GATE_VERIFY) {
        verifier = calloc(1, sizeof(*verifier));
        if (!verifier) {
            /* Cannot verify -> fail closed, never fall through unverified. */
            post_status("failed", "signature_invalid");
            emit_event(SIMPLEOTA_EVENT_UPDATE_FAILED, "signature_invalid",
                       s.offer.build_number, 0, 0);
            clear_offer();
            return;
        }
        /* Key selection: exact key-id match wins; otherwise try all pinned
         * keys (rotation windows). */
        uint8_t selected[SOTA_VERIFIER_MAX_KEYS][32];
        uint8_t num_selected = 0;
        if (s.offer.sig_key_id[0]) {
            for (uint8_t i = 0; i < s.num_keys; ++i) {
                if (strcmp(s.key_ids[i], s.offer.sig_key_id) == 0) {
                    memcpy(selected[0], s.keys[i], 32);
                    num_selected = 1;
                    break;
                }
            }
        }
        if (num_selected == 0) {
            for (uint8_t i = 0; i < s.num_keys; ++i) {
                memcpy(selected[i], s.keys[i], 32);
            }
            num_selected = s.num_keys;
        }
        sota_verifier_begin(verifier, s.offer.signature, selected, num_selected);
    }

    post_status("download_started", NULL);

    char reason[32];
    int64_t content_length = 0;
    esp_http_client_handle_t client = open_download(&content_length, reason);
    if (!client) {
        fail_update(0, false, NULL, verifier, NULL, reason);
        return;
    }

    /* ---- Flash target ----------------------------------------------------- */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst || !running) {
        fail_update(0, false, client, verifier, NULL, "update_begin_failed");
        return;
    }

    /* Prefer a known size (incremental erase); never OTA_SIZE_UNKNOWN,
     * whose up-front full-partition erase can starve the idle task. */
    size_t expected = 0;
    if (content_length > 0) {
        expected = (size_t)content_length;
    } else if (s.offer.size > 0) {
        expected = s.offer.size;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(
        dst, expected > 0 ? expected : OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        fail_update(0, false, client, verifier, NULL, "update_begin_failed");
        return;
    }

    uint8_t *chunk = malloc(CONFIG_SIMPLEOTA_DOWNLOAD_CHUNK);
    if (!chunk) {
        fail_update(handle, true, client, verifier, NULL, "update_begin_failed");
        return;
    }

    sota_sha256_ctx sha;
    sota_sha256_init(&sha);

    size_t total = 0;
    int64_t last_progress_us = esp_timer_get_time();
    /* Rate-limit progress events: whole-percent steps when the size is
     * known, every 64 KB otherwise. A per-chunk callback (~380 for a
     * typical image) would push its cost onto the user inline with the
     * download. */
    unsigned last_pct = 100;
    size_t last_emit = 0;

    for (;;) {
        int r = esp_http_client_read(client, (char *)chunk,
                                     CONFIG_SIMPLEOTA_DOWNLOAD_CHUNK);
        if (r > 0) {
            err = esp_ota_write(handle, chunk, (size_t)r);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
                fail_update(handle, true, client, verifier, chunk,
                            "update_write_failed");
                return;
            }
            sota_sha256_update(&sha, chunk, (size_t)r);
            if (verifier) sota_verifier_update(verifier, chunk, (size_t)r);
            total += (size_t)r;
            last_progress_us = esp_timer_get_time();
            if (expected > 0) {
                unsigned pct = (unsigned)((uint64_t)total * 100 / expected);
                if (pct != last_pct) {
                    last_pct = pct;
                    emit_event(SIMPLEOTA_EVENT_DOWNLOAD_PROGRESS, NULL,
                               s.offer.build_number, total, expected);
                }
            } else if (total - last_emit >= 65536) {
                last_emit = total;
                emit_event(SIMPLEOTA_EVENT_DOWNLOAD_PROGRESS, NULL,
                           s.offer.build_number, total, expected);
            }
        } else if (r == 0) {
            /* End of body (or clean connection close). */
            break;
        } else {
            int64_t stalled_us = esp_timer_get_time() - last_progress_us;
            bool stalled =
                stalled_us >= (int64_t)CONFIG_SIMPLEOTA_HTTP_TIMEOUT_MS * 1000;
            fail_update(handle, true, client, verifier, chunk,
                        stalled ? "download_stalled" : "stream_error");
            return;
        }
    }

    bool complete = esp_http_client_is_complete_data_received(client);
    /* Rule: the download TLS session dies BEFORE any further status POST. */
    esp_http_client_cleanup(client);
    client = NULL;

    if (!complete || (expected > 0 && total != expected)) {
        ESP_LOGE(TAG, "short read: got %u of %u", (unsigned)total,
                 (unsigned)expected);
        fail_update(handle, true, NULL, verifier, chunk, "short_read");
        return;
    }

    post_status("downloaded", NULL);

    /* ---- Checksum first (cheap transport diagnosis)... ------------------- */
    unsigned char digest[32];
    sota_sha256_final(&sha, digest);
    char hex[65];
    for (int i = 0; i < 32; ++i) sprintf(hex + 2 * i, "%02x", digest[i]);
    hex[64] = '\0';

    if (strcmp(hex, s.offer.checksum) != 0) {
        ESP_LOGE(TAG, "checksum mismatch: got %s want %s", hex,
                 s.offer.checksum);
        fail_update(handle, true, NULL, verifier, chunk, "checksum_mismatch");
        return;
    }

    /* ---- ...then the signature: intact-but-unapproved bytes. ------------- */
    if (verifier) {
        bool sig_ok = sota_verifier_final(verifier);
        free(verifier);
        verifier = NULL;
        if (!sig_ok) {
            ESP_LOGE(TAG, "Ed25519 signature verification FAILED");
            fail_update(handle, true, NULL, NULL, chunk, "signature_invalid");
            return;
        }
        ESP_LOGI(TAG, "Ed25519 signature verified");
    }
    free(chunk);
    chunk = NULL;

    /* ---- Commit ----------------------------------------------------------- */
    /* NOTE: after esp_ota_end() the handle is consumed; these failure paths
     * must NOT go through fail_update() (whose esp_ota_abort would act on a
     * spent handle). finish_failed() is the shared no-abort tail. */
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        /* Includes ESP_ERR_OTA_VALIDATE_FAILED (bad app image; with Secure
         * Boot enabled also a Secure Boot rejection: deliberately a distinct
         * reason from signature_invalid). */
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        finish_failed("update_end_failed");
        return;
    }
    /* ---- Trial record, then the slot switch -------------------------------
     * The record goes in first, so a power cut can never leave a bootable
     * new image without one (see sota_trial.h). */
    err = sota_nvs_stage_trial((uint32_t)dst->address, (uint32_t)running->address,
                               s.nvs.build, s.nvs.hash, s.nvs.version,
                               s.offer.deployment_id, s.offer.build_number,
                               s.offer.checksum, s.offer.version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save the trial record: %s", esp_err_to_name(err));
        abandon_staged_record();
        finish_failed("state_save_failed");
        return;
    }
    err = esp_ota_set_boot_partition(dst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        abandon_staged_record();
        finish_failed("update_end_failed");
        return;
    }
    s_update_waiting = true;
    sota_nvs_unstage_trial();

    post_status("flashed", NULL);

    /* Mirror the record into RAM, so the next check reports the new build
     * and the reboot event names this deployment. */
    s.nvs.prev_part = (uint32_t)running->address;
    s.nvs.new_part = (uint32_t)dst->address;
    s.nvs.prev_build = s.nvs.build;
    strlcpy(s.nvs.prev_hash, s.nvs.hash, sizeof(s.nvs.prev_hash));
    strlcpy(s.nvs.prev_version, s.nvs.version, sizeof(s.nvs.prev_version));
    s.nvs.build = s.offer.build_number;
    strlcpy(s.nvs.hash, s.offer.checksum, sizeof(s.nvs.hash));
    strlcpy(s.nvs.version, s.offer.version, sizeof(s.nvs.version));
    strlcpy(s.nvs.fail_dep, s.offer.deployment_id, sizeof(s.nvs.fail_dep));
    s.nvs.fail_build = s.offer.build_number;
    s.nvs.conf_pend = 1;
    s.nvs.trial = 1;

    post_status("validated", NULL);

    uint32_t new_build = s.offer.build_number;
    ESP_LOGI(TAG, "update to build %" PRIu32 " flashed", new_build);

    if (s.disable_auto_reboot) {
        emit_event(SIMPLEOTA_EVENT_BEFORE_REBOOT, NULL, new_build, 0, 0);
        clear_offer();
        return;
    }

    emit_event(SIMPLEOTA_EVENT_BEFORE_REBOOT, NULL, new_build, 0, 0);
    post_status("reboot", NULL);
    clear_offer();
    vTaskDelay(sota_ms_to_ticks(100, configTICK_RATE_HZ));
    esp_restart();
}

/* ---------------------------------------------------------------------------
 * Managed task
 * ------------------------------------------------------------------------- */

static void sota_task(void *arg) {
    (void)arg;

    /* An overdue confirm-timeout (fired between init and start, when
     * there was no task to notify) outranks the polite initial delay. */
    if (s.initial_delay_ms && !s.confirm_timeout_fired) {
        ulTaskNotifyTake(pdTRUE, sota_ms_to_ticks(s.initial_delay_ms, configTICK_RATE_HZ));
    }

    while (!s.stop_requested) {
        if (s.confirm_timeout_fired) {
            s.confirm_timeout_fired = false;
            do_rollback("confirm_timeout"); /* usually does not return */
        }

        bool connected = !s.is_connected || s.is_connected(s.user_ctx);
        if (connected) {
            if (do_check() && !s.stop_requested) {
                do_apply(); /* does not return on auto-reboot success */
            }
        } else {
            ESP_LOGD(TAG, "is_connected() false; polling again shortly");
        }

        /* A trial, and a pending report's first few retries, poll fast so
         * the confirm and the report land promptly. Otherwise, including
         * the armed-but-not-yet-rebooted window, the normal interval. */
        bool report_pending = s.outbox.count > 0;
        uint32_t wait_s = sota_poll_interval_s(
            connected, s.in_trial, report_pending, s.report_failures,
            SOTA_TRIAL_RETRY_INTERVAL_S, s.check_interval_s);
        ulTaskNotifyTake(pdTRUE, sota_ms_to_ticks((uint64_t)wait_s * 1000u,
                                                  configTICK_RATE_HZ));
    }

    s.task = NULL;
    xSemaphoreGive(s.stopped_sem);
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

static void default_device_id(char out[18]) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    /* Byte-identical format to the Arduino client so a device keeps its
     * identity across a framework migration. */
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
}

esp_err_t simpleota_init(const simpleota_config_t *config) {
    if (s.initialized) return ESP_ERR_INVALID_STATE;
    if (!config || !token_looks_safe(config->token)) return ESP_ERR_INVALID_ARG;
    if (config->num_signing_keys > SOTA_VERIFIER_MAX_KEYS)
        return ESP_ERR_INVALID_ARG;

#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (!config->cert_pem) {
        ESP_LOGE(TAG,
                 "no TLS trust anchor: enable CONFIG_MBEDTLS_CERTIFICATE_"
                 "BUNDLE or provide cert_pem");
        return ESP_ERR_INVALID_STATE;
    }
#endif

    memset(&s, 0, sizeof(s));

    /* Pin signing keys first; an unparseable key fails init (fail closed,
     * never continue silently unpinned). */
    for (uint8_t i = 0; i < config->num_signing_keys; ++i) {
        const simpleota_signing_key_t *k = &config->signing_keys[i];
        if (!k->pem || !sota_parse_ed25519_public_key_pem(k->pem, s.keys[i])) {
            ESP_LOGE(TAG, "signing key %u: invalid Ed25519 public key PEM", i);
            memset(&s, 0, sizeof(s));
            return ESP_ERR_INVALID_ARG;
        }
        if (k->key_id) strlcpy(s.key_ids[i], k->key_id, sizeof(s.key_ids[i]));
        s.num_keys++;
    }

    s.token = dup_or_null(config->token);
    s.base_url = dup_or_null(config->base_url ? config->base_url
                                              : SOTA_DEFAULT_BASE_URL);
    if (config->device_id) {
        s.device_id = dup_or_null(config->device_id);
    } else {
        char mac[18];
        default_device_id(mac);
        s.device_id = dup_or_null(mac);
    }
    s.chip_family = dup_or_null(config->chip_family ? config->chip_family
                                                    : CONFIG_IDF_TARGET);
    s.board_id = dup_or_null(config->board_id);
    s.hardware_revision = dup_or_null(config->hardware_revision);
    s.partition_profile = dup_or_null(config->partition_profile);
    s.channel = dup_or_null(config->channel);
    s.labels_json = dup_or_null(config->labels_json);
    s.security_mode = dup_or_null(config->security_mode);
    s.version_label = dup_or_null(config->version_label);
    if (!s.token || !s.base_url || !s.device_id || !s.chip_family) {
        simpleota_deinit();
        return ESP_ERR_NO_MEM;
    }

    s.nvs_schema_version =
        config->nvs_schema_version ? config->nvs_schema_version : 1;
    s.check_interval_s = config->check_interval_s ? config->check_interval_s
                                                  : SOTA_DEFAULT_CHECK_INTERVAL_S;
    s.initial_delay_ms = config->initial_delay_ms;
    s.confirm_timeout_s = config->confirm_timeout_s
                              ? config->confirm_timeout_s
                              : SOTA_DEFAULT_CONFIRM_TIMEOUT_S;
    if (s.confirm_timeout_s < 10) s.confirm_timeout_s = 10;
    if (s.confirm_timeout_s > 86400) s.confirm_timeout_s = 86400;
    s.manual_confirm = config->manual_confirm;
    s.disable_auto_reboot = config->disable_auto_reboot;
    s.is_connected = config->is_connected;
    s.event_cb = config->event_cb;
    s.user_ctx = config->user_ctx;
    s.cert_pem = dup_or_null(config->cert_pem);
    if (config->cert_pem && !s.cert_pem) {
        simpleota_deinit();
        return ESP_ERR_NO_MEM;
    }
    s.crt_bundle_attach = config->crt_bundle_attach;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (!s.cert_pem && !s.crt_bundle_attach)
        s.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    s.outbox_lock = xSemaphoreCreateMutexStatic(&s.outbox_lock_buf);
    s.stopped_sem = xSemaphoreCreateBinary();
    if (!s.stopped_sem) {
        simpleota_deinit();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sota_nvs_load(&s.nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs load failed: %s (continuing fresh)",
                 esp_err_to_name(err));
    }

    s.initialized = true;
    sota_nvs_load_outbox(&s.outbox);
    process_boot_validation();

    /* While a flashed update waits for its reboot, the stored build is the
     * flashed one (so check-ins do not get it offered again), and the
     * running image is the one the update replaced. */
    ESP_LOGI(TAG,
             "init: device=%s chip=%s build=%" PRIu32 " keys=%u interval=%" PRIu32
             "s",
             s.device_id, s.chip_family,
             s_update_waiting ? s.nvs.prev_build : s.nvs.build, s.num_keys,
             s.check_interval_s);
    return ESP_OK;
}

esp_err_t simpleota_start(void) {
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    if (s.task) return ESP_OK; /* already running */
    s.stop_requested = false;
    BaseType_t ok =
        xTaskCreate(sota_task, "simpleota", CONFIG_SIMPLEOTA_TASK_STACK_SIZE,
                    NULL, CONFIG_SIMPLEOTA_TASK_PRIORITY, &s.task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t simpleota_stop(void) {
    if (!s.initialized || !s.task) return ESP_OK;
    s.stop_requested = true;
    xTaskNotifyGive(s.task);
    /* The task exits at a safe point (never mid flash write is interrupted;
     * the loop checks the flag between operations). */
    if (xSemaphoreTake(s.stopped_sem, sota_ms_to_ticks(60000, configTICK_RATE_HZ)) != pdTRUE) {
        ESP_LOGW(TAG, "stop: task did not exit within 60 s");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t simpleota_check_now(void) {
    if (!s.initialized || !s.task) return ESP_ERR_INVALID_STATE;
    xTaskNotifyGive(s.task);
    return ESP_OK;
}

esp_err_t simpleota_confirm(void) {
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    if (!s.in_trial) return ESP_OK;
    esp_err_t err = do_confirm();
    if (err != ESP_OK) return err;
    /* Nudge the task so the confirmed report posts promptly. */
    if (s.task) xTaskNotifyGive(s.task);
    return ESP_OK;
}

/* Post the reboot status for the waiting update, then restart. The post
 * needs a TLS handshake, which does not fit the stack of a typical caller
 * (the main task's default is 3584 bytes), so it runs on the component task
 * or on reboot_task(). */
static void post_reboot_and_restart(void) {
    post_status_for("reboot", NULL, s.nvs.fail_dep, s.nvs.fail_build);
    vTaskDelay(sota_ms_to_ticks(100, configTICK_RATE_HZ));
    esp_restart();
}

static void reboot_task(void *arg) {
    (void)arg;
    post_reboot_and_restart();
}

esp_err_t simpleota_reboot_for_update(void) {
    if (!s.initialized || !s_update_waiting) return ESP_ERR_INVALID_STATE;
    /* Called from event_cb, on the component task: its stack is big enough. */
    if (s.task && xTaskGetCurrentTaskHandle() == s.task) post_reboot_and_restart();
    /* Otherwise stop the component task first, so only one TLS session is
     * open at a time, and post from a task with the component's stack. */
    simpleota_stop();
    if (xTaskCreate(reboot_task, "sota_reboot", CONFIG_SIMPLEOTA_TASK_STACK_SIZE,
                    NULL, CONFIG_SIMPLEOTA_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the reboot task; restarting without the status post");
        esp_restart();
    }
    for (;;) vTaskDelay(portMAX_DELAY); /* reboot_task restarts the chip */
    return ESP_OK;                      /* not reached */
}

bool simpleota_is_trial_boot(void) { return s.in_trial; }

uint32_t simpleota_current_build(void) { return s.nvs.build; }

esp_err_t simpleota_deinit(void) {
    if (s.task) simpleota_stop();
    stop_confirm_timer();
    clear_offer();
    free(s.token);
    free(s.base_url);
    free(s.device_id);
    free(s.chip_family);
    free(s.board_id);
    free(s.hardware_revision);
    free(s.partition_profile);
    free(s.channel);
    free(s.labels_json);
    free(s.security_mode);
    free(s.version_label);
    free(s.cert_pem);
    if (s.stopped_sem) vSemaphoreDelete(s.stopped_sem);
    memset(&s, 0, sizeof(s));
    return ESP_OK;
}
