/**
 * sota_trial.h - what a boot means for a saved trial record.
 *
 * The trial record uses NVS keys that every release of both clients reads
 * and writes: sota_trial (1 = trial, 2 = a rollback happened),
 * sota_new_part (where the update went) and sota_prev_part (the partition
 * the update replaced). Arduino releases up to 0.6.0 write only
 * sota_prev_part; ESP-IDF releases write both. The classifier uses
 * sota_new_part when it is there and falls back to sota_prev_part, so a
 * record from any earlier release of either client reads the way that
 * release meant it.
 *
 * The apply path also writes sota_staged before the boot slot switches and
 * removes it straight after, so a record whose slot switch never happened
 * can be told apart from a rollback. A release that does not know
 * sota_staged ignores it, and a record written without it reads like any
 * other, so updates in either direction between releases read the same way.
 *
 * Any trial value other than 0 and 2 is a trial.
 *
 * No platform types, so the host tests can include this header.
 */

#ifndef SOTA_TRIAL_H
#define SOTA_TRIAL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SOTA_BOOT_NONE = 0,       /**< No trial record. */
    SOTA_BOOT_TRIAL,          /**< Running the new image: supervise it. */
    SOTA_BOOT_ROLLED_BACK,    /**< Not on the new image: restore identity, report. */
    SOTA_BOOT_REPORT_PENDING, /**< A previous boot recorded a rollback: report it. */
    SOTA_BOOT_WAITING,        /**< Flashed, not booted yet (deep-sleep wake). */
    SOTA_BOOT_ABANDONED,      /**< The slot switch never happened: drop quietly. */
    SOTA_BOOT_INCOMPLETE,     /**< The record cannot be checked: drop it. */
    SOTA_BOOT_SLEPT_IN_TRIAL, /**< The new image woke from deep sleep unconfirmed: roll back. */
} sota_boot_outcome_t;

/**
 * sota_staged values. The apply path writes STARTED first, then the
 * previous image's identity, then SNAPSHOT, and only then overwrites the
 * live identity with the new build's.
 */
enum {
    SOTA_STAGED_STARTED = 1,
    SOTA_STAGED_SNAPSHOT = 2,
};

typedef struct {
    uint8_t trial;          /**< sota_trial. */
    uint8_t staged;         /**< sota_staged, 0 when missing. */
    uint32_t new_part;      /**< sota_new_part, 0 when missing. */
    uint32_t prev_part;     /**< sota_prev_part, 0 when missing. */
    uint32_t running_part;  /**< Running partition address, 0 when unknown. */
    uint32_t boot_part;     /**< Partition the next boot selects, 0 when unknown. */
    bool deep_sleep_wake;   /**< This boot is a wake from deep sleep. */
} sota_boot_facts_t;

static inline sota_boot_outcome_t sota_classify_boot(const sota_boot_facts_t *f)
{
    /* sota_staged is written first and sota_trial last, so staged with no
     * trial is a record cut short before the slot switch. */
    if (f->trial == 0) return f->staged ? SOTA_BOOT_ABANDONED : SOTA_BOOT_NONE;
    if (f->trial == 2) return SOTA_BOOT_REPORT_PENDING;
    if (f->running_part == 0) return SOTA_BOOT_INCOMPLETE;

    bool on_new;
    if (f->new_part != 0) {
        on_new = f->running_part == f->new_part;
    } else if (f->prev_part != 0) {
        on_new = f->running_part != f->prev_part;
    } else {
        return SOTA_BOOT_INCOMPLETE;
    }

    /* A trial ends at deep sleep. Where the bootloader resumes the image on
     * a wake without validating it (CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_
     * SLEEP), an image that sleeps before it is confirmed would otherwise
     * get a fresh confirm timeout on every wake, and never roll back. */
    if (on_new) return f->deep_sleep_wake ? SOTA_BOOT_SLEPT_IN_TRIAL : SOTA_BOOT_TRIAL;

    /* A deep-sleep wake can resume the old image while the boot slot already
     * points at the new one, so the slot switch did happen. */
    if (f->deep_sleep_wake && f->boot_part != 0 &&
        (f->new_part != 0 ? f->boot_part == f->new_part
                          : f->boot_part != f->running_part)) {
        return SOTA_BOOT_WAITING;
    }
    if (f->staged) return SOTA_BOOT_ABANDONED;
    /* Any other reset that is not on the new image means it did not run,
     * which is a rollback. */
    return SOTA_BOOT_ROLLED_BACK;
}

/**
 * An abandoned record puts the previous identity back only when it was
 * saved in full. Before that, the live identity was not touched.
 */
static inline bool sota_abandon_restores_identity(uint8_t staged)
{
    return staged >= SOTA_STAGED_SNAPSHOT;
}

/**
 * Arduino's verifyRollbackLater(): keep an image the bootloader still has
 * on probation (pending verify) only when it is a SimpleOTA trial. Any
 * other pending image, such as one written by ArduinoOTA, is left for the
 * core to accept.
 */
static inline bool sota_hold_pending_image(bool image_pending, sota_boot_outcome_t outcome)
{
    return image_pending &&
           (outcome == SOTA_BOOT_TRIAL || outcome == SOTA_BOOT_SLEPT_IN_TRIAL);
}

/** esp_reset_reason_t values used for rollback reasons. */
enum {
    SOTA_RST_POWERON = 1,
    SOTA_RST_SW = 3,
    SOTA_RST_PANIC = 4,
    SOTA_RST_INT_WDT = 5,
    SOTA_RST_TASK_WDT = 6,
    SOTA_RST_WDT = 7,
    SOTA_RST_DEEPSLEEP = 8,
    SOTA_RST_BROWNOUT = 9,
};

/**
 * The reset was a crash or a watchdog. A trial image that crashed has
 * failed its trial: without the bootloader's probation it would otherwise
 * get a fresh confirm timeout on every boot, and never roll back.
 */
static inline bool sota_reset_is_crash(int reset_reason)
{
    return reset_reason == SOTA_RST_PANIC || reset_reason == SOTA_RST_INT_WDT ||
           reset_reason == SOTA_RST_TASK_WDT || reset_reason == SOTA_RST_WDT;
}

/** Reason sent with rolled_back, from the reset that ended the trial. */
static inline const char *sota_rollback_reason(int reset_reason)
{
    switch (reset_reason) {
    case SOTA_RST_PANIC: return "panic";
    case SOTA_RST_INT_WDT: return "int_wdt";
    case SOTA_RST_TASK_WDT: return "task_wdt";
    case SOTA_RST_WDT: return "watchdog";
    case SOTA_RST_BROWNOUT: return "brownout";
    case SOTA_RST_POWERON: return "power_on";
    case SOTA_RST_DEEPSLEEP: return "deep_sleep";
    default: return "reset";
    }
}

#endif /* SOTA_TRIAL_H */
