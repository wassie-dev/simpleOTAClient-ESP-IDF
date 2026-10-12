/**
 * sota_trial.h - what a boot means for a saved trial record.
 *
 * The trial record uses the NVS keys that every release of this component
 * reads and writes: sota_trial (1 = trial, 2 = a rollback happened),
 * sota_new_part (where the update went) and the prev_* snapshot. do_apply()
 * also writes sota_staged before the boot slot switches and removes it
 * straight after, so a record whose slot switch never happened can be told
 * apart from a rollback. A release that does not know sota_staged ignores
 * it, and a record written without it reads like any other, so updates in
 * either direction between releases read the same way.
 *
 * Any trial value other than 0 and 2 is a trial.
 *
 * No ESP-IDF types, so the host tests can include this header.
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
 * sota_staged values. do_apply() writes STARTED first, then the previous
 * image's identity, then SNAPSHOT, and only then overwrites the live
 * identity with the new build's.
 */
enum {
    SOTA_STAGED_STARTED = 1,
    SOTA_STAGED_SNAPSHOT = 2,
};

typedef struct {
    uint8_t trial;          /**< sota_trial. */
    uint8_t staged;         /**< sota_staged, 0 when missing. */
    uint32_t new_part;      /**< sota_new_part, 0 when missing. */
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
    if (f->running_part == 0 || f->new_part == 0) return SOTA_BOOT_INCOMPLETE;
    /* A trial ends at deep sleep. With CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_
     * DEEP_SLEEP the bootloader resumes the image on a wake, which would
     * otherwise give an image that sleeps before it is confirmed a fresh
     * confirm timeout on every wake, so it would never roll back. */
    if (f->running_part == f->new_part) {
        return f->deep_sleep_wake ? SOTA_BOOT_SLEPT_IN_TRIAL : SOTA_BOOT_TRIAL;
    }
    /* A deep-sleep wake can resume the old image while the boot slot already
     * points at the new one, so the slot switch did happen. */
    if (f->deep_sleep_wake && f->boot_part == f->new_part) return SOTA_BOOT_WAITING;
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
