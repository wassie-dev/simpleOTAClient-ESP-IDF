/**
 * sota_app.h - what the engine tells the application: events, log lines,
 * and a wake-up when the confirm timeout fires. Implemented by each
 * client's adapter and by the host tests' fake.
 */

#ifndef SOTA_APP_H
#define SOTA_APP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** In the order of simpleota_event_id_t, so the ESP-IDF adapter can pass
 *  them through. */
typedef enum {
    SOTA_EVENT_CHECK_DONE,        /**< A check completed; no update offered. */
    SOTA_EVENT_UPDATE_AVAILABLE,  /**< An update was offered. */
    SOTA_EVENT_DOWNLOAD_PROGRESS, /**< done / total advanced. */
    SOTA_EVENT_UPDATE_FAILED,     /**< The update was dropped; reason says why. */
    SOTA_EVENT_BEFORE_REBOOT,     /**< Flashed and verified; the reboot comes next. */
    SOTA_EVENT_TRIAL_BOOT,        /**< Running an image that is not confirmed yet. */
    SOTA_EVENT_CONFIRMED,         /**< The trial image's confirmed report was delivered. */
    SOTA_EVENT_ROLLING_BACK,      /**< The trial failed; the previous image starts next. */
} sota_event_id_t;

typedef struct {
    sota_event_id_t id;
    const char *reason; /**< A reason token or server status, or NULL. */
    uint32_t build;     /**< The build the event is about, 0 when none. */
    size_t done;        /**< DOWNLOAD_PROGRESS: bytes written. */
    size_t total;       /**< DOWNLOAD_PROGRESS: bytes expected, 0 when unknown. */
} sota_event_t;

void sota_app_event(void *user, const sota_event_t *event);

/** The confirm timeout fired, possibly in a timer task: arrange for
 *  sota_engine_service() to run soon on a task that may block. */
void sota_app_wake(void *user);

enum { SOTA_LOG_NONE, SOTA_LOG_ERROR, SOTA_LOG_WARN, SOTA_LOG_INFO, SOTA_LOG_DEBUG };

void sota_app_log(int level, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Lines above SOTA_LOG_LEVEL are not compiled in. */
#ifndef SOTA_LOG_LEVEL
#define SOTA_LOG_LEVEL SOTA_LOG_INFO
#endif

#define SOTA_LOG_AT(level, ...) \
    do { if (SOTA_LOG_LEVEL >= (level)) sota_app_log((level), __VA_ARGS__); } while (0)
#define SOTA_LOGE(...) SOTA_LOG_AT(SOTA_LOG_ERROR, __VA_ARGS__)
#define SOTA_LOGW(...) SOTA_LOG_AT(SOTA_LOG_WARN, __VA_ARGS__)
#define SOTA_LOGI(...) SOTA_LOG_AT(SOTA_LOG_INFO, __VA_ARGS__)
#define SOTA_LOGD(...) SOTA_LOG_AT(SOTA_LOG_DEBUG, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* SOTA_APP_H */
