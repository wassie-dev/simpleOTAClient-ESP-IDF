/**
 * sota_nvs.h - NVS persistence for the SimpleOTA component.
 *
 * Namespace "simpleota". The identity and trial keys have the same names
 * and meaning as in SimpleOTAClient-Arduino (all keys are 15 characters or
 * fewer, the NVS limit):
 *
 *   sota_build      u32   build number of the running (committed) image
 *   sota_hash       str   sha256 hex of the running image
 *   sota_ver        str   version label of the running image
 *   sota_trial      u8    0 steady / 1 trial armed / 2 rolled-back, report pending
 *   sota_staged     u8    present while the trial record is written but the
 *                         boot slot has not switched yet: 1 while the prev_*
 *                         snapshot is written, 2 once it is complete
 *                         (see sota_trial.h)
 *   sota_new_part   u32   flash address of the partition the new image went to
 *   sota_prev_part  u32   flash address of the partition we came from
 *   sota_prev_build u32   pre-OTA build number (snapshot for rollback restore)
 *   sota_prev_hash  str   pre-OTA hash
 *   sota_prev_ver   str   pre-OTA version label
 *   sota_fail_dep   str   deployment id the deferred confirmed/rolled_back
 *                         report refers to
 *   sota_fail_build u32   build number for that report
 *   sota_conf_pend  u8    1 while the trial record's "confirmed" report has
 *                         not been queued yet
 *   sota_rb_reason  str   reason token recorded when a rollback is detected
 *                         (see sota_rollback_reason())
 *   sota_outbox     blob  sota_outbox_t: confirmed / rolled_back reports
 *                         waiting to be sent
 */

#ifndef SOTA_NVS_H
#define SOTA_NVS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "sota_report.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOTA_NVS_STR_MAX 96 /* bounds for hash (64 hex) / version / deployment id */

/** Everything the component persists, read in one shot at init. */
typedef struct {
    uint32_t build;
    char hash[SOTA_NVS_STR_MAX];
    char version[SOTA_NVS_STR_MAX];
    uint8_t trial; /* 0 / 1 / 2 */
    uint8_t staged;
    uint32_t new_part;
    uint32_t prev_part;
    uint32_t prev_build;
    char prev_hash[SOTA_NVS_STR_MAX];
    char prev_version[SOTA_NVS_STR_MAX];
    char fail_dep[SOTA_NVS_STR_MAX];
    uint32_t fail_build;
    uint8_t conf_pend;
    char rb_reason[24];
} sota_nvs_state_t;

/** Load all persisted state; missing keys yield zeros/empty strings.
 *  Returns ESP_OK also for a fresh device with no namespace yet. */
esp_err_t sota_nvs_load(sota_nvs_state_t *out);

/**
 * Save the trial record before the boot slot switches: the prev_* snapshot,
 * the partition addresses, the new image's identity as the live one, the
 * report context (fail_dep/fail_build, conf_pend=1) and trial=1, with
 * sota_staged set. Each set reaches flash on its own, so sota_staged is
 * written first and sota_trial last. A record whose switch never happens
 * is dropped at boot, and the prev_* identity restored once the snapshot
 * was complete.
 */
esp_err_t sota_nvs_stage_trial(uint32_t new_part, uint32_t prev_part,
                               uint32_t prev_build, const char *prev_hash,
                               const char *prev_version,
                               const char *deployment_id, uint32_t new_build,
                               const char *new_hash, const char *new_version);

/** The boot slot now points at the new image: remove sota_staged. */
esp_err_t sota_nvs_unstage_trial(void);

/** Write the live identity: build number, hash and version label. */
esp_err_t sota_nvs_set_identity(uint32_t build, const char *hash,
                                const char *version);

/**
 * Roll the live identity back to the prev_* snapshot and set trial=2
 * (rolled-back, report pending). Restores unconditionally: prev_build==0 and
 * empty strings are valid fresh-device state, and skipping them would leave
 * a rolled-back device reporting the new build, which the server would then
 * never offer again.
 */
esp_err_t sota_nvs_rollback_restore(const sota_nvs_state_t *snapshot,
                                    const char *reason);

/** Remove the trial record and its report context. */
esp_err_t sota_nvs_drop_trial(void);

/** Load the report outbox; an empty one when none is stored. */
void sota_nvs_load_outbox(sota_outbox_t *box);

/** Store the report outbox, or remove it when empty. */
esp_err_t sota_nvs_save_outbox(const sota_outbox_t *box);

#ifdef __cplusplus
}
#endif

#endif /* SOTA_NVS_H */
