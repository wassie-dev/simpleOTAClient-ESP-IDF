/**
 * sota_guard.h - download-guard, waiting-for-reboot, and status-report
 * decisions.
 *
 * Pure helpers so the host tests can lock the policy without ESP-IDF.
 * Image-state values match esp_ota_img_states_t.
 */

#ifndef SOTA_GUARD_H
#define SOTA_GUARD_H

#include <stdbool.h>
#include <stdint.h>

/** esp_ota_img_states_t values used to tell a waiting image from a rollback. */
#define SOTA_IMG_NEW 0x0u
#define SOTA_IMG_PENDING_VERIFY 0x1u
#define SOTA_IMG_VALID 0x2u
#define SOTA_IMG_INVALID 0x3u
#define SOTA_IMG_ABORTED 0x4u

typedef enum {
    SOTA_APPLY_DOWNLOAD = 0,
    SOTA_APPLY_WAIT_REBOOT,
    SOTA_APPLY_IN_TRIAL,
    SOTA_APPLY_ROLLBACK_PENDING,
} sota_apply_decision_t;

/**
 * Whether the offer may be downloaded.
 *
 * in_trial: this boot is a supervised SimpleOTA trial.
 * pending_reboot: a flashed image is waiting for reboot.
 * trial: NVS trial byte (1 armed, 2 rolled_back report not yet accepted).
 *
 * An unconfirmed image this component is not supervising is not a reason
 * to skip. esp_ota_begin rejects PENDING_VERIFY and that failure is
 * reported. trial==1 while this boot is not the trial means the flashed
 * image has not been booted yet; downloading would erase that slot.
 * trial==2 must not start a new trial: arming one overwrites the
 * unaccepted rolled_back report.
 */
static inline sota_apply_decision_t sota_apply_decision(bool pending_reboot,
                                                       bool in_trial,
                                                       uint8_t trial)
{
    if (in_trial) return SOTA_APPLY_IN_TRIAL;
    if (pending_reboot || trial == 1) return SOTA_APPLY_WAIT_REBOOT;
    if (trial == 2) return SOTA_APPLY_ROLLBACK_PENDING;
    return SOTA_APPLY_DOWNLOAD;
}

/** CHECK_DONE reason for a skip, or NULL when the offer is downloaded. */
static inline const char *sota_apply_skip_reason(sota_apply_decision_t decision)
{
    switch (decision) {
    case SOTA_APPLY_WAIT_REBOOT: return "waiting_for_reboot";
    case SOTA_APPLY_IN_TRIAL: return "trial_in_progress";
    case SOTA_APPLY_ROLLBACK_PENDING: return "rolled_back_pending";
    default: return NULL;
    }
}

/**
 * True when an armed trial has not been booted yet.
 *
 * boot_is_new: the boot partition address is the flashed slot.
 * state_known: false when this build has no otadata state byte (rollback
 * off). When the byte exists it must still be ESP_OTA_IMG_NEW. ABORTED,
 * INVALID, and PENDING_VERIFY are real rollbacks or an image the
 * bootloader has already tried. A failed read of a state byte that
 * exists is not "unknown" here; the caller treats that as not waiting.
 */
static inline bool sota_waiting_for_reboot(bool boot_is_new, bool state_known,
                                          uint32_t new_slot_state)
{
    if (!boot_is_new) return false;
    if (!state_known) return true;
    return new_slot_state == SOTA_IMG_NEW;
}

/**
 * How a status POST ended, for reports that are retried until the server
 * answers.
 *
 * SOTA_STATUS_ACCEPTED  2xx with accepted=true.
 * SOTA_STATUS_RETRY     No response, a 5xx, 401, 403, 408, or 429, or a
 *                       2xx without an accepted field. Worth sending again.
 * SOTA_STATUS_REJECTED  2xx with accepted=false, or any other 4xx. Sending
 *                       the same report again gets the same answer.
 */
typedef enum {
    SOTA_STATUS_ACCEPTED = 1,
    SOTA_STATUS_RETRY,
    SOTA_STATUS_REJECTED,
} sota_status_outcome_t;

static inline sota_status_outcome_t sota_status_outcome(int http_code,
                                                       bool has_accepted_field,
                                                       bool accepted)
{
    if (http_code >= 200 && http_code < 300) {
        if (!has_accepted_field) return SOTA_STATUS_RETRY;
        return accepted ? SOTA_STATUS_ACCEPTED : SOTA_STATUS_REJECTED;
    }
    if (http_code >= 400 && http_code < 500) {
        if (http_code == 401 || http_code == 403 || http_code == 408 ||
            http_code == 429) {
            return SOTA_STATUS_RETRY;
        }
        return SOTA_STATUS_REJECTED;
    }
    return SOTA_STATUS_RETRY;
}

#endif /* SOTA_GUARD_H */
